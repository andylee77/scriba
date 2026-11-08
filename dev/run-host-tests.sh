#!/bin/bash
# Host-side tests that need no programmer, built with AddressSanitizer and
# UndefinedBehaviorSanitizer (Linux, gcc, libusb-1.0 headers):
#   1. dev/mock/mock_ch347.c: src/ch347_spi.c against a packet-level fake
#      CH347 (frame sizes, firmware versions, chip-select flag modes).
#   2. src/flash_layout.c on synthetic images from make_test_images.py:
#      the text report, and the JSON writer at every buffer size.
# Extra image files given as arguments (your own dumps) are added to step 2.
#
# Usage: dev/run-host-tests.sh [image.bin ...]
set -eu
ROOT=$(git -C "$(dirname "$0")" rev-parse --show-toplevel)
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
SAN="-std=gnu99 -Wall -Wextra -Wno-unused-parameter -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all"

echo "== CH347 driver against the mock device"
# shellcheck disable=SC2086
gcc $SAN -I"$ROOT/src" "$ROOT/dev/mock/mock_ch347.c" "$ROOT/src/ch347_spi.c" -o "$W/mock_ch347"
"$W/mock_ch347" | tail -1

echo "== flash_layout on synthetic images"
python3 "$ROOT/dev/layout/make_test_images.py" "$W/img" >/dev/null
# shellcheck disable=SC2086
gcc $SAN -I"$ROOT/src" "$ROOT/src/flash_layout.c" "$ROOT/dev/layout/layout_cli.c" -o "$W/layout_cli"
# shellcheck disable=SC2086
gcc $SAN -I"$ROOT/src" "$ROOT/src/flash_layout.c" "$ROOT/dev/layout/layout_json.c" -o "$W/layout_json"
"$W/layout_cli" "$W"/img/*.bin "$@" > "$W/layout.txt"
"$W/layout_json" "$W"/img/*.bin "$@" > "$W/layout.json"
python3 - "$W/layout.json" <<'PY'
import json, sys
n = 0
for line in open(sys.argv[1]):
    json.loads(line)
    n += 1
print(f"{n} layouts are valid JSON; every buffer size checked")
PY
grep -A8 "camera_4mb.bin" "$W/layout.txt" | head -9
echo "all host tests passed"
