# seckill-cpp

用 **C++ / Drogon** 从零搭建的高并发秒杀系统实战项目，是博客「秒杀系统」系列的配套源码仓库。重点不在堆功能，而在**每一步的项目思考、实现路径，与关键功能抉择上的权衡**——代码按文章节奏分阶段演进。

> 本仓库是**开源实战项目，由 AI 辅助、自驱动构建**：从架构取舍、代码落地到压测验证，大量工作流由 AI agent 完成，并与博客系列章节同步演进。代码为作者个人原创实现，不依赖任何外部教程专栏。
> 路线图、技术决策、实测基线与开发排障见 **[`docs/PLAN.md`](docs/PLAN.md)**（项目计划书 / 进度看板）。

## 特性

- **阶段一已落地**（`v0.1.x`）：Drogon + MySQL 直连，原子 `UPDATE ... WHERE stock>0` 防超卖，事务化幂等下单（`uk_user_sku` 唯一键兜底）。
- **自实现登录鉴权**：PBKDF2 加盐哈希、自写 JWT（HS256）、Drogon 内置 Redis 异步客户端可吊销会话——零引入 `redis-plus-plus` / `jwt-cpp` 等同步或需 FetchContent 的依赖。
- **短信验证码（自签发 / 日志模式）**：6 位码 CSPRNG 生成 + Redis 存储 + Lua 原子校验、发送限流、登录失败锁定，不接任何短信网关。
- **应用层在途闸门**（`service/InflightGuard.h`，mutex / 自旋 / 原子三后端）挡掉并发窗口内的重复下单，DB 压力降约 75%。
- **实测基线**：官方 JMeter 压测 **QPS≈371 / p95≈359ms / 0 超卖**，curl harness 交叉验证 ≈341。

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
| GET | `/api/lock/stats` | 4.8 在途闸门统计 | — |
| POST | `/api/sms/send` | 发送短信验证码 | Redis |
| POST | `/api/user/register` | 注册（默认需验证码） | MySQL + Redis |
| POST | `/api/user/login` | 登录，返回 Bearer Token | MySQL + Redis |
| POST | `/api/user/logout` | 登出，吊销会话 | Redis |

> Redis 没起时，登录 / 短信相关路由返回 `503`，**秒杀主链路不受影响**。
> 完整请求/响应示例、错误码、配置项、Redis key 约定 → **[`docs/PLAN.md` §7](docs/PLAN.md)**。

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
│   │   ├── SeckillService.*      # 事务化原子扣减（阶段一核心）
│   │   ├── InflightGuard.h       # 4.8 应用层在途闸门（mutex / 自旋 / 原子三后端）
│   │   ├── password.*            # PBKDF2-HMAC-SHA256 + CSPRNG salt + 常量时间比较
│   │   ├── Jwt.*                 # 自实现 HS256 签发/校验
│   │   ├── SessionStore.*        # Redis 可吊销会话 sess:{jti}
│   │   ├── LoginGuard.*          # 3.6 错误次数 + 账号锁定（Lua 原子）
│   │   ├── SmsSender.*           # 验证码送达：自签发 / 日志模式（不接短信网关）
│   │   ├── SmsService.*          # 验证码生成/限流/原子校验
│   │   └── UserService.*         # 注册 / 登录 / 登出 / 鉴权
│   └── logging/              # 异步日志（环形缓冲 + spdlog sink + SK_LOG_* 宏）
├── sql/                      # schema.sql / user_schema.sql / init_user.sql
├── scripts/                  # setup-wsl / build-wsl / debug-wsl / smoke-seckill / jmeter-baseline / verify-auth / lock-bench
└── jmeter/                   # 压测脚本与产物（out/ 为运行副产物，已 gitignore）
```

## 计划与进度

架构按「50 QPS → 30000+」4 阶段演进，每阶段验收硬指标：**QPS 提升一个量级 + 不超卖 + 不重复下单**。当前处于**阶段一收尾**（`v0.1.x`，基线 QPS≈371）。

- 阶段演进路线图、各博客章节 ↔ 代码进度对照 → **[`docs/PLAN.md` §2–§3](docs/PLAN.md)**
- 技术决策记录（选型变更 / IO 线程阻塞点 / 应用层锁边界 / 前端延后） → **[`docs/PLAN.md` §4](docs/PLAN.md)**
- 实测基线与方法对比数据 → **[`docs/PLAN.md` §5](docs/PLAN.md)**
