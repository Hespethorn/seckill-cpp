#include "Jwt.h"

#include <json/json.h>
#include <openssl/hmac.h>

#include <chrono>
#include <memory>

#include "password.h"

namespace seckill::auth {
namespace {

const char *kB64UrlAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string hmacSha256(const std::string &key, const std::string &data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(),
         md, &len);
    return std::string(reinterpret_cast<char *>(md), len);
}

int64_t nowUnixSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string jsonToString(const Json::Value &v) {
    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";  // 紧凑输出：token 里塞空格只会白白变长
    return Json::writeString(wb, v);
}

bool stringToJson(const std::string &s, Json::Value &out) {
    Json::CharReaderBuilder rb;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    std::string errs;
    return reader->parse(s.data(), s.data() + s.size(), &out, &errs);
}

std::string hmacToBase64Url(const std::string &key, const std::string &signingInput) {
    return base64UrlEncode(hmacSha256(key, signingInput));
}

}  // namespace

std::string base64UrlEncode(const std::string &raw) {
    std::string out;
    out.reserve(((raw.size() + 2) / 3) * 4);
    std::size_t i = 0;
    for (; i + 2 < raw.size(); i += 3) {
        unsigned int chunk = (static_cast<unsigned char>(raw[i]) << 16) |
                             (static_cast<unsigned char>(raw[i + 1]) << 8) |
                             (static_cast<unsigned char>(raw[i + 2]));
        out.push_back(kB64UrlAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kB64UrlAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(kB64UrlAlphabet[(chunk >> 6) & 0x3f]);
        out.push_back(kB64UrlAlphabet[chunk & 0x3f]);
    }
    std::size_t remain = raw.size() - i;
    if (remain == 1) {
        unsigned int chunk = static_cast<unsigned char>(raw[i]) << 16;
        out.push_back(kB64UrlAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kB64UrlAlphabet[(chunk >> 12) & 0x3f]);
    } else if (remain == 2) {
        unsigned int chunk = (static_cast<unsigned char>(raw[i]) << 16) |
                             (static_cast<unsigned char>(raw[i + 1]) << 8);
        out.push_back(kB64UrlAlphabet[(chunk >> 18) & 0x3f]);
        out.push_back(kB64UrlAlphabet[(chunk >> 12) & 0x3f]);
        out.push_back(kB64UrlAlphabet[(chunk >> 6) & 0x3f]);
    }
    return out;  // base64url 不补 '='，token 里 '=' 还得转义，纯属自找麻烦
}

bool base64UrlDecode(const std::string &in, std::string &out) {
    static signed char reverse[256];
    static bool reverseInit = false;
    if (!reverseInit) {
        for (int i = 0; i < 256; ++i) reverse[i] = -1;
        for (int i = 0; i < 64; ++i) {
            reverse[static_cast<unsigned char>(kB64UrlAlphabet[i])] = static_cast<signed char>(i);
        }
        reverseInit = true;
    }

    out.clear();
    out.reserve(in.size() * 3 / 4);
    unsigned int acc = 0;
    int bits = 0;
    for (char c : in) {
        signed char v = reverse[static_cast<unsigned char>(c)];
        if (v < 0) return false;  // 含 base64url 字母表外字符（含 '+' '/' '='）一律拒绝
        acc = (acc << 6) | static_cast<unsigned int>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xff));
        }
    }
    return true;
}

std::string genJti() {
    return genSalt(16);  // 16 字节 CSPRNG → 32 字符 hex
}

Jwt::Jwt(std::string secret, std::string issuer, int ttlSeconds)
    : secret_(std::move(secret)),
      issuer_(std::move(issuer)),
      ttlSeconds_(ttlSeconds) {}

std::string Jwt::sign(int64_t userId, const std::string &jti, int64_t nowSeconds) const {
    const int64_t now = nowSeconds > 0 ? nowSeconds : nowUnixSeconds();

    Json::Value header;
    header["alg"] = "HS256";
    header["typ"] = "JWT";

    Json::Value payload;
    payload["iss"] = issuer_;
    payload["sub"] = std::to_string(userId);
    payload["jti"] = jti;
    payload["iat"] = Json::Int64(now);
    payload["exp"] = Json::Int64(now + ttlSeconds_);

    std::string signingInput =
        base64UrlEncode(jsonToString(header)) + "." + base64UrlEncode(jsonToString(payload));
    return signingInput + "." + hmacToBase64Url(secret_, signingInput);
}

bool Jwt::verify(const std::string &token, JwtClaims &out, int64_t nowSeconds) const {
    // 1) 结构：必须是 a.b.c 三段
    std::size_t p1 = token.find('.');
    if (p1 == std::string::npos) return false;
    std::size_t p2 = token.find('.', p1 + 1);
    if (p2 == std::string::npos || token.find('.', p2 + 1) != std::string::npos) return false;

    const std::string headerB64 = token.substr(0, p1);
    const std::string payloadB64 = token.substr(p1 + 1, p2 - p1 - 1);
    const std::string sigB64 = token.substr(p2 + 1);
    if (headerB64.empty() || payloadB64.empty() || sigB64.empty()) return false;

    // 2) 先解 header 判 alg，再验签——顺序不能反。
    //    若先解 payload 再验签，"alg:none" 伪造本会在验签时被拒，但把 alg 检查放在最前
    //    能让"不受支持的算法"这条拒绝路径最短、最不可绕过。
    std::string headerJson;
    if (!base64UrlDecode(headerB64, headerJson)) return false;
    Json::Value header;
    if (!stringToJson(headerJson, header)) return false;
    if (!header.isMember("alg") || header["alg"].asString() != "HS256") return false;

    // 3) 验签（常量时间比对）
    const std::string signingInput = token.substr(0, p2);
    std::string expectedSig;
    if (!base64UrlDecode(sigB64, expectedSig)) return false;
    if (!constantTimeEquals(hmacSha256(secret_, signingInput), expectedSig)) return false;

    // 4) 解 payload，校验 iss 与 exp
    std::string payloadJson;
    if (!base64UrlDecode(payloadB64, payloadJson)) return false;
    Json::Value payload;
    if (!stringToJson(payloadJson, payload)) return false;
    if (!payload.isMember("iss") || !payload.isMember("exp") ||
        !payload.isMember("sub") || !payload.isMember("jti")) {
        return false;
    }
    if (payload["iss"].asString() != issuer_) return false;

    const int64_t now = nowSeconds > 0 ? nowSeconds : nowUnixSeconds();
    out.expiresAt = payload["exp"].asInt64();
    // 不设宽限（leeway）：服务端与签发端同一进程同一时钟，没有跨机时钟漂移要容。
    if (now >= out.expiresAt) return false;

    out.issuer = payload["iss"].asString();
    out.jti = payload["jti"].asString();
    out.issuedAt = payload.isMember("iat") ? payload["iat"].asInt64() : 0;

    const std::string sub = payload["sub"].asString();
    try {
        out.userId = std::stoll(sub);
    } catch (const std::exception &) {
        return false;
    }
    return true;
}

}  // namespace seckill::auth
