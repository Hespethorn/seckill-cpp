#!/usr/bin/env bash
# 第五章 5.1 读接口基线压测 + 5.2 / 5.3 缓存收益量化
#
# 为什么要单独写这个脚本（而不是复用 jmeter-baseline.sh）：
#   那个脚本测的是**写**接口（下单——行锁串行化是瓶颈），这一个测的是**读**接口
#   （商品列表 / 商品详情——重复查询是瓶颈）。两者的瓶颈、优化手段、预期倍数都不同，
#   混在一个脚本里只会得到两条互相污染的曲线。
#
# 核心设计：**同一套流量，跑两轮，只改一个变量**。
#   off 轮：config.json 里 cache.enabled=false —— 每次读都直连 MySQL，这就是 5.1 的"接口基线"。
#   on  轮：cache.enabled=true  —— 走 Redis，命中就不碰数据库。
#   其余（并发、时长、机器、数据量、代码）全部不变，这样"提升倍数"才有意义。
#   一次改一堆东西再比 QPS，得出的数字不能写进任何结论。
#
# 两个接口的并发按 60% / 40% 分配（列表 60、详情 40，总量由第一个参数控制）：
#   列表是聚合查询（20 行），详情是单行点查，两者的绝对 QPS 本就不该直接比较；
#   有意义的是**同一个接口 off→on 的倍数**，以及两者在总流量里的占比。
#
# 用法（WSL / Git Bash 里用 bash 跑，别直接 ./xxx.sh —— 本机 .sh 被关联到 Node）：
#   bash scripts/read-bench.sh              # 默认总并发 100 × 60s
#   bash scripts/read-bench.sh 200 30       # 总并发 200 × 30s
#
# 压测引擎：
#   1) 设了 JMETER_BIN 且指向官方 Apache JMeter → 走 jmeter/read-baseline.jmx，
#      出 HTML 报告 + jmeter/out/read-aggregate.csv（list / detail 分开统计）。
#        export JMETER_BIN=/opt/apache-jmeter-5.6.3/bin/jmeter
#   2) 否则回退零依赖 curl 并发 harness（无需任何 JMeter）。
# 注意：绝不能用 `apt install jmeter`（老/缝合怪包，加载 .jmx 报 xstream
#       ForbiddenClassException 且不认 -e -o），请用官网二进制。
#
# 前置：MySQL + Redis 都在跑、已编译、已导入 sql/seed_sku.sql（20 条商品）。
set -uo pipefail

CONC="${1:-100}"
DUR="${2:-60}"
BASE="http://127.0.0.1:8080"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SRV_LOG="$ROOT/read-bench-server.log"
TMP="$(mktemp -d)"
# JMeter 的 -e -o 在报告目录里真正生成的是 statistics.json（实测 5.6.3 用 JsonExporter
# 写的是 JSON，不是 statistics.csv；那个 csv 需手动另跑命令才有）。之前写死 .csv 导致
# 这里永远判"缺失"→ 已跑完的几十万样本被丢弃并回退 curl 重跑一轮。改读 json。
AGG="$ROOT/jmeter/out/read-report/statistics.json"

CONC_LIST=$(( CONC * 60 / 100 ))
CONC_DETAIL=$(( CONC - CONC_LIST ))
[[ "$CONC_LIST" -lt 1 ]] && CONC_LIST=1
[[ "$CONC_DETAIL" -lt 1 ]] && CONC_DETAIL=1

ENGINE="curl"
JMETER_REQUESTED=0
if [[ -n "${JMETER_BIN:-}" && -x "$JMETER_BIN" && -f jmeter/read-baseline.jmx ]]; then
    ENGINE="jmeter"
    JMETER_REQUESTED=1
fi

my() { mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -N -B -e "$1" 2>/dev/null; }

# 从 /api/cache/stats 的单行 JSON 里取一个字段（读 stdin）
jget() { grep -o "\"$1\":[^,}]*" | head -1 | sed 's/.*:[[:space:]]*//; s/"//g'; }

