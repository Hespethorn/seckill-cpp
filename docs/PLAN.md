# seckill-cpp 计划书 / 工程进度看板

> 本文件是项目**内部计划与进度跟踪**，承载路线图、技术决策、实测基线与开发排障。
> `README.md` 只做项目门面（简介 / 快速开始 / 接口缩略 / 目录），不重复此处内容。
> 约定：**项目先行，博客随后，不持续同步**——代码 / 测试先落地验证，博客章节随后补写。
>
> 专项规格文档：[`docs/CACHE-DESIGN.md`](CACHE-DESIGN.md)（第五章缓存层：Key 规范 / TTL / 失效策略 / 压测方法）。

---

## 1. 项目目标与验收硬指标

用 **C++ / Drogon** 从零搭建的高并发秒杀系统实战项目，是博客「秒杀系统」系列的配套源码仓库。重点不在堆功能，而在**每一步的项目思考、实现路径，与关键功能抉择上的权衡**。

架构按「50 QPS → 30000+」4 阶段演进。每阶段验收**硬指标**（全程不变底线）：

- 压测 **QPS 提升一个量级**；
- **不超卖**（MySQL `sold == orders`）；
- **不重复下单**（同 user+sku 只成一单）。

> 本仓库是作者个人原创实现记录，不依赖任何外部教程专栏。

---

## 2. 阶段演进路线图

| 阶段 | 版本 | 手段 | 预期 QPS | 本仓库对应 | 状态 |
| --- | --- | --- | --- | --- | --- |
| 1 | `v0.1.x` | 直接打数据库（事务 + 原子扣减） | ~50（实测 ≈371 基线） | 二/三/四章 | 收尾中 |
| 2 | `v0.2.x` | Redis 缓存 + 预扣减 + 异步落库 | 数千 | 五章 | 进行中（5.1~5.3 代码+压测完成：20万条 JMeter 实测 on/off 提升 ×1.40(list)/×1.44(detail)，命中率 83.8%，DB 读负载降约 77%；5.1~5.3 博客已写） |
| 3 | `v0.3.x` | MQ 削峰填谷 | 数万 | 六章 | 待开始 |
| 4 | `v1.0.0` | 微服务 + 限流 / 治理 | 30000+ | 七/八章 | 待开始 |

进度以「博客章节」为最小单元跟踪，章节号与博客 Master Plan（`内容规划.md`）一一对应；代码阶段是章节的属性（一个阶段可跨多个章节）。

| 代码阶段 | 版本 | 预期 QPS | 覆盖博客章节 | 状态 |
| --- | --- | --- | --- | --- |
| 阶段一 | `v0.1.x` | ≈371（实测基线，官方 JMeter） | 二（表设计/搭建）、三（登录模块）、四（基础秒杀） | 收尾中 |
| 阶段二 | `v0.2.x` | 数千 | 五（缓存层 Redis） | 进行中（5.1~5.3 代码+压测完成：20万条 JMeter 实测 on/off 提升 ×1.40(list)/×1.44(detail)，命中率 83.8%，DB 读负载降约 77%；5.1~5.3 博客已写） |
| 阶段三 | `v0.3.x` | 数万 | 六（MQ 削峰填谷） | 待开始 |
| 阶段四 | `v1.0.0` | 30000+ | 七（Lua 原子预扣）、八（防刷限流） | 待开始 |

### 阶段一收尾清单（代码侧全部落地，博客按约定随后补）

1. 登录 / 注册模块 → 第三章 3.1–3.2 — ✅ 代码完成（`service/password.*`、`Jwt.*`、`SessionStore.*`、`UserService.*`、`UserController.*`）
2. 短信验证码 + 登录安全加固 → 第三章 3.3–3.7 — ✅ 代码完成（`SmsSender.*` 直连真实腾讯云 API、`SmsService.*`、`LoginGuard.*`）
3. JMeter 基线压测 → 第四章 4.4–4.7 — ✅ 已实测（官方 JMeter **QPS≈371 / p95≈359ms / 0 超卖**，curl harness 交叉验证 ≈341）；博客 4.4/4.5/4.7 待产出
4. 应用层锁（mutex / 自旋 / 原子）优化重复下单 → 第四章 4.8 — ✅ 代码完成（`service/InflightGuard.h` + `scripts/lock-bench.sh`），**对比压测已实测**（见 §5.2：dup 场景应用层挡掉≈1500，DB 压力降 ~75%）

---

## 3. 博客章节 ↔ 代码阶段对照（进度跟踪）

### 第二章 表设计与项目搭建（阶段一 `v0.1.x`，~50 QPS）— 已完成

- [x] 2.1 数据库表设计（`seckill_sku` 原子扣减 + `seckill_order` 幂等键）— `seckcpp0201`
- [x] 2.2 后端接口梳理与设计（REST 路由规划，GET/POST 取舍）— `seckcpp0202`
- [x] 2.3 Drogon 多模块项目搭建（CMake + 子项目）— `seckcpp0203`
- [x] 2.4 接入 MySQL（Drogon 内置 ORM DbClient，`getDbClient` 延迟到 run() 后）— `seckcpp0204`
- [x] 2.5 完善项目基础设施（配置 / 日志 / 全局异常）— `seckcpp0205`
- [x] 2.6 整合高性能异步日志（spdlog + 环形缓冲，`SK_LOG_*` 流式宏）— `seckcpp0206`

### 第三章 登录模块开发（阶段一 `v0.1.x`，自实现鉴权，无 SaToken）— 代码已完成，博客待补

- [x] 3.1 用户注册接口（PBKDF2 加盐哈希 + `uk_phone` 双层查重）— `seckcpp0301`
- [x] 3.2 用户登录接口（签发 JWT + 写 Redis 可吊销会话）— `seckcpp0302`
- [x] 3.3 短信验证码接口（生成 / 发送 / Redis 存储 / 限流）— `seckcpp0303`
- [x] 3.4 注册验证码校验 & Token 持久化到 Redis — `seckcpp0304`
- [x] 3.5 验证码安全加固：Redis Lua 原子校验 + 每日发送上限 + 试错上限 — `seckcpp0305`
- [x] 3.6 登录安全加固：密码错误次数限制 + 账号临时锁定 — `seckcpp0306`
- [x] 3.7 退出登录接口（删 Redis 会话即吊销）— `seckcpp0307`
- [x] 3.8 代码细节重构 — `seckcpp0308`（抽出 `seckill::http` 统一 JSON 响应 helper，SeckillController 三处 handler 复用）
- [ ] 3.9 滑块验证码（自实现或接入行为验证）— `seckcpp0309` — **挪到「开始有前端」阶段再做**；本阶段博客 3.9 只写设计 + 注明，不落地代码（前端引成熟框架 vue3-slide-verify / AJ-Captcha，后端校验仍自写）

> 注：登录模块在阶段一里就把 **Redis 提前到 `v0.1.x` 进场**（原架构演进中 Redis 属阶段二）。

