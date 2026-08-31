# seckill-cpp

用 **C++ / Drogon** 从零搭建的高并发秒杀系统实战项目，是本博客「秒杀系统」系列的配套源码仓库。
重点不在堆功能，而是每一步的**项目思考、实现路径，以及关键功能抉择上的权衡**。仓库按文章节奏分阶段演进：

| 阶段 | 版本 | 手段 | 预期 QPS | 本仓库对应 |
|------|------|------|----------|------------|
| 1 | `v0.1.x` | 直接打数据库（事务 + 原子扣减） | ~50 | **当前** |
| 2 | `v0.2.x` | Redis 预扣减 + 异步落库 | 数千 | 待实现 |
| 3 | `v0.3.x` | MQ 削峰填谷 | 数万 | 待实现 |
| 4 | `v1.0.0` | 微服务 + 限流/治理 | 30000+ | 待实现 |

> 本仓库是作者个人的原创实现记录，不依赖任何外部教程专栏。

## 后续目标（Roadmap）

按「从 50 QPS 一路拉到 30000+」的主线推进，每个阶段都要**先暴露瓶颈 → 再针对性引入手段 → 用压测验证提升**，而不是堆功能。配套博客「秒杀系统」系列逐篇展开实现路径与权衡，完整目录见博客 `source/_posts/Seckill/Cpp/内容规划.md`（Master Plan）。

版本与阶段对应：**`v0.1.x`（阶段一）→ `v0.2.x`（阶段二）→ `v0.3.x`（阶段三）→ `v1.0.0`（阶段四）**。

### 阶段一：地基 + 登录 + 基础秒杀（`v0.1.x`，目标 ~50 QPS 基线）

- [x] 数据库表设计（`seckill_sku` 原子扣减 + `seckill_order` 幂等键）
- [x] Drogon 项目搭建 + 接入 MySQL（内置 ORM DbClient，`getDbClient` 延迟到 handler）
- [x] 事务化原子扣减（`UPDATE ... WHERE stock>0`）+ 幂等下单（`uk_user_sku`）
- [x] 冒烟压测脚本 `scripts/smoke-seckill.sh`（并发抢购 + MySQL 核对不超卖）
- [ ] 登录 / 注册模块（jwt-cpp + Redis 会话，自实现鉴权，无 SaToken）
- [ ] 短信验证码 + 登录安全加固（验证码上限 / 密码错误锁定）
- [ ] JMeter 基线压测：100 用户抢 10 件，量化"行锁串行"这个阶段一瓶颈
- [ ] 应用层锁（`std::mutex` / 自旋 / 原子）优化重复下单

### 阶段二：Redis 缓存层（`v0.2.x`，数千 QPS）

- [ ] 引入 redis-plus-plus，商品列表 / 详情接口加缓存
- [ ] 缓存一致性：Cache-Aside / 延迟双删，讲清取舍
- [ ] 库存预热 + 动态 TTL；防穿透（空值 / 布隆过滤器）、防击穿、防雪崩
- [ ] 本地缓存（LRU）多级缓存
- [ ] JMeter 压测验收：从 ~50 拉到数千 QPS

### 阶段三：MQ 削峰填谷（`v0.3.x`，数万 QPS）

- [ ] Docker 装 RabbitMQ，AMQP-CPP 接入
- [ ] 秒杀异步下单：接口只"写请求 + 返回排队中"，落库交给消费者
- [ ] 消费者可靠性：手动 ACK + 死信队列；生产者可靠性：Confirm + Return
- [ ] 秒杀结果查询 + SSE / WebSocket 实时通知
- [ ] JMeter 压测验收：数千 → 数万 QPS

### 阶段四：Lua 原子预扣 + 防刷限流 + 微服务（`v1.0.0`，30000+ QPS）

- [ ] 库存预热到 Redis，Lua 脚本原子扣减（行锁 → 内存原子操作）
- [ ] Redis Lua 一人一单限制、库存售罄快速失败
- [ ] 防刷 / 限流：固定窗口 / 滑动窗口 / 令牌桶 / 漏桶（自实现中间件）
- [ ] 一次性秒杀令牌：防脚本刷单、按库存控制令牌发放量
- [ ] 微服务拆分 + 服务治理（brpc / etcd / 自写网关与熔断）
- [ ] JMeter 压测验收：数万 → 30000+ QPS

