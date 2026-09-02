#pragma once
#include <drogon/HttpController.h>
#include <functional>
#include <memory>

#include <json/json.h>

#include "service/SeckillService.h"

// 秒杀接口控制器：只做协议转换（JSON <-> 业务参数），不含任何库存逻辑。
//
// 注意（v1.9.10 关键坑）：本类依赖注入 SeckillService，没有默认构造函数，
// 不能当作 Drogon 的 HttpController 自动注册——v1.9.10 起对 HttpController 子类
// 调用 registerController 会触发静态断言：
//   "Controllers created and initialized automatically by drogon cannot be registered here"
// 因此这里不继承 HttpController，改为在 main.cc 用 registerHandler 手动挂路由，
// 以 lambda 把请求转交给本实例（seckillCtrl）。
class SeckillController {
public:
    explicit SeckillController(std::shared_ptr<SeckillService> svc)
        : svc_(std::move(svc)) {}

    void seckill(const drogon::HttpRequestPtr &req,
                 std::function<void(const drogon::HttpResponsePtr &)> &&callback);

    // 4.1 商品列表：GET /api/seckill/list
    void listSkus(const drogon::HttpRequestPtr &req,
                  std::function<void(const drogon::HttpResponsePtr &)> &&callback);

private:
    std::shared_ptr<SeckillService> svc_;
};
