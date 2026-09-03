#include "RegisterGuard.h"

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

// KEYS[1]=key  ARGV[1]=maxPerIp
// 返回 1=可注册  0=已达上限
// 纯读比对，不修改计数（真正的 +1 只在 markSuccess 成功注册时做）。
const char *kCheckScript =
    "local n = redis.call('GET', KEYS[1]) "
    "if not n then return 1 end "
    "if tonumber(n) < tonumber(ARGV[1]) then return 1 end "
    "return 0";

// KEYS[1]=key  ARGV[1]=windowSeconds
// INCR，首次（n==1）设置窗口 TTL，返回最新计数。
// 固定窗口语义：窗口内第一个成功注册创建计数键并定长过期，
// 之后同 IP 的成功注册只累加，过期后自动清零。
const char *kMarkScript =
    "local n = redis.call('INCR', KEYS[1]) "
    "if n == 1 then redis.call('EXPIRE', KEYS[1], ARGV[1]) end "
    "return n";

}  // namespace

RegisterGuard::RegisterGuard(drogon::nosql::RedisClientPtr redis,
                             int maxPerIp, int windowSeconds)
    : redis_(std::move(redis)), maxPerIp_(maxPerIp), windowSeconds_(windowSeconds) {}

void RegisterGuard::check(const std::string &ip, std::function<void(bool)> &&cb) {
    const std::string key = keyFor(ip);
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &r) {
            long long allowed = r.asInteger();
            cb(allowed == 1);
        },
        [cb, ip](const std::exception &e) {
            SK_LOG_ERROR << "REG_IP_CHECK_FAILED ip=" << ip << " err=" << e.what();
            cb(true);  // fail-open
        },
        "EVAL %s 1 %s %d", kCheckScript, key.c_str(), maxPerIp_);
}

void RegisterGuard::markSuccess(const std::string &ip, std::function<void()> &&cb) {
    const std::string key = keyFor(ip);
    redis_->execCommandAsync(
        [cb = std::move(cb)](const drogon::nosql::RedisResult &) { if (cb) cb(); },
        [cb = std::move(cb), ip](const std::exception &e) {
            SK_LOG_ERROR << "REG_IP_MARK_FAILED ip=" << ip << " err=" << e.what();
            if (cb) cb();
        },
        "EVAL %s 1 %s %d", kMarkScript, key.c_str(), windowSeconds_);
}

}  // namespace seckill::auth
