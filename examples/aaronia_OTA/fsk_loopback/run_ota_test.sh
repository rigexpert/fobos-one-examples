#!/bin/bash
# OTA loopback test: Aaronia V6B TX (FileReader) + FobosOne RX
#
# NORMAL mode (auto-restart RTSA each case):
#   bash run_ota_test.sh
#
# NO-RESTART mode (V6B already connected manually, hot-swap .rtsa file):
#   bash run_ota_test.sh --no-restart --rtsa-wait 6

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
OTA_PY="$SCRIPT_DIR/ota_loopback.py"
OUT="$SCRIPT_DIR/ota_loopback.csv"

echo "=== FSK OTA Loopback Test ==="
echo "TX: Aaronia RTSA V6B @ 10.200.0.125 (FileReader mode, 433.92 MHz)"
echo "RX: FobosOne @ 5 Msps"
echo ""

python3 "$OTA_PY" \
    --mode fsk \
    --m-list 2 \
    --sym-rates 10000 50000 100000 \
    --n-bursts 4 \
    --data-syms 64 \
    --seed 42 \
    --center-freq 433920000 \
    --fobos-rate 5000000 \
    --lna 2 \
    --vga 8 \
    --capture-s 5.0 \
    --transattn -40 \
    --rtsa-wait 25 \
    --out "$OUT" \
    "$@"

echo ""
echo "Results in $OUT"