### 第四章 基础秒杀功能（阶段一 `v0.1.x`，Drogon + MySQL 直连）— 部分已完成

- [x] 4.1 秒杀商品列表接口 — `seckcpp0401`（GET `/api/seckill/list`）
- [x] 4.2 秒杀商品详情接口 — `seckcpp0402`（GET `/api/seckill/{skuId}`）
- [x] 4.3 秒杀下单接口（原子 `UPDATE ... WHERE stock>0` + 事务化幂等下单，代码已完成）— `seckcpp0403`
- [x] 4.4 压测：100 用户抢 10 件，核对不超卖（correct 模式：10×200 / 90×409，sold=orders=10）— `seckcpp0404`
- [x] 4.5 压测：暴露/验证重复下单（uk_user_sku + ON DUPLICATE 幂等，0 重复）— `seckcpp0405`
- [x] 4.6 解决超卖：SQL 条件判断（行锁内 `WHERE stock>0`）— `seckcpp0406`
- [x] 4.7 基线性能测试（官方 JMeter：QPS≈371 / p95≈359ms / 0 超卖；curl harness 交叉验证 ≈341）+ 阶段一总结（博客待产出）— `seckcpp0407`
- [x] 4.8 应用层在途闸门：mutex / 自旋 / 原子三后端（`service/InflightGuard.h`），`scripts/lock-bench.sh` 对比压测 — `seckcpp0408`

> 当前 `/api/seckill` 已实现 4.1/4.2/4.3/4.6/4.8 核心逻辑（查询 + 原子扣减 + 幂等 + 应用层闸门），但博客文章 4.1/4.2/4.3/4.6/4.4/4.5/4.7/4.8 尚未写；代码侧超卖 / 重复下单已解决并实测验证（correct 模式 0 超卖、baseline QPS≈371），文章按"项目先行、博客随后"约定待产出。

### 第五章 引入缓存层：读性能优化（阶段二 `v0.2.x`）— 进行中（5.1~5.3 代码+压测完成：20万条 JMeter 实测 ×1.40/×1.44、命中率 83.8%，5.1~5.3 博客已写）

- [x] 5.1 缓存 Key 设计规范与接口基线压测 — `seckcpp0501` — ✅ 规范落地（**[`docs/CACHE-DESIGN.md`](CACHE-DESIGN.md)** + `service/CacheKeys.h`）；压测脚本就绪（`scripts/read-bench.sh` + `jmeter/read-baseline.jmx`），**实测数字已回填 §5.3**（20万条 JMeter：on/off ×1.40(list)/×1.44(detail)，命中率 83.8%）
- [x] 5.2 加 Redis 缓存：商品列表接口 — `seckcpp0502` — ✅ 代码完成（`SkuCache::getList/setList` + `SeckillService::listSkus` Cache-Aside）
- [x] 5.3 加 Redis 缓存：商品详情接口 — `seckcpp0503` — ✅ 代码完成（含空值哨兵，顺带落地 5.6 的一半）+ 下单提交后按配置失效（默认只删详情）
- [ ] 5.4 缓存一致性：Cache-Aside / 延迟双删 — `seckcpp0504`
- [ ] 5.5 缓存预热与动态 TTL — `seckcpp0505`
- [ ] 5.6 防缓存穿透：空值策略 — `seckcpp0506` — **空值哨兵已随 5.3 落地**（`SkuCache::setNull`，TTL 60s），章节正文待补
- [ ] 5.7 防缓存穿透：布隆过滤器 — `seckcpp0507`
- [ ] 5.8 本地缓存（自实现 LRU）做多级缓存 — `seckcpp0508`

> 加缓存的对象是**读**接口（列表 / 详情），下单接口只多一步"提交成功后失效缓存"。
> 规格细节（Key 规范、TTL、失效范围取舍、fail-open 论证、已知不足）全部收敛在
> **[`docs/CACHE-DESIGN.md`](CACHE-DESIGN.md)**，本文件不重复。

### 第六章 消息队列削峰填谷（阶段三 `v0.3.x`，AMQP-CPP）— 待开始

- [ ] 6.1 Docker 安装 RabbitMQ — `seckcpp0601`
- [ ] 6.2 AMQP-CPP 接入 RabbitMQ：基础收发 — `seckcpp0602`
- [ ] 6.3 秒杀异步下单架构设计 — `seckcpp0603`
- [ ] 6.4 MQ 异步秒杀下单编码 — `seckcpp0604`
- [ ] 6.5 异步下单 JMeter 压测 — `seckcpp0605`
- [ ] 6.6 并发消费 MQ：多消费者线程 / 协程 — `seckcpp0606`
- [ ] 6.7 消费者削峰：prefetch + 并发度 — `seckcpp0607`
- [ ] 6.8 消费者可靠性：手动 ACK + 死信队列 — `seckcpp0608`
- [ ] 6.9 生产者可靠性：Confirm + Return — `seckcpp0609`
- [ ] 6.10 秒杀结果查询接口 — `seckcpp0610`
- [ ] 6.11 秒杀结果 SSE / WebSocket 实时通知 — `seckcpp0611`

### 第七章 Redis Lua 脚本原子预扣（阶段四 `v1.0.0` 上半）— 待开始

- [ ] 7.1 为什么 MQ 削峰之后 MySQL 行锁仍是瓶颈 — `seckcpp0701`
- [ ] 7.2 秒杀库存预热到 Redis — `seckcpp0702`
- [ ] 7.3 Redis Lua 库存原子扣减 — `seckcpp0703`
- [ ] 7.4 Redis Lua 一人一单限制 — `seckcpp0704`
- [ ] 7.5 Lua 接入秒杀下单主链路 — `seckcpp0705`
- [ ] 7.6 MQ 发送失败库存回补（1）— `seckcpp0706`
- [ ] 7.7 MQ 发送失败库存回补（2）— `seckcpp0707`
- [ ] 7.8 Redis 预扣后消费者链路优化 — `seckcpp0708`
- [ ] 7.9 库存售罄标记与快速失败 — `seckcpp0709`
- [ ] 7.10 秒杀商品查询接口升级：组合 Redis 实时库存 — `seckcpp0710`
- [ ] 7.11 JMeter 压测验收 — `seckcpp0711`

### 第八章 防刷、限流与秒杀资格校验（阶段四 `v1.0.0` 下半）— 待开始

- [ ] 8.1 为什么秒杀系统必须做防刷与限流 — `seckcpp0801`
- [ ] 8.2 手写接口限流器：Redis Lua 固定窗口 / 滑动窗口 — `seckcpp0802`
- [ ] 8.3 令牌桶 / 漏桶自实现 + 中间件解耦 — `seckcpp0803`
- [ ] 8.4 秒杀令牌设计：给下单接口加资格校验 — `seckcpp0804`
- [ ] 8.5 一次性秒杀令牌：防同一 token 重复提交 — `seckcpp0805`
- [ ] 8.6 秒杀令牌大闸：按库存数量控制令牌发放 — `seckcpp0806`
- [ ] 8.7 并发 Bug 修复：重复扣大闸与 token 覆盖 — `seckcpp0807`