> 每个阶段的验收标准都有一条硬指标：**压测 QPS 提升一个量级，且不超卖、不重复下单**——这是全程不变的底线。

## 当前版本（v0.1.x）做了什么

- `GET  /api/health` 健康检查。
- `POST /api/seckill` 秒杀接口，body：`{"userId":<int64>,"skuId":<int64>}`。
- 防超卖靠 **单行原子 `UPDATE ... WHERE stock>0`**，扣库存与落订单包在**同一个 DB 事务**里，
  要么全成、要么全回滚。

调用示例：

```bash
curl -s -X POST localhost:8080/api/seckill \
  -H 'Content-Type: application/json' \
  -d '{"userId":1,"skuId":1}'
```

- 返回码（HTTP 状态 + JSON）：

| 场景 | HTTP | body |
|------|------|------|
| 下单成功 | 200 | `{"code":0,"msg":"success"}` |
| 库存不足 | 409 | `{"code":1,"msg":"SOLD_OUT"}` |
| 重复下单 | 409 | `{"code":1,"msg":"DUPLICATE_ORDER"}` |
| 参数缺失/非 JSON | 400 | `{"code":400,"msg":"missing or invalid userId/skuId"}` |
| DB 异常 | 500 | `{"code":1,"msg":"DB_ERROR: ..."}` |

> 业务拒绝（409）客户端不该重试；只有 500 才值得重试。

### 阶段一的功能抉择（重点）

- **为什么先读后写会超卖**：应用层 `SELECT stock` 再 `UPDATE` 在并发下读到的是旧值，必然多卖。
  必须下沉到数据库单行原子操作，让 `WHERE stock>0` 在行锁内判定。
- **为什么用事务**：扣库存和落订单是两个写，中间崩溃会留下“库存扣了但没订单”的不一致。
  事务保证原子性——代价是每个请求占用一条 DB 连接并持 sku 行锁到提交，这就是阶段一 QPS 被卡死的根源，
  也是后续引入 Redis / MQ 的动机。
- **幂等**：`seckill_order` 用 `uk_user_sku(user_id, sku_id)` 唯一键，重复下单直接冲突，避免同一用户刷多单。
  这里有个容易踩的坑：**不能裸 `INSERT`**。唯一键冲突会抛异常，若在外层 catch 里一律当 DB 错误回滚，
  库存其实已经被步骤 1 扣掉了——更糟的是这本来是「重复下单」的业务拒绝，不该报成系统错误。
  改用 `INSERT ... ON DUPLICATE KEY UPDATE id = id`，靠 `affectedRows()` 区分：
  `1`=新订单，`0`=命中唯一键即重复下单（此时显式回滚，把多扣的库存还回去）。

## 构建（在 WSL / Ubuntu 22.04+ 上）

沙箱环境没有编译器工具链，真实编译在本地 WSL 完成。

```bash
# 1) 一次性安装依赖 + Drogon（约十几分钟，含 Drogon 源码编译）
bash scripts/setup-wsl.sh

# 2) 启动 MySQL（WSL 不会自启，每次开机都要来一次）
sudo service mysql start

# 3) 建库建表 + 建应用账号
#    注意 Ubuntu 的 mysql-server 给 root 挂的是 auth_socket 插件，
#    输密码登录会被拒（ERROR 1698），必须用 sudo 免密进：
sudo mysql
```
```sql
-- 建库建表
source /mnt/d/GitHub/seckill-cpp/sql/schema.sql;
-- 建应用专用账号（Drogon 用 TCP+密码连，不能用 auth_socket 的 root）
source /mnt/d/GitHub/seckill-cpp/sql/init_user.sql;
```
```bash
# 4) 编译
bash scripts/build-wsl.sh

# 5) 跑起来（默认读 ./config.json，监听 :8080）
./build/src/seckill-cpp
```

### 快速查看数据（每次进库用这一条）

```bash
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill
```

进去之后常用的查看语句：

```sql
-- 库存真相源：剩余/总量/活动时间
SELECT id, name, stock, total, start_time, end_time FROM seckill_sku;

-- 订单：谁买到了、有没有重复
SELECT id, user_id, sku_id, status, create_time FROM seckill_order;

-- 扣减一致性：卖出件数 与 订单数 应相等（无超卖无漏单）
SELECT (SELECT total FROM seckill_sku WHERE id=1) - (SELECT stock FROM seckill_sku WHERE id=1) AS sold,
       (SELECT COUNT(*) FROM seckill_order WHERE sku_id=1) AS orders;

-- 当前连接（看 Drogon 连接池是否建上）
SHOW PROCESSLIST;
```

