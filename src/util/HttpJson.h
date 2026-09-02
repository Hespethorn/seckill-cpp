#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/HttpStatusCode.h>
#include <json/json.h>

#include <functional>
#include <string>

namespace seckill::http {

// 统一 JSON 响应构造（3.8 重构抽出）。
//
// 阶段一所有接口共用同一套协议：成功 { code:0, data? }、失败 { code, msg }，
// 并带对应的 HTTP 状态码。之前每个 handler 都手写 newHttpJsonResponse +
// setStatusCode 样板，既重复又容易在"状态码"上各写各的。这里集中实现，
// 让控制器只关心"业务结果 -> (code, msg/数据, 状态码)"的映射。
//
// 用法：
//   seckill::http::reply(callback, 0, "success");
//   seckill::http::reply(callback, 1, "SOLD_OUT", drogon::k409Conflict);
//   seckill::http::replyData(callback, 0, jsonArray);  // 带 data 载荷

inline drogon::HttpResponsePtr jsonResponse(
    int code, const std::string &msg,
    drogon::HttpStatusCode status = drogon::k200Ok) {
    Json::Value root;
    root["code"] = code;
    root["msg"] = msg;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(status);
    return resp;
}

inline void reply(
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    int code, const std::string &msg,
    drogon::HttpStatusCode status = drogon::k200Ok) {
    callback(jsonResponse(code, msg, status));
}

inline void replyData(
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    int code, const Json::Value &data,
    drogon::HttpStatusCode status = drogon::k200Ok) {
    Json::Value root;
    root["code"] = code;
    root["data"] = data;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    resp->setStatusCode(status);
    callback(resp);
}

}  // namespace seckill::http
