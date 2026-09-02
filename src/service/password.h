// 密码哈希：PBKDF2-HMAC-SHA256（OpenSSL）
//
// 为什么是 PBKDF2 而不是引 bcrypt-cpp：
//   - 依赖更少：OpenSSL 是 Drogon / hiredis 的底层依赖，几乎必然在场；
//     引 bcrypt-cpp 是多一项依赖 + 一段编译面。
//   - 可调迭代次数：硬件变快就把 iterations 上调，没有"库停更就裸奔"的风险。
//   - 算法公开可审计，代码就这几行，出问题能自己定位。
// 代价：salt 生成与 hex 编码要自己管——而这正是本系列想练的底层功力。
#pragma once

#include <cstddef>
#include <string>

namespace seckill::auth {

// 生成 bytes 字节密码学安全随机 salt，返回 hex 字符串（长度 = bytes * 2）。
// 用 OpenSSL 的 CSPRNG（RAND_bytes），绝不用 std::rand()——
// 后者是线性同余、输出可预测，拿来做 salt 等于给攻击者减难度。
std::string genSalt(std::size_t bytes = 16);

// PBKDF2-HMAC-SHA256 派生，返回 hex 字符串（长度 = dkLen * 2）。
// iterations 默认 12 万：低了抗不住 GPU 暴力，高了拖慢登录（本机实测单次约 30~60ms）。
// 返回空串表示 OpenSSL 调用失败（极罕见，调用方应按系统错误兜底）。
std::string pbkdf2Hex(const std::string &password,
                      const std::string &salt,
                      int iterations = 120000,
                      std::size_t dkLen = 32);

// 常量时间比较：长度不同也走完整循环，避免因比较提前返回而泄露"前缀匹配了几位"。
// 密码哈希比对必须用这个，不能用 ==（== 的短路时机是可被计时攻击观测的）。
bool constantTimeEquals(const std::string &a, const std::string &b);

// 二进制 → 小写 hex
std::string toHex(const unsigned char *data, std::size_t len);

}  // namespace seckill::auth
