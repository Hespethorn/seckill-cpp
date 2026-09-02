#include "SmsSender.h"

#include <curl/curl.h>
#include <drogon/drogon.h>  // drogon::app().getLoop()：把回调从工作线程切回 IO 线程
#include <json/json.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <chrono>
#include <cstdio>
#include <ctime>
#include <memory>
#include <thread>

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

const char *kHost = "sms.tencentcloudapi.com";
const char *kEndpoint = "https://sms.tencentcloudapi.com/";
const char *kService = "sms";
const char *kAlgorithm = "TC3-HMAC-SHA256";
const char *kAction = "SendSms";
const char *kVersion = "2021-01-11";

std::string toHexLower(const unsigned char *data, std::size_t len) {
    static const char *kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (std::size_t i = 0; i < len; ++i) {
        out.push_back(kDigits[data[i] >> 4]);
        out.push_back(kDigits[data[i] & 0x0f]);
    }
    return out;
}

std::size_t curlWriteCallback(void *contents, std::size_t size, std::size_t nmemb, void *userp) {
    const std::size_t total = size * nmemb;
    static_cast<std::string *>(userp)->append(static_cast<char *>(contents), total);
    return total;  // 返回非 total 会被 libcurl 当成接收失败
}

std::string utcDateString(std::time_t t) {
    std::tm tm{};
    gmtime_r(&t, &tm);  // TC3 的 CredentialScope 用的是 UTC 日期
    char buf[16];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm);
    return buf;
}

struct SendOutcome {
    bool ok = false;
    std::string detail;
};

// 真正发 HTTP 的地方——只能在工作线程里调用（同步阻塞）。
SendOutcome postSendSms(const SmsSender::Config &cfg,
                        const std::string &phone,
                        const std::string &code) {
    SendOutcome out;

    // 1) 组装业务请求体。手机号必须是 E.164 格式（+86 前缀），
    //    国内业务里最常见的报错就是漏了这个 '+'。
    Json::Value body;
    Json::Value phoneSet(Json::arrayValue);
    phoneSet.append("+86" + phone);
    body["PhoneNumberSet"] = phoneSet;
    body["SmsSdkAppId"] = cfg.sdkAppId;
    body["SignName"] = cfg.signName;
    body["TemplateId"] = cfg.templateId;
    Json::Value params(Json::arrayValue);
    params.append(code);
    params.append("5");  // 模板里的 {2}：有效期（分钟），与 SmsService 的 codeTtl 对齐口径
    body["TemplateParamSet"] = params;

    Json::StreamWriterBuilder wb;
    wb["indentation"] = "";
    const std::string payload = Json::writeString(wb, body);

    // 2) TC3 签名
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    const std::string timestamp = std::to_string(static_cast<long long>(now));
    const std::string date = utcDateString(now);

    // 规范请求串：方法 / URI / 查询串 / 规范头 / 已签名头列表 / 请求体哈希
    const std::string contentType = "application/json; charset=utf-8";
    const std::string canonicalHeaders =
        "content-type:" + contentType + "\n" + "host:" + kHost + "\n";
    const std::string signedHeaders = "content-type;host";
    const std::string canonicalRequest = std::string("POST") + "\n" +
                                         "/" + "\n" +
                                         "" + "\n" +
                                         canonicalHeaders + "\n" +
                                         signedHeaders + "\n" +
                                         sha256Hex(payload);

    // 待签字符串
    const std::string credentialScope = date + "/" + kService + "/tc3_request";
    const std::string stringToSign = std::string(kAlgorithm) + "\n" +
                                     timestamp + "\n" +
                                     credentialScope + "\n" +
                                     sha256Hex(canonicalRequest);

    // 三级 HMAC 密钥派生：SecretDate → SecretService → SecretSigning
    const std::string secretDate = hmacSha256Raw("TC3" + cfg.secretKey, date);
    const std::string secretService = hmacSha256Raw(secretDate, kService);
    const std::string secretSigning = hmacSha256Raw(secretService, "tc3_request");
    const std::string signature = hmacSha256Hex(secretSigning, stringToSign);

    const std::string authorization = std::string(kAlgorithm) +
                                      " Credential=" + cfg.secretId + "/" + credentialScope +
                                      ", SignedHeaders=" + signedHeaders +
                                      ", Signature=" + signature;

    // 3) 发请求
    CURL *curl = curl_easy_init();
    if (!curl) {
        out.detail = "curl_easy_init failed";
        return out;
    }

    std::string response;
    struct curl_slist *headers = nullptr;
    const std::string tsHeader = "X-TC-Timestamp: " + timestamp;
    const std::string regionHeader = "X-TC-Region: " + cfg.region;
    headers = curl_slist_append(headers, ("Content-Type: " + contentType).c_str());
    headers = curl_slist_append(headers, ("Host: " + std::string(kHost)).c_str());
    headers = curl_slist_append(headers, ("Authorization: " + authorization).c_str());
    headers = curl_slist_append(headers, ("X-TC-Action: " + std::string(kAction)).c_str());
    headers = curl_slist_append(headers, ("X-TC-Version: " + std::string(kVersion)).c_str());
    headers = curl_slist_append(headers, tsHeader.c_str());
    headers = curl_slist_append(headers, regionHeader.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, kEndpoint);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    // 超时是硬要求：短信是旁路，绝不能因为下游慢而拖住业务。
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, static_cast<long>(cfg.connectTimeoutSeconds));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(cfg.timeoutSeconds));
    // 多线程程序里必须开 NOSIGNAL：libcurl 的 DNS 超时用 SIGALRM 实现，
    // 在 worker 线程里触发信号会打断整个进程。
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    const CURLcode rc = curl_easy_perform(curl);
    long httpStatus = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpStatus);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        out.detail = std::string("curl error: ") + curl_easy_strerror(rc);
        return out;
    }

    // 4) 解析响应。腾讯云的 HTTP 状态码即使 200，业务也可能失败（签名错、模板未审核等），
    //    真正的成败在 body 的 Response.Error / Response.SendStatusSet[0].Code。
    Json::Value root;
    Json::CharReaderBuilder rb;
    std::unique_ptr<Json::CharReader> reader(rb.newCharReader());
    std::string errs;
    if (!reader->parse(response.data(), response.data() + response.size(), &root, &errs)) {
        out.detail = "bad json response: " + response.substr(0, 200);
        return out;
    }
    const Json::Value &resp = root["Response"];
    if (resp.isMember("Error")) {
        out.detail = "TC error " + resp["Error"]["Code"].asString() + ": " +
                     resp["Error"]["Message"].asString();
        return out;
    }
    if (resp.isMember("SendStatusSet") && resp["SendStatusSet"].isArray() &&
        !resp["SendStatusSet"].empty()) {
        const Json::Value &st = resp["SendStatusSet"][0];
        const std::string codeStr = st["Code"].asString();
        out.ok = (codeStr == "Ok");
        out.detail = codeStr + ": " + st["Message"].asString();
        return out;
    }
    out.detail = "unexpected response: " + response.substr(0, 200);
    return out;
}

}  // namespace

