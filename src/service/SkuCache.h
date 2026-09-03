// 商品读缓存（第五章 5.2 列表 / 5.3 详情）——Redis 异步客户端封装
//
// 定位：**缓存是加速层，不是真相源**。真相永远在 MySQL。因此本类所有读失败
// （Redis 未连、超时、返回异常）都表现为"未命中"，由调用方回源数据库，
// 而不是把错误抛给客户端 —— 这叫 **fail-open**。
//
// 对比登录模块的 fail-close（SessionStore::exists 在 Redis 异常时返回 false = 拒绝）：
//   同一个系统里两个组件对"Redis 挂了"做出相反反应，这不是双标，是代价不同：
//     · 会话校验错放一次 = 被封禁的 token 能继续下单（安全事件，不可逆）
//     · 缓存错放一次     = 这个请求多查一次数据库（慢一点，无正确性损失）
//   判据只有一句：**误放的代价 vs 误杀的代价，哪个更大就防哪个**。
//
// 为什么用 Drogon 内置 RedisClient 而不是 redis-plus-plus：后者是同步 API，
// 在 IO 线程（threads_num=4）里同步等一次 Redis 往返，会把该线程上排队的所有请求卡住。
// 详见 docs/PLAN.md ADR-1。
//
// Value 一律存**紧凑 JSON 字符串**（不是 Hash）：
//   · 读一次 GET 直接拿到完整对象，不需要 HGETALL 再拼装；
//   · 商品详情整体作为一个值失效，不存在"改了 name 忘了改 stock"的半更新态；
//   · 代价是更新必须整值覆盖 —— 但我们压根不更新，只删除（见 invalidate）。
#pragma once

#include <drogon/nosql/RedisClient.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

#include "CacheKeys.h"

namespace seckill::cache {

class SkuCache {
public:
    // 下单成功后删除哪些 key —— 这是"库存新鲜度"与"缓存命中率"的直接交换：
    //   None        谁都不删，只靠 TTL 自愈。命中率最高（压测数字最漂亮），
    //               但用户可能在售罄后最长 TTL 秒内仍看到"还有货"。
    //   Item        只删这一个 sku 的详情（默认）。详情页的库存相对新鲜，
    //               列表页的 stock 允许陈旧 —— 真实秒杀里列表页本就该静态化，
    //               且"列表里剩多少件"的精度对下单决策没有意义。
    //   ItemAndList 详情 + 列表都删。最一致，但在写 QPS 高时列表缓存会被
    //               任意 sku 的成交反复删掉（列表是全量聚合 key），命中率趋零 ——
    //               这就是为什么它不是默认值。
    enum class InvalidateOnOrder { None, Item, ItemAndList };

    struct Config {
        bool enabled = true;
        int listTtlSeconds = 30;    // 列表：库存聚合值，容忍一定陈旧
        int detailTtlSeconds = 60;  // 详情：单品，TTL 可以比列表长
        int nullTtlSeconds = 60;    // 空值占位（防穿透 5.6），必须短
        int jitterSeconds = 30;     // TTL 随机抖动上限，防同时失效（雪崩）
        InvalidateOnOrder invalidateOnOrder = InvalidateOnOrder::Item;
    };

    // 空值哨兵：DB 里确实没有这个 sku 时，缓存这个字符串而不是让请求穿透。
    // 取值必须满足"绝不可能与合法载荷混淆"——业务 value 只能是 JSON 对象或数组，
    // 一个裸的 __nil__ 字符串不可能是合法 JSON，所以用它做哨兵是安全的。
    static constexpr const char *kNullValue = "__nil__";

    SkuCache(drogon::nosql::RedisClientPtr redis, CacheKeys keys, Config cfg)
        : redis_(std::move(redis)), keys_(std::move(keys)), cfg_(cfg) {}