---

## 4. 技术决策记录（ADR）

> 即时记录"为什么这么选"，避免后续改动时丢失上下文。

| ADR | 主题 | 决策日 | 锚点（commit） |
| --- | --- | --- | --- |
| ADR-1 | 登录模块三处选型变更 | 2026-09-02 | `20d06de` / `6a3ab55` |
| ADR-2 | IO 线程两阻塞点必须挪出 | 2026-09-02 | `20d06de` / `6a3ab55` |
| ADR-3 | 4.8 应用层锁该锁在哪（在途标记） | 2026-09-02 | `050c3c5`（落地）/ `4b728ca`（实测） |
| ADR-4 | 前端延后到阶段四之后 | 2026-09-02 | 老周拍板，记录于文档重构 |
| ADR-5 | Drogon 1.9.x 回调式 vs 协程 | 2026-08-28 | `897262e` / `3aa3b31` / `589cb5e` |
| ADR-6 | 同 IP 注册频控（自签发模式下的批量注册防护） | 2026-09-02 | 代码落地于本次会话（新增 `service/RegisterGuard.*`） |
| ADR-7 | 缓存 Key 规范与失效范围取舍（第五章 5.1~5.3） | 2026-09-03 | 代码落地于本次会话（`service/CacheKeys.h`、`SkuCache.*`、规范见 `docs/CACHE-DESIGN.md`） |

> 决策日志索引：每条 ADR 的"拍板日 + 落地 commit"见上表；下方各节附详细论证。

### ADR-1 登录模块三处选型变更（相对博客 3.1 原定方案）

> 决策日：2026-09-02 ｜ 落地提交：`20d06de`（登录/注册 #5）、`6a3ab55`（短信 #6）

博客 3.1 发布时定的是 `redis-plus-plus` + `jwt-cpp`，实际落地时三处都改了，理由如下（博客 3.1 会回头同步修订）：

| 环节 | 原定方案 | 实际采用 | 改的理由 |
| --- | --- | --- | --- |
| Redis 客户端 | `redis-plus-plus` | **Drogon 内置 `nosql::RedisClient`** | `redis-plus-plus` 是**同步** API。Drogon 的 handler 跑在 IO 线程（`threads_num=4`），在 IO 线程里同步等一次 Redis 往返，会把这个线程上排队的请求全部卡住——这是结构性问题。内置客户端异步、与框架事件循环同构，且零额外依赖（只需 `libhiredis-dev`） |
| JWT 签发 | `jwt-cpp` | **自实现 HS256**（`service/Jwt.*`） | HS256 全内涵是「base64url(头).base64url(体).base64url(HMAC-SHA256)」，三个动作各十几行，用已在场的 OpenSSL 就能写。引 header-only 库要 `FetchContent` 从 GitHub 拉，WSL 网络是额外风险点；自写则编译期零外部依赖、签名过程可审计 |
| 短信发送 | 直连腾讯云 API 3.0 + 自实现 TC3 签名（博客 3.1 旧方案） | **自签发 / 日志模式，不接任何短信网关** | 验证码模块的硬核价值在 SmsService（生成/Redis/Lua 原子校验/限流/锁定），"码怎么送达"对自驱动/演示项目是次要的；接网关带来凭据、计费、网络往返负担。2026-09-02 回退：SmsSender 退化为仅写日志（前缀 SMS_CODE），移除 libcurl 与 TC3 签名，项目零凭据可跑 |

净效果：登录模块引入的新依赖只有 `libhiredis-dev` 与 `redis-server`，其余全部复用既有组件。

### ADR-2 两个"必须挪出 IO 线程"的阻塞点

> 决策日：2026-09-02 ｜ 随 #5/#6 实现落地：`20d06de`（PBKDF2 挪线程）；短信 HTTP 原也是阻塞点（`6a3ab55`），已于 2026-09-02 回退为自签发/日志模式，该阻塞点随之消失

Drogon 的 IO 线程上只允许非阻塞操作，下面这处是同步阻塞的，须丢进独立工作线程、算完用 `queueInLoop` 切回：

| 阻塞点 | 耗时量级 | 若留在 IO 线程的后果 |
| --- | --- | --- |
| PBKDF2 密码哈希（`service/password.cc`） | 12 万次迭代 ≈ 30~60ms CPU | 登录接口的慢会**传染**给同线程上排队的秒杀请求 |

> 注：短信发送原是第二处阻塞点（`service/SmsSender.cc` 的 libcurl 同步 HTTP，网络往返 ≈ 几十 ms），下游一慢就拖住业务线程。2026-09-02 已回退为**自签发 / 日志模式**（不发起任何网络请求），该阻塞点随之消失，发送不再需要挪到工作线程。验证码的硬核逻辑（生成 / Redis / Lua 原子校验 / 限流 / 锁定）全在 `SmsService`，与送达方式无关。

### ADR-3 4.8 应用层锁：锁到底该锁在哪

> 决策日：2026-09-02 ｜ 落地提交：`050c3c5`（#7 应用层闸门）；实测数据回填：`4b728ca`

**致命误区**：最直觉的写法是拿 `std::mutex` 把整个 `doSeckill` 包起来、锁在开头解锁在 DB 回调里。这在 Drogon 里是灾难——① 阻塞 IO 线程，一个请求持锁等 MySQL 往返就把 1/4 的请求全卡住，QPS 掉到个位数；② 锁的所有权跨了异步边界，某条异常路径忘了释放，这个 sku 就永久不可买，且极难复现。

**正确切法**：把锁的作用域缩到极致，只保护**在途标记**这个纳秒级临界区——DB 调用前 `tryAcquire` 标记「同一 user+sku 我正在处理」，标记失败说明已有在途请求，**连数据库都不打**直接判重复；DB 回调返回时用 `shared_ptr` 的 deleter 自动清除标记，杜绝漏放。

所以 4.8 的收益不是"加锁变快了"，而是**把无效请求挡在数据库之外**。三种后端取舍：

| 后端 | 判重 | 代价 | 适用 |
| --- | --- | --- | --- |
| 分片 `std::mutex` | 精确 | 争用时 sleep/wake，一次上下文切换 1~2μs | 通用默认 |
| 分片自旋锁 | 精确 | 持锁期间占满一个核空转；临界区仅百纳秒，反而比睡眠唤醒快 | 临界区极短、线程数 ≈ 核数 |
| 原子位图（`fetch_or`） | **不精确**：hash 冲突会误挡无关请求 | 零等待零切换 | 追求极致延迟、能接受极低误杀率 |