# 跑完必须把 cache.enabled 恢复原样：否则下次看压测结果时会以为"缓存一直开着"
ORIG_CACHE=$(awk '/"cache":[[:space:]]*\{/{inc=1}
                  inc && /"enabled":[[:space:]]*(true|false)/{
                    if ($0 ~ /true/) print "true"; else print "false"; exit}' config.json)
trap 'stop_server; set_cache "${ORIG_CACHE:-true}" >/dev/null 2>&1; rm -rf "$TMP"' EXIT

echo "==> 前置检查"
[[ ! -x ./build/src/seckill-cpp ]] && { echo "  [ERROR] 找不到 ./build/src/seckill-cpp，先跑 bash scripts/build-wsl.sh"; exit 1; }
my "SELECT 1" >/dev/null || { echo "  [ERROR] MySQL 连不上（seckill/seckill@127.0.0.1:3306）"; exit 1; }
if ! redis-cli ping 2>/dev/null | grep -q PONG; then
  echo "  [ERROR] Redis 没起 —— 本脚本测的就是 Redis 缓存：sudo service redis-server start"
  exit 1
fi
SKU_COUNT=$(my "SELECT COUNT(*) FROM seckill_sku")
# 一个保证"不存在"的 id：商品 id 连续 1..SKU_COUNT，+1 必超出范围。
# 空值哨兵（防穿透）验证用它，任何数据量级下都合法（不会像写死的 999999 在 100 万量级时反而成了真 id）。
NONE_ID=$(( SKU_COUNT + 1 ))
if [[ -z "${SKU_COUNT:-0}" || "$SKU_COUNT" -lt 20 ]]; then
  echo "  [ERROR] 商品只有 ${SKU_COUNT:-0} 条，压测需要 20 条（否则详情随机 id 大量 404）："
  echo "          mysql -h127.0.0.1 -useckill -pseckill seckill < sql/seed_sku.sql"
  exit 1
fi
echo "  总并发=$CONC（列表 $CONC_LIST / 详情 $CONC_DETAIL） 时长=${DUR}s 商品=$SKU_COUNT 条 引擎=$ENGINE"
echo

stop_server() { pkill -9 -f 'seckill-cpp' 2>/dev/null || true; }

start_server() {
    stop_server
    # 等 8080 真正空闲（监听消失）再起新进程。用 /dev/tcp 探活比 /api/health 准：
    # Drogon 优雅关闭期间 health 可能仍响应，但监听端口会先关。
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
    echo "  [ERROR] 服务起不来（启动即退），日志尾部：" && tail -25 "$SRV_LOG"
    return 1
}

# 只改 cache 块里的 enabled：先定位 "cache": { 再改它后面第一个 enabled。
# 全局 sed 's/"enabled": true/false/' 是这类脚本最常见的坑 —— 一旦将来某个模块
# 也用 enabled 字段，脚本就会静默地改错地方，而压测数字看起来"完全正常"。
set_cache() {
    awk -v v="$1" '
      /"cache":[[:space:]]*\{/ {inc=1}
      inc && /"enabled":[[:space:]]*(true|false)/ { sub(/(true|false)/, v); inc=0 }
      {print}
    ' config.json > config.json.tmp && mv config.json.tmp config.json
}

# 清掉本项目的缓存 key（用 SCAN 不用 KEYS：本地无所谓，但 KEYS 在生产是禁用的，
# 习惯要从压测脚本开始养成）
purge_cache() {
    redis-cli --scan --pattern 'seckill:sku:v1:*' 2>/dev/null |
      while read -r k; do [[ -n "$k" ]] && redis-cli DEL "$k" >/dev/null 2>&1; done
    return 0
}

# 预热：on 轮先把缓存填上，测的才是"稳态命中"而不是"冷启动 + 稳态"的混合体。
# 两种口径都合法，但**必须在报告里写清是哪一个**，否则数字无法复现。
# 商品量大（默认 20 万）时不能全预热——只随机采样前 $WARMUP_CAP 个 key 填满，
# 剩下的在压测中自然命中并回写。随机采样而非顺序前 N 个，更贴近真实热点/长尾分布。
# 注意：WARMUP_CAP 相对总 key 数越小，稳态命中率会越低（更真实），报告要写清口径。
warmup() {
    local cap=2000
    local n=$SKU_COUNT
    [[ "$n" -lt "$cap" ]] && cap=$n
    local i k
    for i in $(seq 1 "$cap"); do
        k=$(( (RANDOM % SKU_COUNT) + 1 ))
        curl -s -o /dev/null "$BASE/api/seckill/list"
        curl -s -o /dev/null "$BASE/api/seckill/$k"
    done
}

# 原始耗时数据 -> "样本数 QPS avg(ms) p50(ms) p95(ms) p99(ms) 错误数"
# 字段顺序与 JMeter 聚合 CSV 的解析结果对齐，后面的表格才能用同一段代码打印
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

run_curl_round() {
    # 随机 skuId 序列（1..SKU_COUNT），撑满时长用
    awk -v n=500000 -v m="$SKU_COUNT" 'BEGIN{srand(); for(i=0;i<n;i++) printf "%d\n", int(rand()*m)+1}' > "$TMP/ids"
    : > "$TMP/list.raw"; : > "$TMP/detail.raw"

    local s e elapsed
    s=$(date +%s.%N)
    # 两个接口并行跑，与 JMeter 的双线程组模型保持一致（串行会让每个接口独占客户端，
    # 与 JMeter 结果不可比）
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

    R_LIST=$(summarize_raw "$TMP/list.raw" "$elapsed" "$(count_err "$TMP/list.raw")")
    R_DETAIL=$(summarize_raw "$TMP/detail.raw" "$elapsed" "$(count_err "$TMP/detail.raw")")
}

run_jmeter_round() {
    # 与 jmeter-baseline.sh 相同的坑：Java 会把本机 http_proxy 当 http.proxyHost，
    # 把请求路由到代理 → 100% connection refused。HC4 初始化失败则在 .jmx 里已绕过。
    unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY 2>/dev/null || true
    # 关键补刀：本机若通过 JAVA_TOOL_OPTIONS / _JAVA_OPTIONS 注入 -Dhttp.proxyHost，
    # 上面的 env unset 根本够不着（那是 JVM 启动参数，不是 shell 环境变量）。
    # 不剥掉它，Java 实现的采样器仍会把请求路由到代理端口 → 全部 connection refused → 0 样本。
    unset JAVA_TOOL_OPTIONS _JAVA_OPTIONS 2>/dev/null || true
    export JVM_ARGS="${JVM_ARGS:-} -Dhttp.proxyHost= -Dhttps.proxyHost= -Dhttp.proxyPort= -Dhttps.proxyPort= -Djava.net.useSystemProxies=false"
    mkdir -p jmeter/out
    rm -f "$AGG" jmeter/out/read-results.csv
    rm -rf jmeter/out/read-report
    # 详情随机上界从 .jmx 里写死的 20 改成实际商品数，与 curl harness 同口径
    # （商品量默认 20 万时这点至关重要，否则只打 1..20 那 20 个 key，测的是热点 key）。
    # 不污染仓库里的 .jmx，用临时副本 sed 注入（SKU_COUNT 是 shell 变量，JMeter 不认 ${}）。
    local jmx_tmp
    jmx_tmp=$(mktemp --tmpdir="$TMP" read-baseline.XXXXXX.jmx)
    cp jmeter/read-baseline.jmx "$jmx_tmp"
    sed -i "s/__Random(1,20,)/__Random(1,${SKU_COUNT},)/g" "$jmx_tmp"
    if ! "$JMETER_BIN" -n -t "$jmx_tmp" \
            -l jmeter/out/read-results.csv -e -o jmeter/out/read-report \
            -j jmeter/out/read-jmeter.log >/dev/null 2>&1; then
        echo "  [WARN] JMeter 退出码非 0，看 jmeter/out/read-jmeter.log"
    fi
    if [[ ! -s "$AGG" ]]; then
        # 没生成 statistics.json 通常是 JMeter 0 样本（代理把请求路由走 / 采样器没起）。
        # 打出 results.csv 的真实样本数，省得用户对着"回退"二字发懵。
        local sc=0
        [[ -f jmeter/out/read-results.csv ]] && \
            sc=$(grep -cE '^[0-9]' jmeter/out/read-results.csv 2>/dev/null || true)
        echo "  [WARN] 没拿到聚合报告（statistics.json 缺失/空，results.csv 样本数=$sc），回退 curl harness"
        if [[ "${sc:-0}" -eq 0 ]]; then
            echo "         样本数=0 且 JMETER_BIN 已设 → 查 jmeter/out/read-jmeter.log（多为 WSL 代理/JAVA_TOOL_OPTIONS）"
        fi
        ENGINE="curl"
        run_curl_round
        return
    fi
    # statistics.json 里每个接口一个扁平块：transaction / sampleCount / errorPct /
    # meanResTime / medianResTime / pct1..pct3(90/95/99) / throughput。transactions 块是
    # 汇总丢弃。输出字段与 curl 的 summarize_raw 完全对齐（样本 QPS avg p50 p95 p99 错误%），
    # 其中 pct2=95th、pct3=99th，errorPct 为百分比（与 curl 的错误计数口径不同，表格已注明）。
    awk '
        NR==1{tx=""; n=qps=avg=p50=p95=p99=err=""}
        function clean(x){ sub(/^[ \t]*"[^"]*"[ \t]*:[ \t]*/,"",x); gsub(/[",]/,"",x); gsub(/^[ \t]+|[ \t]+$/,"",x); return x }
        {
            if ($0 ~ /^[ \t]*"transaction"/)  { if (tx!="") emit(); tx=clean($0); n=qps=avg=p50=p95=p99=err=""; next }
            if ($0 ~ /"sampleCount"/)   n=clean($0)
            else if ($0 ~ /"throughput"/)   qps=clean($0)
            else if ($0 ~ /"meanResTime"/)  avg=clean($0)
            else if ($0 ~ /"medianResTime"/)p50=clean($0)
            else if ($0 ~ /"pct2ResTime"/)  p95=clean($0)
            else if ($0 ~ /"pct3ResTime"/)  p99=clean($0)
            else if ($0 ~ /"errorPct"/)     err=clean($0)
        }
        function emit(){ if (tx=="" || tx=="Total") return; printf "%s|%s %.1f %.2f %.2f %.2f %.2f %s\n", (tx ~ /list/ ? "list":"detail"), n, qps, avg, p50, p95, p99, err; tx="" }
        END{ emit() }
    ' "$AGG" > "$TMP/agg.parsed"
    R_LIST=$(awk -F'|' '/^list/{print $2}' "$TMP/agg.parsed" | head -1)
    R_DETAIL=$(awk -F'|' '/^detail/{print $2}' "$TMP/agg.parsed" | head -1)
}

