#include <drogon/drogon.h>
#include <memory>
#include "controllers/HealthController.h"
#include "controllers/SeckillController.h"
#include "service/SeckillService.h"

int main() {
    // 读取 config.json：其中声明了监听端口、线程数，以及名为 "default" 的 MySQL 客户端。
    drogon::app().loadConfigFile("./config.json");

    // 从 Drogon 托管的连接池取 DB 客户端，注入到业务服务里。
    // 这里故意把“数据访问”和“HTTP 路由”拆开：controller 只负责协议转换，
    // 真正的秒杀逻辑在 SeckillService，方便后续替换存储层（Redis/MQ）而不动路由。
    auto db = drogon::app().getDbClient("default");
    auto seckillSvc = std::make_shared<SeckillService>(db);
    auto seckillCtrl = std::make_shared<SeckillController>(seckillSvc);

    // 路由挂载：
    //  - HealthController 是无依赖的 HttpController，由 Drogon 自动注册（/api/health），
    //    无需（也不能）手动 registerController——v1.9.10 对 HttpController 子类调
    //    registerController 会触发静态断言。
    //  - SeckillController 需要注入 SeckillService（没有默认构造器，无法自动注册），
    //    因此用 registerHandler 手动挂 /api/seckill，lambda 把请求转交给 seckillCtrl 实例。
    //    registerHandler 会持有该 lambda，故按值捕获 seckillCtrl 以延长其生命周期。
    drogon::app().registerHandler(
        "/api/seckill",
        [seckillCtrl](const drogon::HttpRequestPtr &req,
                      std::function<void(const drogon::HttpResponsePtr &)> &&callback,
                      const std::string & /*path*/) {
            seckillCtrl->seckill(req, std::move(callback));
        },
        {drogon::Post});

    LOG_INFO << "seckill-cpp starting on :8080 (stage-1: DB-direct)";
    drogon::app().run();
    return 0;
}
