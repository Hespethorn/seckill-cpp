# seckill-cpp

用 **C++ / Drogon** 从零实现的高并发秒杀系统，博客「秒杀系统」系列的配套源码仓库；重点不在堆功能，而在**每一步的实现路径与关键取舍**——代码按文章节奏分阶段演进。

先看实测结果（全部可由仓库内脚本复现）：

| 指标 | 实测 | 复现方式 |
| --- | --- | --- |
| 数据库直打基线（阶段一） | **QPS≈440 / p95≈359ms / 0 超卖** | `scripts/jmeter-baseline.sh` |
| Redis 读缓存（阶段二） | **list 10557 QPS / detail 8024 QPS**（20 万商品量级） | `scripts/read-bench.sh` |
| 缓存收益 | 命中率 **83.8%**，list **×1.40** / detail **×1.44** | `scripts/read-bench.sh` |
| 防穿透 / 多级缓存自检 | 自实现布隆过滤器 + 本地 LRU（L1） | `scripts/verify-57-bloom.sh` |
| 关键取舍 | 自写 JWT 而非引 `jwt-cpp`、自实现布隆/LRU 而非引库 | [`docs/PLAN.md` §4](docs/PLAN.md)（ADR） |

> 路线图、技术决策、实测基线与开发排障见 **[`docs/PLAN.md`](docs/PLAN.md)**（项目计划书 / 进度看板）。

## 特性

**阶段一（`v0.1.x`）：打数据库**

