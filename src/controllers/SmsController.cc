#include "SmsController.h"

#include <json/json.h>

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

drogon::HttpResponsePtr resp(int httpStatus, int code, const std::string &msg) {
    Json::Value root;
    root["code"] = code;
    root["msg"] = msg;
    auto r = drogon::HttpResponse::newHttpJsonResponse(root);
    r->setStatusCode(static_cast<drogon::HttpStatusCode>(httpStatus));
    return r;
}

}  // namespace

void SmsController::sendCode(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    const auto &json = req->getJsonObject();
    if (!json || !json->isMember("phone")) {
        callback(resp(400, 400, "missing phone"));
        return;
    }
    const std::string phone = (*json)["phone"].asString();

    svc_->sendCode(phone,
        [callback, phone](SmsService::SendResult r, const std::string &detail) {
            switch (r) {
                case SmsService::SendResult::Ok:
                    // 注意：响应体里绝不回显验证码。哪怕本地 mock 模式
                    // （验证码只写进日志），这个约束也不能破——
                    // 一旦有人为了"调试方便"把 code 放进响应，
                    // 上线时忘了删，就是任何人都能给自己手机发码并直接拿到的洞。
                    callback(resp(200, 0, "success"));
                    return;
                case SmsService::SendResult::InvalidPhone:
                    callback(resp(400, 400, "invalid phone"));
                    return;
                case SmsService::SendResult::TooFrequent:
                    // 429 而不是 409：这是"频率限制"，语义上客户端等一会儿可以重试
                    callback(resp(429, 1, "too frequent: " + detail));
                    return;
                case SmsService::SendResult::DailyLimit:
                    callback(resp(429, 1, "daily limit reached: " + detail));
                    return;
                default:
                    SK_LOG_ERROR << "SMS_SEND_REJECTED phone=" << phone
                                 << " detail=" << detail;
                    callback(resp(500, 1, "send failed: " + detail));
                    return;
            }
        });
}

}  // namespace seckill::auth
