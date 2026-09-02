#include "password.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace seckill::auth {

std::string toHex(const unsigned char *data, std::size_t len) {
    static const char *kHexDigits = "0123456789abcdef";
    std::string hex;
    hex.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        unsigned char c = data[i];
        hex.push_back(kHexDigits[c >> 4]);
        hex.push_back(kHexDigits[c & 0x0f]);
    }
    return hex;
}

std::string genSalt(std::size_t bytes) {
    std::string raw;
    raw.resize(bytes);
    // RAND_bytes 失败（熵池不可用）时不能静默返回全零——那等于所有账号共用空 salt。
    if (RAND_bytes(reinterpret_cast<unsigned char *>(raw.data()),
                   static_cast<int>(bytes)) != 1) {
        return {};
    }
    return toHex(reinterpret_cast<const unsigned char *>(raw.data()), bytes);
}

std::string pbkdf2Hex(const std::string &password,
                      const std::string &salt,
                      int iterations,
                      std::size_t dkLen) {
    std::string dk;
    dk.resize(dkLen);
    int rc = PKCS5_PBKDF2_HMAC(
        password.data(),
        static_cast<int>(password.size()),
        reinterpret_cast<const unsigned char *>(salt.data()),
        static_cast<int>(salt.size()),
        iterations,
        EVP_sha256(),
        static_cast<int>(dkLen),
        reinterpret_cast<unsigned char *>(dk.data()));
    if (rc != 1) {
        return {};
    }
    return toHex(reinterpret_cast<const unsigned char *>(dk.data()), dkLen);
}

bool constantTimeEquals(const std::string &a, const std::string &b) {
    // 先把长度差异折进 diff，再无条件跑满整个较长串的长度。
    // 这样"长度不同"和"内容不同"两条路径的耗时不可区分。
    std::size_t maxLen = a.size() > b.size() ? a.size() : b.size();
    unsigned char diff = (a.size() == b.size()) ? 0 : 1;
    for (std::size_t i = 0; i < maxLen; ++i) {
        unsigned char ca = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        diff |= static_cast<unsigned char>(ca ^ cb);
    }
    return diff == 0;
}

}  // namespace seckill::auth