> 分工要记牢：应用层闸门**只挡并发窗口内的重复**；串行发出的两次下单（第一次已完成、订单已落库）由 `seckill_order` 的 `uk_user_sku` 唯一键兜底。两层各管一段，缺一不可。
>
> 压测：`bash scripts/lock-bench.sh 2000 100 4`，会分别跑 `unique`（无重复，测**锁的开销**）与 `dup`（并发重复，测**锁的收益**）两个场景 × 四种模式。只看任一个场景都得不出结论。

### ADR-4 前端页面延后到阶段四之后（决策 2026-09-02）

> 决策日：2026-09-02 ｜ 老周拍板「先不做，阶段四之后再考虑」，记录于本次文档重构（无单一落地 commit，属项目范围决策）

**决策：前端不做，阶段四之后再考虑。** 理由：

1. 验收硬指标是「压测 QPS 提升一个量级 + 不超卖不重复下单」，前端对这条指标**零贡献**。
2. 真实秒杀系统的前端是 **CDN 静态资源 + 按钮置灰**，本就不属于服务端吞吐链路；阶段一做重前端会分散主线精力。
3. 引入 Vue / Vite 会把构建文档从「两条 cmake 命令」变成「先装 Node 再 `npm i`」，博客读者直接劝退。

若将来要做，按**已定形态**落地（别再讨论选型）：

- 单文件 `www/index.html`，原生 JS，**无框架、无 CDN 依赖**；
- `config.json` 加 `document_root: "./www"` 走 Drogon 自带静态文件服务 —— **同源，零 CORS 问题**，不引 Node；
- 前置依赖：3.x 登录 / 短信路由挂载、4.1 / 4.2 商品接口、`/api/seckill` 接 `Authorization: Bearer`（`LoginGuard` / `Jwt` 已就绪）。

做的时候直接避开这几个坑：

| 坑 | 说明与解法 |
| --- | --- |
| 验证码前端拿不到 | `SmsSender` 未配腾讯云凭据时 `enabled=false`，只在日志里打码 → 需加 dev 开关把 code 回显到响应 |
| 压测被页面干扰 | 静态文件与 API 共用 IO 线程，压测期间不要开着页面自动刷新 |
| 改鉴权动到基线 | 下单接口当前 `userId` 走 JSON body、**不校验 token**（登录模块 3.1~3.7 已就绪但未接入主链路）→ **已决定保留 body 双通道**，登录鉴权接入（从 Bearer 取 userId）留待「开始有前端」阶段统一做；这是阶段一为压测便利的有意取舍 |

### ADR-5 Drogon 版本取舍

> 确立于：2026-08-28 ｜ 锚点提交：`897262e` / `3aa3b31` / `589cb5e`（Drogon 1.9.10 API 对齐）；协程版路线为后续待办

本仓库按 **Drogon 1.9.x** 的回调式 orm API 编写：`Transaction::commit()` / `rollback()` 为 `void`，
通过 `>> [](const ResultSet&, const std::exception_ptr&)` 组合回调处理结果与异常。
若使用开启 C++20 协程的新版 Drogon，`commit()` 会返回 `drogon::Task<bool>`，需 `co_await` 才生效；
届时把 `SeckillService.cc` 里的提交改写为协程版本即可（在对应阶段文章里展开）。

### ADR-6 同 IP 注册频控（自签发模式下的批量注册防护）

> 决策日：2026-09-02 ｜ 落地提交：代码落地于本次会话（新增 `service/RegisterGuard.*`，接入 `UserService::registerUser` 闸门）

**背景**：验证码改自签发 / 日志模式后（ADR-1 第三行），"送达"不再是外部约束。但这带来一个真实部署必须面对的问题——攻击者可以用一堆手机号从同一 IP 批量注册。没有频控，自签发模式在真实部署下扛不住批量注册。

**决策**：加一层"同 IP 固定窗口内允许的成功注册数"频控（`RegisterGuard`）。

- 计数**成功注册数**（不是尝试数）：约束的是"一个 IP 能建几个账号"。计尝试数会被错误验证码刷爆配额（DoS 掉正常用户名额），与本意相悖。
- 固定窗口 `INCR + 首次 EXPIRE`（Lua 原子），Redis 存共享计数——多实例部署语义不变（与 `LoginGuard` 同理）。
- **fail-open**：Redis 不可用时放行，频控属可用性维度，宁可放过不可误杀正常注册（对比鉴权 `SessionStore::exists` 的 fail-close）。
- 闸门放在 `registerUser` **最前面**，连手机号格式校验前先拦；IP 由 `UserController::clientIp()` 提供——手动解析 `X-Forwarded-For` 首段（去空白），真实部署走代理时取真实客户端 IP，空则回退 `req->getPeerAddr().toIp()`。**注意**：Drogon 1.9.10 的 `HttpRequest` 只有 `getPeerAddr()`（返回 TCP 对端地址，走反代时拿到的是代理 IP），**没有** `getClientIp()` / `getRealIp()` 之类的内建方法；取反代后真实客户端 IP 必须自己解析请求头，这是本项目踩过的编译坑。
- 阈值默认 `max_per_ip=5` / `window_seconds=3600`，走 `config.json` 的 `register_limit`，随业务可调。
- 拒绝码 `REGISTER_IP_LIMITED` → HTTP 429。

**效果**：自签发验证码模式 + 同 IP 注册频控，二者结合即可在真实部署下站住脚——既不用接短信网关，又能堵住批量刷号。

### ADR-7 缓存 Key 规范与失效范围取舍（第五章 5.1~5.3）

> 决策日：2026-09-03 ｜ 落地：本次会话（`service/CacheKeys.h` + `service/SkuCache.*`）
> 完整规格见 **[`docs/CACHE-DESIGN.md`](CACHE-DESIGN.md)**，此处只记三个"拍板时刻"。

**① Key 统一收敛到一个文件**（`CacheKeys.h`）。散落在业务代码里硬拼字符串，后果是：想清一类缓存只能 `FLUSHDB`（顺手踢掉所有人的登录态）；改一次命名要全局 grep；没人知道某个 key 是否存在。集中之后这个文件就是缓存的"目录"，也是压测时 `redis-cli` 里直接敲的那几个字符串。

**② 新增 key 一律 `<prefix>:<biz>:<version>:<type>[:<id>]` 带版本号，历史 key 不动。** 第三章引入的 `sess:` / `sms:*` / `login:*` / `reg:ip:` 没有前缀与版本，是既有事实。改它们要同时动会话、验证码、限流四个模块，收益为零、风险不小。**规矩从第五章生效**，历史 key 留到阶段四统一迁移——这是"承认历史包袱但不被它绑架"。

**③ 下单后失效范围默认只删详情（`invalidate_on_order: item`），不删列表。** 这是本次最实际的取舍：

| 策略 | 命中率 | 代价 |
| --- | --- | --- |
| `item`（默认） | 高 | 列表页 stock 允许陈旧——真实秒杀列表页本就该静态化，"还剩几件"对下单决策没有意义 |
| `all` | **趋零** | 列表是全量聚合 key，任意 sku 成交都删它，写 QPS 一高这个 key 等于不存在 |
| `none` | 最高 | 售罄后用户最长 TTL 秒内仍看到"还有货" |

