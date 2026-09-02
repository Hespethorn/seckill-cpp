// 应用层"在途请求"闸门（4.8）：用锁优化重复下单
//
// ── 先说清一个致命误区 ──────────────────────────────────────────────
// 最直觉的写法是：拿 std::mutex 把整个 doSeckill 包起来，锁在开头、解锁在 DB 回调里。
// 这在 Drogon 里是灾难，有两个独立的原因：
//   1) 阻塞 IO 线程：Drogon 的 handler 跑在 IO 线程上（本项目 threads_num=4）。
//      一个请求持锁等待 MySQL 往返（几 ms），就把这 1/4 的请求全部卡住，QPS 直接掉到个位数。
//   2) 锁的"所有权"跨了异步边界：锁在 A 上下文加、在 B 上下文解，
//      一旦某条异常路径忘了释放，这个 sku 就永久不可买了——而且极难复现。
//
// ── 正确的切法 ──────────────────────────────────────────────────────
// 把"锁"的作用域缩到极致：只保护**在途标记**这个纳秒级的临界区。
//   tryAcquire(key)：DB 调用之前，先在同一用户+同一商品的维度上标记"我正在处理"；
//                    标记失败说明已有一个在途请求，直接判重复下单，**连 DB 都不用打**。
//   release(key)   ：DB 回调返回时清除标记。用 shared_ptr 的 deleter 自动释放，杜绝漏放。
// 于是并发的重复请求不再去挤 MySQL 行锁，而是在应用层就被挡掉——
// 这才是 4.8 真正的收益：不是"加锁变快了"，而是"把无效请求挡在了数据库之外"。
//
// ── 三种后端与取舍 ──────────────────────────────────────────────────
//   Mutex  分片互斥锁 + 哈希集合：精确判重，但线程会在锁上睡眠/唤醒（一次上下文切换约 1~2us）
//   Spin   分片自旋锁 + 哈希集合：精确判重，临界区只有一次哈希插入（~100ns），
//                                 自旋等待比睡眠唤醒更快；代价是持锁期间占满一个核空转
//   Atomic 无锁位图（fetch_or） ：零等待零切换，但位图容量有限 → hash 冲突会**误挡**
//                                 无关请求（把它判成重复下单）。用精确性换极致延迟。
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_set>

namespace seckill {

// 自旋锁：临界区极短（一次哈希插入，约百纳秒）时比互斥锁快。
// 互斥锁的 sleep/wake 一次就是微秒级上下文切换，比临界区本身还贵。
class SpinLock {
public:
    void lock() {
        while (flag_.test_and_set(std::memory_order_acquire)) {
            // 空转等待。临界区是纳秒级的，转几圈就出去了，不值得睡眠。
        }
    }
    void unlock() { flag_.clear(std::memory_order_release); }

private:
    std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
};

// 在途标记的 RAII 句柄。可移动、不可拷贝；最后一个副本析构时自动释放。
// 内部用 shared_ptr 的自定义 deleter 实现"最后一个持有者负责释放"——
// 因为 Drogon 的回调链会把 token 拷贝进每一层 lambda，不能靠单一析构点。
class InflightToken {
public:
    InflightToken() = default;
    InflightToken(const InflightToken &) = default;
    InflightToken &operator=(const InflightToken &) = default;
    InflightToken(InflightToken &&) noexcept = default;
    InflightToken &operator=(InflightToken &&) noexcept = default;

    bool valid() const { return static_cast<bool>(owner_); }

private:
    friend class InflightGuard;
    explicit InflightToken(std::shared_ptr<void> owner) : owner_(std::move(owner)) {}
    std::shared_ptr<void> owner_;
};

class InflightGuard {
public:
    enum class Mode { None, Mutex, Spin, Atomic };

