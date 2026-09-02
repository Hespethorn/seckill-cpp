// 验证码送达：自签发 / 日志可读模式（不接任何短信网关）
//
// 设计取舍：验证码模块的硬核价值在 SmsService —— 6 位码生成（CSPRNG）、
// Redis 存储 + TTL、Lua 原子校验、发送限流、登录失败锁定。至于"码怎么送到用户"，
// 对一个自驱动 / 演示 / 投稿验收性质的项目是次要的，且引入短信网关会带来
// 凭据、计费、网络往返等外部负担。因此本服务不接任何短信网关，
// 验证码只写进日志（前缀 SMS_CODE），由持有者自行读取、用于后续校验。
//
// 送达通道是可插拔的：若将来要接真实短信，实现一个 SendProvider 接口替换本类即可，
// 对 SmsService 及以下链路零侵入。
#pragma once

#include <functional>
#include <string>

namespace seckill::auth {

// 验证码送达器。当前实现：自签发 + 写日志，不发起任何外部网络请求。
class SmsSender {
public:
    // sendCode 在 IO 线程内同步回调 cb(ok, detail)。
    // ok 恒为 true（自签发必然成功）；detail 供日志 / 响应展示用。
    void sendCode(const std::string &phone,
                  const std::string &code,
                  std::function<void(bool, const std::string &)> &&cb);
};

}  // namespace seckill::auth
