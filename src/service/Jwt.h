// 自实现 JWT（HS256），不引 jwt-cpp
//
// 为什么不引 jwt-cpp：
//   - HS256 的全部内涵就是「base64url(header).base64url(payload).base64url(HMAC-SHA256(secret, 前两段))」。
//     三个动作：base64url 编解码、HMAC、JSON 拼装，各十几行，全用已经在场的 OpenSSL + jsoncpp。
//   - header-only 库要 FetchContent 从 GitHub 拉，WSL 网络是额外风险点；
//     自己写则编译期零外部依赖、代码完全可审计。
//   - 与本项目"自实现核心组件"的主线一致（环形缓冲、Lua 限流、TC3 签名都是同一套路）。
//
// 安全红线（代码里逐条落实）：
//   1. 校验时强制 alg == "HS256"：否则攻击者把 alg 改成 "none" 就能伪造任意 token
//      （经典 JWT 漏洞），或引诱服务端用公钥当 HMAC 密钥验证 RS256（算法混淆）。
//   2. 签名比对走 constantTimeEquals，不用 ==。
//   3. 过期判定只看 exp，且服务端时钟以本机为准。
#pragma once

#include <cstdint>
#include <string>

namespace seckill::auth {

struct JwtClaims {
    int64_t userId = 0;      // sub
    std::string jti;         // JWT ID，同时是 Redis 会话 key 的后半段
    std::string issuer;      // iss
    int64_t issuedAt = 0;    // iat（unix 秒）
    int64_t expiresAt = 0;   // exp（unix 秒）
};

class Jwt {
public:
    Jwt(std::string secret, std::string issuer, int ttlSeconds);

    // 签发一个 HS256 token。nowSeconds 传 0 表示取系统当前时间（便于测试注入固定时间）。
    std::string sign(int64_t userId, const std::string &jti, int64_t nowSeconds = 0) const;

    // 校验：结构 → alg → 签名（常量时间）→ iss → exp，任一不过返回 false。
    // 返回 true 时 out 被填充；返回 false 时 out 内容未定义，调用方不应使用。
    bool verify(const std::string &token, JwtClaims &out, int64_t nowSeconds = 0) const;

    int ttlSeconds() const { return ttlSeconds_; }
    const std::string &issuer() const { return issuer_; }

private:
    std::string secret_;
    std::string issuer_;
    int ttlSeconds_;
};

// 生成 32 字符的 jti（16 字节随机 → hex），用作 token 唯一 ID。
// 复用 genSalt 的 CSPRNG 实现，语义上它就是"一个密码学安全的随机串"。
std::string genJti();

// base64url（无 padding）编解码，暴露出来只为单测
std::string base64UrlEncode(const std::string &raw);
bool base64UrlDecode(const std::string &in, std::string &out);

}  // namespace seckill::auth
