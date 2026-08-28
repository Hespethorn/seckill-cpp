#!/usr/bin/env bash
# 在 WSL (Ubuntu 22.04+) 上安装 seckill-cpp 的构建依赖与 Drogon。
# 用法: bash scripts/setup-wsl.sh
set -euo pipefail

echo "==> 更新 apt 并安装基础工具链"
sudo apt-get update
sudo apt-get install -y git g++ cmake ninja-build \
    libjsoncpp-dev libboost-all-dev libssl-dev libc-ares-dev \
    libcurl4-openssl-dev libmysqlclient-dev uuid-dev zlib1g-dev

echo "==> 安装 Drogon v1.9.x（从源码编译，含 orm/MySQL 支持）"
WORK="${HOME}/build"
mkdir -p "$WORK"
cd "$WORK"
if [ ! -d drogon ]; then
  git clone --depth 1 --branch v1.9.10 https://github.com/drogonframework/drogon.git
fi
cd drogon
git submodule update --init --recursive
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_EXAMPLES=OFF
cmake --build build -j"$(nproc)"
sudo cmake --install build
sudo ldconfig

echo "==> 完成。接下来："
echo "    mysql 建库建表:  mysql -u root -p < sql/schema.sql"
echo "    编译项目:        bash scripts/build-wsl.sh"
