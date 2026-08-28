#!/usr/bin/env bash
# 发布一个版本：在 WSL 上 构建 -> 提交 -> 打 tag -> 推 GitHub。
# 用法: bash scripts/release.sh <版本号, 如 0.1.0>
set -euo pipefail
VERSION="${1:?用法: bash scripts/release.sh <版本号, 如 0.1.0>}"
cd "$(dirname "$0")/.."

echo "==> 构建"
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"

echo "==> 提交并打 tag v$VERSION"
git add -A
if git diff --cached --quiet; then
  echo "    无新改动，跳过提交"
else
  git commit -m "release: v$VERSION"
fi
git tag -a "v$VERSION" -m "seckill-cpp v$VERSION"

echo "==> 推送到 origin（默认分支 main，含 tag）"
git push origin main --tags

echo "==> 已发布 v$VERSION -> https://github.com/Hespethorn/seckill-cpp/releases/tag/v$VERSION"