declare -A RES
cache_stats() { curl -s "$BASE/api/cache/stats" 2>/dev/null; }

run_round() {
    local enabled="$1" label="$2"
    echo "==> 轮次：$label（cache.enabled=$enabled）"
    set_cache "$enabled"
    purge_cache
    start_server || return 1

    # 确认服务端真的读到了我们要的开关。改错位置 / 没重启都会在这里暴露，
    # 而不是等到最后看着"两轮数字一模一样"发呆。
    local actual
    actual=$(cache_stats | jget enabled)
    if [[ "$actual" != "$enabled" ]]; then
        echo "  [WARN] 期望 enabled=$enabled，服务端实际为 ${actual:-unknown}"
        echo "         config.json: $(awk '/"cache":[[:space:]]*\{/{inc=1} inc&&/enabled/{print; exit}' config.json)"
        tail -3 "$SRV_LOG"
    fi

    [[ "$enabled" == "true" ]] && warmup

    local before after
    before=$(cache_stats)
    if [[ "$ENGINE" == "jmeter" ]]; then run_jmeter_round; else run_curl_round; fi
    after=$(cache_stats)

    RES["$label:list"]="$R_LIST"
    RES["$label:detail"]="$R_DETAIL"
    RES["$label:before"]="$before"
    RES["$label:after"]="$after"

    if [[ "$enabled" == "true" ]]; then
        # 空值缓存验证（5.6 防穿透）：查一个不存在的 id（NONE_ID = SKU_COUNT+1，必超范围），看 Redis 是否留哨兵
        curl -s -o /dev/null "$BASE/api/seckill/$NONE_ID" 2>/dev/null
        sleep 0.3
        RES["on:null_value"]=$(redis-cli GET "seckill:sku:v1:item:$NONE_ID" 2>/dev/null)
        RES["on:null_ttl"]=$(redis-cli TTL "seckill:sku:v1:item:$NONE_ID" 2>/dev/null)
        RES["on:list_key_ttl"]=$(redis-cli TTL 'seckill:sku:v1:list' 2>/dev/null)
    fi

    # 服务还活着吗？压测途中崩溃会把"缓存效果好"变成"进程死了所以没人应答"
    curl -s -o /dev/null "$BASE/api/health" 2>/dev/null || {
        echo "  [ERROR] 压测途中 seckill-cpp 崩溃，日志尾部："; tail -25 "$SRV_LOG"; return 1; }
    echo
}