- **Drogon + MySQL 直连**：原子 `UPDATE ... WHERE stock>0` 防超卖，事务化幂等下单（`uk_user_sku` 唯一键兜底）。
- **自实现登录鉴权**：PBKDF2 加盐哈希、自写 JWT（HS256）、Drogon 内置 Redis 异步客户端可吊销会话——零引入 `redis-plus-plus` / `jwt-cpp` 等同步或需 FetchContent 的依赖。
- **短信验证码（自签发 / 日志模式）**：6 位码 CSPRNG 生成 + Redis 存储 + Lua 原子校验、发送限流、登录失败锁定，不接任何短信网关。
- **IP 预防（双保险）**：① **同 IP 注册频控**（`service/RegisterGuard.*`）——固定窗口内限制**成功注册数**（默认 5 次 / 小时），Lua 原子 `INCR + 首次 EXPIRE`，Redis 挂时 fail-open 放行，超限返回 HTTP 429；堵住自签发验证码模式下"同 IP 批量注册刷号"。② **反代取真实客户端 IP**——走 Drogon 官方 `RealIpResolver` 插件：先校验 TCP 对端是否命中 `trust_ips`（可信代理），命中才解析 `X-Forwarded-For`，且**从右往左**跳过代理链取首个不可信 IP，不命中则直接采用 TCP 对端地址。**坑点提醒**：不要自己取 `X-Forwarded-For` 首段——首段是调用方可任意伪造的值，直连场景一行 header 就能换掉频控 key，反代场景 nginx 的 `$proxy_add_x_forwarded_for` 又是"追加"而非覆盖，伪造值反而被拼在最左边。本项目初版即踩此坑，后修正为官方插件方案。`trust_ips` 的语义是**「我信任谁的 X-Forwarded-For」**——本项目当前**不挂反向代理**，故配置为 `[]`（XFF 一律忽略，伪造没有入口）；将来接入 nginx 时再把它的地址填进去（Docker 网络填网段）。**注意别填宽**：把客户端也能连到的地址写进 `trust_ips`，等于让那个来源来决定频控 key，伪造当场生效。**上游联动**：使用过程中还发现该插件只支持 IPv4——`trust_ips` 里填 IPv6 会拿到误导性的报错，而 `X-Forwarded-For` 里的 IPv6 条目会被**静默丢弃**、回退成代理地址（反代后 IPv6 客户端会收缩成同一个 key），已向上游提 issue [`drogonframework/drogon#2596`](https://github.com/drogonframework/drogon/issues/2596)。
- **应用层在途闸门**（`service/InflightGuard.h`，mutex / 自旋 / 原子三后端）挡掉并发窗口内的重复下单，DB 压力降约 75%。

**阶段二（`v0.2.x`）：加缓存**

- **读缓存 Cache-Aside**（`service/SkuCache.*` + `service/CacheKeys.h`）：列表 / 详情两接口接 Redis，Key 规范统一收敛到 `seckill:sku:v1:*`；命中率统计走 `/api/cache/stats`。实测 **list ×1.40 / detail ×1.44**、命中率 83.8%、DB 读负载降约 77%。
- **空值哨兵防穿透**（5.6，随 5.3 落地）：查不到的 id 写 `__nil__` 短 TTL，挡住"打不存在的 key 反弹 DB"。
- **延迟双删**（5.4，默认关）：`double_delete_ms` 可配，专用后台线程到点二次 DEL（绝不 sleep 在 IO 线程）。
- **缓存预热**（5.5）：`POST /api/cache/warm` 一次 SQL 重建列表 + 批量预热详情。
- **布隆过滤器**（5.7，自实现，默认关）：detail 前置过滤不存在的 id，未构建时 fail-open 放行。
- **本地 LRU 多级缓存**（5.8，自实现，默认关）：L1 本地 + L2 Redis + L3 MySQL，L1 命中省一次网络往返。

**实测基线**：官方 JMeter 压测 **QPS≈440（干净态）/ p95≈359ms / 0 超卖**；缓存后读接口 **list 10557 QPS / detail 8024 QPS**（20 万商品量级）。

## 快速开始（WSL / Ubuntu 22.04+）

```bash
# 1) 安装依赖 + Drogon（一次性，约十几分钟）
bash scripts/setup-wsl.sh

# 2) 启动服务（WSL 不自启，每次开机都要来一次；秒杀链路只需 MySQL）
sudo service mysql start && sudo service redis-server start

# 3) 建库建表 + 应用账号
sudo mysql
source /mnt/d/GitHub/seckill-cpp/sql/schema.sql;
source /mnt/d/GitHub/seckill-cpp/sql/user_schema.sql;   # 用不到登录可跳过
source /mnt/d/GitHub/seckill-cpp/sql/init_user.sql;

# 4) 编译
bash scripts/build-wsl.sh

# 5) 跑起来（默认读 ./config.json，监听 :8080）
./build/src/seckill-cpp
```

完整构建说明、调试脚本、常见坑（WSL + MySQL + Drogon 排障）→ **[`docs/PLAN.md` §6](docs/PLAN.md)**。

## 接口一览

| 方法 | 路径 | 说明 | 依赖 |
| --- | --- | --- | --- |
| GET | `/api/health` | 健康检查 | — |
| POST | `/api/seckill` | 秒杀下单（`userId` 走 body，暂不校验 token） | MySQL |
| GET | `/api/seckill/list` | 商品列表（Redis 缓存，TTL 30s + 抖动） | MySQL + Redis |
| GET | `/api/seckill/{skuId}` | 商品详情（Redis 缓存，含空值哨兵防穿透） | MySQL + Redis |
| GET | `/api/lock/stats` | 4.8 在途闸门统计 | — |
| GET | `/api/cache/stats` | 5.1 缓存命中率 / 回源次数 / 实际 key 与 TTL | Redis |
| POST | `/api/sms/send` | 发送短信验证码 | Redis |
| POST | `/api/user/register` | 注册（默认需验证码） | MySQL + Redis |
| POST | `/api/user/login` | 登录，返回 Bearer Token | MySQL + Redis |
| POST | `/api/user/logout` | 登出，吊销会话 | Redis |

> Redis 没起时，登录 / 短信相关路由返回 `503`，**秒杀主链路不受影响**（缓存自动降级为直连 MySQL）。
> 完整请求/响应示例、错误码、配置项、Redis key 约定 → **[`docs/PLAN.md` §7](docs/PLAN.md)**。
> 缓存层专项规格（Key 规范 / TTL 与抖动 / 失效范围取舍 / 压测方法）→ **[`docs/CACHE-DESIGN.md`](docs/CACHE-DESIGN.md)**。

## 项目结构

```
seckill-cpp/
├── CMakeLists.txt            # find_package(Drogon / spdlog / OpenSSL)
├── config.json               # 监听端口 + MySQL + Redis + jwt/sms/lock 配置
├── src/
│   ├── main.cc               # Drogon 启动 + 路由装配（getDbClient/RedisClient 延迟到 handler）
│   ├── controllers/          # 仅做协议转换（HTTP <-> 业务参数）
│   │   ├── HealthController.*    # /api/health
│   │   ├── SeckillController.*   # /api/seckill
│   │   ├── UserController.*      # /api/user/{register,login,logout}
│   │   └── SmsController.*       # /api/sms/send
│   ├── service/
│   │   ├── SeckillService.*      # 事务化原子扣减（阶段一核心）+ 5.2/5.3 读缓存路径
│   │   ├── CacheKeys.h           # 5.1 缓存 Key 唯一构造入口（seckill:sku:v1:...）
│   │   ├── SkuCache.*            # 5.1 商品缓存：异步 GET/SETEX/DEL + 空值哨兵 + 命中统计（fail-open）
│   │   │                         #   └ 内含 5.4 DelayDeleter（延迟双删线程）/ 5.8 本地 LRU L1
│   │   ├── BloomFilter.h         # 5.7 自实现布隆过滤器（位数组 + 双哈希，防穿透前置过滤）
│   │   ├── LocalLruCache.h       # 5.8 自实现线程安全 LRU（L1 本地缓存）
│   │   ├── InflightGuard.h       # 4.8 应用层在途闸门（mutex / 自旋 / 原子三后端）
│   │   ├── password.*            # PBKDF2-HMAC-SHA256 + CSPRNG salt + 常量时间比较
│   │   ├── Jwt.*                 # 自实现 HS256 签发/校验
│   │   ├── SessionStore.*        # Redis 可吊销会话 sess:{jti}
│   │   ├── LoginGuard.*          # 3.6 错误次数 + 账号锁定（Lua 原子）
│   │   ├── RegisterGuard.*       # 同 IP 注册频控（固定窗口 + Lua 原子，fail-open，429；Redis key reg:ip:<ip>）
│   │   ├── SmsSender.*           # 验证码送达：自签发 / 日志模式（不接短信网关）
│   │   ├── SmsService.*          # 验证码生成/限流/原子校验
│   │   └── UserService.*         # 注册 / 登录 / 登出 / 鉴权
│   └── logging/              # 异步日志（环形缓冲 + spdlog sink + SK_LOG_* 宏）
├── sql/                      # schema.sql / user_schema.sql / init_user.sql / seed_sku.sql
├── scripts/                  # setup-wsl / build-wsl / debug-wsl / smoke-seckill / verify-auth
│                             #   / jmeter-baseline / read-bench / local-bench / lock-bench
│                             #   / cache-warm / verify-54-double-delete / verify-57-bloom
├── jmeter/                   # 压测脚本与产物（out/ 为运行副产物，已 gitignore）
└── docs/                     # PLAN.md（计划书/ADR/基线）/ CACHE-DESIGN.md（缓存专项规格）
```

## 计划与进度

架构按「50 QPS → 30000+」4 阶段演进，每阶段验收硬指标：**QPS 提升一个量级 + 不超卖 + 不重复下单**。

**当前状态：阶段二收官（`v0.2.x`）**——读写两条链路的优化均已完成并实测验证：

| 阶段 | 版本 | 手段 | 状态 |
| --- | --- | --- | --- |
| 一 | `v0.1.x` | 直打数据库（事务 + 原子扣减 + 应用层闸门） | ✅ 完成，干净态基线 QPS≈440 |
| 二 | `v0.2.x` | Redis 读缓存 + 穿透防护 + 多级缓存 | ✅ 完成，list ×1.40 / detail ×1.44 |
| 三 | `v0.3.x` | MQ 削峰填谷（AMQP-CPP） | 归档（不再排期） |
| 四 | `v1.0.0` | 微服务 + Lua 预扣 + 限流治理 | 归档（不再排期） |

> 阶段三 / 四的完整规划保留在博客 Master Plan 与 `docs/PLAN.md` §2–§3，作为"未来可续"的路线记录；本项目当前以阶段二收官。

- 阶段演进路线图、各博客章节 ↔ 代码进度对照 → **[`docs/PLAN.md` §2–§3](docs/PLAN.md)**
- 技术决策记录（框架选型 / 选型变更 / IO 线程阻塞点 / 应用层锁边界 / 缓存策略） → **[`docs/PLAN.md` §4](docs/PLAN.md)**
- 实测基线与方法对比数据 → **[`docs/PLAN.md` §5](docs/PLAN.md)**
- 缓存层专项规格（Key 规范 / TTL 抖动 / 失效取舍 / 压测方法） → **[`docs/CACHE-DESIGN.md`](docs/CACHE-DESIGN.md)**

## 关于本项目

- **构建方式（诚实说明）**：本仓库是**开源实战项目，由 AI 辅助、自驱动构建**——从架构取舍、代码落地到压测验证，大量工作流由 AI agent 完成，并与博客系列章节同步演进。代码为作者个人原创实现，不依赖任何外部教程专栏。
- **数据可复现**：本文档中的 QPS / 命中率 / 延迟数字均取自仓库内脚本（`scripts/jmeter-baseline.sh`、`scripts/read-bench.sh`、`scripts/verify-57-bloom.sh` 等），非估算值。
- **决策可追溯**：关键选型与变更记录于 `docs/PLAN.md` §4（ADR），实测口径与对比数据见 §5；开发过程中的排障经验（WSL + MySQL + Drogon）见 §6。

## License

[MIT](LICENSE)
