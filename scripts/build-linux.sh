#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if ! command -v cmake >/dev/null 2>&1; then
  echo "缺少 cmake，请先安装：sudo apt install build-essential cmake pkg-config libsdl2-dev"
  exit 1
fi
if ! pkg-config --exists sdl2; then
  echo "缺少 SDL2 开发包，请先安装：sudo apt install libsdl2-dev"
  exit 1
fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
echo
echo "构建完成：build/taiji"
