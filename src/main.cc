#include <drogon/drogon.h>
#include <json/json.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

#include "controllers/HealthController.h"
#include "controllers/SeckillController.h"
#include "controllers/SmsController.h"
#include "controllers/UserController.h"
#include "logging/AsyncLogger.h"
#include "service/InflightGuard.h"
#include "service/Jwt.h"
#include "service/LoginGuard.h"
#include "service/RegisterGuard.h"
#include "service/SeckillService.h"
#include "service/SessionStore.h"
#include "service/SkuCache.h"
#include "service/SmsSender.h"
#include "service/SmsService.h"
#include "service/UserService.h"

using seckill::InflightGuard;
using namespace seckill::auth;  // NOLINT(build/namespaces) —— 仅本文件，为缩短初始化代码

namespace {

// Drogon 1.9.10 关键坑：getDbClient() / getRedisClient() 只能在 app.run() 之后调用！
// 官方文档原话: "This method cannot be called before running app.run(),
// otherwise the user will get an empty shared_ptr."
//
// 原因：config.json 里的 db_clients / redis_clients 在 loadConfigFile 阶段只是"注册了配置"，
// 真正的客户端对象（含连接池）是 run() 时才创建的。main() 里提前取必然拿到空 shared_ptr。
//
// 解法：把"取客户端 + 组装服务"延迟到第一个请求到达时（handler 执行必然在 run() 之后），
// 用 static 局部变量保证只初始化一次、线程安全（C++11 magic static）。

struct AppBundle {
    std::shared_ptr<InflightGuard> lock;
    std::shared_ptr<seckill::cache::SkuCache> skuCache;  // Redis 不可用时为 nullptr
    std::shared_ptr<SeckillController> seckill;
    std::shared_ptr<UserController> user;  // Redis 不可用时为 nullptr
    std::shared_ptr<SmsController> sms;    // 同上
    bool authReady = false;
};

std::string cfgStr(const Json::Value &root, const char *key, const std::string &def) {
    return (root.isMember(key) && root[key].isString()) ? root[key].asString() : def;
}
int cfgInt(const Json::Value &root, const char *key, int def) {
    return (root.isMember(key) && root[key].isNumeric()) ? root[key].asInt() : def;
}
bool cfgBool(const Json::Value &root, const char *key, bool def) {
    return (root.isMember(key) && root[key].isBool()) ? root[key].asBool() : def;
}

InflightGuard::Mode parseLockMode(const std::string &s) {
    if (s == "mutex") return InflightGuard::Mode::Mutex;
    if (s == "spin") return InflightGuard::Mode::Spin;
    if (s == "atomic") return InflightGuard::Mode::Atomic;
    return InflightGuard::Mode::None;
}

const char *lockModeName(InflightGuard::Mode m) {
    switch (m) {
        case InflightGuard::Mode::Mutex: return "mutex";
        case InflightGuard::Mode::Spin: return "spin";
        case InflightGuard::Mode::Atomic: return "atomic";
        default: return "none";
    }
}

// 下单后删哪些缓存 key。默认 "item"：只作废被买到的那个 sku 的详情，
// 列表靠 TTL 自愈 —— 列表是全量聚合 key，任何一个 sku 成交都删它，
// 在写 QPS 高时命中率会趋零（详见 SkuCache::InvalidateOnOrder）。
seckill::cache::SkuCache::InvalidateOnOrder parseInvalidateOnOrder(
    const std::string &s) {
    using M = seckill::cache::SkuCache::InvalidateOnOrder;
    if (s == "none") return M::None;
    if (s == "all") return M::ItemAndList;
    return M::Item;
}

const char *invalidateName(seckill::cache::SkuCache::InvalidateOnOrder m) {
    using M = seckill::cache::SkuCache::InvalidateOnOrder;
    switch (m) {
        case M::None: return "none";
        case M::ItemAndList: return "all";
        default: return "item";
    }
}

AppBundle buildBundle() {
    AppBundle b;
    const Json::Value cc = drogon::app().getCustomConfig();

    // ── 秒杀：应用层在途闸门（4.8）──────────────────────────────────────
    {
        const Json::Value &lk = cc["seckill_lock"];
        const auto mode = parseLockMode(cfgStr(lk, "mode", "none"));
        b.lock = std::make_shared<InflightGuard>(
            mode,
            static_cast<std::size_t>(cfgInt(lk, "shards", 64)),
            static_cast<std::size_t>(cfgInt(lk, "bits", 65536)));
        LOG_INFO << "seckill in-flight guard: mode=" << lockModeName(mode);
    }

    auto db = drogon::app().getDbClient("default");
    if (!db) {
        LOG_FATAL << "getDbClient(\"default\") returned null after run(): "
                     "check db_clients in config.json and Drogon MySQL support.";
        exit(1);
    }

    // ── 缓存层（5.1-5.3）与登录 / 短信都依赖 Redis ───────────────────────
    // Redis 不可用时**只关掉缓存与登录 / 短信**，秒杀主链路照常可用（直连 DB）。
    // 这样基线压测不会因为中间件没装就整站 500，也便于在没装 Redis 的机器上先跑通秒杀。
    auto redis = drogon::app().getRedisClient("default");
    if (!redis) {
        LOG_WARN << "Redis client unavailable -> cache disabled, /api/user/* and "
                    "/api/sms/* disabled; seckill endpoints still work (DB-direct). "
                    "Start redis-server and restart to enable them.";
    }

    {
        const Json::Value &c = cc["cache"];
        seckill::cache::SkuCache::Config cacheCfg;
        cacheCfg.enabled = cfgBool(c, "enabled", true);
        cacheCfg.listTtlSeconds = cfgInt(c, "list_ttl_seconds", 30);
        cacheCfg.detailTtlSeconds = cfgInt(c, "detail_ttl_seconds", 60);
        cacheCfg.nullTtlSeconds = cfgInt(c, "null_ttl_seconds", 60);
        cacheCfg.jitterSeconds = cfgInt(c, "jitter_seconds", 30);
        cacheCfg.invalidateOnOrder =
            parseInvalidateOnOrder(cfgStr(c, "invalidate_on_order", "item"));

        seckill::cache::CacheKeys keys(cfgStr(c, "key_prefix", "seckill"),
                                       cfgStr(c, "key_version", "v1"));
        // Redis 不可用时 enabled 强制关掉：否则每个读请求都会去打一个空客户端。
        if (!redis) cacheCfg.enabled = false;
        b.skuCache =
            std::make_shared<seckill::cache::SkuCache>(redis, keys, cacheCfg);
        LOG_INFO << "sku cache: enabled=" << (cacheCfg.enabled ? 1 : 0)
                 << " listTTL=" << cacheCfg.listTtlSeconds
                 << " detailTTL=" << cacheCfg.detailTtlSeconds
                 << " jitter=" << cacheCfg.jitterSeconds
                 << " invalidateOnOrder=" << invalidateName(cacheCfg.invalidateOnOrder)
                 << " keyPrefix=" << keys.prefix() << ":" << keys.version();
    }

    b.seckill = std::make_shared<SeckillController>(
        std::make_shared<SeckillService>(db, b.lock, b.skuCache));

    if (!redis) return b;

    const Json::Value &jwtCfg = cc["jwt"];
    auto jwt = std::make_shared<Jwt>(cfgStr(jwtCfg, "secret", "seckill-cpp-dev-secret"),
                                     cfgStr(jwtCfg, "issuer", "seckill-cpp"),
                                     cfgInt(jwtCfg, "ttl_seconds", 7200));
    auto sessions = std::make_shared<SessionStore>(redis, jwt->ttlSeconds());

    const Json::Value &guardCfg = cc["login_guard"];
    auto guard = std::make_shared<LoginGuard>(redis,
                                              cfgInt(guardCfg, "max_fail", 5),
                                              cfgInt(guardCfg, "lock_seconds", 600));

    const Json::Value &regLimitCfg = cc["register_limit"];
    auto registerGuard = std::make_shared<RegisterGuard>(redis,
                                                        cfgInt(regLimitCfg, "max_per_ip", 5),
                                                        cfgInt(regLimitCfg, "window_seconds", 3600));

    const Json::Value &smsCfg = cc["sms"];
    // 验证码送达：自签发 / 日志可读模式，不接任何短信网关（详见 SmsSender.h）。
    auto sender = std::make_shared<SmsSender>();

    SmsService::Config smsSvcCfg;
    smsSvcCfg.codeTtlSeconds = cfgInt(smsCfg, "code_ttl_seconds", 300);
    smsSvcCfg.resendIntervalSeconds = cfgInt(smsCfg, "resend_interval_seconds", 60);
    smsSvcCfg.dailyLimit = cfgInt(smsCfg, "daily_limit", 10);
    smsSvcCfg.maxVerifyAttempts = cfgInt(smsCfg, "max_verify_attempts", 5);
    auto smsService = std::make_shared<SmsService>(redis, sender, smsSvcCfg);

    const Json::Value &authCfg = cc["auth"];
    UserService::Config userCfg;
    userCfg.requireSmsOnRegister = cfgBool(authCfg, "require_sms_on_register", true);
    userCfg.requireSmsOnLogin = cfgBool(authCfg, "require_sms_on_login", false);
    userCfg.minPasswordLength = cfgInt(authCfg, "min_password_length", 6);

    auto userService = std::make_shared<UserService>(db, sessions, jwt, guard,
                                                     registerGuard, smsService, userCfg);
    b.user = std::make_shared<UserController>(userService);
    b.sms = std::make_shared<SmsController>(smsService);
    b.authReady = true;
    return b;
}

// 刻意不叫 app()：Drogon 的全局入口也叫 drogon::app()，
// 同名会让 "app() 到底是谁" 变成一个要靠上下文猜的问题。
const AppBundle &bundle() {
    static const AppBundle b = buildBundle();
    return b;
}

drogon::HttpResponsePtr unavailable(const char *what) {
    Json::Value root;
    root["code"] = 503;
    root["msg"] = std::string(what) + " unavailable (redis not connected)";
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(drogon::k503ServiceUnavailable);
    return resp;
}

}  // namespace

