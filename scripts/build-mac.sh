#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/outputs/MaToy.app"
mkdir -p "$APP/Contents/MacOS" "$ROOT/build/ModuleCache"
cp "$ROOT/source/mac/Info.plist" "$APP/Contents/Info.plist"
swiftc -module-cache-path "$ROOT/build/ModuleCache" -parse-as-library -O \
  "$ROOT/source/mac/Control.swift" \
  "$ROOT/source/mac/Voice.swift" \
  "$ROOT/source/mac/Camera.swift" \
  -o "$APP/Contents/MacOS/PassportControl"
codesign --force --sign - "$APP"
codesign --verify --strict --verbose=2 "$APP"
echo "应用已构建：$APP"
echo '此脚本不自动打开应用。更新二进制后，macOS 可能需要重新授权。'
