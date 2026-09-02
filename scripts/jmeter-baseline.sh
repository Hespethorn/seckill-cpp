#!/usr/bin/env bash
# 阶段一基线压测（对应博客 4.4 / 4.5 / 4.7）
# 前置：MySQL 在跑 + seckill-cpp 已编译并在 :8080 运行。
# 用法（在 WSL / Git Bash 里用 `bash scripts/jmeter-baseline.sh`，别直接 ./xxx.sh）：
#   bash scripts/jmeter-baseline.sh            # 默认：60s 并发压测测基线 QPS/延迟
#   bash scripts/jmeter-baseline.sh correct    # 正确性突发：100 抢 10，核对不超卖
#
# 压测引擎优先级：
#   1) 若设置了 JMETER_BIN 且指向【官方 Apache JMeter】二进制（推荐，可出 HTML 报告）：
#        export JMETER_BIN=/opt/apache-jmeter-5.6.3/bin/jmeter
#      走 `jmeter -n -t jmeter/seckill-baseline.jmx -l ... -e -o ...` 直接出 HTML 报告。
#   2) 否则回退到零依赖 curl 并发 harness（无需任何 JMeter）。
# 注意：Ubuntu/Debian `apt install jmeter` 装的是老/缝合怪包（加载 .jmx 报
#       xstream ForbiddenClassException，且不认 -e -o），不可用；请用官网二进制。
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-baseline}"

# 1) MySQL 必须就绪
if ! mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e "SELECT 1;" >/dev/null 2>&1; then
  echo "[!] MySQL 未就绪，请先: sudo service mysql start"
  exit 1
fi

# 2) 服务必须就绪
if ! curl -sf -X GET 127.0.0.1:8080/api/health >/dev/null 2>&1; then
  echo "[!] seckill-cpp 未运行，请先: ./build/src/seckill-cpp"
  exit 1
fi

mkdir -p jmeter

# ---------- 零依赖 curl 并发 harness（保底 / 无 JMeter 时）----------
run_curl_harness() {
  local DURATION=60 CONCURRENCY=100 STOCK=100000000 RAW
  echo "[*] 基线模式（curl harness）：清空订单表 + 库存置 $STOCK，持续 ${DURATION}s 并发压测"
  mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=$STOCK, total=$STOCK WHERE id=1;"

  RAW=$(mktemp)
  local START END ELAPSED TOTAL OK SOLD FAIL QPS
  START=$(date +%s.%N)
  seq 1 1000000 | timeout "$DURATION" xargs -P "$CONCURRENCY" -I{} \
    curl -s -o /dev/null -w "%{http_code} %{time_total}\n" \
      -X POST 127.0.0.1:8080/api/seckill \
      -H 'Content-Type: application/json' \
      -d '{"userId":{},"skuId":1}' > "$RAW" 2>/dev/null || true
  END=$(date +%s.%N)
  ELAPSED=$(awk "BEGIN{printf \"%.2f\", $END-$START}")
  TOTAL=$(grep -cE '^[0-9]{3} ' "$RAW" || true)
  OK=$(grep -cE '^200 ' "$RAW" || true)
  SOLD=$(grep -cE '^409 ' "$RAW" || true)
  FAIL=$((TOTAL - OK - SOLD))
  QPS=$(awk "BEGIN{printf \"%.1f\", $TOTAL/$ELAPSED}")
  echo "[*] 总请求=$TOTAL  成功(200)=$OK  SOLD_OUT(409)=$SOLD  失败(含连接错)=$FAIL"
  echo "[*] 实际时长=${ELAPSED}s  吞吐 QPS≈$QPS"
  echo "[*] 延迟(秒) 分布:"
  awk 'NF>=2{print $2}' "$RAW" | sort -n | awk '
    {a[NR]=$1}
    END{
      n=NR;
      if(n==0){print "  无样本"; exit}
      avg=0; for(i=1;i<=n;i++)avg+=a[i]; avg/=n;
      p50=a[int(n*0.50)]; p95=a[int(n*0.95)]; p99=a[int(n*0.99)];
      printf "  avg=%.4f  p50=%.4f  p95=%.4f  p99=%.4f  (n=%d)\n", avg, p50, p95, p99, n;
    }'
  rm -f "$RAW"
}

