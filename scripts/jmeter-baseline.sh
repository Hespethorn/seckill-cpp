#!/usr/bin/env bash
# 阶段一基线压测（对应博客 4.4 / 4.5 / 4.7）
# 前置：MySQL 在跑 + seckill-cpp 已编译并在 :8080 运行。
# 用法（在 WSL / Git Bash 里用 `bash scripts/jmeter-baseline.sh`，别直接 ./xxx.sh）：
#   bash scripts/jmeter-baseline.sh            # 默认：60s 并发压测测基线 QPS/延迟（零依赖 curl harness）
#   bash scripts/jmeter-baseline.sh correct    # 正确性突发：100 抢 10，核对不超卖
# 注：apt 装的 jmeter 在 WSL 加载 .jmx 会报 xstream ForbiddenClassException，故默认走 curl。
#     装了官方 Apache JMeter 后：export JMETER_BIN=/path/to/apache-jmeter/bin/jmeter 再运行即走 JMeter 路径。
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:-baseline}"

# 0) 依赖检查：Java + JMeter
if ! command -v jmeter >/dev/null 2>&1; then
  echo "[*] 未检测到 jmeter，尝试安装（需 sudo + 网络）…"
  sudo apt-get update -y
  sudo apt-get install -y default-jre jmeter
fi

# 1) MySQL 必须就绪
if ! mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e "SELECT 1;" >/dev/null 2>&1; then
  echo "[!] MySQL 未就绪，请先: sudo service mysql start"
  exit 1
fi

# 2) 服务必须就绪
if ! curl -sf -X GET localhost:8080/api/health >/dev/null 2>&1; then
  echo "[!] seckill-cpp 未运行，请先: ./build/src/seckill-cpp"
  exit 1
fi

mkdir -p jmeter

if [ "$MODE" = "correct" ]; then
  # 正确性突发：库存置 10，100 个唯一用户并发抢，期望 10 成功 / 90 SOLD_OUT / 0 超卖
  echo "[*] 正确性模式：清空订单表 + 库存置 10，100 抢 10"
  mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
    -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=10, total=10 WHERE id=1;"
  # 用 curl 并发模拟（简单可复现，不依赖 JMeter GUI）
  : > /tmp/seckill-correct.out
  for i in $(seq 1 100); do
    curl -s -o /dev/null -w "%{http_code}\n" -X POST localhost:8080/api/seckill \
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

# 3) 基线模式：给足库存（默认 1e8，确保 60s 内不会售罄，专测"卖出"事务路径），持续压测测稳态 QPS + 延迟
# 说明：本机 apt 装的 jmeter 在 WSL 上加载 .jmx 会报 xstream ForbiddenClassException(ScriptWrapper)，
#       故默认用零依赖的 curl 并发 harness 取基线；若装了官方 Apache JMeter，可设 JMETER_BIN 走原 .jmx 路径。
DURATION=60
CONCURRENCY=100
STOCK=100000000

echo "[*] 基线模式：清空订单表 + 库存置 $STOCK，持续 ${DURATION}s 并发压测"
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
  -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=$STOCK, total=$STOCK WHERE id=1;"

RAW=$(mktemp)

if [ -n "${JMETER_BIN:-}" ] && command -v "$JMETER_BIN" >/dev/null 2>&1; then
  # —— 官方 Apache JMeter 路径（需自行下载二进制并 export JMETER_BIN=/path/to/bin/jmeter）——
  echo "[*] 使用官方 JMeter: $JMETER_BIN"
  rm -rf jmeter/report jmeter/results.csv jmeter/aggregate.csv
  "$JMETER_BIN" -n -t jmeter/seckill-baseline.jmx -l jmeter/results.csv -j jmeter/jmeter.log || true
  "$JMETER_BIN" -g jmeter/results.csv -o jmeter/report 2>/dev/null \
    || echo "[!] HTML 报告生成失败（不影响 CSV），可忽略"
  [ -f jmeter/aggregate.csv ] && cat jmeter/aggregate.csv
else
  # —— 零依赖 curl 并发 harness（默认）——
  echo "[*] 使用 curl 并发 harness（apt jmeter 在 WSL 不可用，跳过）"
  START=$(date +%s.%N)
  seq 1 1000000 | timeout "$DURATION" xargs -P "$CONCURRENCY" -I{} \
    curl -s -o /dev/null -w "%{http_code} %{time_total}\n" \
      -X POST localhost:8080/api/seckill \
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
fi

echo "[*] 不超卖兜底核对（sold 应 == orders，且无超卖）:"
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e \
  "SELECT (SELECT total FROM seckill_sku WHERE id=1)-(SELECT stock FROM seckill_sku WHERE id=1) AS sold, (SELECT COUNT(*) FROM seckill_order WHERE sku_id=1) AS orders;"
