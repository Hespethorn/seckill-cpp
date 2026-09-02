// 短信验证码服务（3.3-3.5）：生成 / 发送 / 存储 / 校验 / 限流
//
// 三条安全红线，每一条都对应一个具体的攻击面：
//   1. 每日发送上限（dailyLimit）—— 防"短信轰炸"：攻击者拿你的接口当免费短信机，
//      打别人的手机号，不仅败坏业务口碑，账单也是你的。
//   2. 重发冷却（resendInterval）—— 防"短信轰炸"的另一半：
//      没有冷却，攻击者可以 1 秒发 60 条，把上限瞬间打满，真用户反而收不到码。
//   3. 校验次数上限（maxVerifyAttempts）—— 防"验证码爆破"：
//      6 位数字只有 100 万种组合，不限次数的话脚本几十分钟就能撞出来。
//      超限即作废该码，逼攻击者重新走发送流程（而发送流程有上限卡着）。
//
// 为什么校验必须是一个 Lua 脚本而不是 GET → 比对 → DEL：
//   三步分开做，两个并发请求可能同时 GET 到同一个正确验证码，
//   都比对通过、都去 DEL —— 一个验证码被兑换了两次。
//   注册场景下这意味着同一个手机号能绕过验证码重复注册（虽然 uk_phone 最终会兜住，
//   但验证码这层已经形同虚设）。Lua 在 Redis 单线程里执行，"读-比对-删"不可分割。
#pragma once

#include <drogon/nosql/RedisClient.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "SmsSender.h"

namespace seckill::auth {

class SmsService {
public:
    struct Config {
        int codeTtlSeconds = 300;        // 验证码有效期 5 分钟
        int resendIntervalSeconds = 60;  // 同一号码重发间隔
        int dailyLimit = 10;             // 每号码每日发送上限
        int maxVerifyAttempts = 5;       // 单个验证码最多试几次
    };

    enum class SendResult {
        Ok,
        InvalidPhone,
        TooFrequent,    // 冷却未过
        DailyLimit,     // 当日配额用尽
        RedisError,
    };

    enum class VerifyResult {
        Ok,
        InvalidPhone,
        EmptyCode,      // 验证码不存在或已过期
        WrongCode,      // 不对，还可以再试
        TooManyAttempts,// 试错次数超限，该验证码已作废
        RedisError,
    };

    SmsService(drogon::nosql::RedisClientPtr redis,
               std::shared_ptr<SmsSender> sender,
               Config cfg);

    // 生成验证码 → 过限流 → 存 Redis → 调发送器。
    // cb(result, detail)：detail 给日志用（含冷却剩余秒数 / 当日第几条 / 发送失败原因）。
    void sendCode(const std::string &phone,
                  std::function<void(SendResult, const std::string &)> &&cb);

    // 校验并消费验证码（成功即作废，一次性）。
    void verifyCode(const std::string &phone,
                    const std::string &code,
                    std::function<void(VerifyResult)> &&cb);

    const Config &config() const { return cfg_; }

    static bool isValidPhone(const std::string &phone);

    static std::string codeKey(const std::string &phone) { return "sms:code:" + phone; }
    static std::string cooldownKey(const std::string &phone) { return "sms:cd:" + phone; }
    static std::string tryKey(const std::string &phone) { return "sms:try:" + phone; }
    static std::string dailyKey(const std::string &phone, const std::string &day);

private:
    drogon::nosql::RedisClientPtr redis_;
    std::shared_ptr<SmsSender> sender_;
    Config cfg_;
};

}  // namespace seckill::auth