    // bits：Atomic 模式的位图容量（位）。越大误挡率越低，内存 = bits/8 字节。
    // 默认 65536 位 = 8KB，100 并发在途时误挡率约 0.15%。
    explicit InflightGuard(Mode mode, std::size_t shards = 64, std::size_t bits = 1u << 16)
        : mode_(mode), shards_(shards), bits_(bits) {
        if (mode_ == Mode::Mutex) {
            mutexes_ = std::make_unique<std::mutex[]>(shards_);
            sets_ = std::make_unique<std::unordered_set<std::uint64_t>[]>(shards_);
        } else if (mode_ == Mode::Spin) {
            spins_ = std::make_unique<SpinLock[]>(shards_);
            sets_ = std::make_unique<std::unordered_set<std::uint64_t>[]>(shards_);
        } else if (mode_ == Mode::Atomic) {
            words_ = (bits_ + 63) / 64;
            bitmap_ = std::make_unique<std::atomic<std::uint64_t>[]>(words_);
            for (std::size_t i = 0; i < words_; ++i) {
                bitmap_[i].store(0, std::memory_order_relaxed);
            }
        }
    }

    // 把 (userId, skuId) 混成一个 uint64 键。
    // 用 splitmix64 风格的乘法混合而不是简单位拼接：拼接会让"相邻的 userId"
    // 落在同一个分片/相邻位上，分片锁退化成全局锁、位图冲突率飙升。
    static std::uint64_t makeKey(std::int64_t userId, std::int64_t skuId) {
        std::uint64_t h = static_cast<std::uint64_t>(userId) * 0x9E3779B97F4A7C15ull;
        h ^= static_cast<std::uint64_t>(skuId) + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= h >> 29;
        h *= 0xBF58476D1CE4E5B9ull;
        h ^= h >> 32;
        return h;
    }

    // 标记在途。返回 valid()==false 表示"已经有同键请求在处理"——调用方应直接判重复。
    InflightToken tryAcquire(std::uint64_t key) {
        bool got = false;
        switch (mode_) {
            case Mode::None:
                got = true;  // 关闭闸门：只返回一个空壳 token，让调用方代码保持统一
                break;
            case Mode::Mutex:
            case Mode::Spin: {
                const std::size_t idx = key % shards_;
                if (mode_ == Mode::Mutex) {
                    std::lock_guard<std::mutex> lk(mutexes_[idx]);
                    got = sets_[idx].insert(key).second;
                } else {
                    std::lock_guard<SpinLock> lk(spins_[idx]);
                    got = sets_[idx].insert(key).second;
                }
                break;
            }
            case Mode::Atomic: {
                const std::size_t word = (key % bits_) >> 6;
                const std::uint64_t mask = 1ull << (key & 63);
                const std::uint64_t before =
                    bitmap_[word].fetch_or(mask, std::memory_order_acq_rel);
                got = (before & mask) == 0;
                break;
            }
        }

        if (got) {
            acquired_.fetch_add(1, std::memory_order_relaxed);
        } else {
            rejected_.fetch_add(1, std::memory_order_relaxed);
        }
        if (!got) return InflightToken();

        // deleter 负责释放：最后一个 shared_ptr 副本析构时才跑一次
        std::shared_ptr<void> owner(static_cast<void *>(this), [this, key](void *) {
            release(key);
        });
        return InflightToken(std::move(owner));
    }

    Mode mode() const { return mode_; }
    // 压测用来量化收益：应用层挡掉了多少次本会打到 DB 的请求
    std::uint64_t acquired() const { return acquired_.load(std::memory_order_relaxed); }
    std::uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

private:
    void release(std::uint64_t key) {
        switch (mode_) {
            case Mode::Mutex:
            case Mode::Spin: {
                const std::size_t idx = key % shards_;
                if (mode_ == Mode::Mutex) {
                    std::lock_guard<std::mutex> lk(mutexes_[idx]);
                    sets_[idx].erase(key);
                } else {
                    std::lock_guard<SpinLock> lk(spins_[idx]);
                    sets_[idx].erase(key);
                }
                break;
            }
            case Mode::Atomic: {
                const std::size_t word = (key % bits_) >> 6;
                const std::uint64_t mask = 1ull << (key & 63);
                bitmap_[word].fetch_and(~mask, std::memory_order_acq_rel);
                break;
            }
            case Mode::None:
                break;
        }
    }

    Mode mode_;
    std::size_t shards_;
    std::size_t bits_;
    std::size_t words_ = 0;

    std::unique_ptr<std::mutex[]> mutexes_;
    std::unique_ptr<SpinLock[]> spins_;
    std::unique_ptr<std::unordered_set<std::uint64_t>[]> sets_;
    std::unique_ptr<std::atomic<std::uint64_t>[]> bitmap_;

    std::atomic<std::uint64_t> acquired_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

}  // namespace seckill
