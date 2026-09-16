#!/usr/bin/env bash
# 5.4 延迟双删验证：临时开 double_delete_ms=1000 -> 下单一次 -> 验证二次 DEL 到点触发 -> 恢复配置
#
# 用法：bash scripts/verify-54-double-delete.sh
# 前置：MySQL/Redis 在跑、已 bash scripts/build-wsl.sh 编译、8080 未被占用。
#
# 为什么脚本自己改配置并在结束时恢复：5.4 的延迟双删默认关闭（见 PLAN ADR-8），
# 要验证它必须临时打开，而"忘了改回来"会污染后续基线压测——所以用 trap 兜底恢复。
set -uo pipefail
cd "$(dirname "$0")/.."

CONFIG="config.json"
BACKUP="$(mktemp)"
cp "$CONFIG" "$BACKUP"
SERVER_LOG="$PWD/logs/verify-54.log"

stop_server() { pkill -9 -f 'seckill-cpp' 2>/dev/null || true; }

start_server() {
    stop_server
    for _ in $(seq 1 50); do (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null || break; sleep 0.2; done
    nohup ./build/src/seckill-cpp > "$SERVER_LOG" 2>&1 < /dev/null &
    disown 2>/dev/null || true
    for _ in $(seq 1 60); do
        curl -s -o /dev/null "http://127.0.0.1:8080/api/health" 2>/dev/null && return 0
        sleep 0.2
    done
    echo "[ERROR] 服务未起来，日志尾部："
    tail -25 "$SERVER_LOG"
    return 1
}

# 退出时恢复配置并停掉测试用的服务实例
cleanup() {
    cp "$BACKUP" "$CONFIG"
    rm -f "$BACKUP"
    echo "[cleanup] config.json 已恢复（double_delete_ms 回到原值）"
}
trap cleanup EXIT

# 1) 临时打开延迟双删
python3 - "$CONFIG" <<'PY'
import json, sys
p = sys.argv[1]
with open(p) as f:
    cfg = json.load(f)
cfg["custom_config"]["cache"]["double_delete_ms"] = 1000
with open(p, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
print("[setup] double_delete_ms = 1000")
PY

# 2) 重启服务加载新配置
echo "== 启动服务 =="
start_server || exit 1

echo "== 确认配置已生效 =="
curl -s http://127.0.0.1:8080/api/cache/stats | grep -o '"double_delete_ms":[0-9]*'

# 3) 先读一次详情建立缓存（否则没有 key 可删，验证不出东西）
echo "== 预热详情缓存 =="
curl -s -o /dev/null http://127.0.0.1:8080/api/seckill/1
redis-cli EXISTS seckill:sku:v1:item:1

echo "== stats BEFORE 下单 =="
curl -s http://127.0.0.1:8080/api/cache/stats | grep -o '"delayed_delete":[0-9]*'

# 4) 下单（会触发一次立即 DEL + 一次延迟 DEL）
echo "== 下单一次 (userId=100001, skuId=1) =="
curl -s -X POST http://127.0.0.1:8080/api/seckill -H 'Content-Type: application/json' \
     -d '{"userId":100001,"skuId":1}'
echo
echo "== 下单后立刻看（延迟 DEL 还没到点）=="
curl -s http://127.0.0.1:8080/api/cache/stats | grep -o '"delayed_delete":[0-9]*'

# 5) 等过延迟窗口
echo "== 等待 2s（延迟 1000ms 到点）=="
sleep 2
echo "== 2s 后（delayed_delete 应 +1）=="
curl -s http://127.0.0.1:8080/api/cache/stats | grep -o '"delayed_delete":[0-9]*'

# 6) 确认 key 确实被二次删掉
echo "== key 是否已被删除（应为 0）=="
redis-cli EXISTS seckill:sku:v1:item:1

stop_server
echo ""
echo "验证完成。判据：delayed_delete 从 0 -> 1，且该 key EXISTS 由 1 -> 0。"
