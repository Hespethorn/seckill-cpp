#!/usr/bin/env bash
# 秒杀阶段一冒烟压测：并发抢购 + MySQL 验证不超卖
# 用法: bash scripts/smoke-seckill.sh [并发人数] [库存] [skuId]
# 示例: bash scripts/smoke-seckill.sh 100 10 1
#
# 流程:
#   1. 检查服务健康 (GET /api/health)
#   2. 重置库存为 N 件、清空订单 (制造干净的初始状态)
#   3. 并发 N 个不同用户抢购 (POST /api/seckill)
#   4. MySQL 核对: 卖出件数 == 订单数 && 订单数 <= 库存 (不超卖)
set -euo pipefail
cd "$(dirname "$0")/.."

CONCURRENCY="${1:-100}"
STOCK="${2:-10}"
SKU_ID="${3:-1}"
BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
# mysql 命令行传密码会打 "Using a password" 警告到 stderr, 统一丢弃
MYSQL=(mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill 2>/dev/null)

echo "==> [1/4] 检查服务健康..."
if curl -sf "${BASE_URL}/api/health" >/dev/null; then
    echo "    OK: ${BASE_URL} 可达"
else
    echo "    FAIL: 服务没起来! 先启动: ./build/src/seckill-cpp" >&2
    exit 1
fi

echo "==> [2/4] 重置库存到 ${STOCK} 件, 清空订单..."
"${MYSQL[@]}" -e "UPDATE seckill_sku SET stock=${STOCK}, total=${STOCK} WHERE id=${SKU_ID}; DELETE FROM seckill_order;"
echo "    当前:"
"${MYSQL[@]}" -e "SELECT id, name, stock, total FROM seckill_sku WHERE id=${SKU_ID};"

echo "==> [3/4] ${CONCURRENCY} 个用户并发抢购 (每人一个 userId)..."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
for i in $(seq 1 "$CONCURRENCY"); do
    (
        uid=$((2000 + i))   # 每个请求独立 userId, 排除重复下单干扰, 只测超卖
        curl -s -o "${TMP}/r_${i}.json" -w "%{http_code}" \
            -X POST "${BASE_URL}/api/seckill" \
            -H 'Content-Type: application/json' \
            -d "{\"userId\":${uid},\"skuId\":${SKU_ID}}" \
            > "${TMP}/c_${i}.code" 2>/dev/null
    ) &
done
wait

# 统计: 200=成功 / 409 SOLD_OUT=售罄 / 409 DUPLICATE=重复 / 其他=错误
OK=0; SOLD=0; DUP=0; ERR=0
for i in $(seq 1 "$CONCURRENCY"); do
    code=$(cat "${TMP}/c_${i}.code" 2>/dev/null || echo 000)
    case "$code" in
        200) OK=$((OK + 1)) ;;
        409)
            if grep -q "SOLD_OUT" "${TMP}/r_${i}.json" 2>/dev/null; then
                SOLD=$((SOLD + 1))
            else
                DUP=$((DUP + 1))
            fi
            ;;
        *) ERR=$((ERR + 1)) ;;
    esac
done
echo "    成功=${OK}  售罄=${SOLD}  重复=${DUP}  异常=${ERR}"

echo "==> [4/4] MySQL 验证不超卖..."
STOCK_LEFT=$("${MYSQL[@]}" -N -e "SELECT stock FROM seckill_sku WHERE id=${SKU_ID};" 2>/dev/null)
ORDER_CNT=$("${MYSQL[@]}" -N -e "SELECT COUNT(*) FROM seckill_order;" 2>/dev/null)
SOLD_CNT=$((STOCK - STOCK_LEFT))
echo "    初始库存=${STOCK}  剩余=${STOCK_LEFT}  卖出=${SOLD_CNT}  订单数=${ORDER_CNT}"

if [ "${SOLD_CNT}" -eq "${ORDER_CNT}" ] && [ "${ORDER_CNT}" -le "${STOCK}" ]; then
    echo "    ✅ 通过: 卖出件数 == 订单数 (${SOLD_CNT} 件), 未超卖"
else
    echo "    ❌ 失败: 卖出(${SOLD_CNT}) != 订单(${ORDER_CNT}) 或 订单 > 库存(${STOCK})" >&2
    exit 1
fi
