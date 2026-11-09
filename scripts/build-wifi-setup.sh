#!/bin/bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PROJECT="$ROOT/source/wifi-setup"
BUILD="$ROOT/build/wifi-setup"
if ! command -v idf.py >/dev/null 2>&1; then
  echo '先激活外部 ESP-IDF 5.5.3 环境。' >&2
  exit 1
fi
case "$(idf.py --version)" in
  'ESP-IDF v5.5.3'*) ;;
  *) echo '临时配网固件使用 ESP-IDF v5.5.3。' >&2; exit 1 ;;
esac
mkdir -p "$BUILD"
cd "$PROJECT"
SDKCONFIG_DEFAULTS="$PROJECT/sdkconfig.defaults" \
  idf.py -B "$BUILD" -D "SDKCONFIG=$BUILD/sdkconfig" build
idf.py -B "$BUILD" merge-bin -o "$BUILD/MaToy-full.bin"
python3 "$ROOT/source/firmware/tools/verify_firmware.py" "$BUILD"
echo "临时配网应用：$BUILD/MaToy.bin"
echo "临时配网完整镜像：$BUILD/MaToy-full.bin"
