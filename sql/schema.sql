-- seckill-cpp 阶段一表结构
-- 设计取舍：库存直接落在 seckill_sku 表上，靠 UPDATE ... WHERE stock>0 做原子扣减；
-- 订单表只记录“谁买到了哪件商品”，不冗余库存，避免双写不一致。
-- 阶段二会引入 Redis 预扣减，这里先保留最朴素的单表结构。

CREATE DATABASE IF NOT EXISTS seckill
  DEFAULT CHARACTER SET utf8mb4
  DEFAULT COLLATE utf8mb4_general_ci;

USE seckill;

-- 秒杀商品：stock 是扣减的唯一真相源
CREATE TABLE IF NOT EXISTS seckill_sku (
  id          BIGINT       NOT NULL AUTO_INCREMENT,
  name        VARCHAR(128) NOT NULL DEFAULT '',
  stock       INT          NOT NULL DEFAULT 0,
  total       INT          NOT NULL DEFAULT 0,
  start_time  DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  end_time    DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  create_time DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  KEY idx_stock (stock)          -- 仅用于排查，扣减走主键行锁，不靠这个索引
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 秒杀订单：同一用户对同一商品用 uk_user_sku 做幂等，重复下单直接唯一键冲突
CREATE TABLE IF NOT EXISTS seckill_order (
  id          BIGINT       NOT NULL AUTO_INCREMENT,
  user_id     BIGINT       NOT NULL,
  sku_id      BIGINT       NOT NULL,
  status      TINYINT      NOT NULL DEFAULT 1 COMMENT '1=已下单',
  create_time DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (id),
  UNIQUE KEY uk_user_sku (user_id, sku_id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 阶段一演示数据：100 件库存的商品
INSERT INTO seckill_sku (id, name, stock, total)
VALUES (1, 'stage1-demo-sku', 100, 100)
ON DUPLICATE KEY UPDATE name = VALUES(name), total = VALUES(total);
-- 注意：演示数据不覆盖 stock，避免重复导入把库存刷回 100。