int main() {
    // 读取 config.json：其中声明了监听端口、线程数、MySQL 与 Redis 客户端。
    drogon::app().loadConfigFile("./config.json");

    // 异步日志：业务线程只 push 环形缓冲，落盘在后台线程（见 logging/AsyncLogger.cc）。
    {
        const auto &cc = drogon::app().getCustomConfig();
        std::string logFile = "./logs/seckill.log";
        std::string logLevel = "warn";
        if (cc.isMember("async_log")) {
            const auto &al = cc["async_log"];
            if (al.isMember("file")) logFile = al["file"].asString();
            if (al.isMember("level")) logLevel = al["level"].asString();
        }
        seckill::log::AsyncLogger::instance().start(
            logFile, spdlog::level::from_str(logLevel));
        LOG_INFO << "async logger started: file=" << logFile << " level=" << logLevel;
    }

    // ── 路由挂载 ────────────────────────────────────────────────────────
    //  - HealthController 是无依赖的 HttpController，由 Drogon 自动注册（/api/health），
    //    无需（也不能）手动 registerController——v1.9.10 对 HttpController 子类调
    //    registerController 会触发静态断言。
    //  - 其余控制器需要注入依赖（无默认构造器，无法自动注册），
    //    因此用 registerHandler 手动挂路由，lambda 把请求转交给 app() 里的实例。
    //    registerHandler 会持有该 lambda，故按值捕获实例以延长其生命周期。
    //    注意：handler 必须是两参形式 (req, callback)。Drogon v1.9.10 的 HttpBinder 对
    //    三参形式 (req, callback, path) 且路由无 {} 占位符时，会按 req->as<std::string>()
    //    兜底绑定第 3 个参数，而 fromRequest<std::string> 未特化 -> 运行时 exit(1)。
    //    无占位符的路由一律用两参形式，需要 path 时再走带 {} 的路由 + 三参（path 来自占位符）。

    drogon::app().registerHandler(
        "/api/seckill",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            // run() 之后才能安全取 db client（见 buildBundle 注释）
            bundle().seckill->seckill(req, std::move(callback));
        },
        {drogon::Post});

    // 4.1 秒杀商品列表（只读，GET）
    drogon::app().registerHandler(
        "/api/seckill/list",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            bundle().seckill->listSkus(req, std::move(callback));
        },
        {drogon::Get});

    // 4.2 秒杀商品详情（按 id，GET，路径参数走三参 handler）
    drogon::app().registerHandler(
        "/api/seckill/{skuId}",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback,
           const std::string &skuIdStr) {
            bundle().seckill->detailSku(req, std::move(callback), skuIdStr);
        },
        {drogon::Get});

    // 在途闸门统计：压测后直接看应用层挡掉了多少请求（4.8 的量化收益）
    drogon::app().registerHandler(
        "/api/lock/stats",
        [](const drogon::HttpRequestPtr &,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto inflight = bundle().lock;
            Json::Value root;
            root["code"] = 0;
            root["data"]["mode"] = lockModeName(inflight->mode());
            root["data"]["acquired"] = Json::UInt64(inflight->acquired());
            root["data"]["rejected"] = Json::UInt64(inflight->rejected());
            callback(drogon::HttpResponse::newHttpJsonResponse(root));
        },
        {drogon::Get});

    // 缓存命中率观测（5.1 的硬要求）：没有它，"缓存到底生效了没有"只能靠感觉。
    // 压测时打两次（前后各一次）取差值，就是这一轮的真实命中率。
    // 顺带把实际 key 与 TTL 回显出来，方便直接拿去 redis-cli 里 GET / TTL 核对。
    drogon::app().registerHandler(
        "/api/cache/stats",
        [](const drogon::HttpRequestPtr &,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto cache = bundle().skuCache;
            Json::Value root;
            root["code"] = 0;
            Json::Value &d = root["data"];
            if (!cache || !cache->enabled()) {
                d["enabled"] = false;
                d["reason"] = "cache disabled or redis unavailable";
            } else {
                const auto s = cache->stats();
                const uint64_t total = s.hit + s.miss;
                d["enabled"] = true;
                d["hit"] = Json::UInt64(s.hit);
                d["miss"] = Json::UInt64(s.miss);
                d["err"] = Json::UInt64(s.err);
                d["write"] = Json::UInt64(s.write);
                d["hit_rate"] = total > 0 ? static_cast<double>(s.hit) / static_cast<double>(total) : 0.0;
                d["keys"]["list"] = cache->keys().list();
                d["keys"]["item_sample"] = cache->keys().item(1);
                d["ttl"]["list"] = cache->config().listTtlSeconds;
                d["ttl"]["detail"] = cache->config().detailTtlSeconds;
                d["ttl"]["null"] = cache->config().nullTtlSeconds;
                d["ttl"]["jitter"] = cache->config().jitterSeconds;
                d["invalidate_on_order"] = invalidateName(cache->config().invalidateOnOrder);
            }
            callback(drogon::HttpResponse::newHttpJsonResponse(root));
        },
        {drogon::Get});

    drogon::app().registerHandler(
        "/api/user/register",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto &b = bundle();
            if (!b.authReady) {
                callback(unavailable("register"));
                return;
            }
            b.user->registerUser(req, std::move(callback));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/user/login",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto &b = bundle();
            if (!b.authReady) {
                callback(unavailable("login"));
                return;
            }
            b.user->login(req, std::move(callback));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/user/logout",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto &b = bundle();
            if (!b.authReady) {
                callback(unavailable("logout"));
                return;
            }
            b.user->logout(req, std::move(callback));
        },
        {drogon::Post});

    drogon::app().registerHandler(
        "/api/sms/send",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            auto &b = bundle();
            if (!b.authReady) {
                callback(unavailable("sms"));
                return;
            }
            b.sms->sendCode(req, std::move(callback));
        },
        {drogon::Post});

    LOG_INFO << "seckill-cpp starting on :8080 (stage-1: DB-direct + auth module)";
    drogon::app().run();

    return 0;
}
