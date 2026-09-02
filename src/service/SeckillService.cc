#include "SeckillService.h"

#include <drogon/orm/Exception.h>
#include <drogon/orm/Result.h>

#include <exception>
#include <json/json.h>

#include "logging/LogStream.h"

using namespace seckill;  // InflightGuard / InflightToken 都定义在 seckill 命名空间

SeckillService::SeckillService(drogon::orm::DbClientPtr client,
                               std::shared_ptr<InflightGuard> inflight)
    : db_(std::move(client)), inflight_(std::move(inflight)) {}

void SeckillService::doSeckill(
    int64_t userId,
    int64_t skuId,
    std::function<void(bool, const std::string &)> &&callback) {
    // ── 4.8 应用层在途闸门 ───────────────────────────────────────────────
    // 同一 (userId, skuId) 若已有请求正在处理（还没从 DB 回来），后续请求直接判重复，
    // 不再去挤 MySQL 行锁。注意这道闸门**只挡并发窗口内的重复**，
    // 串行发出的两次下单（第一次已完成、订单已落库）不会被它拦住——
    // 那种情况由 seckill_order 的 uk_user_sku 唯一键兜底。
    // 两层分工：应用层挡并发重复（省 DB 往返），数据库唯一键挡一切重复（保正确性）。
    InflightToken token;
    if (inflight_ && inflight_->mode() != InflightGuard::Mode::None) {
        token = inflight_->tryAcquire(InflightGuard::makeKey(userId, skuId));
        if (!token.valid()) {
            // 与"数据库命中唯一键"返回同一个业务码：客户端语义一致（不该重试），
            // 但日志里区分开，便于压测时统计应用层到底挡掉了多少。
            SK_LOG_WARN << "INFLIGHT_REJECT userId=" << userId << " skuId=" << skuId;
            callback(false, "DUPLICATE_ORDER");
            return;
        }
    }
    // 从这里开始，token 会被按值捕获进下面每一层 lambda。
    // 这是必须的：如果只捕获进最外层，最外层 lambda 发起第一条 SQL 后就析构了，
    // 标记会被提前清掉，闸门形同虚设。每一层都持有一份 shared_ptr，
    // 最后一份析构时才真正释放。

    // 开一个 DB 事务。newTransactionAsync 回调拿到的是 shared_ptr<Transaction>（const 引用）。
    //
    // v1.9.10 事务语义（见 orm_lib/src/TransactionImpl.cc 析构）：
    //   - Transaction 没有 commit() 成员；提交发生在“最后一个 shared_ptr<Transaction>
    //     析构”时自动发 commit，成功才触发 setCommitCallback 注册的回调。
    //   - rollback() 显式回滚并置 isCommitedOrRolledback_ 标志，析构便不再自动 commit，
    //     commitCallback 也不会再触发——所以回滚路径自己调业务回调，提交路径交给析构。
    //   - 流式 << >> 不支持 (Result, exception_ptr) 合并回调（FunctionTraits 无该特化，
    //     合并式会被误判成“按列提取值”回调而默认构造 Result 失败）。故每条 SQL 用
    //     execSqlAsync(sql, 成功回调, 异常回调, 参数...)：成功回调 void(const Result&)
    //     走 callbackHolder_，异常回调 void(const exception_ptr&) 走 exceptionPtrCallback_，
    //     二者由 Drogon 内部各挂一个 >>，成功/错误不会重复触发。
    //   - cb 在各层 lambda 里按值拷贝（std::function 可拷贝），确保每条路径恰好调用一次、
    //     且不会因为链式 std::move 变成 moved-from 空函数。
    db_->newTransactionAsync(
        [userId, skuId, cb = std::move(callback), token](
            const std::shared_ptr<drogon::orm::Transaction> &tx) {
            if (!tx) {
                // 文档说明：超时情况下回调会拿到空的 shared_ptr。
                // 这条是系统级故障信号（连接池耗尽/DB 不可达），必须留痕。
                SK_LOG_ERROR << "no transaction (timeout) userId=" << userId
                             << " skuId=" << skuId;
                cb(false, "DB_ERROR: no transaction (timeout)");
                return;
            }

            // 提交结果回调：事务析构自动 commit 之后触发（仅成功才调）。
            tx->setCommitCallback(
                [cb, token](bool committed) {
                    if (committed)
                        cb(true, "OK");
                    else
                        cb(false, "DB_ERROR: commit failed");
                });

            // 步骤 1：单行原子扣减。affectedRows==0 说明 stock 已为 0（或行不存在），
            // 直接回滚并判为售罄——防超卖的关键，绝不先 SELECT 再 UPDATE。
            tx->execSqlAsync(
                "UPDATE seckill_sku SET stock = stock - 1 "
                "WHERE id = ? AND stock > 0",
                [tx, userId, skuId, cb, token](const drogon::orm::Result &result) {
                    if (result.affectedRows() == 0) {
                        tx->rollback();
                        // 售罄是业务预期内的结果（不是故障），用 warn 而非 error：
                        // 洪峰期这个日志会刷屏，靠级别短路 + drop-newest 兜底。
                        SK_LOG_WARN << "SOLD_OUT skuId=" << skuId;
                        cb(false, "SOLD_OUT");
                        return;
                    }

                    // 步骤 2：扣减成功才落订单。与扣减在同一事务内，保证一致性。
                    //
                    // 用 INSERT ... ON DUPLICATE KEY UPDATE 而不是裸 INSERT：
                    // seckill_order 上有 uk_user_sku(user_id, sku_id) 唯一键，
                    // 同一用户重复秒杀时，裸 INSERT 会抛唯一键冲突异常，
                    // 导致外层 catch 把它当成「DB 错误」并回滚事务。
                    // 问题在于：此时库存行已经被步骤 1 扣掉了，事务一回滚库存会退回，
                    // 但更糟的是这个路径本该是「重复下单」的业务拒绝，不是系统错误。
                    // 这里让冲突时命中 UPDATE 空操作（affectedRows==0 即重复单），
                    // 显式判为 DUPLICATE，并把多扣的库存还回去。
                    tx->execSqlAsync(
                        "INSERT INTO seckill_order (user_id, sku_id, status, "
                        "create_time) VALUES (?, ?, 1, NOW()) "
                        "ON DUPLICATE KEY UPDATE id = id",
                        [tx, userId, skuId, cb, token](const drogon::orm::Result &r2) {
                            // affectedRows 语义（MySQL）：
                            //   1 = 真的插入了一条新订单
                            //   0 = 命中唯一键、走的 UPDATE 但值没变 = 重复下单
                            //   2 = 命中唯一键且 UPDATE 真的改了值（这里 UPDATE id=id 不会触发）
                            if (r2.affectedRows() == 0) {
                                // 重复下单：回滚把步骤 1 扣掉的库存还回去，
                                // 否则用户每重复点一次就白白吃掉一件库存。
                                tx->rollback();
                                SK_LOG_WARN << "DUPLICATE_ORDER userId=" << userId
                                            << " skuId=" << skuId;
                                cb(false, "DUPLICATE_ORDER");
                                return;
                            }

                            // 下单成功：info 级（生产可调高 level 关掉，避免洪峰期刷屏）
                            SK_LOG_INFO << "SECKILL_OK userId=" << userId
                                        << " skuId=" << skuId;

                            // 提交路径：v1.9.10 没有 commit() 成员，不手动提交。
                            // 本 lambda 返回后，tx 在各层 lambda 的拷贝相继析构，
                            // 触发自动 commit → setCommitCallback → cb(true, "OK")。
                            // 回滚路径已各自显式调 cb，commitCallback 不会重复触发。
                        },
                        [tx, userId, skuId, cb, token](const std::exception_ptr &eptr) {
                            tx->rollback();
                            try {
                                std::rethrow_exception(eptr);
                            } catch (const std::exception &ex) {
                                // INSERT 失败是系统级故障（不是业务拒绝），必须留痕
                                SK_LOG_ERROR << "INSERT_ORDER_FAILED userId=" << userId
                                             << " skuId=" << skuId
                                             << " err=" << ex.what();
                                cb(false, std::string("DB_ERROR: ") + ex.what());
                            }
                        },
                        userId, skuId);
                },
                [userId, skuId, tx, cb, token](const std::exception_ptr &eptr) {
                    tx->rollback();
                    try {
                        std::rethrow_exception(eptr);
                    } catch (const std::exception &ex) {
                        // UPDATE 失败同上：库存扣减这一步挂了，是故障不是售罄
                        SK_LOG_ERROR << "DEDUCT_STOCK_FAILED userId=" << userId
                                     << " skuId=" << skuId
                                     << " err=" << ex.what();
                        cb(false, std::string("DB_ERROR: ") + ex.what());
                    }
                },
                skuId);
        });
}

