#!/bin/bash
# verify_tx.sh — Quick TX hardware check via Signal Generator
# Run after connecting Power USB + clicking Connect+Start in RTSA GUI
# 
# Checks: V6B 435 MHz center + 1 MHz offset → CW tone at 436 MHz
# Expected: strong CW peak (-60 dBFS or better) at 436 MHz ± 50 kHz

CAPTURE="$(cd "$(dirname "$0")" && pwd)/c/capture"
FOBOS_CENTER=437000000  # 437 MHz capture center; expected tone at -1000 kHz
FOBOS_RATE=10000000
N_SAMPLES=2000000
LNA=0
VGA=0

echo "=== Signal Generator TX Verification ==="
echo "Expected: CW tone at 436 MHz (= V6B 435 MHz + 1 MHz offset)"
echo ""

# Set V6B center to 435 MHz via /control
echo "[1/3] Setting V6B center to 435 MHz via PUT /control..."
curl -s -X PUT http://10.200.0.125:54665/control \
  -H 'Content-Type: application/json' \
  -d '{"frequencyStart": 432500000, "frequencyEnd": 437500000, "type": "capture"}' \
  && echo " OK" || echo " FAILED (is RTSA running?)"
sleep 2

echo "[2/3] Capturing 2s at 437 MHz with FobosOne..."
"$CAPTURE" $FOBOS_CENTER $FOBOS_RATE $N_SAMPLES $LNA $VGA > /tmp/tx_verify.bin
SIZE=$(stat -c%s /tmp/tx_verify.bin)
EXPECTED=$((N_SAMPLES * 8))
echo "  Got ${SIZE}B (expected ${EXPECTED}B)"
if [ "$SIZE" -lt "$((EXPECTED - 8))" ]; then
    echo "  WARNING: capture short, may have USB error"
fi

echo "[3/3] Analyzing spectrum..."
python3 - << 'EOF'
import numpy as np, sys

Fs = 10e6
Fc = 437e6
n = 2000000

raw = open('/tmp/tx_verify.bin', 'rb').read(n*8)
if len(raw) < n*8:
    n = len(raw) // 8
    raw = raw[:n*8]

iq = np.frombuffer(raw, dtype=np.float32).copy()
x = iq[0::2] + 1j * iq[1::2]

fft_size = 8192
n_blocks = len(x) // fft_size
X = np.fft.fftshift(np.fft.fft(x[:n_blocks*fft_size].reshape(n_blocks, fft_size), axis=1))
psd = np.abs(X)**2
avg = psd.mean(axis=0)
avg_db = 10*np.log10(avg + 1e-30)

f = np.linspace(-Fs/2, Fs/2, fft_size)
noise = np.median(avg_db)

peak_i = np.argmax(avg_db)
peak_f = f[peak_i]
peak_db = avg_db[peak_i]
peak_rf = Fc + peak_f

print(f"  Noise floor:  {noise:.1f} dB")
print(f"  Peak: {peak_rf/1e6:.4f} MHz (offset {peak_f/1e3:+.1f} kHz), {peak_db:.1f} dB ({peak_db-noise:+.1f} dBc)")

if peak_db - noise > 20:
    print(f"\n  PASS: Strong CW signal detected at {peak_rf/1e6:.4f} MHz")
    expected_rf = 436e6
    error_hz = abs(peak_rf - expected_rf)
    if error_hz < 200e3:
        print(f"  Frequency OK: {error_hz/1e3:.1f} kHz from expected 436 MHz")
    else:
        print(f"  WARNING: {error_hz/1e3:.1f} kHz from expected 436 MHz — check V6B center freq")
else:
    print(f"\n  FAIL: No signal detected (peak only {peak_db-noise:+.1f} dBc above noise)")
    print("  Check: Power USB connected? RTSA running? V6B connected and started?")
EOF
