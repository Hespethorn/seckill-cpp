// 验证码送达：自签发 / 日志可读模式（详见 SmsSender.h）。
#include "SmsSender.h"

#include "logging/LogStream.h"

namespace seckill::auth {

void SmsSender::sendCode(const std::string &phone,
                         const std::string &code,
                         std::function<void(bool, const std::string &)> &&cb) {
    // 自签发：验证码只写进日志，由持有者读取后用于 /api/sms/verify。
    // 不接任何短信网关 —— 无凭据、无计费、无网络往返，项目开箱即跑。
    SK_LOG_WARN << "SMS_CODE phone=" << phone << " code=" << code
                << " (self-issued; read from logs for verify)";
    cb(true, "SELF_ISSUED");
}

}  // namespace seckill::auth
