#pragma once
#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "InflightGuard.h"

// 秒杀业务服务（阶段一：直接打数据库）。
//
// 设计取舍（本系列反复出现的主题）：
//   - 防超卖靠数据库行的原子 UPDATE（WHERE stock > 0），而不是在应用层先 SELECT 再 UPDATE。
//     应用层“先读后写”在并发下必然超卖，必须下沉到单行原子操作。
//   - 扣库存与落订单放在同一个 DB 事务里，保证“扣了库存一定有条订单”，要么全成要么全回滚。
//   - 代价：每个请求占用一条 DB 连接并持有 sku 行锁直到事务提交，QPS 上限被行锁串行化卡死
//     （实测基线 ~371 QPS，见 README 阶段一基线小节），这正是阶段二引入 Redis 预扣减、
//     阶段三引入 MQ 削峰的原因。
//   - 4.8 在此之上加了一道应用层“在途闸门”（InflightGuard）：同一 (userId, skuId)
//     已有请求在处理时，后续请求**连数据库都不打**直接在应用层判重复。
//     它不是"加锁变快"，而是把无效请求挡在数据库之外。
class SeckillService {
public:
    // inflight 传 nullptr 或 Mode::None 即关闭闸门（阶段一压测基线就是这个状态）
    explicit SeckillService(drogon::orm::DbClientPtr client,
                            std::shared_ptr<seckill::InflightGuard> inflight = nullptr);

    // 异步秒杀，回调语义：
    //   (true,  "OK")               下单成功
    //   (false, "SOLD_OUT")         库存不足
    //   (false, "DUPLICATE_ORDER")  重复下单：
    //                               - 数据库唯一键命中（真的已有订单）
    //                               - 或应用层在途闸门拦下（有同键请求正在处理）
    //   (false, "DB_ERROR: ...")    系统异常
    void doSeckill(int64_t userId,
                   int64_t skuId,
                   std::function<void(bool, const std::string &)> &&callback);

private:
    drogon::orm::DbClientPtr db_;
    std::shared_ptr<seckill::InflightGuard> inflight_;
};
