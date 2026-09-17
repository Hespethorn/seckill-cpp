#include "UserController.h"

#include <drogon/plugins/RealIpResolver.h>
#include <json/json.h>

#include <string>

#include "logging/LogStream.h"

namespace seckill::auth {
namespace {

constexpr char kBearerPrefix[] = "Bearer ";

// 从 Authorization: Bearer <token> 里取 token。
// 返回空串表示没带或格式不对——调用方按"未认证"处理。
std::string extractBearerToken(const drogon::HttpRequestPtr &req) {
    const std::string &auth = req->getHeader("Authorization");
    if (auth.size() <= sizeof(kBearerPrefix) - 1) return {};
    if (auth.compare(0, sizeof(kBearerPrefix) - 1, kBearerPrefix) != 0) return {};
    std::string token = auth.substr(sizeof(kBearerPrefix) - 1);
    // 去掉可能的首尾空白（某些客户端会在 header 值里带空格）
    while (!token.empty() && (token.back() == ' ' || token.back() == '\r')) token.pop_back();
    return token;
}

drogon::HttpResponsePtr jsonResp(int httpStatus, int code, const std::string &msg) {
    Json::Value root;
    root["code"] = code;
    root["msg"] = msg;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(httpStatus));
    return resp;
}

drogon::HttpResponsePtr okWithData(Json::Value data) {
    Json::Value root;
    root["code"] = 0;
    root["msg"] = "success";
    root["data"] = std::move(data);
    return drogon::HttpResponse::newHttpJsonResponse(root);
}

// 业务拒绝（409）与系统错误（500）与频控（429）的分界：
//   429 = 频控触发（同 IP 注册达到上限），客户端应放缓后重试
//   409 = 客户端换个参数重试也没用，或者不该重试（密码错、已注册、被锁定）
//   500 = 系统故障，值得重试
int statusForFailure(const std::string &msg) {
    if (msg == "REGISTER_IP_LIMITED") return 429;
    if (msg.rfind("DB_ERROR:", 0) == 0 || msg.rfind("REDIS_ERROR:", 0) == 0 ||
        msg == "HASH_FAILED") {
        return 500;
    }
    return 409;
}

// 取客户端真实地址（同 IP 注册频控以此作 key）。
//
// 走 Drogon 官方 RealIpResolver 插件（见 config.json 的 plugins 段）：它在 pre-routing
// 阶段先校验 TCP 对端是否命中 trust_ips（可信代理），命中才去解析 X-Forwarded-For，
// 且从右往左跳过代理链、取第一个不可信 IP；不命中则直接采用 TCP 对端地址。
//
// ⚠️ 不要自己取 X-Forwarded-For 首段。首段是调用方可任意伪造的值：
//   ① 直连场景——客户端自己发一个 X-Forwarded-For 就能换掉频控 key，等于频控失效；
//   ② 反代场景——nginx 的 $proxy_add_x_forwarded_for 是"追加"，伪造值反而被拼在最左边，
//      取首段拿到的依然是伪造值。
// 插件未注册时 GetRealAddr 内部回退 getPeerAddr()，属安全降级方向（见上游 RealIpResolver.cc）。
std::string clientIp(const drogon::HttpRequestPtr &req) {
    return drogon::plugin::RealIpResolver::GetRealAddr(req).toIp();
}

}  // namespace

void UserController::registerUser(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    const auto &json = req->getJsonObject();
    if (!json || !json->isMember("phone") || !json->isMember("password")) {
        callback(jsonResp(400, 400, "missing phone/password"));
        return;
    }
    const std::string phone = (*json)["phone"].asString();
    const std::string password = (*json)["password"].asString();
    const std::string code =
        json->isMember("code") ? (*json)["code"].asString() : std::string();
    const std::string ip = clientIp(req);

    svc_->registerUser(phone, password, code, ip,
        [callback](bool ok, const std::string &msg) {
            if (!ok) {
                SK_LOG_WARN << "REGISTER_REJECTED msg=" << msg;
                callback(jsonResp(statusForFailure(msg), 1, msg));
                return;
            }
            callback(okWithData(Json::Value()));
        });
}

void UserController::login(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    const auto &json = req->getJsonObject();
    if (!json || !json->isMember("phone") || !json->isMember("password")) {
        callback(jsonResp(400, 400, "missing phone/password"));
        return;
    }
    const std::string phone = (*json)["phone"].asString();
    const std::string password = (*json)["password"].asString();

    svc_->login(phone, password,
        [callback](bool ok, const std::string &token, const std::string &msg) {
            if (!ok) {
                SK_LOG_WARN << "LOGIN_REJECTED msg=" << msg;
                callback(jsonResp(statusForFailure(msg), 1, msg));
                return;
            }
            Json::Value data;
            data["token"] = token;
            data["tokenType"] = "Bearer";
            callback(okWithData(std::move(data)));
        });
}

void UserController::logout(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    const std::string token = extractBearerToken(req);
    if (token.empty()) {
        callback(jsonResp(401, 401, "missing bearer token"));
        return;
    }
    svc_->logout(token, [callback](bool ok, const std::string &msg) {
        if (!ok) {
            callback(jsonResp(401, 401, msg));
            return;
        }
        callback(okWithData(Json::Value()));
    });
}

}  // namespace seckill::auth
