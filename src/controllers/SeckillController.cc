#include "SeckillController.h"
#include <json/json.h>

void SeckillController::seckill(
    const drogon::HttpRequestPtr &req,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    // 1) 参数校验：秒杀必须带 userId + skuId，且为 JSON 体。
    const auto *json = req->getJsonObject();
    if (json == nullptr || !json->isMember("userId") || !json->isMember("skuId")) {
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
                // 库存不足用 409，其它（DB 异常等）用 500，方便上游区分重试策略。
                resp->setStatusCode(msg == "SOLD_OUT" ? drogon::k409Conflict
                                                      : drogon::k500InternalServerError);
            }
            callback(resp);
        });
}
