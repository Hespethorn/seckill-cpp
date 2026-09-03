-- seckill-cpp 阶段二（第五章）读接口压测用商品种子数据
--
-- 数据量级：默认 **20 万条**（id 1..200000，连续）。
--   为什么上到 20 万：阶段一 1 条、初版 20 条都太小——列表只 SELECT 若干行、
--   详情只命中少数 key，缓存层在"小数据"下看不出真实收益（value 才 2.6KB、
--   detail 只有 20 个 key，那是热点 key benchmark，不是缓存层整体收益）。
--   真实秒杀商品是成千上万量级，20 万条下：
--     - 详情接口打 **20 万个不同 key**（随机 skuId 1..200000），缓存 key 基数才贴近
--       真实热点/长尾分布；命中率、Redis 内存占用、回源分布才测得出来。
--     - 列表接口仍 LIMIT 100（见 SeckillService::queryListFromDb 自我保护），
--       value 体积稳定在"前 100 条" ≈ 13KB，**不随总量爆炸**——这正是加 LIMIT 100
--       要防的"大 key"，大数据量级下被印证。
--     - Redis 内存：20 万 detail key × ~200B ≈ 40MB，确认本地 redis 没设过小
--       maxmemory 即可（默认不限）。
--
-- 数据量级怎么调：
--   用数字表 CROSS JOIN 生成连续 id。当前 = d1(0..19, 20 行) × d2..d5(0..9) = 20×10^4 = 20 万。
--   · 想 10 万条：d1 改成 0..9（10 行），id 公式不变 → 1..100000。
--   · 想 100 万条：再加一张 0..9 数字表 d6，id 改成
--       d1*100000 + d2*10000 + d3*1000 + d4*100 + d5*10 + d6 + 1（1..1000000）。
--   数字表 CROSS JOIN 在 MySQL 5.7+ 都兼容（不依赖 CTE / 递归），WSL 的 5.7 / 8.0 都能跑。
--
-- 与 schema.sql 保持同一约定：**不覆盖 stock**。重复导入只刷新名称与场次时间，
-- 不会把库存刷回初始值（stage1 的 id=1 若存在，stock 不被本脚本改动，无功能性影响）。

USE seckill;

INSERT INTO seckill_sku (id, name, stock, total, start_time, end_time)
SELECT id,
       CONCAT('sku-', id),
       stock,
       stock AS total,
       '2026-09-01 00:00:00',
       '2026-12-31 23:59:59'
FROM (
  SELECT (d1.n * 10000 + d2.n * 1000 + d3.n * 100 + d4.n * 10 + d5.n + 1) AS id,
         (50 + FLOOR(RAND() * 4950)) AS stock   -- stock 50~5000，随机分布
  FROM (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9
        UNION ALL SELECT 10 UNION ALL SELECT 11 UNION ALL SELECT 12 UNION ALL SELECT 13 UNION ALL SELECT 14
        UNION ALL SELECT 15 UNION ALL SELECT 16 UNION ALL SELECT 17 UNION ALL SELECT 18 UNION ALL SELECT 19) d1
  CROSS JOIN (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d2
  CROSS JOIN (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d3
  CROSS JOIN (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d4
  CROSS JOIN (SELECT 0 n UNION ALL SELECT 1 UNION ALL SELECT 2 UNION ALL SELECT 3 UNION ALL SELECT 4
        UNION ALL SELECT 5 UNION ALL SELECT 6 UNION ALL SELECT 7 UNION ALL SELECT 8 UNION ALL SELECT 9) d5
) gen
ON DUPLICATE KEY UPDATE
  name       = VALUES(name),
  total      = VALUES(total),
  start_time = VALUES(start_time),
  end_time   = VALUES(end_time);

-- 验证：应看到 id 1..200000 共 20 万行（id=1 可能是 schema.sql 的 stage1-demo-sku，stock 不变）
SELECT COUNT(*) AS sku_count, MIN(id) AS min_id, MAX(id) AS max_id FROM seckill_sku;