void SeckillService::listSkus(
    std::function<void(bool, const Json::Value &)> &&callback) {
    // 只读查询，不需要事务——直接走 DbClient。start_time/end_time 用 DATE_FORMAT
    // 显式转成字符串，避免 Drogon 把 DATETIME 绑成 tm/Date 类型带来的歧义。
    // 列表接口只暴露"够前端展示"的字段，不返回 create_time 等内部字段。
    //
    // 回调所有权：callback 是右值引用参数，而异步 DB 回调在 listSkus 返回后才触发，
    // 所以必须先按值捕获进 cb（一次 move 到函数作用域），再让成功/失败两个 lambda 各自【拷贝】cb。
    // 绝不可对同一个 callback 做两次 std::move：第一次 move 后 callback 已空，
    // 第二次 move 进错误 lambda 的会是一个空 std::function，DB 一报错就 bad_function_call 直接宕机。
    auto cb = std::move(callback);
    db_->execSqlAsync(
        "SELECT id, name, stock, total, "
        "DATE_FORMAT(start_time, '%Y-%m-%d %H:%i:%s') AS start_time, "
        "DATE_FORMAT(end_time, '%Y-%m-%d %H:%i:%s') AS end_time "
        "FROM seckill_sku ORDER BY id ASC LIMIT 100",
        [cb](const drogon::orm::Result &result) {
            Json::Value arr(Json::arrayValue);
            for (const auto &row : result) {
                Json::Value item;
                item["id"] = row["id"].as<int64_t>();
                item["name"] = row["name"].as<std::string>();
                item["stock"] = row["stock"].as<int>();
                item["total"] = row["total"].as<int>();
                item["startTime"] = row["start_time"].as<std::string>();
                item["endTime"] = row["end_time"].as<std::string>();
                arr.append(item);
            }
            cb(true, arr);
        },
        [cb](const std::exception_ptr &eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception &ex) {
                SK_LOG_ERROR << "LIST_SKU_FAILED err=" << ex.what();
                cb(false, Json::Value());
            }
        });
}

