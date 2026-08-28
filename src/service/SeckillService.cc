#include "SeckillService.h"
#include <drogon/orm/Exception.h>

SeckillService::SeckillService(drogon::orm::DbClientPtr client)
    : db_(std::move(client)) {}

void SeckillService::doSeckill(
    int64_t userId,
    int64_t skuId,
    std::function<void(bool, const std::string &)> &&callback) {
    // 开一个 DB 事务。Drogon 的 newTransactionAsync 把事务对象通过回调交给我们，
    // 事务内用 *tx << sql << param >> callback 串联语句。
    db_->newTransactionAsync(
        [userId, skuId, cb = std::move(callback)](drogon::orm::TransactionPtr &&tx) mutable {
            // 步骤 1：单行原子扣减。affectedRows==0 说明 stock 已经为 0（或该行不存在），
            // 直接回滚并判为售罄——这是防超卖的关键，绝不先 SELECT 再 UPDATE。
            *tx << "UPDATE seckill_sku SET stock = stock - 1 "
                   "WHERE id = ? AND stock > 0"
                << skuId
                >> [tx, userId, skuId, cb = std::move(cb)](
                       const drogon::orm::ResultSet &result,
                       const std::exception_ptr &eptr) mutable {
                if (eptr) {
                    tx->rollback();
                    try {
                        std::rethrow_exception(eptr);
                    } catch (const std::exception &ex) {
                        cb(false, std::string("DB_ERROR: ") + ex.what());
                    }
                    return;
                }

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
                // 这里让冲突时命中 UPDATE 空操作（affectedRows==0 或 2 需区分），
                // 显式判为 DUPLICATE，不再回滚导致库存异常。
                *tx << "INSERT INTO seckill_order (user_id, sku_id, status, create_time) "
                       "VALUES (?, ?, 1, NOW()) "
                       "ON DUPLICATE KEY UPDATE id = id"
                    << userId << skuId
                    >> [tx, cb = std::move(cb)](
                           const drogon::orm::ResultSet &r2,
                           const std::exception_ptr &eptr) mutable {
                        if (eptr) {
                            tx->rollback();
                            try {
                                std::rethrow_exception(eptr);
                            } catch (const std::exception &ex) {
                                cb(false, std::string("DB_ERROR: ") + ex.what());
                            }
                            return;
                        }

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

                        // 提交事务：此时库存已扣、订单已落，对外可见。
                        tx->commit();
                        cb(true, "OK");
                    };
            };
        });
}
