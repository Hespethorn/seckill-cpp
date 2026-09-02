// 用户控制器（3.1-3.2 / 3.7）：只做协议转换，不含业务逻辑
//
// 与 SeckillController 保持一致：不继承 HttpController（Drogon v1.9.10 对
// 无默认构造、需要注入依赖的 HttpController 子类调 registerController 会静态断言失败），
// 改在 main.cc 里用 registerHandler 手动挂路由。
#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>
#include <memory>

#include "service/UserService.h"

namespace seckill::auth {

class UserController {
public:
    explicit UserController(std::shared_ptr<UserService> svc) : svc_(std::move(svc)) {}

    // POST /api/user/register  {phone, password, code}
    void registerUser(const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/user/login     {phone, password}
    void login(const drogon::HttpRequestPtr &req,
               std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // POST /api/user/logout    Authorization: Bearer <token>
    void logout(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback);

private:
    std::shared_ptr<UserService> svc_;
};

}  // namespace seckill::auth
