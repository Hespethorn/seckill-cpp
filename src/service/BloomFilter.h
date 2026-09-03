// 5.7 进程内布隆过滤器（自实现）——防缓存穿透的"集合预过滤"
//
// 定位与空值哨兵（5.6）的分工：
//   5.6 空值哨兵：把"查无此物"也缓存下来，挡住对**同一个不存在 id** 的重复穿透。
//                代价：每个不存在的 id 都要占一个 key（攻击者用海量随机 id 打，
//                内存会跟着涨）。
//   5.7 布隆过滤器：在内存里维护"DB 中真实存在的 sku id 全集"的紧凑摘要。
//                查询先过布隆——它说"一定不存在"，那就真不存在，**连 Redis 都不打**，
//                直接返回 404；它说"可能存在"才放行进缓存/DB。代价是**误判率**：
//                可能有 0.1% 的真实查询被放行（只是多打一次 DB），但绝无漏报。
//
// 一句话：**空值哨兵挡"重复打同一个不存在的 id"，布隆挡"海量随机不存在的 id"。**
// 二者互补而非替代：布隆挡的是全集之外，哨兵挡的是布隆放行后仍 miss 的边角。
//
// 集合从哪来：秒杀商品的 sku 集合在活动开始前就定死、几乎不变，正是布隆喜欢的
// "海量且基本静态"场景（docs/CACHE-DESIGN.md §3.3 当时说"等商品量级上去再引入"，
// 本 5.7 落地时种子数据已是 20 万量级，条件成立）。
//
// 为什么自实现而不引三方库：
//   - 布隆过滤器的全部内涵是"m 个 bit + k 个哈希 + double hashing"，约 60 行；
//   - 本项目一贯原则：能用已在场依赖自写的就不引新库（对比自实现 JWT HS256）。
//
// 线程模型（本文件不内置锁，靠外层用"构建隔离 + 整体替换"保证并发安全）：
//   构建期只由单个预热线程 add，完成后整体替换指针发布；查询期 only maybeContains，
//   无写并发，因此不需要锁。ready()==false 时 maybeContains 恒返回 true（放行）——
//   fail-open：布隆没建好（冷启动 / 未预热）时绝不能误杀正常请求。
#pragma once

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace seckill::cache {

class BloomFilter {
public:
    // 空对象：ready()==false，maybeContains 恒 true（fail-open，见文件头）。
    BloomFilter() = default;

    // 按"预期元素数 + 允许误判率"创建。
    //   m = ceil(-n * ln(p) / ln(2)^2)   bit 数
    //   k = max(1, round(m / n * ln(2))) 哈希个数
    // 注意 capacity 是**预期**值：实际插入超过它时误判率上升（不会出错）。
    static std::shared_ptr<BloomFilter> create(std::size_t expectedCount,
                                               double falsePositiveRate) {
        if (expectedCount == 0) expectedCount = 1;
        if (falsePositiveRate <= 0.0) falsePositiveRate = 1e-6;
        if (falsePositiveRate >= 1.0) falsePositiveRate = 0.99;
        const double ln2 = std::log(2.0);
        const std::size_t m = static_cast<std::size_t>(
            std::ceil(-static_cast<double>(expectedCount) * std::log(falsePositiveRate) /
                      (ln2 * ln2)));
        int k = static_cast<int>(std::round(
            static_cast<double>(m) / static_cast<double>(expectedCount) * ln2));
        if (k < 1) k = 1;
        // 持有构造私有权限的辅助
        struct Maker : BloomFilter {
            Maker(std::size_t bits, int hashes) : BloomFilter(bits, hashes) {}
        };
        return std::make_shared<Maker>(m, k);
    }

    bool ready() const { return bitsCount_ > 0; }

    // 插入（仅构建期调用，单线程）。
    void add(uint64_t x) {
        if (!ready()) return;
        uint64_t h1 = splitmix64(x);
        uint64_t h2 = splitmix64(x ^ 0x94d049bb133111ebULL);
        if (h2 == 0) h2 = 1;  // 双哈希要求第二路非零，否则所有位索引坍缩到第一路
        for (int i = 0; i < hashes_; ++i) {
            setBit(static_cast<std::size_t>((h1 + static_cast<uint64_t>(i) * h2) %
                                            bitsCount_));
        }
        added_.fetch_add(1, std::memory_order_relaxed);
    }

    // 查询。false = **一定**不在集合里；true = 可能在（也可能是误判）。
    // 未构建（ready=false）时恒 true：布隆没建好就放行，绝不误杀。
    bool maybeContains(uint64_t x) const {
        if (!ready()) return true;
        uint64_t h1 = splitmix64(x);
        uint64_t h2 = splitmix64(x ^ 0x94d049bb133111ebULL);
        if (h2 == 0) h2 = 1;
        for (int i = 0; i < hashes_; ++i) {
            if (!testBit(static_cast<std::size_t>(
                    (h1 + static_cast<uint64_t>(i) * h2) % bitsCount_)))
                return false;
        }
        return true;
    }

    std::size_t bitCount() const { return bitsCount_; }
    int numHashes() const { return hashes_; }
    std::size_t addedCount() const {
        return added_.load(std::memory_order_relaxed);
    }

private:
    void setBit(std::size_t idx) {
        bits_[idx >> 6] |= (uint64_t{1} << (idx & 63));
    }
    bool testBit(std::size_t idx) const {
        return (bits_[idx >> 6] & (uint64_t{1} << (idx & 63))) != 0;
    }

protected:
    // create() 用 Maker 子类构造（位参数只能由 create 按公式推导，不对外暴露）。
    BloomFilter(std::size_t bits, int hashes)
        : bits_((bits + 63) / 64), bitsCount_(bits), hashes_(hashes) {}

private:
    // splitmix64：一个足够好的 64 位整数混合器，任何 64 位输入都均匀散开。
    // 用它生成两路独立哈希（h1、h2 = splitmix64(x ^ 固定盐)），再做双哈希。
    static uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
        return x ^ (x >> 31);
    }

    std::vector<uint64_t> bits_;
    std::size_t bitsCount_ = 0;
    int hashes_ = 0;
    std::atomic<std::size_t> added_{0};
};

}  // namespace seckill::cache
