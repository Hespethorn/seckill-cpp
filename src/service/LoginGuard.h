// 登录安全加固（3.6）：密码错误次数限制 + 账号临时锁定
//
// 为什么计数必须放在 Redis 而不是进程内存：
//   进程内存里的计数只对单个实例有效。攻击者只要把撞库请求打散到多个实例，
//   每台实例各记各的，"5 次就锁"就变成了"5 × N 次才锁"。
//   Redis 是共享的单一计数源，多实例部署时语义不变。
//
// 为什么"INCR + 判阈值 + SETEX 锁"必须用一个 Lua 脚本包起来：
//   三条命令分开做，并发下会有两个竞态：
//     a) 计数竞态：10 个请求同时读到 4、各自 +1 写回 5，实际失败了 10 次却只记了 5 次；
//     b) 锁定竞态：两个请求同时跨过阈值，各自都去 SETEX，虽然是幂等操作，
//        但会各自返回"本次触发锁定"，业务侧可能重复告警。
//   Lua 在 Redis 里单线程原子执行，把"读-改-写"变成不可分割的一步。
#pragma once

#include <drogon/nosql/RedisClient.h>

#include <cstdint>
#include <functional>
#include <string>

namespace seckill::auth {

// onFailure 的结果语义
struct FailResult {
    enum class Kind {
        Counting,       // 还在累加中，未锁定
        LockedNow,      // 本次失败触发了锁定
        AlreadyLocked,  // 此前已被锁定（理论上不该走到 onFailure，checkLocked 应先拦住）
    };
    Kind kind = Kind::Counting;
    int failCount = 0;     // Counting 时的累计失败次数
    int lockSeconds = 0;   // LockedNow / AlreadyLocked 时的剩余锁定秒数
};

class LoginGuard {
public:
    explicit LoginGuard(drogon::nosql::RedisClientPtr redis, int maxFail, int lockSeconds);

    // 登录前检查是否被锁定。cb(locked, remainSeconds)：locked=false 时 remainSeconds 无意义。
    void checkLocked(const std::string &phone,
                     std::function<void(bool, int)> &&cb);

    // 密码校验失败时调用：计数 +1，达到 maxFail 即锁定 lockSeconds 秒。
    void onFailure(const std::string &phone,
                   std::function<void(const FailResult &)> &&cb);

    // 登录成功：清空失败计数（不清锁定标记——被锁期间不可能登录成功）。
    void onSuccess(const std::string &phone, std::function<void()> &&cb);

    int maxFail() const { return maxFail_; }
    int lockSeconds() const { return lockSeconds_; }

    static std::string failKey(const std::string &phone) { return "login:fail:" + phone; }
    static std::string lockKey(const std::string &phone) { return "login:lock:" + phone; }

private:
    drogon::nosql::RedisClientPtr redis_;
    int maxFail_;
    int lockSeconds_;
};

}  // namespace seckill::auth