把这个选择做成配置项而不是写死，是因为它本质是"新鲜度 ↔ 命中率"的交换，不同业务答案不同。

**④ 缓存必须 fail-open**（对比 `SessionStore` 的 fail-close）：错放一次缓存 = 多查一次库（无正确性损失），错放一次会话 = 被封禁 token 能下单（安全事件）。判据是**误放的代价 vs 误杀的代价**。

---

### 阶段一的功能抉择（重点）

- **为什么先读后写会超卖**：应用层 `SELECT stock` 再 `UPDATE` 在并发下读到旧值，必然多卖。必须下沉到数据库单行原子操作，让 `WHERE stock>0` 在行锁内判定。
- **为什么用事务**：扣库存和落订单是两个写，中间崩溃会留下"库存扣了但没订单"的不一致。事务保证原子性——代价是每个请求占用一条 DB 连接并持 sku 行锁到提交，这就是阶段一 QPS 被卡死的根源，也是后续引入 Redis / MQ 的动机。
- **幂等**：`seckill_order` 用 `uk_user_sku(user_id, sku_id)` 唯一键，重复下单直接冲突，避免同一用户刷多单。坑：**不能裸 `INSERT`**——唯一键冲突会抛异常，若在外层 catch 里一律当 DB 错误回滚，库存其实已被步骤 1 扣掉，更糟的是这本来是「重复下单」的业务拒绝，不该报成系统错误。改用 `INSERT ... ON DUPLICATE KEY UPDATE id = id`，靠 `affectedRows()` 区分：`1`=新订单，`0`=命中唯一键即重复下单（此时显式回滚，把多扣的库存还回去）。

---

## 5. 实测基线与验收数据

### 5.1 阶段一基线（实测，2026-09-02，WSL i7-14650HX + 本地 MySQL）

| 项 | 数值 |
| --- | --- |
| 工具 | `scripts/jmeter-baseline.sh` → 官方 Apache JMeter 5.6.3（`JMETER_BIN`，采样器 `implementation=Java` 绕过 WSL 的 HTTPHC4Impl 初始化失败）出 HTML 报告；无 JMeter 时回退零依赖 curl harness |
| 并发 / 时长 | 100 并发 / 60s，库存 1e8（不售罄，专测"卖出"事务路径） |
| **QPS** | **≈ 371**（聚合 throughput 371.03/s；22352 样本） |
| avg / p50 / p90 / p95 / p99 延迟 | 247ms / 251ms / 333ms / 359ms / 399ms（max 436ms） |
| 错误 / 不超卖 | 错误率 0%；MySQL sold=orders=22352（0 超卖） |
| 交叉验证 | curl harness 另测 QPS≈341.3 / p95≈0.375s / 0 超卖；correct 模式 100 抢 10 → 10×200 + 90×409，sold=orders=10 |

> HTML 报告：`jmeter/out/report/index.html`（副产物统一收口到 `jmeter/out/`）。

> 这是后续所有优化的**对比基线**：阶段五（缓存预扣）、阶段六（MQ 异步）、阶段七（Lua 原子）每步都应显著拉高 QPS、压低延迟。根因：每个请求占一条 DB 连接并持 `seckill_sku` 行锁到事务提交，行锁串行化即吞吐瓶颈。

### 5.2 4.8 应用层锁对比压测（2026-09-02，WSL i7-14650HX + 本地 MySQL，Drogon `threads_num=4`）

`bash scripts/lock-bench.sh 2000 100 4`：2000 请求 = 500 用户 × 4 次同 `(userId,skuId)`；`unique` 场景每用户只发一次，`dup` 场景每用户并发 4 次（第一次成功后其余 3 次应在途命中被挡）。

| 模式 | 场景 | QPS | 耗时 s | 200 | 409 | **应用层挡掉** |
| --- | --- | --- | --- | --- | --- | --- |
| none | unique | 350.6 | 5.7038 | 2000 | 0 | 0 |
| none | dup | 831.8 | 2.4045 | 500 | 1500 | 0 |
| mutex | unique | 401.0 | 4.9873 | 2000 | 0 | 0 |
| mutex | dup | 924.2 | 2.1641 | 500 | 1500 | **1500** |
| spin | unique | 386.6 | 5.1727 | 2000 | 0 | 0 |
| spin | dup | 1160.2 | 1.7238 | 500 | 1500 | **1500** |
| atomic | unique | 337.1 | 5.9329 | 1999 | 1 | 0 |
| atomic | dup | 1005.2 | 1.9896 | 500 | 1500 | **1501** |

**结论速读**：

- **unique 场景（锁的纯开销）**：none=350.6 作基准，mutex **+14.4%**（401.0）、spin **+10.3%**（386.6）、atomic **−3.9%**（337.1，反而略慢，因为位图 `fetch_or` 的缓存行争用在无重复时没有收益）。说明「在途标记」本身是有成本的，用不用锁都得付这份钱——所以这套机制的目标从来不是"加锁变快"。
- **dup 场景（锁的真实收益）**：none=831.8 但**挡掉=0**（1500 个重复全部一路打到 DB，靠 `uk_user_sku` 唯一键在事务里兜掉）；只要开了应用层闸门，mutex/spin/atomic **挡掉都≈1500**，意味着 75% 的重复请求在进 DB 之前就被内存层拦下，DB 写压力直接降一个量级。
- **各后端表现**：dup 场景 QPS 相比 none 提升 —— mutex **+11.1%**（924.2）、spin **+39.5%**（1160.2，临界区仅百纳秒、无上下文切换，收益最大）、atomic **+20.8%**（1005.2）。闸门把无效请求挡掉后，原本被这些请求占用的 DB 连接/行锁让出来，整体吞吐反而更高。
- **atomic 的代价**：位图 `fetch_or` **不精确**——dup 场景「挡掉=1501」比实际重复数 1500 多 1，即 hash 冲突误杀了一个正常请求（unique 场景也出现 1 个 409）。生产环境若选 atomic，需接受这个极低误杀率，或调大 bits/shards 降低冲突概率。
- **选哪个**：通用默认 `mutex`（精确、无误杀）；临界区极短且线程数≈核数可上 `spin`（延迟最低）；`atomic` 只在对延迟极度敏感、且能容忍偶发误杀时用。

### 5.3 读接口基线：商品列表 / 商品详情（第五章 5.1）

同一套流量跑两轮、只改 `cache.enabled` 一个变量：

```bash
mysql -h127.0.0.1 -useckill -pseckill seckill < sql/seed_sku.sql   # 默认 20 万条商品（id 1..200000）
bash scripts/read-bench.sh            # 总并发 100（列表 60 / 详情 40）× 60s
```

