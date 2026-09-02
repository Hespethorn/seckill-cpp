#include "LoginGuard.h"

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

// KEYS[1]=lockKey  KEYS[2]=failKey  ARGV[1]=maxFail  ARGV[2]=lockSeconds
// 返回 {码, 秒数}：
//   {-1, ttl}  已锁定（ttl = 剩余秒数）
//   {-2, lockSec} 本次失败触发锁定
//   {n, 0}     累计失败 n 次，尚未锁定
//
// 注意 failKey 的 TTL 也设成 lockSeconds：这是"滑动窗口"语义——
// 用户第 1 次输错后，只要 lockSeconds 内凑不满 maxFail 次，计数就自然清零。
// 若把 TTL 设得很短（比如 60s），攻击者可以慢慢撞；设得很长，误输几次的用户
// 会莫名其妙在几小时后被锁。与锁定窗口取齐是最省心且语义一致的选择。
const char *kOnFailureScript =
    "if redis.call('EXISTS', KEYS[1]) == 1 then"
    "  return {-1, redis.call('TTL', KEYS[1])}"
    " end "
    "local n = redis.call('INCR', KEYS[2]) "
    "if n == 1 then redis.call('EXPIRE', KEYS[2], ARGV[2]) end "
    "if n >= tonumber(ARGV[1]) then "
    "  redis.call('SETEX', KEYS[1], ARGV[2], '1') "
    "  redis.call('DEL', KEYS[2]) "
    "  return {-2, tonumber(ARGV[2])} "
    "end "
    "return {n, 0}";

// KEYS[1]=lockKey，返回剩余锁定秒数（0 = 未锁定）
const char *kCheckScript =
    "if redis.call('EXISTS', KEYS[1]) == 1 then "
    "  return redis.call('TTL', KEYS[1]) "
    "end "
    "return 0";

}  // namespace

LoginGuard::LoginGuard(drogon::nosql::RedisClientPtr redis, int maxFail, int lockSeconds)
    : redis_(std::move(redis)), maxFail_(maxFail), lockSeconds_(lockSeconds) {}

void LoginGuard::checkLocked(const std::string &phone,
                             std::function<void(bool, int)> &&cb) {
    const std::string lk = lockKey(phone);
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &r) {
            long long ttl = r.asInteger();
            cb(ttl > 0, static_cast<int>(ttl));
        },
        [cb, phone](const std::exception &e) {
            // Redis 不可用时的取舍：这里选择 fail-open（放行）。
            // 理由——锁定检查是"可用性"维度的保护，Redis 挂掉时把所有登录都拒掉
            // 会造成比撞库更大的业务损失；而撞库本身还有 failKey 计数在兜。
            // 注意这与 SessionStore::exists 的 fail-close 相反：
            // 鉴权要收紧（宁可误杀），限流要放松（宁可放过），两者不能混为一谈。
            SK_LOG_ERROR << "LOCK_CHECK_FAILED phone=" << phone << " err=" << e.what();
            cb(false, 0);
        },
        "EVAL %s 1 %s", kCheckScript, lk.c_str());
}

void LoginGuard::onFailure(const std::string &phone,
                           std::function<void(const FailResult &)> &&cb) {
    const std::string lk = lockKey(phone);
    const std::string fk = failKey(phone);
    redis_->execCommandAsync(
        [cb, phone](const drogon::nosql::RedisResult &r) {
            FailResult res;
            auto arr = r.asArray();
            if (arr.size() < 2) {
                // 脚本返回结构异常，按"继续计数"处理，不放大也不缩小限制
                res.kind = FailResult::Kind::Counting;
                res.failCount = 1;
                cb(res);
                return;
            }
            long long code = arr[0].asInteger();
            long long secs = arr[1].asInteger();
            if (code == -1) {
                res.kind = FailResult::Kind::AlreadyLocked;
                res.lockSeconds = static_cast<int>(secs);
            } else if (code == -2) {
                res.kind = FailResult::Kind::LockedNow;
                res.lockSeconds = static_cast<int>(secs);
                SK_LOG_WARN << "LOGIN_LOCKED phone=" << phone << " seconds=" << secs;
            } else {
                res.kind = FailResult::Kind::Counting;
                res.failCount = static_cast<int>(code);
            }
            cb(res);
        },
        [cb, phone](const std::exception &e) {
            SK_LOG_ERROR << "FAIL_COUNT_FAILED phone=" << phone << " err=" << e.what();
            // Redis 挂了也不能让登录流程断在这里：退化成"不计数"，
            // 请求继续走登录失败分支（密码校验已经失败了，本来就通不过）。
            FailResult res;
            res.kind = FailResult::Kind::Counting;
            res.failCount = 1;
            cb(res);
        },
        "EVAL %s 2 %s %s %d %d", kOnFailureScript,
        lk.c_str(), fk.c_str(), maxFail_, lockSeconds_);
}

void LoginGuard::onSuccess(const std::string &phone, std::function<void()> &&cb) {
    const std::string fk = failKey(phone);
    redis_->execCommandAsync(
        [cb](const drogon::nosql::RedisResult &) { cb(); },
        [cb, phone](const std::exception &e) {
            SK_LOG_ERROR << "FAIL_RESET_FAILED phone=" << phone << " err=" << e.what();
            cb();
        },
        "DEL %s", fk.c_str());
}

}  // namespace seckill::auth
