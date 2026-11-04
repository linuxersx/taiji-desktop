#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if ! command -v cmake >/dev/null 2>&1; then
  echo "缺少 cmake，请先执行：brew install cmake sdl2"
  exit 1
fi
if ! pkg-config --exists sdl2; then
  echo "缺少 SDL2，请先执行：brew install sdl2"
  exit 1
fi
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(sysctl -n hw.ncpu)"

APP=build/Taiji.app
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS"
cp build/taiji "$APP/Contents/MacOS/taiji"
cp -R build/assets "$APP/Contents/MacOS/assets"
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleExecutable</key><string>taiji</string>
<key>CFBundleIdentifier</key><string>com.zhyoha.taiji.desktop</string>
<key>CFBundleName</key><string>Taiji</string>
<key>CFBundleDisplayName</key><string>太极</string>
<key>CFBundlePackageType</key><string>APPL</string>
<key>CFBundleShortVersionString</key><string>1.0.0</string>
<key>NSHighResolutionCapable</key><true/>
</dict></plist>
PLIST
echo
echo "构建完成：build/taiji"
echo "应用包：build/Taiji.app"
