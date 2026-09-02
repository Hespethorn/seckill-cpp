// 短信验证码控制器（3.3）：POST /api/sms/send  {phone}
#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>

#include "service/SmsService.h"

namespace seckill::auth {

class SmsController {
public:
    explicit SmsController(std::shared_ptr<SmsService> svc) : svc_(std::move(svc)) {}

    void sendCode(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&callback);

private:
    std::shared_ptr<SmsService> svc_;
};

}  // namespace seckill::auth
