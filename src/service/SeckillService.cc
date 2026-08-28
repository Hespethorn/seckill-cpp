#include "SeckillService.h"
#include <drogon/orm/Result.h>
#include <drogon/orm/Exception.h>

SeckillService::SeckillService(drogon::orm::DbClientPtr client)
    : db_(std::move(client)) {}

void SeckillService::doSeckill(
    int64_t userId,
    int64_t skuId,
    std::function<void(bool, const std::string &)> &&callback) {
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
        [userId, skuId, cb = std::move(callback)](
            const std::shared_ptr<drogon::orm::Transaction> &tx) {
            if (!tx) {
                // 文档说明：超时情况下回调会拿到空的 shared_ptr。
                cb(false, "DB_ERROR: no transaction (timeout)");
                return;
            }

            // 提交结果回调：事务析构自动 commit 之后触发（仅成功才调）。
            tx->setCommitCallback(
                [cb](bool committed) {
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
                [tx, userId, skuId, cb](const drogon::orm::Result &result) {
                    if (result.affectedRows() == 0) {
                        tx->rollback();
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
                        [tx, cb](const drogon::orm::Result &r2) {
                            // affectedRows 语义（MySQL）：
                            //   1 = 真的插入了一条新订单
                            //   0 = 命中唯一键、走的 UPDATE 但值没变 = 重复下单
                            //   2 = 命中唯一键且 UPDATE 真的改了值（这里 UPDATE id=id 不会触发）
                            if (r2.affectedRows() == 0) {
                                // 重复下单：回滚把步骤 1 扣掉的库存还回去，
                                // 否则用户每重复点一次就白白吃掉一件库存。
                                tx->rollback();
                                cb(false, "DUPLICATE_ORDER");
                                return;
                            }

                            // 提交路径：v1.9.10 没有 commit() 成员，不手动提交。
                            // 本 lambda 返回后，tx 在各层 lambda 的拷贝相继析构，
                            // 触发自动 commit → setCommitCallback → cb(true, "OK")。
                            // 回滚路径已各自显式调 cb，commitCallback 不会重复触发。
                        },
                        [tx, cb](const std::exception_ptr &eptr) {
                            tx->rollback();
                            try {
                                std::rethrow_exception(eptr);
                            } catch (const std::exception &ex) {
                                cb(false, std::string("DB_ERROR: ") + ex.what());
                            }
                        },
                        userId, skuId);
                },
                [tx, cb](const std::exception_ptr &eptr) {
                    tx->rollback();
                    try {
                        std::rethrow_exception(eptr);
                    } catch (const std::exception &ex) {
                        cb(false, std::string("DB_ERROR: ") + ex.what());
                    }
                },
                skuId);
        });
}
