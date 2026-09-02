#!/usr/bin/env bash
# 4.8 应用层锁对比压测：none / mutex / spin / atomic × 两种场景
#
# 为什么要分两个场景——这是本脚本设计的重点：
#   unique 场景：每个用户只发一次请求，**没有重复**。此时应用层锁纯属额外开销。
#               它测的是"加了锁，代价有多大"。
#   dup    场景：每个用户并发发 K 次同一请求，**大量重复**。
#               此时锁能在 DB 之前挡掉重复请求，它测的是"加了锁，省了多少"。
#   只看其中任何一个都得不到结论：只看 unique 会得出"锁是负优化"，
#   只看 dup 会得出"锁是银弹"。两个一起看，才知道该在什么流量形态下开它。
#
# 用法: bash scripts/lock-bench.sh [每轮请求数] [并发数] [重复倍数]
#   例: bash scripts/lock-bench.sh 2000 100 4
set -uo pipefail

TOTAL="${1:-2000}"
CONC="${2:-100}"
DUP="${3:-4}"
BASE="http://127.0.0.1:8080"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

MODES=(none mutex spin atomic)
TMP="$(mktemp -d)"

# 跑完要把 config.json 的锁模式恢复原样：否则下一轮回来看见 mode=atomic，
# 以为是上次压测的结论，实际只是脚本改过没改回来。
ORIG_MODE=$(grep -o '"mode": "[a-z]*"' config.json | head -1 |
            sed 's/.*"mode": "\(.*\)"/\1/')
trap 'rm -rf "$TMP"; stop_server; set_mode "${ORIG_MODE:-none}"' EXIT
SRV_PID=""

my() { mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e "$1" 2>/dev/null; }

echo "==> 前置检查"
if [[ ! -x ./build/src/seckill-cpp ]]; then
    echo "  [ERROR] 找不到 ./build/src/seckill-cpp，先跑 bash scripts/build-wsl.sh"
    exit 1
fi
if ! my "SELECT 1" >/dev/null; then
    echo "  [ERROR] MySQL 连不上（seckill/seckill@127.0.0.1:3306）"
    exit 1
fi
if ! curl -s -o /dev/null http://127.0.0.1:6379 2>/dev/null && ! redis-cli ping 2>/dev/null | grep -q PONG; then
    echo "  [WARN] Redis 似乎没起 —— 秒杀不受影响，但登录接口会 503"
fi
echo "  可执行文件和 MySQL 就绪，原始锁模式=$ORIG_MODE"
echo

stop_server() {
    [[ -n "$SRV_PID" ]] && kill "$SRV_PID" 2>/dev/null
    [[ -n "$SRV_PID" ]] && wait "$SRV_PID" 2>/dev/null
    SRV_PID=""
}

start_server() {
    stop_server
    # 杀掉任何残留的 seckill-cpp（含手动起的）：否则新进程 bind 8080 失败，
    # 压测会打到旧进程，锁模式不切换，应用层挡掉恒为 0。
    pkill -f 'seckill-cpp' 2>/dev/null || true
    sleep 1
    ./build/src/seckill-cpp > "$TMP/server.log" 2>&1 &
    SRV_PID=$!
    for _ in $(seq 1 60); do
        if curl -s -o /dev/null "$BASE/api/health" 2>/dev/null; then
            return 0
        fi
        sleep 0.2
    done
    echo "  [ERROR] 服务起不来，日志尾部：" && tail -5 "$TMP/server.log"
    return 1
}

set_mode() {
    # config.json 里 "mode" 只出现一次（seckill_lock.mode），直接替换是安全的
    sed -i 's/"mode": "[a-z]*"/"mode": "'"$1"'"/' config.json
}

reset_data() {
    # 每轮都把库存和订单清干净：否则上一轮卖光的库存会让下一轮的
    # "售罄 409" 混进 "重复下单 409"，两类拒绝搅在一起就没法比了。
    my "UPDATE seckill_sku SET stock=100000000, total=100000000 WHERE id=1;
        DELETE FROM seckill_order;"
}

gen_ids() {
    local scene="$1" total="$2" dup="$3" out="$4"
    : > "$out"
    if [[ "$scene" == "unique" ]]; then
        seq 1 "$total" >> "$out"
    else
        local users=$((total / dup))
        ((users < 1)) && users=1
        local i j
        for ((i = 1; i <= users; i++)); do
            for ((j = 0; j < dup; j++)); do
                echo "$i" >> "$out"
            done
        done
    fi
}

