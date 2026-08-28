# seckill-cpp

用 **C++ / Drogon** 从零搭建的高并发秒杀系统实战项目，是本博客「秒杀系统」系列的配套源码仓库。
重点不在堆功能，而是每一步的**项目思考、实现路径，以及关键功能抉择上的权衡**。仓库按文章节奏分阶段演进：

| 阶段 | 手段 | 预期 QPS | 本仓库对应 |
|------|------|----------|------------|
| 1 | 直接打数据库（事务 + 原子扣减） | ~50 | **当前** `v0.1.x` |
| 2 | Redis 预扣减 + 异步落库 | 数千 | 待实现 |
| 3 | MQ 削峰填谷 | 数万 | 待实现 |
| 4 | 微服务 + 限流/治理 | 30000+ | 待实现 |

> 本仓库是作者个人的原创实现记录，不依赖任何外部教程专栏。

## 当前版本（v0.1.x）做了什么

- `GET  /api/health` 健康检查。
- `POST /api/seckill` 秒杀接口，body：`{"userId":<int64>,"skuId":<int64>}`。
- 防超卖靠 **单行原子 `UPDATE ... WHERE stock>0`**，扣库存与落订单包在**同一个 DB 事务**里，
  要么全成、要么全回滚。返回：`{"code":0,"msg":"success"}`；售罄 `{"code":1,"msg":"SOLD_OUT"}`（HTTP 409）。

### 阶段一的功能抉择（重点）

- **为什么先读后写会超卖**：应用层 `SELECT stock` 再 `UPDATE` 在并发下读到的是旧值，必然多卖。
  必须下沉到数据库单行原子操作，让 `WHERE stock>0` 在行锁内判定。
- **为什么用事务**：扣库存和落订单是两个写，中间崩溃会留下“库存扣了但没订单”的不一致。
  事务保证原子性——代价是每个请求占用一条 DB 连接并持 sku 行锁到提交，这就是阶段一 QPS 被卡死的根源，
  也是后续引入 Redis / MQ 的动机。
- **幂等**：`seckill_order` 用 `uk_user_sku(user_id, sku_id)` 唯一键，重复下单直接冲突，避免同一用户刷多单。

## 构建（在 WSL / Ubuntu 22.04+ 上）

沙箱环境没有编译器工具链，真实编译在本地 WSL 完成。

```bash
# 1) 一次性安装依赖 + Drogon（约十几分钟，含 Drogon 源码编译）
bash scripts/setup-wsl.sh

# 2) 建库建表
mysql -u root -p seckill < sql/schema.sql
#   或 mysql 客户端里先 CREATE USER 'seckill'@'localhost' IDENTIFIED BY 'seckill';
#   再 GRANT ALL ON seckill.* TO 'seckill'@'localhost';

# 3) 编译
bash scripts/build-wsl.sh

# 4) 跑起来（默认读 ./config.json，监听 :8080）
./build/seckill-cpp
```

### Drogon 版本取舍说明

本仓库按 **Drogon 1.9.x** 的回调式 orm API 编写：`Transaction::commit()` / `rollback()` 为 `void`，
通过 `>> [](const ResultSet&, const std::exception_ptr&)` 组合回调处理结果与异常。
若你使用开启了 C++20 协程的新版 Drogon，`commit()` 会返回 `drogon::Task<bool>`，需 `co_await` 才能生效；
届时把 `SeckillService.cc` 里的提交改写为协程版本即可（会在对应阶段文章里展开）。

## 发布每个版本

```bash
# 在 WSL 上：构建 -> 提交 -> 打 tag -> 推 GitHub
bash scripts/release.sh 0.1.0
```

## 目录结构

```
seckill-cpp/
├── CMakeLists.txt            # find_package(Drogon)
├── config.json               # 监听端口 + MySQL 客户端("default")
├── src/
│   ├── main.cc               # Drogon 启动 + 控制器/服务装配
│   ├── controllers/          # 仅做协议转换（HTTP <-> 业务参数）
│   └── service/              # SeckillService：事务化原子扣减（核心逻辑）
├── sql/schema.sql            # 阶段一表结构
└── scripts/                  # setup-wsl / build-wsl / release
```