    // enabled=false 时所有方法都是空操作，调用方（SeckillService）据此直连 DB。
    bool enabled() const { return cfg_.enabled; }
    const CacheKeys &keys() const { return keys_; }
    const Config &config() const { return cfg_; }

    // ── 读 ────────────────────────────────────────────────────────────
    // cb(hit, value)：
    //   hit=true  -> value 是缓存里的原始字符串（可能是 kNullValue 空值哨兵）
    //   hit=false -> 未命中 / 缓存不可用，调用方应回源 DB（本类绝不返回"错误"）
    using GetCallback = std::function<void(bool, const std::string &)>;

    void getList(GetCallback &&cb);
    void getDetail(int64_t skuId, GetCallback &&cb);

    // ── 写 ────────────────────────────────────────────────────────────
    // 全部 fire-and-forget：回写失败只记日志，不影响已经拿到数据的请求。
    // 回写慢/失败的最坏结果是下次还是 miss，绝不会让本次请求失败。
    void setList(const std::string &json);
    void setDetail(int64_t skuId, const std::string &json);
    void setNull(int64_t skuId);

    // ── 失效（写侧）────────────────────────────────────────────────────
    // 下单成功后调用：DEL 详情 + 列表。
    //
    // 为什么是"删"不是"改"：
    //   1) 列表 value 里含全部 sku 的 stock，要改就得重算整个 JSON，不如交给下次读重建；
    //   2) 改值要回答"我手上的新库存来自哪个事务"，时序一乱就把旧值盖回去；
    //      DEL 是幂等的，且与时序无关 —— 任意一次 DEL 之后到达的读请求必然 miss 重建。
    //
    // 为什么必须"先提交 DB 再 DEL"（顺序不能反）：
    //   反序（DEL → 更新 DB）的经典事故：DEL 之后、DB 更新之前来了一个读请求，
    //   它 miss 回源读到**旧值**并写回缓存；等 DB 更新完，缓存里已经是脏的旧值，
    //   且要等 TTL 到期才自愈——这就是最常见的"缓存不一致"来源。
    //   本项目的 DEL 挂在事务 commit 回调里（见 SeckillService::doSeckill），
    //   天然保证了 DB 先落定。
    //   5.4 会展开：即便顺序对了，并发下仍有一个窄窗口能让旧值回填 → 延迟双删。
    void invalidate(int64_t skuId);      // 详情 + 列表（ItemAndList）
    void invalidateItem(int64_t skuId);  // 只删详情
    void invalidateList();               // 只删列表

    // 按配置决定下单后删哪些 key（把上面的策略判断收在一处，业务代码不写 if）
    void invalidateOnOrder(int64_t skuId);

    // ── 可观测 ────────────────────────────────────────────────────────
    // 没有这几个计数器，"缓存到底生效了没有"就只能靠感觉。压测时看 hit/(hit+miss)。
    struct Stats {
        uint64_t hit = 0;
        uint64_t miss = 0;
        uint64_t err = 0;   // Redis 异常次数（不区分读写）
        uint64_t write = 0; // 回写次数（含空值占位）
    };
    Stats stats() const;

private:
    // 实际 TTL = 基准 + [0, jitter) 随机。
    // 没有抖动时，同一批 key 会在同一秒集体失效，全部请求瞬间回源 DB —— 缓存雪崩。
    // 抖动把这个"瞬间"摊平到一个时间窗里。
    int ttlWithJitter(int baseSeconds) const;

    void get(const std::string &key, GetCallback &&cb);
    void setex(const std::string &key, const std::string &value, int ttlSeconds);
    void del(const std::string &key);

    drogon::nosql::RedisClientPtr redis_;
    CacheKeys keys_;
    Config cfg_;

    mutable std::atomic<uint64_t> hit_{0};
    mutable std::atomic<uint64_t> miss_{0};
    mutable std::atomic<uint64_t> err_{0};
    mutable std::atomic<uint64_t> write_{0};
};

}  // namespace seckill::cache