# 跑一轮突发压测，输出 "elapsed ok409 other"
run_burst() {
    local scene="$1"
    gen_ids "$scene" "$TOTAL" "$DUP" "$TMP/ids"
    local n
    n=$(wc -l < "$TMP/ids")

    local start end
    start=$(date +%s.%N)
    # xargs -P 起固定并发；每个请求一个短命 curl，测的是服务端吞吐而非客户端能力
    xargs -a "$TMP/ids" -P "$CONC" -I{} \
        curl -s -o /dev/null -w '%{http_code}\n' \
             -X POST "$BASE/api/seckill" \
             -H 'Content-Type: application/json' \
             -d '{"userId":{},"skuId":1}' > "$TMP/codes" 2>/dev/null
    end=$(date +%s.%N)

    local ok dup409 other
    ok=$(grep -c '^200$' "$TMP/codes" || true)
    dup409=$(grep -c '^409$' "$TMP/codes" || true)
    other=$(grep -vc -e '^200$' -e '^409$' "$TMP/codes" || true)
    echo "$start $end $n ${ok:-0} ${dup409:-0} ${other:-0}"
}

echo "==> 4.8 应用层锁对比压测"
echo "    每轮请求数=$TOTAL  并发=$CONC  重复倍数=$DUP"
echo "    模式: ${MODES[*]}"
echo

printf '%-8s %-8s %-10s %-9s %-9s %-9s %-12s\n' \
       "模式" "场景" "QPS" "耗时s" "200" "409" "应用层挡掉"
printf '%s\n' "-------------------------------------------------------------------------"

declare -A QPS_UNIQUE QPS_DUP
declare -A REJ_DUP

for mode in "${MODES[@]}"; do
    set_mode "$mode"
    if ! start_server; then
        echo "  [ERROR] 模式 $mode 启动失败，跳过"
        continue
    fi
    # 确认服务确实读到了我们要的模式（防止 config 改错位置导致白跑一轮）
    actual=$(curl -s "$BASE/api/lock/stats" | grep -o '"mode":"[a-z]*"' | cut -d'"' -f4)
    if [[ "$actual" != "$mode" ]]; then
        echo "  [WARN] 期望模式 $mode，服务端实际为 ${actual:-unknown}"
        echo "           config.json 当前 mode: $(grep -o '\"mode\": \"[a-z]*\"' config.json | head -1)"
        echo "           server.log 尾部:"; tail -3 "$TMP/server.log"
    fi

    for scene in unique dup; do
        reset_data
        read -r start end n ok d409 other < <(run_burst "$scene")
        elapsed=$(awk -v a="$start" -v b="$end" 'BEGIN {d=b-a; print (d>0?d:0.001)}')
        qps=$(awk -v n="$n" -v t="$elapsed" 'BEGIN {printf "%.1f", n/t}')

        rejected=0
        if [[ "$scene" == "dup" ]]; then
            rejected=$(curl -s "$BASE/api/lock/stats" |
                       grep -o '"rejected":[0-9]*' | cut -d: -f2)
            REJ_DUP[$mode]="$rejected"
            QPS_DUP[$mode]="$qps"
        else
            QPS_UNIQUE[$mode]="$qps"
        fi

        printf '%-8s %-8s %-10s %-9s %-9s %-9s %-12s\n' \
               "$mode" "$scene" "$qps" "$elapsed" "$ok" "$d409" "$rejected"
    done
done
stop_server

echo
echo "==> 结论速读"
echo
echo "【unique 场景 · 锁的开销】没有重复请求时，锁纯粹是额外成本："
for mode in "${MODES[@]}"; do
    [[ -z "${QPS_UNIQUE[$mode]:-}" ]] && continue
    base="${QPS_UNIQUE[none]:-}"
    delta=""
    if [[ "$mode" != "none" && -n "$base" ]]; then
        delta=$(awk -v a="${QPS_UNIQUE[$mode]}" -v b="$base" \
                'BEGIN {if (b>0) printf "  (相对 none: %+.1f%%)", (a-b)/b*100; else print ""}')
    fi
    printf '    %-8s QPS=%-10s%s\n' "$mode" "${QPS_UNIQUE[$mode]}" "$delta"
done

echo
echo "【dup 场景 · 锁的收益】大量并发重复时，锁能在打 DB 之前就挡掉："
for mode in "${MODES[@]}"; do
    [[ -z "${QPS_DUP[$mode]:-}" ]] && continue
    base="${QPS_DUP[none]:-}"
    delta=""
    if [[ "$mode" != "none" && -n "$base" ]]; then
        delta=$(awk -v a="${QPS_DUP[$mode]}" -v b="$base" \
                'BEGIN {if (b>0) printf "  (相对 none: %+.1f%%)", (a-b)/b*100; else print ""}')
    fi
    printf '    %-8s QPS=%-10s 应用层挡掉=%-8s%s\n' \
           "$mode" "${QPS_DUP[$mode]}" "${REJ_DUP[$mode]:-0}" "$delta"
done

echo
echo "注：dup 场景里 none 模式的 409 全部由 MySQL 唯一键兜底产生（每个都要真打一次 DB）；"
echo "    开了锁之后，一部分 409 变成应用层直接返回（见"应用层挡掉"列），DB 压力随之下降。"
echo "    atomic 模式是位图，存在 hash 冲突导致的误挡——它最快，但会误杀一小部分正常请求。"
