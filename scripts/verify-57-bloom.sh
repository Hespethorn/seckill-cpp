#!/usr/bin/env bash
# 5.7 布隆过滤器验证：重建布隆 -> 连打 100 次不存在的 id -> rejected=100 且 DB miss 不涨。
#
# 用法：bash scripts/verify-57-bloom.sh [不存在的id]
# 前置：MySQL/Redis 在跑、服务已启动、config.json 的 cache.bloom_enabled=true（否则本脚本会提示）。
# 说明：布隆默认关闭（PLAN ADR-8），需手动在 config.json 打开后重启服务再跑本脚本。
set -uo pipefail
cd "$(dirname "$0")/.."

NONE_ID="${1:-200001}"   # 种子 20 万条（id 1..200000），200001 必不存在

STATS_URL='http://127.0.0.1:8080/api/cache/stats'

# 用 python 精确提取嵌套 JSON 字段（比 grep 稳，避免正则歧义）
extract() {
    python3 -c '
import json, sys
d = json.load(sys.stdin)
bloom = d.get("bloom") or {}
print("enabled={} ready={} rejected={} hit={} miss={}".format(
    bloom.get("enabled"), bloom.get("ready"), bloom.get("rejected", 0),
    d.get("hit", 0), d.get("miss", 0)))
'
}

echo "== 0) 确认布隆已开启 =="
curl -s "$STATS_URL" | extract
ENABLED=$(curl -s "$STATS_URL" | python3 -c 'import json,sys; print((json.load(sys.stdin).get("bloom") or {}).get("enabled"))')
if [ "$ENABLED" != "True" ]; then
    echo "[ERROR] bloom_enabled=false。请把 config.json 的 cache.bloom_enabled 改为 true 并重启服务后重跑。"
    exit 1
fi

echo ""
echo "== 1) 重建布隆（POST /api/cache/warm rebuild_bloom=true）=="
curl -s -X POST 'http://127.0.0.1:8080/api/cache/warm' \
     -H 'Content-Type: application/json' -d '{"limit":1,"rebuild_bloom":true}'
echo ""
echo "重建后状态（ready 应为 True）："
curl -s "$STATS_URL" | extract

echo ""
echo "== 2) 记录基线 =="
BEFORE=$(curl -s "$STATS_URL" | extract)
echo "$BEFORE"

echo ""
echo "== 3) 连打 100 次不存在的 id=$NONE_ID =="
for _ in $(seq 1 100); do
    curl -s -o /dev/null "http://127.0.0.1:8080/api/seckill/$NONE_ID"
done
echo "完成 100 次"

sleep 0.5
echo ""
echo "== 4) 结果对比 =="
AFTER=$(curl -s "$STATS_URL" | extract)
echo "BEFORE: $BEFORE"
echo "AFTER : $AFTER"
echo ""
echo "判据：rejected 应 +100（全部被布隆挡下）；miss 应基本不变（没有 DB 回源）。"
