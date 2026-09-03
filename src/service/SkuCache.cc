#include "SkuCache.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#include "logging/LogStream.h"

namespace seckill::cache {
namespace {

// 5.4 延迟双删的专用执行线程（延时队列实现）。
//
// 为什么需要这个线程而不是在 IO 线程里 sleep：Drogon 的 handler 跑在 IO 线程上，
// 一次 sleep 会把该线程上排队的所有请求一起卡住（红线，见 docs/PLAN.md ADR-2）。
// 这里开一个**专用**后台线程，消费"到点再删"的任务队列；投递端（IO 线程）只做
// push + notify，纳秒级返回。到点后由本线程发起 Redis 的异步 DEL（execCommandAsync
// 是线程安全的，会把命令投递回 Redis 客户端所在的事件循环，不在这里阻塞等结果）。
class DelayDeleter {
public:
    using FireFn = std::function<void(std::vector<std::string>)>;

    DelayDeleter(std::chrono::milliseconds delay, FireFn fire)
        : delay_(delay), fire_(std::move(fire)), stop_(false) {
        thread_ = std::thread([this] { run(); });
    }

    ~DelayDeleter() {
        {
            std::lock_guard<std::mutex> lk(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

    void schedule(std::vector<std::string> keys) {
        if (keys.empty()) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            queue_.push_back({std::chrono::steady_clock::now() + delay_,
                              std::move(keys)});
        }
        cv_.notify_one();
    }

private:
    struct Task {
        std::chrono::steady_clock::time_point fireAt;
        std::vector<std::string> keys;
    };

    void run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            if (queue_.empty()) {
                cv_.wait(lk);
                continue;
            }
            auto &next = queue_.front();
            if (std::chrono::steady_clock::now() < next.fireAt) {
                cv_.wait_until(lk, next.fireAt);
                continue;
            }
            auto keys = std::move(next.keys);
            queue_.pop_front();
            lk.unlock();  // 执行 DEL 期间不持锁：新任务可以继续入队
            fire_(std::move(keys));
            lk.lock();
        }
    }

    std::chrono::milliseconds delay_;
    FireFn fire_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Task> queue_;
    bool stop_;
    std::thread thread_;
};

}  // namespace

SkuCache::SkuCache(drogon::nosql::RedisClientPtr redis, CacheKeys keys, Config cfg)
    : redis_(std::move(redis)), keys_(std::move(keys)), cfg_(cfg) {
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
    // 这里也顺带说明为什么不用布隆过滤器（5.7 的事）：布隆过滤器解决的是
    // "海量 id 且集合基本不变"的穿透；秒杀商品是几十到几百个、还会上下架，
    // 空值缓存的性价比更高，且没有误判。等商品量级上去再引入。
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
    s.miss = miss_.load(std::memory_order_relaxed);
    s.err = err_.load(std::memory_order_relaxed);
    s.write = write_.load(std::memory_order_relaxed);
    s.delayedDelete = delayedDelete_.load(std::memory_order_relaxed);
    return s;
}

}  // namespace seckill::cache
