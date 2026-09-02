// 短信发送：直连腾讯云短信 API 3.0（TC3-HMAC-SHA256 自签名 + libcurl）
//
// 为什么不引官方 tencentcloud-sdk-cpp：
//   官方 C++ SDK 要拉 core 模块（几百个文件、依赖 protobuf/boost 若干组件），
//   编译一次十几分钟，而我们需要的只是「签一个 TC3 签名 + POST 一段 JSON」。
//   TC3 签名算法是公开标准（HMAC 链 + SHA256，见代码），自己实现约 120 行，
//   依赖只有 libcurl 与 OpenSSL——这两个在 setup-wsl.sh 里本来就装了。
//   换来的是：零编译负担、签名过程完全可审计、出问题能直接打印待签串比对。
//
// 关键工程约束：libcurl 是同步阻塞的。Drogon 的 handler 跑在 IO 线程上，
//   直接 curl_easy_perform 会把该线程上所有请求卡住几十毫秒（短信 API 往返）。
//   所以发送必须挪到独立线程执行，完成后用 queueInLoop 把回调切回 IO 线程。
//   这里用一次性 std::thread + detach：短信是低频操作（有每日上限，见 SmsService），
//   建线程约 50us，相对 API 往返的 50ms 可以忽略；换线程池省不下什么，反而多一套生命周期管理。
#pragma once

#include <functional>
#include <string>

namespace seckill::auth {

class SmsSender {
public:
    struct Config {
        bool enabled = false;          // 未配置凭据时关掉，降级为"只打日志"
        std::string secretId;
        std::string secretKey;
        std::string region = "ap-guangzhou";
        std::string sdkAppId;          // 短信应用 ID（数字串）
        std::string signName;          // 已审核通过的签名内容
        std::string templateId;        // 已审核通过的模板 ID
        int timeoutSeconds = 5;        // 整个请求超时
        int connectTimeoutSeconds = 3; // 建连超时（防慢下游拖死工作线程）
    };

    explicit SmsSender(Config cfg);

    // 发送验证码短信。回调在 Drogon IO 线程上触发（内部已做线程切换）。
    // cb(ok, detail)：ok=false 时 detail 是可直接写进日志的错误描述。
    void sendCode(const std::string &phone,
                  const std::string &code,
                  std::function<void(bool, const std::string &)> &&cb);

    bool enabled() const { return cfg_.enabled; }

private:
    Config cfg_;
};

// 暴露出来只为自测与排障
std::string sha256Hex(const std::string &data);
std::string hmacSha256Hex(const std::string &key, const std::string &data);
std::string hmacSha256Raw(const std::string &key, const std::string &data);

}  // namespace seckill::auth
