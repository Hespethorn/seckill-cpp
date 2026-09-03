#include "UserController.h"

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

// 取客户端地址：本 Drogon 版本无 getClientIp()，这里手动解析：
//   真实部署走反代时，原始客户端 IP 在 X-Forwarded-For 首段（形如 "client, proxy1, proxy2"）；
//   直连或无该头时回退到 TCP 对端地址。频控以此地址为 key。
std::string clientIp(const drogon::HttpRequestPtr &req) {
    const std::string &xff = req->getHeader("X-Forwarded-For");
    if (!xff.empty()) {
        std::string ip = xff.substr(0, xff.find(','));  // 首段即最原始客户端
        const size_t s = ip.find_first_not_of(" \t");
        const size_t e = ip.find_last_not_of(" \t");
        if (s != std::string::npos) ip = ip.substr(s, e - s + 1);
        if (!ip.empty()) return ip;
    }
    return req->getPeerAddr().toIp();
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
