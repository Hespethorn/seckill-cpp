-- 为 Drogon 程序创建专用账号（在 `sudo mysql` 里执行：source sql/init_user.sql）
--
-- 为什么不能直接用 root：Ubuntu/Debian 的 mysql-server 给 root 挂的是 auth_socket
-- 插件，只允许「操作系统 root 身份」登录，程序用 TCP + 密码连会被拒（ERROR 1698）。
-- 这里建一个走 mysql_native_password 的专用账号，专给应用程序用。
--
-- 权限按最小够用原则：只给 seckill 库，不给全局权限，也不给 WITH GRANT OPTION。

-- 关键坑：Drogon 的 MySQL 客户端在 host=127.0.0.1 时仍可能走 Unix socket 连 'localhost'
-- （尤其用 libmariadb 客户端时），而 'seckill'@'127.0.0.1' 账号只匹配 TCP 连接，
-- socket 连接会被当成 'seckill'@'localhost' -> ERROR 1045 Access denied。
-- 因此两个 host 都建上：'127.0.0.1' 覆盖 TCP，'localhost' 覆盖 socket。

CREATE USER IF NOT EXISTS 'seckill'@'127.0.0.1'
  IDENTIFIED WITH mysql_native_password BY 'seckill';

CREATE USER IF NOT EXISTS 'seckill'@'localhost'
  IDENTIFIED WITH mysql_native_password BY 'seckill';

GRANT SELECT, INSERT, UPDATE, DELETE ON seckill.* TO 'seckill'@'127.0.0.1';
GRANT SELECT, INSERT, UPDATE, DELETE ON seckill.* TO 'seckill'@'localhost';

FLUSH PRIVILEGES;

-- 验证：应看到 seckill@127.0.0.1 和 seckill@localhost 两行，plugin 均为 mysql_native_password
SELECT user, host, plugin FROM mysql.user WHERE user = 'seckill';
