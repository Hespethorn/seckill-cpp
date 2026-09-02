#include "SeckillController.h"
#include <json/json.h>
#include "logging/LogStream.h"

void SeckillController::seckill(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    // 1) 参数校验：秒杀必须带 userId + skuId，且为 JSON 体。
    //    注意：v1.9.10 起 getJsonObject() 返回 std::shared_ptr<Json::Value>（按值），
    //    不是裸指针，故用 const auto & 接，并以 !json 判空、(*json)[...] 取值。
    const auto &json = req->getJsonObject();
    if (!json || !json->isMember("userId") || !json->isMember("skuId")) {
        // 参数校验失败是客户端问题（不该出现在正常流量里），用 warn 留痕即可，
        // 既便于发现异常调用方，又不会像 error 那样在监控里被当成系统故障告警。
        SK_LOG_WARN << "BAD_REQUEST missing/invalid userId/skuId"
                    << " path=" << req->getPath()
                    << " remote=" << req->getPeerAddr().toIpPort();
        Json::Value err;
        err["code"] = 400;
        err["msg"] = "missing or invalid userId/skuId";
        auto resp = drogon::HttpResponse::newHttpJsonResponse(err);
        resp->setStatusCode(drogon::k400BadRequest);
        callback(resp);
        return;
    }

    const int64_t userId = (*json)["userId"].asInt64();
    const int64_t skuId = (*json)["skuId"].asInt64();

    // 2) 交给服务层处理；回调里把结果转成统一 JSON 协议。
    //    注意：doSeckill 内部是异步 DB 操作，回调在 Drogon 的 IO 线程上触发，
    //    不要在此处持有 req 的引用做同步阻塞。
    svc_->doSeckill(userId, skuId,
        [callback](bool ok, const std::string &msg) mutable {
            Json::Value root;
            root["code"] = ok ? 0 : 1;
            root["msg"] = ok ? "success" : msg;

            auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
            if (!ok) {
                // 用不同的 HTTP 状态码区分「业务拒绝」和「系统错误」：
                // 业务拒绝（售罄 / 重复下单）客户端不该重试；系统错误才值得重试。
                if (msg == "SOLD_OUT" || msg == "DUPLICATE_ORDER") {
                    resp->setStatusCode(drogon::k409Conflict);
                } else {
                    resp->setStatusCode(drogon::k500InternalServerError);
                }
            }
            callback(resp);
        });
}

void SeckillController::listSkus(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    svc_->listSkus(
        [callback](bool ok, const Json::Value &data) mutable {
            Json::Value root;
            int httpStatus = drogon::k200Ok;
            if (ok) {
                root["code"] = 0;
                root["data"] = data;  // Json 数组
            } else {
                root["code"] = 1;
                root["msg"] = "query failed";
                httpStatus = drogon::k500InternalServerError;
            }
            auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
            resp->setStatusCode(httpStatus);
            callback(resp);
        });
}