# ── 执行两轮 ──────────────────────────────────────────────────────────
run_round false "off" || exit 1
run_round true  "on"  || exit 1
stop_server

echo "===== 读接口：基线（off）vs 缓存（on）====="
echo "    总并发=$CONC  时长=${DUR}s  商品=$SKU_COUNT 条  引擎=$ENGINE"
printf '%-8s %-6s %-9s %-10s %-9s %-9s %-9s %-9s %-8s\n' \
       "接口" "缓存" "样本" "QPS" "avg(ms)" "p50(ms)" "p95(ms)" "p99(ms)" "错误"
printf '%s\n' "-----------------------------------------------------------------------------------"
for iface in list detail; do
    for r in off on; do
        read -r n qps avg p50 p95 p99 err <<< "${RES["$r:$iface"]:-0 0 0 0 0 0 0}"
        printf '%-8s %-6s %-9s %-10s %-9s %-9s %-9s %-9s %-8s\n' \
            "$iface" "$r" "${n:-0}" "${qps:-0}" "${avg:-0}" "${p50:-0}" "${p95:-0}" "${p99:-0}" "${err:-0}"
    done
done

echo
echo "===== 提升倍数（on 相对 off）====="
for iface in list detail; do
    q1=$(awk '{print $2}' <<< "${RES["off:$iface"]:-0 0}")
    q2=$(awk '{print $2}' <<< "${RES["on:$iface"]:-0 0}")
    awk -v a="${q2:-0}" -v b="${q1:-0}" -v i="$iface" 'BEGIN{
        if (b>0) printf "  %-8s QPS  %-10s -> %-10s   ×%.2f   (%+.0f%%)\n", i, b, a, a/b, (a-b)/b*100;
        else     printf "  %-8s 基线 QPS 为 0，无法计算倍数\n", i }'
