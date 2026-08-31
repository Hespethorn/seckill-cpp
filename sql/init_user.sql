-- 为 Drogon 程序创建专用账号（在 `sudo mysql` 里执行：source sql/init_user.sql）
--
-- 为什么不能直接用 root：Ubuntu/Debian 的 mysql-server 给 root 挂的是 auth_socket
-- 插件，只允许「操作系统 root 身份」登录，程序用 TCP + 密码连会被拒（ERROR 1698）。
-- 这里建一个走 mysql_native_password 的专用账号，专给应用程序用。
--
-- 为什么一次建三个 host：Drogon 的 MySQL 客户端连接时，host 到底被当成
-- 'localhost'（Unix socket）还是 '127.0.0.1'（TCP）取决于客户端库与 MySQL 的
-- 反查行为，错误里出现哪个 host 就对应哪个账号。三个都建上，无论 socket / TCP /
-- 反查成 localhost 都能匹配，避免再踩 ERROR 1045。MySQL 的 bind-address 默认仍是
-- 127.0.0.1，所以网络层只接受本机连接，'seckill'@'%' 不会暴露到外网。
--
-- 权限按最小够用原则：只给 seckill 库，不给全局权限，也不给 WITH GRANT OPTION。

-- 先清掉旧账号（含可能残留的错误密码），保证下面建出来的密码一定是对的。
DROP USER IF EXISTS 'seckill'@'127.0.0.1';
DROP USER IF EXISTS 'seckill'@'localhost';
DROP USER IF EXISTS 'seckill'@'%';

CREATE USER 'seckill'@'127.0.0.1'
  IDENTIFIED WITH mysql_native_password BY 'seckill';
CREATE USER 'seckill'@'localhost'
  IDENTIFIED WITH mysql_native_password BY 'seckill';
CREATE USER 'seckill'@'%'
  IDENTIFIED WITH mysql_native_password BY 'seckill';

GRANT SELECT, INSERT, UPDATE, DELETE ON seckill.* TO 'seckill'@'127.0.0.1';
GRANT SELECT, INSERT, UPDATE, DELETE ON seckill.* TO 'seckill'@'localhost';
GRANT SELECT, INSERT, UPDATE, DELETE ON seckill.* TO 'seckill'@'%';

FLUSH PRIVILEGES;

-- 验证：应看到 seckill 的 127.0.0.1 / localhost / % 三行，plugin 均为 mysql_native_password
SELECT user, host, plugin FROM mysql.user WHERE user = 'seckill';
