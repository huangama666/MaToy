#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/source/firmware"
BUILD="$ROOT/build/firmware"
if ! command -v idf.py >/dev/null 2>&1; then
  echo '先 source ESP-IDF 5.5.3 的 export.sh，然后再运行本脚本。' >&2
  exit 1
fi
case "$(idf.py --version)" in
  'ESP-IDF v5.5.3'*) ;;
  *) echo 'MaToy 固件使用 ESP-IDF v5.5.3。' >&2; exit 1 ;;
esac
mkdir -p "$BUILD"
cd "$PROJECT"
SDKCONFIG_DEFAULTS="$PROJECT/sdkconfig.defaults" \
  idf.py -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" build
idf.py -B "$BUILD" merge-bin -o "$BUILD/MaToy-full.bin"
python3 tools/verify_firmware.py "$BUILD"
echo "应用镜像：$BUILD/MaToy.bin"
echo "完整镜像：$BUILD/MaToy-full.bin"