std::string sha256Hex(const std::string &data) {
    unsigned char md[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char *>(data.data()), data.size(), md);
    return toHexLower(md, SHA256_DIGEST_LENGTH);
}

std::string hmacSha256Raw(const std::string &key, const std::string &data) {
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char *>(data.data()), data.size(),
         md, &len);
    return std::string(reinterpret_cast<char *>(md), len);
}

std::string hmacSha256Hex(const std::string &key, const std::string &data) {
    const std::string raw = hmacSha256Raw(key, data);
    return toHexLower(reinterpret_cast<const unsigned char *>(raw.data()), raw.size());
}

SmsSender::SmsSender(Config cfg) : cfg_(std::move(cfg)) {}

void SmsSender::sendCode(const std::string &phone,
                         const std::string &code,
                         std::function<void(bool, const std::string &)> &&cb) {
    if (!cfg_.enabled) {
        // 未配置凭据（本地开发/压测）：不真的发，但把验证码写进日志，
        // 让整条链路（生成 → 存 Redis → 校验 → 限流）依然能被端到端验证。
        SK_LOG_WARN << "SMS_MOCK phone=" << phone << " code=" << code
                    << " (sms.enabled=false or credentials empty)";
        cb(true, "MOCK_SENT (sms disabled)");
        return;
    }

    // 阻塞的 HTTP 调用挪到独立线程，回调切回 IO 线程
    std::thread([cfg = cfg_, phone, code, cb = std::move(cb)]() {
        SendOutcome outcome = postSendSms(cfg, phone, code);
        if (!outcome.ok) {
            SK_LOG_ERROR << "SMS_SEND_FAILED phone=" << phone
                         << " detail=" << outcome.detail;
        } else {
            SK_LOG_INFO << "SMS_SENT phone=" << phone;
        }
        // Drogon 的响应构造必须在 IO 线程，回调里会碰 HttpRequest/Response
        drogon::app().getLoop()->queueInLoop(
            [cb = std::move(cb), outcome]() { cb(outcome.ok, outcome.detail); });
    }).detach();
}

}  // namespace seckill::auth
