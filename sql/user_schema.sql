-- seckill-cpp 登录模块表结构（第三章 3.1+）
--
-- 设计取舍：
--   1. 密码永远不落明文。password_hash 是 PBKDF2-HMAC-SHA256 的派生结果（32 字节 → hex 64 字符），
--      salt 是每账号独立的 16 字节随机数（hex 32 字符）。固定长度所以用 CHAR，不用 VARCHAR。
--   2. uk_phone 唯一索引是"防并发插重"的最后一道防线：即使应用层 SELECT 查重存在竞态
--      （两个同手机号请求同时查到"未注册"），第二个 INSERT 也会撞唯一索引报 ER_DUP_ENTRY。
--      应用层查重是快路径，数据库唯一约束是兜底，两层都不放过。
--   3. 不存"明文密码 + 一次性比较"，验证时拿库里的 salt 重新派生再比对。

USE seckill;

CREATE TABLE IF NOT EXISTS `user` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `phone`         VARCHAR(20)  NOT NULL                COMMENT '登录手机号，唯一',
  `password_hash` CHAR(64)     NOT NULL                COMMENT 'PBKDF2-HMAC-SHA256 派生，hex 编码',
  `salt`          CHAR(32)     NOT NULL                COMMENT '16 字节随机 salt，hex 编码',
  `nickname`      VARCHAR(64)  NOT NULL DEFAULT ''     COMMENT '昵称，可空',
  `status`        TINYINT      NOT NULL DEFAULT 1      COMMENT '1=正常 0=禁用',
  `create_time`   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_phone` (`phone`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- 登录审计留痕：谁在什么时候从哪个 IP 登录成功/失败。
-- 3.6 的"密码错误次数限制 + 账号锁定"主要靠 Redis 计数（快），
-- 这张表是慢审计——用于事后追溯撞库/盗号，不参与实时判定，所以没有唯一索引压力。
CREATE TABLE IF NOT EXISTS `login_log` (
  `id`          BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `phone`       VARCHAR(20)  NOT NULL                COMMENT '尝试登录的手机号（可能不存在于 user 表）',
  `user_id`     BIGINT UNSIGNED NOT NULL DEFAULT 0   COMMENT '登录成功时的 user.id，失败为 0',
  `result`      TINYINT      NOT NULL                COMMENT '1=成功 0=密码错 2=账号锁定 3=用户不存在',
  `ip`          VARCHAR(64)  NOT NULL DEFAULT '',
  `create_time` DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`id`),
  KEY `idx_phone_time` (`phone`, `create_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
