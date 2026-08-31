#!/usr/bin/env bash
# seckill-cpp WSL 崩溃(SIGSEGV)定位辅助
# 用法:
#   bash scripts/debug-wsl.sh check   # 检查 MySQL/表/账号(最常踩的坑)
#   bash scripts/debug-wsl.sh asan    # ASan+UBSan 构建并启动, 精确定位崩溃行
#   bash scripts/debug-wsl.sh gdb     # Debug(-g) 构建后进 gdb 跑, 看带行号 bt
# 注意: 本脚本大量使用 grep/ldd/find 探测, 这些命令"没找到"时返回非零,
# 在 set -euo pipefail 下会中断脚本。故所有探测命令末尾统一加 || true。
set -euo pipefail
cd "$(dirname "$0")/.."

MYSQL_PING=(mysqladmin -h 127.0.0.1 -P 3306 -u seckill -pseckill ping)

check_mysql() {
  echo "==> [check] MySQL 服务状态..."
  if "${MYSQL_PING[@]}" >/dev/null 2>&1; then
    echo "    OK: seckill 账号可 ping 通"
  else
    echo "    FAIL: MySQL 未启动或 seckill 账号不可用!"
    echo "    依次执行:"
    echo "      sudo service mysql start"
    echo "      sudo mysql < sql/schema.sql"
    echo "      sudo mysql < sql/init_user.sql"
    return 1
  fi

  echo "==> [check] 表与演示数据..."
  mysql -h 127.0.0.1 -P 3306 -u seckill -pseckill seckill \
    -e "SELECT id, name, stock FROM seckill_sku; SELECT COUNT(*) AS orders FROM seckill_order;" 2>&1 \
    | grep -v "Using a password" || true

  echo "==> [check] Drogon 是否真的带 MySQL 后端(空=没带=getDbClient 返回空)..."
  echo "-- 1) 全盘定位 libdrogon.so(不依赖 ldconfig, 避免缓存未更新):"
  find /usr /lib /opt -name 'libdrogon*' -type f 2>/dev/null | head -10 || true
  echo "    (若上面为空: 库不在常规路径, 继续看 2/3)"
  echo "-- 2) ldconfig 缓存里的 drogon 记录:"
  ldconfig -p 2>/dev/null | grep -i drogon || echo "    (ldconfig 缓存无 drogon -> 装了但没跑 ldconfig, 或库在非标准路径)"
  echo "-- 3) 可执行文件实际链接的 Drogon 路径(这个最准, 程序运行时用哪个就崩在哪个):"
  ldd ./build/src/seckill-cpp 2>/dev/null | grep -i drogon || echo "    (build 目录无产物或未链接 drogon)"
  echo "-- 4) 找到的 libdrogon.so 链接的 DB 库(逐个检查):"
  for f in $(find /usr /lib /opt -name 'libdrogon.so*' -type f 2>/dev/null | head -3); do
    echo "    >>> $f"
    ldd "$f" 2>/dev/null | grep -iE "mysql|maria" || echo "        (未链接任何 mysql/mariadb 库 -> 这个版本是跳过版!)"
  done
  echo "-- 5) CMake 链接用的库路径(DrogonConfig 记录):"
  grep -riE "IMPORTED_LOCATION|libdrogon\.so" /usr/local/lib/cmake/Drogon/ 2>/dev/null | head -8 || echo "    (无 /usr/local/lib/cmake/Drogon)"
  echo "-- 6) Drogon 源码 build 目录的 MySQL 探测结果(只代表 configure 时):"
  grep -iE "BUILD_MYSQL|USE_MYSQL|MYSQL_LIBRARIES|MARIADB" ~/build/drogon/build/CMakeCache.txt 2>/dev/null | head -10 || echo "    (找不到 ~/build/drogon/build/CMakeCache.txt)"
  echo "-- 7) 本机安装的 mariadb/mysql 客户端库:"
  ls -l /usr/lib/x86_64-linux-gnu/libmariadb* /usr/lib/x86_64-linux-gnu/libmysql* 2>/dev/null | head -8 || echo "    (系统里没有 mariadb/mysql 客户端库!)"
}

build_asan() {
  echo "==> [asan] 配置 build-asan (Debug + ASan/UBSan)..."
  cmake -G Ninja -B build-asan -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer -g -O1" >/dev/null
  cmake --build build-asan -j"$(nproc)"
  echo "==> [asan] 启动 ASan 版(打印崩溃报告后退出), 另开终端发请求:"
  echo "    curl -s -X POST http://127.0.0.1:8080/api/seckill -H 'Content-Type: application/json' -d '{\"userId\":1001,\"skuId\":1}'"
  echo ""
  ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 ./build-asan/src/seckill-cpp
}

build_gdb() {
  echo "==> [gdb] 配置 build-debug (-g)..."
  cmake -G Ninja -B build-debug -DCMAKE_BUILD_TYPE=Debug >/dev/null
  cmake --build build-debug -j"$(nproc)"
  echo "==> [gdb] 启动 gdb, 程序挂起后另开终端发请求触发崩溃:"
  echo "    curl -s -X POST http://127.0.0.1:8080/api/seckill -H 'Content-Type: application/json' -d '{\"userId\":1001,\"skuId\":1}'"
  echo ""
  gdb -ex run -ex bt --args ./build-debug/src/seckill-cpp
}

case "${1:-}" in
  check) check_mysql ;;
  asan)  build_asan ;;
  gdb)   build_gdb ;;
  *) echo "用法: bash scripts/debug-wsl.sh {check|asan|gdb}" >&2; exit 1 ;;
esac
