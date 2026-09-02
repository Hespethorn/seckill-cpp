#include "SeckillController.h"
#include <json/json.h>
#include <string>
#include "logging/LogStream.h"
#include "util/HttpJson.h"

using namespace seckill::http;  // reply / replyData 统一 JSON 响应构造

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
        reply(std::move(callback), 400, "missing or invalid userId/skuId",
              drogon::k400BadRequest);
        return;
    }

    const int64_t userId = (*json)["userId"].asInt64();
    const int64_t skuId = (*json)["skuId"].asInt64();

    // 2) 交给服务层处理；回调里把结果转成统一 JSON 协议。
    //    注意：doSeckill 内部是异步 DB 操作，回调在 Drogon 的 IO 线程上触发，
    //    不要在此处持有 req 的引用做同步阻塞。
    svc_->doSeckill(userId, skuId,
        [callback](bool ok, const std::string &msg) mutable {
            // 用不同的 HTTP 状态码区分「业务拒绝」和「系统错误」：
            // 业务拒绝（售罄 / 重复下单）客户端不该重试；系统错误才值得重试。
            if (ok) {
                reply(std::move(callback), 0, "success");
            } else if (msg == "SOLD_OUT" || msg == "DUPLICATE_ORDER") {
                reply(std::move(callback), 1, msg, drogon::k409Conflict);
            } else {
                reply(std::move(callback), 1, msg, drogon::k500InternalServerError);
            }
        });
}

void SeckillController::listSkus(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
    svc_->listSkus(
        [callback](bool ok, const Json::Value &data) mutable {
            if (ok) {
                replyData(std::move(callback), 0, data);
            } else {
                reply(std::move(callback), 1, "query failed",
                      drogon::k500InternalServerError);
            }
        });
}

void SeckillController::detailSku(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback,
    const std::string &skuIdStr) {
    // 路径参数校验：skuId 必须是正整数。非法直接 400，不进 DB。
    int64_t skuId = -1;
    try {
        skuId = std::stoll(skuIdStr);
    } catch (...) {
        skuId = -1;
    }
    if (skuId <= 0) {
        reply(std::move(callback), 400, "invalid skuId", drogon::k400BadRequest);
        return;
    }

    svc_->detailSku(skuId,
        [callback](bool ok, const Json::Value &data) mutable {
            if (ok) {
                replyData(std::move(callback), 0, data);
            } else {
                reply(std::move(callback), 1, "sku not found", drogon::k404NotFound);
            }
        });
}
