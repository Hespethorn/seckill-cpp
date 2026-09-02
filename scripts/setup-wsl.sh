#!/usr/bin/env bash
# 在 WSL (Ubuntu 22.04+) 上安装 seckill-cpp 的构建依赖与 Drogon。
# 用法: bash scripts/setup-wsl.sh
set -euo pipefail

echo "==> 更新 apt 并安装基础工具链"
sudo apt-get update
sudo apt-get install -y git g++ cmake ninja-build \
    libjsoncpp-dev libboost-all-dev libssl-dev libc-ares-dev \
    uuid-dev zlib1g-dev \
    libmariadb-dev libmariadb-dev-compat \
    libspdlog-dev \
    libhiredis-dev redis-server

# 注意: Drogon 的 cmake_modules/FindMySQL.cmake 只认 mysqlclient_r / mariadbclient /
# mariadb 这几个库名, 而 Ubuntu 的 libmysqlclient-dev 只装出 libmysqlclient.so (5.5+ 已
# 去掉 _r 变体), 不在查找名单里 -> MySQL 探测静默失败 -> 装出的 libdrogon.so 是
# "跳过版" DB 管理器, 运行时报 "No database is supported"。必须用 libmariadb-dev
# (提供 libmariadbclient.so, 命中查找名单) 才能把 ORM/MySQL 后端编进去。
#
# libhiredis-dev 同理: Drogon 的 Redis 支持 (nosql::RedisClient, 登录模块的会话/验证码
# 全靠它) 是编译期开关——探测不到 hiredis 时 BUILD_REDIS 会静默关闭,
# 运行时 getRedisClient() 拿到空指针, 登录接口直接 503。

echo "==> 安装 Drogon v1.9.x（从源码编译，含 orm/MySQL + Redis 支持）"
WORK="${HOME}/build"

# 已装检测：drogon_ctl 在 PATH 且 libdrogon.so 已注册 → 跳过 clone/编译/install，省十几分钟。
# 想强制重编重装：FORCE=1 bash scripts/setup-wsl.sh
drogon_already_installed() {
    # 标准 install 完成：drogon_ctl 在 PATH 且 libdrogon.so 已注册/就位
    command -v drogon_ctl >/dev/null 2>&1 || return 1
    if ldconfig -p 2>/dev/null | grep -q 'libdrogon\.so'; then
        return 0
    fi
    # 兜底：install 过但 ldconfig 缓存未刷新（/usr/local/lib 下有 .so）
    [ -f /usr/local/lib/libdrogon.so ] && return 0
    return 1
}

if [[ "${FORCE:-0}" != "1" ]] && drogon_already_installed; then
    echo "    [SKIP] Drogon 已安装（drogon_ctl + libdrogon.so 均存在），跳过编译安装"
    echo "            FORCE=1 bash scripts/setup-wsl.sh 可强制重编重装"
else
    mkdir -p "$WORK"
    cd "$WORK"
    if [ ! -d drogon ]; then
      git clone --depth 1 --branch v1.9.10 https://github.com/drogonframework/drogon.git
    fi
    cd drogon
    git submodule update --init --recursive
    # 已有本地编译产物则跳过重编，直接 install（省十几分钟）
    if [ -f build/lib/libdrogon.so ]; then
        echo "    检测到本地编译产物 build/lib/libdrogon.so，直接 install（不重编）"
        sudo cmake --install build
        sudo ldconfig
    else
        # -DBUILD_REDIS=ON 显式打开: 它默认就是 ON, 但一旦 hiredis 没探测到就会静默关掉,
        # 显式写上能让 cmake 在缺少依赖时直接报错, 而不是装出一个"没有 Redis 的 Drogon"。
        cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF \
            -DBUILD_MYSQL=ON -DBUILD_REDIS=ON
        cmake --build build -j"$(nproc)"
        sudo cmake --install build
        sudo ldconfig
    fi
fi

echo "==> 启动 Redis（WSL 不会自启服务，每次开机都要来一次）"
if redis-cli ping 2>/dev/null | grep -q PONG; then
    echo "    Redis 已在运行: $(redis-cli ping)（跳过启动）"
else
    sudo service redis-server start || sudo redis-server --daemonize yes
    # 自检: 拿不到 PONG 说明 Redis 没起来, 登录模块会全部 503
    if redis-cli ping 2>/dev/null | grep -q PONG; then
        echo "    Redis OK: $(redis-cli ping)"
    else
        echo "    [WARN] Redis 未响应 PONG, 登录/短信接口将不可用（秒杀不受影响）"
    fi
fi

echo "==> 完成。接下来："
echo "    mysql 建库建表:  mysql -u root -p < sql/schema.sql"
echo "    登录模块建表:    mysql -u root -p < sql/user_schema.sql"
echo "    编译项目:        bash scripts/build-wsl.sh"
