#pragma once
#include <drogon/HttpController.h>

// 健康检查控制器：供网关/探活/K8s liveness 使用，不碰 DB。
class HealthController : public drogon::HttpController<HealthController> {
public:
    METHOD_LIST_BEGIN
        // GET /api/health
        ADD_METHOD_TO(HealthController::health, "/api/health", drogon::Get);
    METHOD_LIST_END

    void health(const drogon::HttpRequestPtr &req,
                std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
};
