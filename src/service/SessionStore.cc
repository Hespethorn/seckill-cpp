#include "SessionStore.h"

#include "logging/LogStream.h"

namespace seckill::auth {

SessionStore::SessionStore(drogon::nosql::RedisClientPtr redis, int ttlSeconds)
    : redis_(std::move(redis)), ttlSeconds_(ttlSeconds) {}

void SessionStore::save(const std::string &jti,
                        int64_t userId,
                        std::function<void(bool)> &&cb) {
    const std::string k = key(jti);
    const std::string v = std::to_string(userId);
    // SETEX key ttl value：单命令原子，不需要 Lua。TTL 与 JWT 的 exp 对齐，
    // token 过期那一刻 Redis key 也自然消失，不留垃圾。
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &) { cb(true); },
        [cb, jti](const std::exception &e) {
            SK_LOG_ERROR << "SESS_SAVE_FAILED jti=" << jti << " err=" << e.what();
            cb(false);
        },
        "SETEX %s %d %s", k.c_str(), ttlSeconds_, v.c_str());
}

void SessionStore::exists(const std::string &jti, std::function<void(bool)> &&cb) {
    const std::string k = key(jti);
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &r) {
            // EXISTS 返回 0/1；Redis 不可用时我们走的是下面的异常回调 → false。
            // 这里"失败即拒绝"（fail-close）：宁可让已登录用户重新登录，
            // 也不能在 Redis 抖动时把被封禁的会话放过去。
            cb(r.asInteger() > 0);
        },
        [cb, jti](const std::exception &e) {
            SK_LOG_ERROR << "SESS_EXISTS_FAILED jti=" << jti << " err=" << e.what();
            cb(false);
        },
        "EXISTS %s", k.c_str());
}

void SessionStore::revoke(const std::string &jti, std::function<void(bool)> &&cb) {
    const std::string k = key(jti);
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &r) {
            // DEL 返回被删的 key 数：0 表示这个会话本来就不存在（已过期/已退出），
            // 退出登录接口据此区分"退掉了"和"本来就没登录"。
            cb(r.asInteger() > 0);
        },
        [cb, jti](const std::exception &e) {
            SK_LOG_ERROR << "SESS_REVOKE_FAILED jti=" << jti << " err=" << e.what();
            cb(false);
        },
        "DEL %s", k.c_str());
}

}  // namespace seckill::auth
