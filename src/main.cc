#include <drogon/drogon.h>
#include <memory>
#include "controllers/HealthController.h"
#include "controllers/SeckillController.h"
#include "service/SeckillService.h"

// Drogon 1.9.10 关键坑：getDbClient() 只能在 app.run() 之后调用！
// 官方文档原话: "This method cannot be called before running app.run(),
// otherwise the user will get an empty shared_ptr."
//
// 原因：config.json 里的 db_clients 在 loadConfigFile 阶段只是"注册了配置"，
// 真正的 DbClient 对象（含连接池）是 run() 时才创建的。main() 里提前
// getDbClient("default") 必然拿到空 shared_ptr -> 首个请求在
// SeckillService::doSeckill 第一行 db_->... 空指针解引用 -> SIGSEGV。
//
// 解法：把"取客户端 + 组装服务"延迟到第一个 /api/seckill 请求到达时
// （handler 执行必然在 run() 之后），用 static 局部变量保证只初始化一次、
// 线程安全（C++11 magic static）。
std::shared_ptr<SeckillController> getSeckillController() {
    static std::shared_ptr<SeckillController> ctrl = [] {
        auto db = drogon::app().getDbClient("default");
        if (!db) {
            LOG_FATAL << "getDbClient(\"default\") returned null after run(): "
                         "check db_clients in config.json and Drogon MySQL support.";
            exit(1);
        }
        return std::make_shared<SeckillController>(
            std::make_shared<SeckillService>(db));
    }();
    return ctrl;
}

int main() {
    // 读取 config.json：其中声明了监听端口、线程数，以及名为 "default" 的 MySQL 客户端。
    // 注意：loadConfigFile 失败会抛异常/LOG_FATAL，程序起不来；
    // 能走到这里说明配置已成功注册，但 db 客户端对象要等 run() 才创建。
    drogon::app().loadConfigFile("./config.json");

    // 路由挂载：
    //  - HealthController 是无依赖的 HttpController，由 Drogon 自动注册（/api/health），
    //    无需（也不能）手动 registerController——v1.9.10 对 HttpController 子类调
    //    registerController 会触发静态断言。
    //  - SeckillController 需要注入 SeckillService（没有默认构造器，无法自动注册），
    //    因此用 registerHandler 手动挂 /api/seckill，lambda 把请求转交给 seckillCtrl 实例。
    //    registerHandler 会持有该 lambda，故按值捕获 seckillCtrl 以延长其生命周期。
    //    注意：handler 必须是两参形式 (req, callback)。Drogon v1.9.10 的 HttpBinder 对
    //    三参形式 (req, callback, path) 且路由无 {} 占位符时，会按 req->as<std::string>()
    //    兜底绑定第 3 个参数，而 fromRequest<std::string> 未特化 -> 运行时 exit(1)。
    //    无占位符的路由一律用两参形式，需要 path 时再走带 {} 的路由 + 三参（path 来自占位符）。
    drogon::app().registerHandler(
        "/api/seckill",
        [](const drogon::HttpRequestPtr &req,
           std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
            // run() 之后才能安全取 db client（见 getSeckillController 注释）
            getSeckillController()->seckill(req, std::move(callback));
        },
        {drogon::Post});

    LOG_INFO << "seckill-cpp starting on :8080 (stage-1: DB-direct)";
    drogon::app().run();
    return 0;
}
