#include "SmsService.h"

#include <openssl/rand.h>

#include <cstdint>
#include <cstring>
#include <ctime>

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

// KEYS[1]=codeKey  KEYS[2]=cooldownKey  KEYS[3]=dailyKey
// ARGV[1]=code  ARGV[2]=codeTtl  ARGV[3]=cooldown  ARGV[4]=dailyLimit  ARGV[5]=dailyTtl
// 返回 {码, 数值}：
//   {-1, cdTtl}  冷却未过（cdTtl = 还剩几秒）
//   {-2, used}   当日配额用尽（used = 已发条数）
//   {0,  n}      通过，这是今天的第 n 条
const char *kSendScript =
    "if redis.call('EXISTS', KEYS[2]) == 1 then "
    "  return {-1, redis.call('TTL', KEYS[2])} "
    "end "
    "local used = tonumber(redis.call('GET', KEYS[3]) or '0') "
    "if used >= tonumber(ARGV[4]) then return {-2, used} end "
    "redis.call('SETEX', KEYS[1], ARGV[2], ARGV[1]) "
    "redis.call('SETEX', KEYS[2], ARGV[3], '1') "
    "local n = redis.call('INCR', KEYS[3]) "
    "if n == 1 then redis.call('EXPIRE', KEYS[3], ARGV[5]) end "
    "return {0, n}";

// KEYS[1]=codeKey  KEYS[2]=tryKey  ARGV[1]=inputCode  ARGV[2]=maxAttempts
// 返回 {码, 数值}：
//   {0,  0}  校验通过（该码已被删除，一次性）
//   {-1, 0}  验证码不存在或已过期
//   {-2, t}  验证码错误，已试 t 次
//   {-3, 0}  试错次数超限，验证码作废
//
// 关键点：比对与删除在同一个原子步骤里完成，杜绝"一码多用"。
const char *kVerifyScript =
    "local stored = redis.call('GET', KEYS[1]) "
    "if not stored then return {-1, 0} end "
    "local tries = tonumber(redis.call('GET', KEYS[2]) or '0') "
    "if tries >= tonumber(ARGV[2]) then "
    "  redis.call('DEL', KEYS[1]) "
    "  return {-3, 0} "
    "end "
    "if stored == ARGV[1] then "
    "  redis.call('DEL', KEYS[1]) "
    "  redis.call('DEL', KEYS[2]) "
    "  return {0, 0} "
    "end "
    "local t = redis.call('INCR', KEYS[2]) "
    "redis.call('EXPIRE', KEYS[2], redis.call('TTL', KEYS[1])) "
    "return {-2, t}";

// 6 位数字验证码，拒绝采样保证均匀分布。
// 直接用 rand() % 1000000 有两个问题：std::rand() 可预测（能推出后续所有验证码）；
// 取模会让前 967296 个值之后的部分回绕，造成轻微偏斜。
// 这里用 CSPRNG 取 32 位随机数，落在 [0, 10^6 * 4294) 之外就重采样。
std::string genNumericCode() {
    const std::uint32_t kLimit = 4294000000u;  // 10^6 * 4294，是 10^6 的整数倍
    std::uint32_t v = 0;
    do {
        if (RAND_bytes(reinterpret_cast<unsigned char *>(&v), sizeof(v)) != 1) {
            return {};
        }
    } while (v >= kLimit);
    const std::uint32_t n = v % 1000000u;
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06u", n);
    return std::string(buf);
}

std::string localDayString() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y%m%d", &tm);
    return buf;
}

// 距本地次日 0 点还有多少秒。让"每日上限"的 key 恰好在跨天时过期，
// 语义才是真正的自然日，而不是"从第一次发送起滚动 24 小时"。
int secondsUntilTomorrow() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    tm.tm_hour = 0;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_mday += 1;
    tm.tm_isdst = -1;  // 让 mktime 自己判断夏令时（中国不用，但代码要通用）
    std::time_t midnight = std::mktime(&tm);
    long long diff = static_cast<long long>(midnight) - static_cast<long long>(now);
    if (diff <= 0) diff = 86400;  // 兜底：算不出来就按一整天
    return static_cast<int>(diff);
}

}  // namespace

