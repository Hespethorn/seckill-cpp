#!/usr/bin/env bash
# 5.8 多级缓存（本地 LRU L1）收益量化：同一套流量跑两轮，只改 cache.local_enabled。
#
#   on    轮：cache.enabled=true、local_enabled=false —— 只有 Redis 二级（L2）
#   local 轮：cache.enabled=true、local_enabled=true  —— Redis + 本地 LRU（L1+L2）
#
# 压测口径（与 read-bench.sh 的"全量随机长尾"刻意不同）：
#   详情只打 1..HOT 的热点子集（默认 2000，与预热范围一致）——真实秒杀就是少数爆品
#   被几万人刷，L1 缓存的意义正在于此；若像 read-bench 那样打全 20 万随机 id，
#   4096 容量的本地 LRU 命中率趋零，测出来"L1 没用"，那不是 L1 没用，是场景不对。
#   列表仍是单 key（所有用户刷同一份列表），天然是 L1 的最佳对象。
#
# 用法（WSL 里 bash scripts/local-bench.sh，别直接 ./xxx.sh）：
#   bash scripts/local-bench.sh            # 总并发 100 × 20s × 2 轮
#   bash scripts/local-bench.sh 200 30     # 总并发 200 × 30s
#   bash scripts/local-bench.sh 100 20 5000  # 热点子集调成 1..5000
#
# 前置：MySQL + Redis 在跑、已编译、已 seed（商品数 ≥ HOT）。
# 跑完自动把 local_enabled 恢复原值。
set -uo pipefail

CONC="${1:-100}"
DUR="${2:-20}"
HOT="${3:-2000}"        # 详情随机上界 = 热点子集大小（默认 2000，预热同范围）
BASE="http://127.0.0.1:8080"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SRV_LOG="$ROOT/local-bench-server.log"
TMP="$(mktemp -d)"

CONC_LIST=$(( CONC * 60 / 100 ))
CONC_DETAIL=$(( CONC - CONC_LIST ))
[[ "$CONC_LIST" -lt 1 ]] && CONC_LIST=1
[[ "$CONC_DETAIL" -lt 1 ]] && CONC_DETAIL=1

my() { mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -N -B -e "$1" 2>/dev/null; }
jget() { grep -o "\"$1\":[^,}]*" | head -1 | sed 's/.*:[[:space:]]*//; s/"//g'; }