if [ "$MODE" = "correct" ]; then
  # 正确性突发：库存置 10，100 个唯一用户并发抢，期望 10 成功 / 90 SOLD_OUT / 0 超卖
  echo "[*] 正确性模式：清空订单表 + 库存置 10，100 抢 10"
  mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=10, total=10 WHERE id=1;"
  : > /tmp/seckill-correct.out
  for i in $(seq 1 100); do
    curl -s -o /dev/null -w "%{http_code}\n" -X POST 127.0.0.1:8080/api/seckill \
      -H 'Content-Type: application/json' -d "{\"userId\":$i,\"skuId\":1}" &
  done > /tmp/seckill-correct.out
  wait
  echo "[*] 返回码分布:"
  sort /tmp/seckill-correct.out | uniq -c
  echo "[*] 不超卖核对（应 sold=10, orders=10）:"
  mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e \
    "SELECT (SELECT total FROM seckill_sku WHERE id=1)-(SELECT stock FROM seckill_sku WHERE id=1) AS sold, (SELECT COUNT(*) FROM seckill_order WHERE sku_id=1) AS orders;"
  exit 0
fi

# ---------- 基线模式 ----------
if [ -n "${JMETER_BIN:-}" ] && [ -x "$JMETER_BIN" ]; then
  echo "[*] 使用官方 JMeter: $JMETER_BIN"
  # 关键修复：禁用 Java/JMeter 代理。本机若设了 http_proxy（如 127.0.0.1:9305），
  # Java 会把它当 http.proxyHost，把请求路由到代理而非 127.0.0.1:8080 → 100% connection refused。
  echo "[*] 代理环境: http_proxy=${http_proxy:-<空>} https_proxy=${https_proxy:-<空>} JAVA_TOOL_OPTIONS=${JAVA_TOOL_OPTIONS:-<空>}"
  unset http_proxy https_proxy HTTP_PROXY HTTPS_PROXY no_proxy NO_PROXY 2>/dev/null || true
  export JVM_ARGS="${JVM_ARGS:-} -Dhttp.proxyHost= -Dhttps.proxyHost= -Dhttp.proxyPort= -Dhttps.proxyPort= -Djava.net.useSystemProxies=false"
  # 给足库存（1e8，确保 60s 内不售罄，专测"卖出"事务路径）并清空历史订单
  mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=100000000, total=100000000 WHERE id=1;"
  rm -rf jmeter/report jmeter/results.csv jmeter/aggregate.csv
  if "$JMETER_BIN" -n -t jmeter/seckill-baseline.jmx \
        -l jmeter/results.csv -e -o jmeter/report -j jmeter/jmeter.log; then
    echo "[*] 聚合报告 (jmeter/aggregate.csv):"
    [ -f jmeter/aggregate.csv ] && cat jmeter/aggregate.csv
    echo "[*] HTML 报告: jmeter/report/index.html"
  else
    echo "[!] JMeter 运行失败（可能缺 JRE 或 .jmx 不兼容），回退 curl harness"
    run_curl_harness
  fi
else
  echo "[*] 未设置 JMETER_BIN（或不可执行），使用零依赖 curl harness"
  echo "[*] 若要用官方 JMeter（出 HTML 报告）：export JMETER_BIN=/path/to/apache-jmeter/bin/jmeter"
  run_curl_harness
fi

echo "[*] 不超卖兜底核对（sold 应 == orders，且无超卖）:"
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e \
  "SELECT (SELECT total FROM seckill_sku WHERE id=1)-(SELECT stock FROM seckill_sku WHERE id=1) AS sold, (SELECT COUNT(*) FROM seckill_order WHERE sku_id=1) AS orders;"