> **数据量级**：`seed_sku.sql` 默认 **20 万条**（连续 id，幂等不覆盖 stock，数字表 CROSS JOIN 生成，MySQL 5.7+ 兼容）。列表接口 `LIMIT 100` 仍只返回前 100 条、value ≈ 13KB，**不随总量爆炸**；详情随机打 `1..200000` 共 20 万个 key，缓存 key 基数才贴近真实热点/长尾。下面给出**两个量级**的实测对照：小数量级（20 条）是迁移前基线、仅验证缓存逻辑正确；**20 万条（纯 JMeter 引擎）才是本章权威结论**——本地 20 条对照组太强会掩盖缓存收益，真实量级下才看得到乘数级提升。

#### 小数据量级（20 条，curl harness 回退）— 仅验证逻辑

| 接口 | 缓存 | 样本 | QPS | avg(ms) | p50(ms) | p95(ms) | p99(ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| list | off | 87809 | 1463.4 | 1.1 | 0.8 | 2.7 | 4.5 |
| list | on | 91616 | 1526.7 | 1.0 | 0.7 | 2.7 | 4.5 |
| detail | off | 88372 | 1472.7 | 0.9 | 0.7 | 2.3 | 4.2 |
| detail | on | 92396 | 1539.7 | 0.8 | 0.5 | 2.4 | 4.3 |

- 命中率 100%（hit=184014 / miss=3）；err=0；列表 TTL 44s（30s+抖动）；空值哨兵 `/api/seckill/999999` → `__nil__` TTL 78s。
- 提升倍数：list ×1.04（+4%）、detail ×1.05（+5%）。**这 +4~5% 不是缓存没用，是本地 20 条的「对照组」太强**（见下结论）。

#### 真实数据量级（20 万条，JMeter 5.6.3 引擎）— 权威结论

`wsl -e env LANG=C.UTF-8 JMETER_BIN=/opt/apache-jmeter-5.6.3/bin/jmeter bash scripts/read-bench.sh 100 60`，列表 60 / 详情 40 并发 ×60s：

| 接口 | 缓存 | 样本 | QPS | avg(ms) | p50(ms) | p95(ms) | p99(ms) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| list | off | 452687 | 7544.2 | 6.95 | 7.00 | 9.00 | 12.00 |
| list | on | 633476 | 10556.9 | 3.84 | 3.00 | 7.00 | 9.00 |
| detail | off | 335555 | 5591.2 | 6.22 | 6.00 | 8.00 | 11.00 |
| detail | on | 481528 | 8023.5 | 3.23 | 3.00 | 7.00 | 10.00 |

| 观测项 | 值 |
| --- | --- |
| on 轮命中率 | 83.8%（hit=934466 / miss=180538 / err=0） |
| DB 回源对比 | off 轮 60s 内 ≈788k 次读全打 MySQL；on 轮同窗口仅 180k 次回源（其余 93.4 万由缓存挡下），**DB 读负载下降约 77%，同时总 QPS +40~44%** |
| Redis 异常（err） | 0（缓存确实生效，非假象） |
| 列表 key 剩余 TTL | 基准 30s + 抖动（脚本实测在此区间） |
| 空值哨兵 | `/api/seckill/200001` 后 Redis 写入 `__nil__`，TTL 84s（60s + 抖动） |

**提升倍数（on 相对 off，20 万量级）**：list ×1.40（+40%）、detail ×1.44（+44%）。

> 口径自洽：`on` 轮 QPS 更高，所以 60s 内样本数比 off 轮更多（list 633k vs 452k、detail 481k vs 335k）；on 轮总样本 1,115,004 = hit(934,466) + miss(180,538)，数字闭合。

**结论解读（这章最该讲清的点）**：

- **数据量级决定缓存收益的「能见度」**。小数据量级（20 条）下 MySQL 一次 SELECT 本就 <1ms，缓存只是把「1ms 查询」换成「亚毫秒 Redis GET」，单请求省的时间被 `threads_num=4` 的 IO 线程与压测并发模型摊平，QPS 只动 4~5%；但缓存真正的 KPI 是「保护数据库」——20 条量级 on 轮 18 万次读里仅 3 次回源（降载 99.998%）。
- **量级一上去，缓存从「降载」变成「乘数级 QPS」**：20 万量级下 key 空间放大了 miss 回源的代价（DB 查的数据更杂、连接更紧张），缓存命中把 83.8% 的读挡在门外，list QPS 从 7544 拉到 10556（+40%）、detail 从 5591 拉到 8023（+44%），**平均延迟还砍半**（list 6.95→3.84ms、detail 6.22→3.23ms）。这印证博客要讲的核心：真实部署里 MySQL 在远端、有连接池上限、洪峰读放大会先打满 DB，**缓存挡掉的不是 4% 的 QPS，是 83.8% 可能压垮 DB 的读流量**。
- **run 1 / run 2 的「−20%」是脏数据，不可信**：那两轮虽设了 `JMETER_BIN`，但 JMeter 因 WSL 代理（`JAVA_TOOL_OPTIONS`/`_JAVA_OPTIONS` 注入的 `-Dhttp.proxyHost`）0 样本、回退到 curl harness；脚本又另起一个 curl 进程抢 8080 端口，热路径退化 + 20 万 key 仅预热 1% 导致大量 miss 回源，最终 on 比 off 还低 20%、命中率仅 58%。**run 3 用纯 JMeter 干净环境（`wsl -e env ... JMETER_BIN=...` 不带代理注入）才拿到可信的 +40%/+44%**——所以「缓存提升反直觉地为负」一定是压测链路错配，不是代码问题。
- **空值哨兵在 20 万量级仍正确**：`/api/seckill/200001`（连续 id 下必超范围）写入 `__nil__`、TTL 84s，证明 5.6 的防穿透逻辑在大 key 空间下不漏。

**JMeter 路径（run 3 已生效）**：`seed_sku.sql` 改 20 万条 + 干净 WSL 环境（无代理变量）后，`JMETER_BIN` 指到的 Apache JMeter 5.6.3 正常采样、出 HTML 报告（`jmeter/out/read-report/index.html`）+ 聚合 `statistics.json`。关键避坑：WSL 上 `JAVA_TOOL_OPTIONS`/`_JAVA_OPTIONS` 里的 `-Dhttp.proxyHost` 会把 Java 采样器路由到代理端口导致 0 样本；脚本已 `unset` 这些变量，最稳还是用 `wsl -e env LANG=C.UTF-8 JMETER_BIN=...` 起一个没有代理注入的会话。5.6.3 的聚合器写的是 `statistics.json`（JsonExporter），不是 `statistics.csv`，`AGG` 路径已对齐。

> 口径说明：`on` 轮先预热再压，测的是**稳态命中**；`miss` 即回源 DB 次数。
> 压测前后各读一次 `/api/cache/stats` 取差值，避免把预热阶段的计数算进来。
> 坑位：两轮数字完全一样 → 先确认服务端的 `enabled` 是否真的跟着变了（改 config 没重启 = 白跑一轮）。

---

## 6. 开发排障（WSL + MySQL + Drogon）

沙箱环境没有编译器工具链，真实编译在本地 WSL 完成。

### 6.1 构建与运行（完整步骤）

```bash
# 1) 一次性安装依赖 + Drogon（约十几分钟，含 Drogon 源码编译）
bash scripts/setup-wsl.sh

# 2) 启动 MySQL 与 Redis（WSL 不会自启，每次开机都要来一次）
sudo service mysql start
sudo service redis-server start
# 秒杀链路只需要 MySQL；Redis 只服务登录/短信模块，没起也只是那几个接口 503

# 3) 建库建表 + 建应用账号
#    Ubuntu 的 mysql-server 给 root 挂 auth_socket 插件，输密码登录会被拒（ERROR 1698），必须用 sudo 免密进：
sudo mysql
```
```sql
source /mnt/d/GitHub/seckill-cpp/sql/schema.sql;       -- 建库建表（秒杀）
source /mnt/d/GitHub/seckill-cpp/sql/user_schema.sql;  -- 登录模块建表（user / login_log），用不到登录可跳过
source /mnt/d/GitHub/seckill-cpp/sql/init_user.sql;    -- 建应用专用账号（Drogon 用 TCP+密码连，不能用 auth_socket 的 root）
```
```bash
# 4) 编译
bash scripts/build-wsl.sh

# 5) 跑起来（默认读 ./config.json，监听 :8080）
./build/src/seckill-cpp
```

> WSL 不会自启服务，想省事可在 `~/.bashrc` 加一行：`sudo service mysql status >/dev/null || sudo service mysql start`

### 6.2 调试与验证脚本

仓库附带辅助脚本（WSL 里用 `bash scripts/xxx.sh` 调用，注意本机 `.sh` 可能被关联到 Node.js，别直接 `./xxx.sh`）：

| 脚本 | 用途 |
| --- | --- |
| `scripts/debug-wsl.sh check` | 一键体检：MySQL 连通、表数据、Drogon 是否带 MySQL 后端、可执行文件实际链接的库 |
| `scripts/debug-wsl.sh asan` | ASan/UBSan 构建并启动，精确定位崩溃行（SIGSEGV 首选） |
| `scripts/debug-wsl.sh gdb` | Debug(-g) 构建进 gdb，看带行号 bt |
| `scripts/smoke-seckill.sh [并发] [库存] [skuId]` | 冒烟压测：N 个用户并发抢购 + MySQL 核对不超卖，默认 `100 10 1` |

### 6.3 常见坑

| 报错 | 原因 | 解法 |
| --- | --- | --- |
| `ERROR 2002 ... socket '/var/run/mysqld/mysqld.sock'` | MySQL 服务没启动 | `sudo service mysql start` |
| `ERROR 1698 Access denied for user 'root'@'localhost'` | root 走 `auth_socket`，只能用 OS root 身份登录 | 用 `sudo mysql`（不加 `-p`），或建专用账号 |
| `systemctl` 报 "System has not been booted with systemd" | WSL 默认不开 systemd | 改用 `service mysql start` |
| 程序连不上但 `sudo mysql` 能进 | 应用走 TCP 密码认证，root 是 socket 认证 | 执行 `sql/init_user.sql` 建 `seckill` 账号 |
| `service mysql start` 卡住/失败 | `/var/run/mysqld` 目录缺失或权限错 | `sudo mkdir -p /var/run/mysqld && sudo chown mysql:mysql /var/run/mysqld` |
| 首个请求 SIGSEGV，崩在 `SeckillService::doSeckill` 的 `db_->...` | `getDbClient("default")` 在 `app.run()` 之前调用必然返回空 shared_ptr（Drogon 1.9.10 的 db 客户端要 run() 才创建） | 把 `getDbClient` + 组装 Service 延迟到 handler 里（static 延迟初始化），不要在 `main()` 里提前取 |
| 登录 / 短信接口全返 `503 ... (redis not connected)` | `getRedisClient("default")` 拿到空指针——要么 `config.json` 没配 `redis_clients`，要么 Drogon 编译时没探测到 hiredis | ① `config.json` 加 `redis_clients` 段；② `sudo apt install libhiredis-dev` 后**重新编译安装 Drogon**（`-DBUILD_REDIS=ON`）；③ `sudo service redis-server start` |
| `redis-cli ping` 报 `Could not connect` | WSL 不自启服务 | `sudo service redis-server start`，或 `sudo redis-server --daemonize yes` |
| 调用 Redis 接口报 `SESS_*_FAILED` / `SMS_LUA_FAILED` | Redis 连不上或 Lua 脚本报错 | 看 `logs/seckill.log` 里的具体异常；先 `redis-cli ping` 确认服务在 |
| 短信接口返回成功但手机没收到 | 验证码走自签发 / 日志模式，不接短信网关 | 验证码从日志里 `SMS_CODE phone=... code=...` 这条读取后填入校验接口 |

### 6.4 快速查看数据

```bash
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill
```
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

---

## 7. 接口与配置参考

### 7.1 接口一览

| 方法 | 路径 | 说明 | 依赖 |
| --- | --- | --- | --- |
| GET | `/api/health` | 健康检查 | — |
| POST | `/api/seckill` | 秒杀下单（**不校验 token**，`userId` 走 body；鉴权接入留待前端阶段） | MySQL |
| GET | `/api/seckill/list` | 商品列表（5.2 起走 Redis 缓存） | MySQL（+Redis） |
| GET | `/api/seckill/{skuId}` | 商品详情（5.3 起走 Redis 缓存，含空值哨兵） | MySQL（+Redis） |
| GET | `/api/lock/stats` | 4.8 在途闸门统计（mode / acquired / rejected） | — |
| GET | `/api/cache/stats` | 5.1 缓存命中率（hit / miss / err / write / hit_rate + 实际 key 与 TTL） | Redis |
| POST | `/api/sms/send` | 发送短信验证码 | **Redis** |
| POST | `/api/user/register` | 注册（默认需验证码） | MySQL + Redis |
| POST | `/api/user/login` | 登录，返回 Bearer Token | MySQL + Redis |
| POST | `/api/user/logout` | 登出，吊销会话 | Redis |

> Redis 没起时，登录 / 短信相关路由返回 `503`，**秒杀链路不受影响**——压测基线不会因中间件缺失而跑不起来。

### 7.2 秒杀接口

- `POST /api/seckill`，body：`{"userId":<int64>,"skuId":<int64>}`。
- 防超卖靠 **单行原子** **`UPDATE ... WHERE stock>0`**，扣库存与落订单包在**同一个 DB 事务**里，要么全成、要么全回滚。

```bash
curl -s -X POST localhost:8080/api/seckill \
  -H 'Content-Type: application/json' \
  -d '{"userId":1,"skuId":1}'
```

返回码（HTTP 状态 + JSON）：

| 场景 | HTTP | body |
| --- | --- | --- |
| 下单成功 | 200 | `{"code":0,"msg":"success"}` |
| 库存不足 | 409 | `{"code":1,"msg":"SOLD_OUT"}` |
| 重复下单 | 409 | `{"code":1,"msg":"DUPLICATE_ORDER"}` |
| 参数缺失/非 JSON | 400 | `{"code":400,"msg":"missing or invalid userId/skuId"}` |
| DB 异常 | 500 | `{"code":1,"msg":"DB_ERROR: ..."}` |

> 业务拒绝（409）客户端不该重试；只有 500 才值得重试。

### 7.3 登录模块接口

```bash
# 1) 发验证码（mock 模式下验证码只写进 logs/seckill.log，不会真发短信）
curl -s -X POST localhost:8080/api/sms/send \
  -H 'Content-Type: application/json' -d '{"phone":"13800001111"}'

# 2) 注册（code 从日志里捞：grep SMS_CODE logs/seckill.log）
curl -s -X POST localhost:8080/api/user/register \
  -H 'Content-Type: application/json' \
  -d '{"phone":"13800001111","password":"abc123","code":"123456"}'

# 3) 登录 → 拿 token
curl -s -X POST localhost:8080/api/user/login \
  -H 'Content-Type: application/json' \
  -d '{"phone":"13800001111","password":"abc123"}'
# 返回 {"code":0,"msg":"success","data":{"token":"eyJ...","tokenType":"Bearer"}}

# 4) 登出（吊销 Redis 会话，此后该 token 立即失效）
curl -s -X POST localhost:8080/api/user/logout \
  -H 'Authorization: Bearer eyJ...'

# 一键自检整条链路（发码→注册→查重→登录→撞库锁定→登出→旧 token 失效）
bash scripts/verify-auth.sh 13800001111 abc123
```

登录 / 注册错误码：

| HTTP | msg | 含义 |
| --- | --- | --- |
| 200 | `success` | 成功（登录时 `data.token` 为 JWT） |
| 400 | `INVALID_PHONE` / `WEAK_PASSWORD` / `missing phone/password` | 参数问题 |
| 409 | `PHONE_REGISTERED` | 手机号已注册（含并发下撞 `uk_phone` 的情形） |
| 409 | `USER_NOT_FOUND` | 手机号不存在（为防账号枚举，同样计一次失败） |
| 409 | `WRONG_PASSWORD:<已试次数>` | 密码错，明确告知还能试几次 |
| 409 | `ACCOUNT_LOCKED:<剩余秒数>` | 连续失败达 `max_fail`，临时锁定 |
| 409 | `ACCOUNT_DISABLED` | 账号被禁用（`user.status=0`） |
| 409 | `CODE_EXPIRED` / `CODE_WRONG` / `CODE_TOO_MANY_ATTEMPTS` | 验证码问题 |
| 401 | `missing bearer token` / `TOKEN_INVALID` | 登出时 token 缺失或验签失败 |
| 429 | `too frequent: ...` / `daily limit reached: ...` | 短信重发冷却 / 每日上限 |
| 500 | `DB_ERROR:*` / `REDIS_ERROR:*` / `HASH_FAILED` | 系统故障，可重试 |
| 503 | `* unavailable (redis not connected)` | Redis 未起，登录模块整体不可用 |

### 7.4 配置项（`config.json` 的 `custom_config`）

| 段 | 键 | 默认 | 说明 |
| --- | --- | --- | --- |
| `jwt` | `secret` / `issuer` / `ttl_seconds` | — / `seckill-cpp` / `7200` | JWT 密钥（**生产必须换**）、签发者、有效期 |
| `sms` | `code_ttl_seconds` / `resend_interval_seconds` / `daily_limit` / `max_verify_attempts` | `300` / `60` / `10` / `5` | 验证码有效期、重发冷却、每日上限、单个码试错上限（送达为自签发/日志模式，不接短信网关，无需任何腾讯云凭据） |
| `login_guard` | `max_fail` / `lock_seconds` | `5` / `600` | 密码错误阈值与锁定时长 |
| `auth` | `require_sms_on_register` / `require_sms_on_login` / `min_password_length` | `true` / `false` / `6` | 是否强制验证码、密码最小长度 |
| `seckill_lock` | `mode` / `shards` / `bits` | `none` / `64` / `65536` | 4.8 闸门：`none`/`mutex`/`spin`/`atomic` |
| `cache` | `enabled` | `true` | 5.1 缓存总开关（`false` = 接口基线，两轮压测就靠它切换） |
| `cache` | `key_prefix` / `key_version` | `seckill` / `v1` | Key 前缀与结构版本号（改结构就升版本，不清库） |
| `cache` | `list_ttl_seconds` / `detail_ttl_seconds` / `null_ttl_seconds` | `30` / `60` / `60` | 列表 / 详情 / 空值哨兵的基准 TTL |
| `cache` | `jitter_seconds` | `30` | TTL 随机抖动上限（防雪崩，实际 TTL = 基准 + [0,jitter)） |
| `cache` | `invalidate_on_order` | `item` | 下单后失效范围：`item`（默认，只删详情）/ `all`（详情+列表）/ `none`（只等 TTL） |

### 7.5 Redis key 约定（排障时直接查）

```
sess:{jti}              会话（value=userId，TTL 与 JWT exp 对齐）
login:fail:{phone}      密码失败计数（TTL = lock_seconds，滑动窗口）
login:lock:{phone}      账号锁定标记（TTL = 剩余锁定时长）
sms:code:{phone}        验证码（校验通过即删除，一次性）
sms:cd:{phone}          重发冷却标记
sms:try:{phone}         该验证码已试错次数
sms:day:{phone}:{yyyyMMdd}  当日已发条数（TTL 到次日 0 点）
reg:ip:{ip}                 同 IP 成功注册计数（固定窗口）

# 第五章起的新规范：<prefix>:<biz>:<version>:<type>[:<id>]（构造入口 service/CacheKeys.h）
seckill:sku:v1:list         商品列表（全量，紧凑 JSON，TTL = 30s + 抖动）
seckill:sku:v1:item:{id}    商品详情 / 空值哨兵 __nil__（TTL = 60s + 抖动）
seckill:sku:v1:stock:{id}   库存计数（阶段四 7.2 预留，当前不读写）
```

排障速查：

```bash
redis-cli GET  seckill:sku:v1:item:1        # 看详情缓存内容
redis-cli TTL  seckill:sku:v1:list          # 剩余 TTL（基准 30s + 抖动，验证抖动生效）
redis-cli GET  seckill:sku:v1:item:999999   # 查过不存在的 id 后应为 __nil__（空值哨兵）
redis-cli --scan --pattern 'seckill:sku:v1:*'   # 按前缀扫（生产禁用 KEYS）
curl -s localhost:8080/api/cache/stats      # 命中率与回源次数
```

全量 key 清单与规范论证见 **[`docs/CACHE-DESIGN.md`](CACHE-DESIGN.md) §2.3**。
