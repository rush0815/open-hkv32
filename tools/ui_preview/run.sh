#!/bin/sh
# PC preview of all display screens (needs: g++, python3 with pillow + qrcode).
# Uses the Adafruit GFX sources downloaded by PlatformIO (run "pio run" once first).
# Usage: tools/ui_preview/run.sh [output-dir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=${1:-$ROOT/.pio/ui_preview}
GFX="$ROOT/.pio/libdeps/tinypico/Adafruit GFX Library"
mkdir -p "$OUT"

g++ -std=gnu++17 -O1 -DARDUINO=100 \
  -I"$ROOT/tools/ui_preview/shim" -I"$GFX" -I"$ROOT/src" -I"$ROOT/include" -I"$ROOT/lib/hkv_core/src" \
  "$ROOT/tools/ui_preview/preview.cpp" "$ROOT/src/ui/ui_render.cpp" "$GFX/Adafruit_GFX.cpp" \
  "$ROOT/lib/hkv_core/src/hkv/allocator.cpp" "$ROOT/lib/hkv_core/src/hkv/calendar.cpp" \
  -o "$OUT/preview"

# Example setup QR code (same content format as the firmware uses).
python3 -c "
import qrcode
q = qrcode.QRCode(error_correction=qrcode.constants.ERROR_CORRECT_L, border=0)
q.add_data('WIFI:T:WPA;S:HKV-12345678;P:Ab3dEf7h;;'); q.make(fit=True)
print('\n'.join(''.join('1' if v else '0' for v in row) for row in q.get_matrix()))
" > "$OUT/qr.txt"

"$OUT/preview" "$OUT/preview.ppm" "$OUT/qr.txt"
python3 -c "from PIL import Image; Image.open('$OUT/preview.ppm').save('$OUT/preview.png')"
echo "$OUT/preview.png"
