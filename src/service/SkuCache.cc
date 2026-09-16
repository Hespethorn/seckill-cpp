#include "SkuCache.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "logging/LogStream.h"

namespace seckill::cache {

SkuCache::SkuCache(drogon::nosql::RedisClientPtr redis, CacheKeys keys, Config cfg)
    : redis_(std::move(redis)), keys_(std::move(keys)), cfg_(cfg) {
    // 5.8 L1 本地 LRU：多级缓存的第一级。命中走内存，省掉 L2 Redis 的网络往返。
    // 与 Redis 无依赖（进程内结构），但整体仍在 cache.enabled 之下（见 get）。
    if (cfg_.localEnabled) {
        local_ = std::make_unique<LocalLruCache>(cfg_.localCapacity);
        SK_LOG_INFO << "cache L1 local-lru enabled: capacity=" << cfg_.localCapacity;
    }
    // doubleDeleteMs > 0 才拉起延迟删除线程；0 是默认（单 DEL + TTL 自愈已够用）。
    // 线程的 fire 回调直接落到本类的 del()：延迟线程析构先于 redis_（成员逆序析构），
    // 所以 join 期间访问 redis_ 是安全的。
    if (cfg_.doubleDeleteMs > 0 && redis_) {
        delayDeleter_ = std::make_unique<DelayDeleter>(
            std::chrono::milliseconds(cfg_.doubleDeleteMs),
            [this](std::vector<std::string> keys) {
                delayedDelete_.fetch_add(1, std::memory_order_relaxed);
                for (const auto &k : keys) del(k);
            });
        SK_LOG_INFO << "cache double-delete enabled: delay=" << cfg_.doubleDeleteMs
                    << "ms";
    }
}

SkuCache::~SkuCache() {
    // 先停延迟删除线程（unique_ptr 析构会 join），再按声明逆序析构其余成员。
    delayDeleter_.reset();
}

void SkuCache::scheduleDelayedDelete(std::vector<std::string> keys) {
    if (delayDeleter_) delayDeleter_->schedule(std::move(keys));
}

int SkuCache::ttlWithJitter(int baseSeconds) const {
    if (cfg_.jitterSeconds <= 0) return baseSeconds;
    // thread_local 是刻意的：Drogon 的 handler 跑在多个 IO 线程上，
    // 用全局 rand()/一个共享 mt19937 会构成数据竞争（虽然只是"抖动值算错"这种轻后果，
    // 但 UB 就是 UB）。每线程一份随机数发生器，既无锁也无竞争。
    static thread_local std::mt19937 rng{std::random_device{}()};
    std::uniform_int_distribution<int> dist(0, cfg_.jitterSeconds);
    return baseSeconds + dist(rng);
}

void SkuCache::get(const std::string &key, GetCallback &&cb) {
    if (!cfg_.enabled || !redis_) {
        cb(false, std::string());
        return;
    }
    // ── 5.8 L1：先查本地 LRU（同步，命中省一次 Redis 网络往返）───────────
    // 命中直接回调返回，不回源；计数 hit 与 localHit 都加（localHit ⊆ hit，
    // 供 /api/cache/stats 观察 L1 分担了多少命中）。
    if (local_) {
        auto v = local_->get(key);
        if (v) {
            hit_.fetch_add(1, std::memory_order_relaxed);
            localHit_.fetch_add(1, std::memory_order_relaxed);
            cb(true, *v);
            return;
        }
    }
    redis_->execCommandAsync(
        [this, cb](const drogon::nosql::RedisResult &r) {
            std::string v;
            try {
                v = r.asString();
            } catch (const std::exception &) {
                // GET 未命中时 Redis 回 nil，Drogon 的 asString() 对非字符串类型抛异常。
                // 这里刻意不去判 RedisResultType 枚举：缓存的读语义只有"命中/未命中"，
                // 取不到字符串就等于没命中，细分类型没有业务价值，反而多一处版本耦合。
                v.clear();
            }
            if (v.empty()) {
                miss_.fetch_add(1, std::memory_order_relaxed);
                cb(false, std::string());
                return;
            }
            hit_.fetch_add(1, std::memory_order_relaxed);
            cb(true, v);
        },
        [this, cb, key](const std::exception &e) {
            err_.fetch_add(1, std::memory_order_relaxed);
            SK_LOG_ERROR << "CACHE_GET_FAILED key=" << key << " err=" << e.what();
            // fail-open：缓存挂了表现为未命中，请求继续回源 DB（见头文件顶部论述）
            cb(false, std::string());
        },
        "GET %s", key.c_str());
}

void SkuCache::setex(const std::string &key, const std::string &value, int ttlSeconds) {
    if (!cfg_.enabled || !redis_) return;
    write_.fetch_add(1, std::memory_order_relaxed);
    // 结果回调是空的：回写是"尽力而为"，成功与否都不改变本次请求的响应。
    // 若回写失败，下次读会 miss，再查一次库而已 —— 正确性由数据库保证。
    redis_->execCommandAsync(
        [](const drogon::nosql::RedisResult &) {},
        [this, key](const std::exception &e) {
            err_.fetch_add(1, std::memory_order_relaxed);
            SK_LOG_ERROR << "CACHE_SET_FAILED key=" << key << " err=" << e.what();
        },
        "SETEX %s %d %s", key.c_str(), ttlSeconds, value.c_str());
    // 5.8 L1：同步写本地（TTL 与 Redis 同值，失效语义一致）
    if (local_) local_->put(key, value, ttlSeconds);
}

void SkuCache::del(const std::string &key) {
    if (!cfg_.enabled || !redis_) return;
    redis_->execCommandAsync(
        [](const drogon::nosql::RedisResult &) {},
        [this, key](const std::exception &e) {
            err_.fetch_add(1, std::memory_order_relaxed);
            SK_LOG_ERROR << "CACHE_DEL_FAILED key=" << key << " err=" << e.what();
        },
        "DEL %s", key.c_str());
    // 5.8 L1：下单失效必须同步删本地，否则本地残留旧值（窗口期读脏）
    if (local_) local_->del(key);
}

void SkuCache::getList(GetCallback &&cb) {
    get(keys_.list(), std::move(cb));
}

void SkuCache::getDetail(int64_t skuId, GetCallback &&cb) {
    get(keys_.item(skuId), std::move(cb));
}

void SkuCache::setList(const std::string &json) {
    setex(keys_.list(), json, ttlWithJitter(cfg_.listTtlSeconds));
}

void SkuCache::setDetail(int64_t skuId, const std::string &json) {
    setex(keys_.item(skuId), json, ttlWithJitter(cfg_.detailTtlSeconds));
}

void SkuCache::setNull(int64_t skuId) {
    // 空值占位（5.6 防穿透）：DB 里没有的商品也缓存下来，TTL 用最短的一档。
    //
    // 为什么 TTL 必须短：这是安全与一致性的折中。设长了，一个刚上架的商品
    // 要等几分钟才能被看到；设短了，穿透防护就弱。60s 意味着攻击者用随机 id
    // 打库，每个 id 每分钟最多穿透一次 —— 足以把"打穿"变成"打不穿"。
    //
    // 与 5.7 布隆过滤器的分工：布隆挡"全集之外的海量随机 id"（在 Redis 之前），
    // 空值哨兵挡"布隆放行后仍 miss 的边角"（如上下架过渡期的 id），二者互补。
    setex(keys_.item(skuId), kNullValue, ttlWithJitter(cfg_.nullTtlSeconds));
}

void SkuCache::invalidate(int64_t skuId) {
    if (!cfg_.enabled || !redis_) return;
    const std::string itemKey = keys_.item(skuId);
    const std::string listKey = keys_.list();
    // 一条 DEL 删两个 key：省一次往返，且失败时两个 key 的命运一致
    // （不会出现"详情删了列表没删"这种更难受的中间态）。
    redis_->execCommandAsync(
        [](const drogon::nosql::RedisResult &) {},
        [this, itemKey](const std::exception &e) {
            err_.fetch_add(1, std::memory_order_relaxed);
            SK_LOG_ERROR << "CACHE_INVALIDATE_FAILED item=" << itemKey
                         << " err=" << e.what();
        },
        "DEL %s %s", itemKey.c_str(), listKey.c_str());
    // 5.8 L1：与 Redis 同步删本地（否则单实例下本地残留旧 stock）
    if (local_) {
        local_->del(itemKey);
        local_->del(listKey);
    }
    // 5.4 延迟双删：DEL 之后还有一段"旧值回填窗口"（读请求在 DEL 前读到旧值、
    // 在 DEL 后才回写）。延时到点后把同一组 key 再删一次，清掉窗口内回填的旧值。
    scheduleDelayedDelete({itemKey, listKey});
}

void SkuCache::invalidateItem(int64_t skuId) {
    if (!cfg_.enabled || !redis_) return;
    const std::string key = keys_.item(skuId);
    del(key);
    scheduleDelayedDelete({key});
}

void SkuCache::invalidateOnOrder(int64_t skuId) {
    switch (cfg_.invalidateOnOrder) {
        case InvalidateOnOrder::Item:
            invalidateItem(skuId);
            break;
        case InvalidateOnOrder::ItemAndList:
            invalidate(skuId);
            break;
        case InvalidateOnOrder::None:
        default:
            break;  // 什么都不做，等 TTL 自然过期
    }
}

void SkuCache::invalidateList() {
    if (!cfg_.enabled || !redis_) return;
    const std::string key = keys_.list();
    del(key);
    scheduleDelayedDelete({key});
}

SkuCache::Stats SkuCache::stats() const {
    Stats s;
    s.hit = hit_.load(std::memory_order_relaxed);
    s.localHit = localHit_.load(std::memory_order_relaxed);
    s.miss = miss_.load(std::memory_order_relaxed);
    s.err = err_.load(std::memory_order_relaxed);
    s.write = write_.load(std::memory_order_relaxed);
    s.delayedDelete = delayedDelete_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace seckill::cache