ORIG_LOCAL=$(awk '/"cache":[[:space:]]*\{/{inc=1}
                  inc && /"local_enabled":[[:space:]]*(true|false)/{
                    if ($0 ~ /true/) print "true"; else print "false"; exit}' config.json)
trap 'stop_server; set_field "local_enabled" "${ORIG_LOCAL:-false}" >/dev/null 2>&1; rm -rf "$TMP"' EXIT

echo "==> 前置检查"
[[ ! -x ./build/src/seckill-cpp ]] && { echo "  [ERROR] 找不到 ./build/src/seckill-cpp，先跑 bash scripts/build-wsl.sh"; exit 1; }
my "SELECT 1" >/dev/null || { echo "  [ERROR] MySQL 连不上（seckill/seckill@127.0.0.1:3306）"; exit 1; }
redis-cli ping 2>/dev/null | grep -q PONG || { echo "  [ERROR] Redis 没起：sudo service redis-server start"; exit 1; }
SKU_COUNT=$(my "SELECT COUNT(*) FROM seckill_sku")
[[ -z "${SKU_COUNT:-0}" || "$SKU_COUNT" -lt "$HOT" ]] && { echo "  [ERROR] 商品数 ${SKU_COUNT:-0} < 热点子集 $HOT，先 seed（sql/seed_sku.sql）或调小第 3 参数"; exit 1; }
echo "  总并发=$CONC（列表 $CONC_LIST / 详情 $CONC_DETAIL） 时长=${DUR}s 热点子集=1..$HOT 商品=$SKU_COUNT 条"
echo

stop_server() { pkill -9 -f 'seckill-cpp' 2>/dev/null || true; }

start_server() {
    stop_server
    for _ in $(seq 1 50); do
        (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null || break
        sleep 0.2
    done
    ./build/src/seckill-cpp > "$SRV_LOG" 2>&1 < /dev/null &
    disown 2>/dev/null || true
    for _ in $(seq 1 60); do
        curl -s -o /dev/null "$BASE/api/health" 2>/dev/null && return 0
        sleep 0.2
    done
    echo "  [ERROR] 服务起不来，日志尾部：" && tail -25 "$SRV_LOG"
    return 1
}

# 只改 cache 块里的 cache.enabled / local_enabled（照抄 read-bench.sh 的防误改定位法）
set_field() {
    awk -v f="$1" -v v="$2" '
      /"cache":[[:space:]]*\{/ {inc=1}
      inc && index($0, "\"" f "\"") && $0 ~ f ".*:" { sub(/[^:]*:[[:space:]]*[^,]*/, "\"" f "\": " v); inc=0 }
      {print}
    ' config.json > config.json.tmp && mv config.json.tmp config.json
}

purge_cache() {
    redis-cli --scan --pattern 'seckill:sku:v1:*' 2>/dev/null |
      while read -r k; do [[ -n "$k" ]] && redis-cli DEL "$k" >/dev/null 2>&1; done
    return 0
}

# 用 5.5 预热端点填缓存（比 read-bench 的 curl 连打预热更可控：detail 范围 = 热点子集）
warmup() {
    curl -s -X POST "$BASE/api/cache/warm" -H 'Content-Type: application/json' \
         -d "{\"limit\":$HOT}" >/dev/null 2>&1
    sleep 0.5   # warm 的回写是 fire-and-forget，等异步 SETEX 落地
}

summarize_raw() {
    awk 'NF>=2{print $2}' "$1" | sort -n | awk -v e="$2" -v err="$3" '
      {a[NR]=$1}
      END {
        c=NR
        if (c==0) {printf "0 0 0 0 0 0 0\n"; exit}
        s=0; for(i=1;i<=c;i++) s+=a[i]; avg=s/c
        i50=int(c*0.50); i95=int(c*0.95); i99=int(c*0.99)
        if(i50<1)i50=1; if(i95<1)i95=1; if(i99<1)i99=1
        printf "%d %.1f %.1f %.1f %.1f %.1f %s\n", c, c/e, avg*1000, a[i50]*1000, a[i95]*1000, a[i99]*1000, err
      }'
}
count_err() { grep -vcE '^200 ' "$1" 2>/dev/null || echo 0; }

run_round() {
    local enabled="$1" localOn="$2" label="$3"
    echo "==> 轮次：$label（cache.enabled=$enabled, local_enabled=$localOn）"
    set_field "enabled" "$enabled"
    set_field "local_enabled" "$localOn"
    purge_cache
    start_server || return 1
    # 确认服务端真的读到了开关（改错字段 / 没重启都会在这里暴露）
    local actual
    actual=$(curl -s "$BASE/api/cache/stats" | jget enabled)
    [[ "$actual" != "$enabled" ]] && { echo "  [WARN] 期望 enabled=$enabled 实际 ${actual:-unknown}"; tail -3 "$SRV_LOG"; }
    warmup

    local before after
    before=$(curl -s "$BASE/api/cache/stats")

    awk -v n=300000 -v m="$HOT" 'BEGIN{srand(); for(i=0;i<n;i++) printf "%d\n", int(rand()*m)+1}' > "$TMP/ids"
    : > "$TMP/list.raw"; : > "$TMP/detail.raw"
    local s e elapsed
    s=$(date +%s.%N)
    seq 1 1000000 | timeout "$DUR" xargs -P "$CONC_LIST" -I{} \
        curl -s -o /dev/null -w "%{http_code} %{time_total}\n" "$BASE/api/seckill/list" \
        >> "$TMP/list.raw" 2>/dev/null &
    local lp=$!
    timeout "$DUR" xargs -a "$TMP/ids" -P "$CONC_DETAIL" -I{} \
        curl -s -o /dev/null -w "%{http_code} %{time_total}\n" "$BASE/api/seckill/{}" \
        >> "$TMP/detail.raw" 2>/dev/null &
    local dp=$!
    wait "$lp"; wait "$dp"
    e=$(date +%s.%N)
    elapsed=$(awk -v a="$s" -v b="$e" 'BEGIN{d=b-a; print (d>0?d:0.001)}')
    after=$(curl -s "$BASE/api/cache/stats")

    local rl rd
    rl=$(summarize_raw "$TMP/list.raw" "$elapsed" "$(count_err "$TMP/list.raw")")
    rd=$(summarize_raw "$TMP/detail.raw" "$elapsed" "$(count_err "$TMP/detail.raw")")
    RES["$label:list"]="$rl"
    RES["$label:detail"]="$rd"
    RES["$label:before"]="$before"
    RES["$label:after"]="$after"
    curl -s -o /dev/null "$BASE/api/health" || { echo "  [ERROR] 压测途中崩溃，日志尾部："; tail -25 "$SRV_LOG"; return 1; }
    echo
}

declare -A RES
run_round true false "on"    || exit 1
run_round true true  "local" || exit 1
stop_server

echo "===== 多级缓存：Redis（on）vs 本地LRU+Redis（local）====="
echo "    总并发=$CONC  时长=${DUR}s  热点子集=1..$HOT  引擎=curl"
printf '%-8s %-7s %-9s %-9s %-9s %-9s %-9s %-9s %-8s\n' \
       "接口" "缓存" "样本" "QPS" "avg(ms)" "p50(ms)" "p95(ms)" "p99(ms)" "错误"
printf '%s\n' "-----------------------------------------------------------------------------"
for iface in list detail; do
    for r in on local; do
        read -r n qps avg p50 p95 p99 err <<< "${RES["$r:$iface"]:-0 0 0 0 0 0 0}"
        printf '%-8s %-7s %-9s %-9s %-9s %-9s %-9s %-9s %-8s\n' \
            "$iface" "$r" "${n:-0}" "${qps:-0}" "${avg:-0}" "${p50:-0}" "${p95:-0}" "${p99:-0}" "${err:-0}"
    done
done

echo
echo "===== 提升倍数（local 相对 on）====="
for iface in list detail; do
    q1=$(awk '{print $2}' <<< "${RES["on:$iface"]:-0 0}")
    q2=$(awk '{print $2}' <<< "${RES["local:$iface"]:-0 0}")
    awk -v a="${q2:-0}" -v b="${q1:-0}" -v i="$iface" 'BEGIN{
        if (b>0) printf "  %-8s QPS %-10s -> %-10s  ×%.2f  (%+.0f%%)\n", i, b, a, a/b, (a-b)/b*100;
        else     printf "  %-8s on 轮 QPS 为 0，无法计算\n", i }'
done

echo
echo "===== 命中结构与 L1 分担（压测期间增量）====="
for r in on local; do
    b=$(echo "${RES["$r:before"]}" | jget hit); a=$(echo "${RES["$r:after"]}" | jget hit)
    bm=$(echo "${RES["$r:before"]}" | jget miss); am=$(echo "${RES["$r:after"]}" | jget miss)
    bh=$(echo "${RES["$r:before"]}" | jget local_hit); ah=$(echo "${RES["$r:after"]}" | jget local_hit)
    awk -v r="$r" -v h="$a" -v h0="$b" -v m="$am" -v m0="$bm" -v l="$ah" -v l0="$bh" 'BEGIN{
        dh=h-h0; dm=m-m0; dl=l-l0; t=dh+dm
        printf "  %-5s hit=%-9d (L1 local_hit=%-8d)  miss=%-8d 命中率=%s\n",
               r, dh, dl, dm, (t>0? sprintf("%.1f%%", dh/t*100) : "n/a")}'
done
echo "  口径：miss = 回源 DB 次数（两轮应接近，L1 不改变回源语义）；local_hit = L1 挡下的读"
echo "  看 L1 收益：同流量下 local 轮 QPS 应更高 / avg·p50 更低，且 hit ≈ local_hit + Redis命中"

echo
echo "缓存开关已恢复：local_enabled=$ORIG_LOCAL"
