#pragma once
#include <drogon/orm/DbClient.h>
#include <functional>
#include <string>

// 秒杀业务服务（阶段一：直接打数据库）。
//
// 设计取舍（本系列反复出现的主题）：
//   - 防超卖靠数据库行的原子 UPDATE（WHERE stock > 0），而不是在应用层先 SELECT 再 UPDATE。
//     应用层“先读后写”在并发下必然超卖，必须下沉到单行原子操作。
//   - 扣库存与落订单放在同一个 DB 事务里，保证“扣了库存一定有条订单”，要么全成要么全回滚。
//   - 代价：每个请求占用一条 DB 连接并持有 sku 行锁直到事务提交，QPS 上限被 DB 锁和连接数卡死
//     （实测 ~50 QPS 量级），这正是阶段二要引入 Redis 预扣减、阶段三引入 MQ 削峰的原因。
class SeckillService {
public:
    explicit SeckillService(drogon::orm::DbClientPtr client);

    // 异步秒杀：成功回调 (true,"OK")；库存不足 (false,"SOLD_OUT")；DB 异常 (false, 错误信息)。
    void doSeckill(int64_t userId,
                   int64_t skuId,
                   std::function<void(bool, const std::string &)> &&callback);

private:
    drogon::orm::DbClientPtr db_;
};
