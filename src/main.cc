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

    // 注册控制器。HttpController 子类经 registerController 后，METHOD_LIST 里声明的
    // 路由（/api/health、/api/seckill）会自动挂载。
    drogon::app().registerController(std::make_shared<HealthController>());
    drogon::app().registerController(std::make_shared<SeckillController>(seckillSvc));

    LOG_INFO << "seckill-cpp starting on :8080 (stage-1: DB-direct)";
    drogon::app().run();
    return 0;
}
