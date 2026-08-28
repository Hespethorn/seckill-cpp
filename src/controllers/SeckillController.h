#pragma once
#include <drogon/HttpController.h>
#include <memory>
#include "service/SeckillService.h"

// 秒杀接口控制器：只做协议转换（JSON <-> 业务参数），不含任何库存逻辑。
class SeckillController : public drogon::HttpController<SeckillController> {
public:
    explicit SeckillController(std::shared_ptr<SeckillService> svc)
        : svc_(std::move(svc)) {}

    METHOD_LIST_BEGIN
        // POST /api/seckill  body: {"userId":<int64>,"skuId":<int64>}
        ADD_METHOD_TO(SeckillController::seckill, "/api/seckill", drogon::Post);
    METHOD_LIST_END

    void seckill(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);

private:
    std::shared_ptr<SeckillService> svc_;
};
