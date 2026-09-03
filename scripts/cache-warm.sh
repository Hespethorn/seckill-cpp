#!/usr/bin/env bash
# 5.5 缓存预热：把 DB 里的商品批量搬进缓存（重建列表 + 预热前 N 个详情）。
#
# 什么时候用：
#   - 压测 on 轮之前（替代 read-bench.sh 内部那段 curl 预热，口径更可控）；
#   - 真实发布时作为"活动开始前"的运营动作，让洪峰第一波就命中缓存。
#
# 用法（WSL / Git Bash 里用 bash 跑，别直接 ./xxx.sh —— 本机 .sh 被关联到 Node）：
#   bash scripts/cache-warm.sh            # 预热前 1000 个 sku 的详情 + 重建列表
#   bash scripts/cache-warm.sh 5000       # 预热前 5000 个
#   bash scripts/cache-warm.sh 20000 http://127.0.0.1:8080
#
# 幂等：重复执行只是覆盖回写，无副作用（setex 覆盖 + 重建列表）。
# 前置：服务已在跑（./build/src/seckill-cpp）。
set -euo pipefail

LIMIT="${1:-1000}"
BASE="${2:-http://127.0.0.1:8080}"

echo "==> POST $BASE/api/cache/warm  limit=$LIMIT"
curl -s -X POST "$BASE/api/cache/warm" \
  -H 'Content-Type: application/json' \
  -d "{\"limit\":$LIMIT}"
echo
echo "==> 验证：redis 里实际存在的 item key 数（应为 ~$LIMIT + 1 个 list key）"
redis-cli --scan --pattern 'seckill:sku:v1:item:*' 2>/dev/null | wc -l
redis-cli EXISTS seckill:sku:v1:list
