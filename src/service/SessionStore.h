// Redis 会话存储：JWT 的"可吊销层"
//
// 为什么 JWT 之外还要一层 Redis：
//   JWT 本身无状态，签发后服务端没有"让它失效"的手段——用户改密码、被封号，
//   旧 token 在 exp 之前照样能通过签名校验。秒杀这种有黄牛、有风控的场景，
//   "踢不掉人"是硬伤。
//
// 权威态放在 Redis：登录成功写 sess:{jti} -> userId，TTL 与 JWT 的 exp 对齐。
//   校验时 EXISTS sess:{jti} 失败即拒绝；吊销 = DEL sess:{jti}，一行命令。
//
// 为什么用 Drogon 内置的 nosql::RedisClient 而不是 redis-plus-plus：
//   redis-plus-plus 是同步 API。Drogon 的 IO 线程数是 config 里的 threads_num（本项目 4），
//   在 IO 线程里同步等一次 Redis 往返（本地 0.2~0.5ms，跨机几 ms）就会把这 1/4 的请求
//   全部卡住——这是比"多一个依赖"严重得多的结构性问题。
//   Drogon 内置客户端是异步的（底层 hiredis + 事件循环），与框架同构，且零额外依赖。
#pragma once

#include <drogon/nosql/RedisClient.h>

#include <cstdint>
#include <functional>
#include <string>

namespace seckill::auth {

class SessionStore {
public:
    explicit SessionStore(drogon::nosql::RedisClientPtr redis, int ttlSeconds);

    // 登录成功：SETEX sess:{jti} ttl userId
    void save(const std::string &jti, int64_t userId, std::function<void(bool)> &&cb);

    // 鉴权：EXISTS sess:{jti}（吊销后返回 false）
    void exists(const std::string &jti, std::function<void(bool)> &&cb);

    // 吊销 / 退出登录：DEL sess:{jti}
    void revoke(const std::string &jti, std::function<void(bool)> &&cb);

    int ttlSeconds() const { return ttlSeconds_; }

    static std::string key(const std::string &jti) { return "sess:" + jti; }

private:
    drogon::nosql::RedisClientPtr redis_;
    int ttlSeconds_;
};

}  // namespace seckill::auth