void SeckillService::detailSku(
    int64_t skuId,
    std::function<void(bool, const Json::Value &)> &&callback) {
    // 回调所有权同 listSkus：先 move 进 cb，两个 lambda 各自拷贝 cb，
    // 避免对同一个 callback 二次 move 导致错误路径调用空 std::function 而宕机。
    auto cb = std::move(callback);
    db_->execSqlAsync(
        "SELECT id, name, stock, total, "
        "DATE_FORMAT(start_time, '%Y-%m-%d %H:%i:%s') AS start_time, "
        "DATE_FORMAT(end_time, '%Y-%m-%d %H:%i:%s') AS end_time "
        "FROM seckill_sku WHERE id = ?",
        [cb](const drogon::orm::Result &result) {
            if (result.size() == 0) {
                cb(false, Json::Value());  // 不存在
                return;
            }
            const auto &row = result[0];
            Json::Value item;
            item["id"] = row["id"].as<int64_t>();
            item["name"] = row["name"].as<std::string>();
            item["stock"] = row["stock"].as<int>();
            item["total"] = row["total"].as<int>();
            item["startTime"] = row["start_time"].as<std::string>();
            item["endTime"] = row["end_time"].as<std::string>();
            cb(true, item);
        },
        [cb](const std::exception_ptr &eptr) {
            try {
                std::rethrow_exception(eptr);
            } catch (const std::exception &ex) {
                SK_LOG_ERROR << "DETAIL_SKU_FAILED err=" << ex.what();
                cb(false, Json::Value());
            }
        },
        skuId);
}
