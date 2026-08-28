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
                *tx << "INSERT INTO seckill_order (user_id, sku_id, status, create_time) "
                       "VALUES (?, ?, 1, NOW())"
                    << userId << skuId
                    >> [tx, cb = std::move(cb)](
                           const drogon::orm::ResultSet &,
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
                        // 提交事务：此时库存已扣、订单已落，对外可见。
                        tx->commit();
                        cb(true, "OK");
                    };
            };
        });
}