bool SmsService::isValidPhone(const std::string &phone) {
    if (phone.size() != 11 || phone[0] != '1') return false;
    for (char c : phone) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

std::string SmsService::dailyKey(const std::string &phone, const std::string &day) {
    return "sms:day:" + phone + ":" + day;
}

SmsService::SmsService(drogon::nosql::RedisClientPtr redis,
                       std::shared_ptr<SmsSender> sender,
                       Config cfg)
    : redis_(std::move(redis)), sender_(std::move(sender)), cfg_(cfg) {}

void SmsService::sendCode(const std::string &phone,
                          std::function<void(SendResult, const std::string &)> &&cb) {
    if (!isValidPhone(phone)) {
        cb(SendResult::InvalidPhone, "invalid phone");
        return;
    }

    const std::string code = genNumericCode();
    if (code.empty()) {
        SK_LOG_ERROR << "SMS_CODE_GEN_FAILED phone=" << phone;
        cb(SendResult::RedisError, "csprng unavailable");
        return;
    }

    const std::string ck = codeKey(phone);
    const std::string cdk = cooldownKey(phone);
    const std::string dk = dailyKey(phone, localDayString());
    const int dailyTtl = secondsUntilTomorrow();

    redis_->execCommandAsync(
        // 限流与写码已经在同一 Lua 里完成，这里拿到的是"配额已扣、码已存"的结果
        [this, phone, code, cb](const drogon::nosql::RedisResult &r) {
            auto arr = r.asArray();
            if (arr.size() < 2) {
                cb(SendResult::RedisError, "unexpected lua result");
                return;
            }
            const long long code0 = arr[0].asInteger();
            const long long num = arr[1].asInteger();
            if (code0 == -1) {
                SK_LOG_WARN << "SMS_TOO_FREQUENT phone=" << phone << " retryIn=" << num << "s";
                cb(SendResult::TooFrequent, "retry in " + std::to_string(num) + "s");
                return;
            }
            if (code0 == -2) {
                SK_LOG_WARN << "SMS_DAILY_LIMIT phone=" << phone << " used=" << num;
                cb(SendResult::DailyLimit, "daily limit reached, used=" + std::to_string(num));
                return;
            }

            // 配额已扣、验证码已落 Redis，现在才真的去发短信。
            // 若发送失败：码仍在 Redis 里但用户没收到 —— 这里选择不回滚配额。
            // 取舍：回滚能省一点配额，但需要再写一个 Lua（且要处理"部分失败"），
            // 而配额本身就是防轰炸的粗粒度闸门，多扣一条无实质损失。
            sender_->sendCode(phone, code,
                [cb, num](bool ok, const std::string &detail) {
                    if (!ok) {
                        cb(SendResult::RedisError, "send failed: " + detail);
                        return;
                    }
                    cb(SendResult::Ok, detail + " (today #" + std::to_string(num) + ")");
                });
        },
        [cb, phone](const std::exception &e) {
            SK_LOG_ERROR << "SMS_LUA_FAILED phone=" << phone << " err=" << e.what();
            cb(SendResult::RedisError, e.what());
        },
        "EVAL %s 3 %s %s %s %s %d %d %d %d",
        kSendScript,
        ck.c_str(), cdk.c_str(), dk.c_str(),
        code.c_str(), cfg_.codeTtlSeconds, cfg_.resendIntervalSeconds,
        cfg_.dailyLimit, dailyTtl);
}

void SmsService::verifyCode(const std::string &phone,
                            const std::string &code,
                            std::function<void(VerifyResult)> &&cb) {
    if (!isValidPhone(phone)) {
        cb(VerifyResult::InvalidPhone);
        return;
    }
    const std::string ck = codeKey(phone);
    const std::string tk = tryKey(phone);

    redis_->execCommandAsync(
        [cb, phone](const drogon::nosql::RedisResult &r) {
            auto arr = r.asArray();
            if (arr.size() < 2) {
                cb(VerifyResult::RedisError);
                return;
            }
            const long long code0 = arr[0].asInteger();
            const long long num = arr[1].asInteger();
            if (code0 == 0) {
                cb(VerifyResult::Ok);
            } else if (code0 == -1) {
                SK_LOG_WARN << "SMS_CODE_MISSING phone=" << phone;
                cb(VerifyResult::EmptyCode);
            } else if (code0 == -2) {
                SK_LOG_WARN << "SMS_CODE_WRONG phone=" << phone << " attempt=" << num;
                cb(VerifyResult::WrongCode);
            } else {
                SK_LOG_WARN << "SMS_CODE_BRUTE phone=" << phone;
                cb(VerifyResult::TooManyAttempts);
            }
        },
        [cb, phone](const std::exception &e) {
            SK_LOG_ERROR << "SMS_VERIFY_FAILED phone=" << phone << " err=" << e.what();
            cb(VerifyResult::RedisError);
        },
        "EVAL %s 2 %s %s %s %d",
        kVerifyScript, ck.c_str(), tk.c_str(), code.c_str(), cfg_.maxVerifyAttempts);
}

}  // namespace seckill::auth
