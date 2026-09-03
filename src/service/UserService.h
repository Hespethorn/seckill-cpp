// 用户服务（3.1 注册 / 3.2 登录 / 3.7 登出）
//
// 一个容易被忽略的性能坑：PBKDF2 就是设计为慢的（12 万次迭代 ≈ 30~60ms CPU）。
//   它就是一段同步 CPU 密集计算。如果直接在 Drogon 的 IO 线程里跑，
//   这个线程上排队的秒杀请求会被一起卡住几十毫秒——
//   登录接口的慢，会传染给完全无关的秒杀接口。
//   所以哈希计算一律丢进独立工作线程，算完用 queueInLoop 切回 IO 线程。
//   这也是为什么 PBKDF2（密码哈希）要丢进独立工作线程（见 UserService.cc）：IO 线程上只准有非阻塞操作。
//   注意：短信验证码（SmsService）是自签发 + 同步写日志，全程在 IO 线程跑，并不需要独立线程；
//   它属于注册链路的安全闸门（防批量注册/爆破），与"物理隔离"无关。
#pragma once

#include <drogon/nosql/RedisClient.h>
#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "Jwt.h"
#include "LoginGuard.h"
#include "RegisterGuard.h"
#include "SessionStore.h"
#include "SmsService.h"

namespace seckill::auth {

class UserService {
public:
    struct Config {
        bool requireSmsOnRegister = true;   // 注册要验证码：防脚本批量注册
        bool requireSmsOnLogin = false;     // 登录默认不要：有错误次数锁定兜底（3.6）
        int minPasswordLength = 6;
    };

    // 业务拒绝码（HTTP 409）：PHONE_REGISTERED / USER_NOT_FOUND / WRONG_PASSWORD /
    //   ACCOUNT_LOCKED / ACCOUNT_DISABLED / CODE_* / WEAK_PASSWORD / INVALID_PHONE
    //   频控拒绝（HTTP 429）：REGISTER_IP_LIMITED
    // 系统错误（HTTP 500）：DB_ERROR:xxx / REDIS_ERROR:xxx / HASH_FAILED

    UserService(drogon::orm::DbClientPtr db,
                std::shared_ptr<SessionStore> sessions,
                std::shared_ptr<Jwt> jwt,
                std::shared_ptr<LoginGuard> guard,
                std::shared_ptr<RegisterGuard> registerGuard,
                std::shared_ptr<SmsService> sms,
                Config cfg);

    // 注册（3.1 + 3.4 验证码校验）。code 在 requireSmsOnRegister=false 时可留空。
    // ip 为客户端地址（用于同 IP 注册频控，见 RegisterGuard）。
    void registerUser(const std::string &phone,
                      const std::string &password,
                      const std::string &code,
                      const std::string &ip,
                      std::function<void(bool, const std::string &)> &&cb);

    // 登录（3.2 + 3.6 错误次数限制）。成功时 token 为签发的 JWT。
    void login(const std::string &phone,
               const std::string &password,
               std::function<void(bool, const std::string &, const std::string &)> &&cb);

    // 登出（3.7）：校验 token → 删 Redis 会话。
    void logout(const std::string &token,
                std::function<void(bool, const std::string &)> &&cb);

    // 鉴权中间件用：验签 + 查会话是否存在。返回 userId（失败为 0）。
    void authenticate(const std::string &token, std::function<void(int64_t)> &&cb);

private:
    drogon::orm::DbClientPtr db_;
    std::shared_ptr<SessionStore> sessions_;
    std::shared_ptr<Jwt> jwt_;
    std::shared_ptr<LoginGuard> guard_;
    std::shared_ptr<RegisterGuard> registerGuard_;
    std::shared_ptr<SmsService> sms_;
    Config cfg_;
};

}  // namespace seckill::auth