### 调试与验证脚本

仓库附带两个辅助脚本（WSL 里用 `bash scripts/xxx.sh` 调用，注意本机 `.sh` 可能被关联到 Node.js，别直接 `./xxx.sh`）：

| 脚本 | 用途 |
|------|------|
| `scripts/debug-wsl.sh check` | 一键体检：MySQL 连通、表数据、Drogon 是否带 MySQL 后端、可执行文件实际链接的库 |
| `scripts/debug-wsl.sh asan` | ASan/UBSan 构建并启动，精确定位崩溃行（SIGSEGV 首选） |
| `scripts/debug-wsl.sh gdb` | Debug(-g) 构建进 gdb，看带行号 bt |
| `scripts/smoke-seckill.sh [并发] [库存] [skuId]` | 冒烟压测：N 个用户并发抢购 + MySQL 核对不超卖，默认 `100 10 1` |

### 常见坑（WSL + MySQL + Drogon）

| 报错 | 原因 | 解法 |
|------|------|------|
| `ERROR 2002 ... socket '/var/run/mysqld/mysqld.sock'` | MySQL 服务没启动 | `sudo service mysql start` |
| `ERROR 1698 Access denied for user 'root'@'localhost'` | root 走 `auth_socket`，只能用 OS root 身份登录 | 用 `sudo mysql`（不加 `-p`），或建专用账号 |
| `systemctl` 报 "System has not been booted with systemd" | WSL 默认不开 systemd | 改用 `service mysql start` |
| 程序连不上但 `sudo mysql` 能进 | 应用走 TCP 密码认证，root 是 socket 认证 | 执行 `sql/init_user.sql` 建 `seckill` 账号 |
| `service mysql start` 卡住/失败 | `/var/run/mysqld` 目录缺失或权限错 | `sudo mkdir -p /var/run/mysqld && sudo chown mysql:mysql /var/run/mysqld` |
| 服务正常启动，首个请求 SIGSEGV，崩在 `SeckillService::doSeckill` 的 `db_->...` | **`getDbClient("default")` 在 `app.run()` 之前调用必然返回空 shared_ptr**（Drogon 1.9.10 的 db 客户端要 run() 才创建，见 `main.cc` 注释） | 把 `getDbClient` + 组装 Service 的代码延迟到 handler 里（static 延迟初始化），不要在 `main()` 里提前取 |

> WSL 不会自启服务，想省事可在 `~/.bashrc` 加一行：
> `sudo service mysql status >/dev/null || sudo service mysql start`

### Drogon 版本取舍说明

本仓库按 **Drogon 1.9.x** 的回调式 orm API 编写：`Transaction::commit()` / `rollback()` 为 `void`，
通过 `>> [](const ResultSet&, const std::exception_ptr&)` 组合回调处理结果与异常。
若你使用开启了 C++20 协程的新版 Drogon，`commit()` 会返回 `drogon::Task<bool>`，需 `co_await` 才能生效；
届时把 `SeckillService.cc` 里的提交改写为协程版本即可（会在对应阶段文章里展开）。

## 目录结构

```
seckill-cpp/
├── CMakeLists.txt            # find_package(Drogon)
├── config.json               # 监听端口 + MySQL 客户端("default")
├── src/
│   ├── main.cc               # Drogon 启动 + 控制器/服务装配（getDbClient 延迟到 handler）
│   ├── controllers/          # 仅做协议转换（HTTP <-> 业务参数）
│   └── service/              # SeckillService：事务化原子扣减（核心逻辑）
├── sql/
│   ├── schema.sql            # 阶段一表结构
│   └── init_user.sql         # 应用专用账号（避开 root 的 auth_socket 限制）
└── scripts/
    ├── setup-wsl.sh          # 一次性装依赖 + Drogon（含 MySQL 后端）
    ├── build-wsl.sh          # CMake 配置 + 编译
    ├── debug-wsl.sh          # 排错三件套：check（体检）/ asan（定位）/ gdb（带行号 bt）
    └── smoke-seckill.sh      # 冒烟压测：并发抢购 + MySQL 验证不超卖
```