done

echo
echo "===== 缓存命中率（on 轮压测期间的增量）====="
b_hit=$(echo "${RES["on:before"]}" | jget hit); a_hit=$(echo "${RES["on:after"]}" | jget hit)
b_mis=$(echo "${RES["on:before"]}" | jget miss); a_mis=$(echo "${RES["on:after"]}" | jget miss)
e0=$(echo "${RES["on:before"]}" | jget err);    e1=$(echo "${RES["on:after"]}" | jget err)
awk -v h="$a_hit" -v h0="$b_hit" -v m="$a_mis" -v m0="$b_mis" -v e="$e1" -v e0="$e0" 'BEGIN{
    dh=h-h0; dm=m-m0; de=e-e0; t=dh+dm
    printf "  hit=%-8d miss=%-8d err=%-6d 命中率=%s\n", dh, dm, de, (t>0? sprintf("%.1f%%", dh/t*100) : "n/a")}'
echo "  口径：miss = 回源 DB 的次数；err = Redis 异常次数（正常应为 0）"

echo
echo "===== 空值缓存（5.6 防穿透）验证 ====="
echo "  /api/seckill/$NONE_ID 查询后 Redis 里的值: '${RES["on:null_value"]:-<空>}'  TTL=${RES["on:null_ttl"]:-n/a}s"
if [[ "${RES["on:null_value"]:-}" == "__nil__" ]]; then
    echo "  → 空值哨兵已写入：随机不存在的 id 在 TTL 内只会回源一次"
else
    echo "  → 未拿到哨兵（缓存未开 / key 前缀不一致 / Redis 被清）"
fi
echo "  列表 key 剩余 TTL: ${RES["on:list_key_ttl"]:-n/a}s（基准 30s + 抖动，说明抖动生效）"

echo
if [[ "$ENGINE" == "jmeter" ]]; then
    echo "HTML 报告: jmeter/out/read-report/index.html   聚合 CSV: $AGG"
elif [[ "$JMETER_REQUESTED" -eq 1 ]]; then
    # 用户明明设了 JMETER_BIN，却因 0 样本回退了——这种情形下还写
    # "export JMETER_BIN 可出报告" 是自相矛盾的，改为如实说明并指日志。
    echo "JMeter 已设但未产出聚合（已回退 curl harness），HTML 报告未生成。"
    echo "要拿 HTML 报告：查 jmeter/out/read-jmeter.log 解决 0 样本（多为 WSL 代理/JAVA_TOOL_OPTIONS），"
    echo "重跑即出：bash scripts/read-bench.sh"
else
    echo "提示：export JMETER_BIN=/opt/apache-jmeter-5.6.3/bin/jmeter 可出 HTML 报告"
fi
echo "缓存开关已恢复为 cache.enabled=$ORIG_CACHE"
echo "坑位提醒：若两轮 QPS 完全一样，先确认 /api/cache/stats 的 enabled 是否真的跟着变了"
echo "          （改了 config 没重启 = 白跑一轮；残留 InnoDB 事务持锁也会让数字失真）"
