#include "HealthController.h"
#include <json/json.h>

void HealthController::health(
    const drogon::HttpRequestPtr &,
    std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
    Json::Value root;
    root["status"] = "UP";
    root["service"] = "seckill-cpp";
    root["stage"] = "1-db-direct";

    auto resp = drogon::HttpResponse::newHttpJsonResponse(root);
    callback(resp);
}
