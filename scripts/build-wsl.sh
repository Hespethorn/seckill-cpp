#!/usr/bin/env bash
# 在 WSL 上配置并编译 seckill-cpp。用法: bash scripts/build-wsl.sh
set -euo pipefail
cd "$(dirname "$0")/.."

cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "==> 构建产物: ./build/src/seckill-cpp"

