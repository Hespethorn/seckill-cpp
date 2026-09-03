#pragma once
#include <drogon/orm/DbClient.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <json/json.h>

#include "InflightGuard.h"
#include "SkuCache.h"

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
//
// 第五章（5.2 / 5.3）：两个**读**接口（商品列表 / 商品详情）加上 Redis 缓存层。
//   注意加缓存的对象是读接口，不是下单接口 —— 下单必须落到 MySQL 拿行锁做原子扣减，
//   缓存替代不了真相源。读接口则是纯粹的"重复劳动"：同一份商品数据在洪峰期
//   每秒被查几千次，每次都让 MySQL 做一遍同样的 SELECT，这才是缓存该干的活。
//   缓存策略是 Cache-Aside（旁路缓存）：读走缓存、miss 回源并回写，写则删除缓存。
class SeckillService {
public:
    // inflight 传 nullptr 或 Mode::None 即关闭闸门（阶段一压测基线就是这个状态）
    // cache    传 nullptr 或 cache.enabled=false 即直连 DB（5.1 的"接口基线"就是这个状态）
    explicit SeckillService(drogon::orm::DbClientPtr client,
                            std::shared_ptr<seckill::InflightGuard> inflight = nullptr,
                            std::shared_ptr<seckill::cache::SkuCache> cache = nullptr);

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

    // 4.1 秒杀商品列表（只读，不需要事务；5.2 起走 Redis 缓存）。回调 (ok, data)：
    //   ok=true  -> data 为 Json 数组，元素含 id/name/stock/total/startTime/endTime
    //   ok=false -> data 为空（查询失败，已记日志）
    void listSkus(std::function<void(bool, const Json::Value &)> &&callback);

    // 4.2 秒杀商品详情（按 id 查单个，只读；5.3 起走 Redis 缓存）。回调 (ok, data)：
    //   ok=true  -> data 为 Json 对象（单条商品）
    //   ok=false -> data 为空，表示 sku 不存在（前端应展示 404）
    //               ——注意：命中"空值缓存"时同样走这条路径，区别是它没打数据库
    void detailSku(int64_t skuId,
                   std::function<void(bool, const Json::Value &)> &&callback);

    const std::shared_ptr<seckill::cache::SkuCache> &cache() const { return cache_; }

private:
    // 回源数据库的真实查询（缓存 miss 或缓存关闭时走这里），查到后按 writeCache 回写。
    void queryListFromDb(std::function<void(bool, const Json::Value &)> cb,
                         bool writeCache);
    void queryDetailFromDb(int64_t skuId,
                           std::function<void(bool, const Json::Value &)> cb,
                           bool writeCache);

    drogon::orm::DbClientPtr db_;
    std::shared_ptr<seckill::InflightGuard> inflight_;
    std::shared_ptr<seckill::cache::SkuCache> cache_;
};
