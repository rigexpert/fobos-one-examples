#!/usr/bin/env python3
"""
CPFSK burst generator - compatible with gen_fsk.c / demod_fsk_file.c format.

Usage: gen_fsk_py.py M K n_bursts data_syms seed [bw] [--fs Fs] [--iq-hz f0 f1 ...]
  M           modulation order (2, 4, ...)
  K           samples per symbol
  n_bursts    number of bursts
  data_syms   data symbols per burst
  seed        RNG seed
  bw          fractional bandwidth (default 0.35); ignored when --iq-hz is given
  --fs Fs     sample rate Hz; required when --iq-hz is given
  --iq-hz f0 f1 ...
              explicit IQ tone frequencies in Hz (one per symbol, M total).
              Positive values produce positive IQ frequencies (exp(+j 2pi f t)).
              Without --iq-hz the bw-based conjugate path is used.

Output: float32 interleaved IQ to stdout
Stderr: INFO line + one TXSYM line per burst

V6B TX note: V6B inverts the spectrum (LSB mixing). Positive IQ frequency f
  appears at RF = LO - f * scale_factor. If V6B cannot handle negative IQ,
  use --iq-hz with positive frequencies only, e.g. --iq-hz 200000 400000.
"""
import sys, numpy as np

# ── Parse positional args and optional flags ──────────────────────────────────

positional = []
kv_args = {}
i = 1
while i < len(sys.argv):
    if sys.argv[i].startswith('--'):
        key = sys.argv[i][2:]
        vals = []
        while i + 1 < len(sys.argv) and not sys.argv[i + 1].startswith('--'):
            i += 1
            vals.append(sys.argv[i])
        kv_args[key] = vals
    else:
        positional.append(sys.argv[i])
    i += 1

if len(positional) < 5:
    print(f"Usage: {sys.argv[0]} M K n_bursts data_syms seed [bw] [--fs Fs] [--iq-hz f0 f1 ...]",
          file=sys.stderr)
    sys.exit(1)

M         = int(positional[0])
K         = int(positional[1])
n_bursts  = int(positional[2])
data_syms = int(positional[3])
seed      = int(positional[4])
bw        = float(positional[5]) if len(positional) > 5 else 0.35

PREAMBLE_SYMS = 8
NOISE_GAP = 24

# ── Determine tone frequencies ────────────────────────────────────────────────

use_explicit_hz = 'iq-hz' in kv_args
if use_explicit_hz:
    Fs = float(kv_args['fs'][0]) if 'fs' in kv_args else None
    if Fs is None:
        print("Error: --iq-hz requires --fs <sample_rate_hz>", file=sys.stderr)
        sys.exit(1)
    iq_hz = [float(x) for x in kv_args['iq-hz']]
    if len(iq_hz) != M:
        print(f"Error: --iq-hz needs {M} values for M={M}, got {len(iq_hz)}", file=sys.stderr)
        sys.exit(1)
    tone_freqs_norm = np.array([f / Fs for f in iq_hz])
    print(f"INFO M={M} K={K} n_bursts={n_bursts} data_syms={data_syms} "
          f"preamble={PREAMBLE_SYMS} gap={NOISE_GAP} "
          f"mode=explicit iq_hz={iq_hz} Fs={Fs:.1f}", file=sys.stderr)
else:
    Fs = None
    # CPFSK tone frequencies matching liquid-dsp fskmod spacing.
    # For M=2: tones at -bw/2 and +bw/2 (normalized cycles/sample).
    if M == 2:
        tone_freqs_norm = np.array([-bw / 2, bw / 2])
    else:
        tone_freqs_norm = (np.arange(M) - (M - 1) / 2.0) * bw / (M - 1)
    print(f"INFO M={M} K={K} n_bursts={n_bursts} data_syms={data_syms} "
          f"preamble={PREAMBLE_SYMS} gap={NOISE_GAP} bw={bw}", file=sys.stderr)

rng = np.random.default_rng(seed)
phase = 0.0
t_sym = np.arange(K)

silence = np.zeros(K * 2, dtype=np.float32)


def write_symbol(freq_norm: float, phase_in: float) -> float:
    """Write one symbol and return updated phase."""
    phases = phase_in + 2.0 * np.pi * freq_norm * t_sym
    if use_explicit_hz:
        # Direct: exp(+j phases), tone at +freq_norm*Fs Hz
        iq = np.exp(1j * phases).astype(np.complex64)
    else:
        # Conjugate: exp(-j phases), compensates for V6B LSB spectrum inversion
        iq = np.exp(-1j * phases).astype(np.complex64)
    buf = np.empty(K * 2, dtype=np.float32)
    buf[0::2] = iq.real
    buf[1::2] = iq.imag
    sys.stdout.buffer.write(buf.tobytes())
    return phase_in + 2.0 * np.pi * freq_norm * K


preamble_syms = [0 if i % 2 == 0 else M - 1 for i in range(PREAMBLE_SYMS)]

for b in range(n_bursts):
    for _ in range(NOISE_GAP):
        sys.stdout.buffer.write(silence.tobytes())

    phase = 0.0

    for sym in preamble_syms:
        phase = write_symbol(tone_freqs_norm[sym], phase)

    data = rng.integers(0, M, size=data_syms).tolist()
    for sym in data:
        phase = write_symbol(tone_freqs_norm[sym], phase)

    syms_str = " ".join(str(s) for s in data)
    print(f"TXSYM b={b} n={data_syms} {syms_str}", file=sys.stderr)

# Trailing silence so the demodulator can finish the last burst
for _ in range(NOISE_GAP):
    sys.stdout.buffer.write(silence.tobytes())

sys.stdout.buffer.flush()
