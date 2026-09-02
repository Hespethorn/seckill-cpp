#!/usr/bin/env bash
# 阶段一基线压测（对应博客 4.4 / 4.5 / 4.7）
# 前置：MySQL 在跑 + seckill-cpp 已编译并在 :8080 运行。
# 用法（在 WSL / Git Bash 里用 `bash scripts/jmeter-baseline.sh`，别直接 ./xxx.sh）：
#   bash scripts/jmeter-baseline.sh            # 默认：持续 60s 测基线 QPS/延迟
#   bash scripts/jmeter-baseline.sh correct    # 正确性突发：100 抢 10，核对不超卖
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

# 3) 基线模式：库存给足（1w），让 DB 事务/行锁路径成为瓶颈，持续 60s 测稳态 QPS
echo "[*] 基线模式：清空订单表 + 库存置 10000，持续 60s 压测"
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill \
  -e "DELETE FROM seckill_order; UPDATE seckill_sku SET stock=10000, total=10000 WHERE id=1;"

rm -rf jmeter/report jmeter/results.csv jmeter/aggregate.csv
# 注意：本机 apt 装的 jmeter 包装脚本不支持 `-e -o`（会报 Unknown option -e），
# 所以先只出 CSV（-l），再用 `-g` 从 CSV 生成 HTML 报告（best-effort，失败不阻断）。
jmeter -n -t jmeter/seckill-baseline.jmx \
  -l jmeter/results.csv \
  -j jmeter/jmeter.log || true
jmeter -g jmeter/results.csv -o jmeter/report 2>/dev/null \
  || echo "[!] HTML 报告生成失败（不影响 CSV 结果），可忽略"

echo "[*] 聚合报告 (jmeter/aggregate.csv):"
[ -f jmeter/aggregate.csv ] && cat jmeter/aggregate.csv
echo "[*] 不超卖兜底核对（sold 应 == orders，且无超卖）:"
mysql -h127.0.0.1 -P3306 -useckill -pseckill seckill -e \
  "SELECT (SELECT total FROM seckill_sku WHERE id=1)-(SELECT stock FROM seckill_sku WHERE id=1) AS sold, (SELECT COUNT(*) FROM seckill_order WHERE sku_id=1) AS orders;"
echo "[*] HTML 报告: jmeter/report/index.html"
