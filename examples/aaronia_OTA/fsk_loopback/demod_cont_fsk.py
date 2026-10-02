#!/usr/bin/env python3
"""
Continuous 2-FSK demodulator (Fobos SDR RX, or from an IQ file).

Designed for the Aaronia-played file fsk_386mhz_100kbd.iq:
  2-FSK, 1 MHz shift (bit1=+0.5 MHz, bit0=-0.5 MHz), 100 ksym/s,
  pattern 0xAB (MSB-first) repeated continuously.

Pipeline:
  1. Capture IQ from Fobos (async, robust on RPi5) OR read an .iq/.npy file.
  2. Auto-find the FSK center, mix to baseband, FFT-bandpass the channel.
  3. Report envelope health (detects Aaronia TX-stream underflow gaps).
  4. FM discriminator -> per-symbol decision.
     - Threshold = MIDPOINT between the two tone clusters (NOT the mean/median:
       0xAB is 5 ones/3 zeros, so a mean/median slicer is biased).
     - Symbol timing chosen to MINIMISE BER vs the known 0xAB (robust to the
       Aaronia<->Fobos sample-clock mismatch).
  5. If the TX is gapping, demodulate the longest gap-free segment.

Usage:
  Live:  python3 demod_cont_fsk.py [center_hz] [samplerate_hz] [capture_s] [lna] [vga] [pattern_hex]
  File:  python3 demod_cont_fsk.py path/to/file.iq  [samplerate_hz] [pattern_hex]
         (.iq = interleaved float32 I/Q;  .npy = complex array)
Defaults (live): 385.55e6, 10e6, 0.20 s, LNA=2, VGA=15, pattern=ab
"""
import sys, ctypes, os, time
import numpy as np

SYMBOL_RATE = 100_000.0
BUF_LEN     = 65536

# ── Args: file mode vs live mode ──────────────────────────────────────────────
FILE_MODE = len(sys.argv) > 1 and os.path.exists(sys.argv[1])

if FILE_MODE:
    IQ_FILE     = sys.argv[1]
    Fs          = float(sys.argv[2]) if len(sys.argv) > 2 else 10_000_000.0
    PATTERN_HEX = sys.argv[3]        if len(sys.argv) > 3 else 'ab'
    Fc          = 0.0
else:
    Fc          = float(sys.argv[1]) if len(sys.argv) > 1 else 385_550_000.0
    Fs          = float(sys.argv[2]) if len(sys.argv) > 2 else 10_000_000.0
    CAPTURE_S   = float(sys.argv[3]) if len(sys.argv) > 3 else 0.20
    LNA         = int(sys.argv[4])   if len(sys.argv) > 4 else 2   # Fobos LNA: 0..3
    VGA         = int(sys.argv[5])   if len(sys.argv) > 5 else 15  # Fobos VGA: 0..31
    PATTERN_HEX = sys.argv[6]        if len(sys.argv) > 6 else 'ab'

if not FILE_MODE:
    LNA = max(0, min(3, LNA))    # Fobos LNA valid range 0..3
    VGA = max(0, min(31, VGA))   # Fobos VGA valid range 0..31
PATTERN   = int(PATTERN_HEX, 16) & 0xFF
PATT_BITS = np.array([(PATTERN >> (7 - i)) & 1 for i in range(8)], dtype=np.int8)
SPS_NOM   = Fs / SYMBOL_RATE

# ── Acquire IQ ────────────────────────────────────────────────────────────────
if FILE_MODE:
    print(f"== file ==  {IQ_FILE}  Fs={Fs/1e6:.3f} Msps  pattern=0x{PATTERN:02X}")
    if IQ_FILE.endswith('.npy'):
        x = np.load(IQ_FILE).astype(np.complex64)
    else:
        d = np.fromfile(IQ_FILE, dtype=np.float32)
        x = (d[0::2] + 1j*d[1::2]).astype(np.complex64)
    print(f"loaded {x.size} complex samples ({x.size/Fs*1e3:.1f} ms)")
else:
    print(f"== capture ==  Fc={Fc/1e6:.3f} MHz  Fs={Fs/1e6:.3f} Msps  "
          f"dur={CAPTURE_S:.3f}s  LNA={LNA} VGA={VGA}  pattern=0x{PATTERN:02X}")
    LIB = os.path.realpath(os.path.join(os.path.dirname(__file__),
        '../../libfobos-sdr-agile/build/libfobos_sdr.so'))
    lib = ctypes.CDLL(LIB)
    for fn in ('fobos_sdr_open','fobos_sdr_close','fobos_sdr_set_frequency',
               'fobos_sdr_set_samplerate','fobos_sdr_set_lna_gain',
               'fobos_sdr_set_vga_gain','fobos_sdr_read_async','fobos_sdr_cancel_async'):
        getattr(lib, fn).restype = ctypes.c_int
    CB_TYPE = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_float),
                               ctypes.c_uint32, ctypes.c_void_p, ctypes.c_void_p)
    dev = ctypes.c_void_p()
    if lib.fobos_sdr_open(ctypes.byref(dev), 0) != 0:
        print("fobos_sdr_open error"); sys.exit(1)
    lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(LNA))
    lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(VGA))
    lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(Fs))
    lib.fobos_sdr_set_frequency(dev, ctypes.c_double(Fc))
    need = int(CAPTURE_S * Fs); chunks = []; got = [0]
    def cb(p, n, d, u):
        if got[0] >= need:
            lib.fobos_sdr_cancel_async(dev); return
        a = ctypes.cast(p, ctypes.c_void_p).value
        chunks.append(np.frombuffer((ctypes.c_float*(n*2)).from_address(a),
                                    dtype=np.float32, count=n*2).copy())
        got[0] += n
        if got[0] >= need: lib.fobos_sdr_cancel_async(dev)
    t0 = time.monotonic()
    lib.fobos_sdr_read_async(dev, CB_TYPE(cb), None, 16, ctypes.c_uint32(BUF_LEN))
    lib.fobos_sdr_close(dev)
    x = (np.concatenate(chunks)[:need*2]).astype(np.float32)
    x = (x[0::2] + 1j*x[1::2]).astype(np.complex64)
    print(f"captured {got[0]} samples in {time.monotonic()-t0:.2f}s")
    np.save('/tmp/fsk_capture.npy', x)

# ── Auto-find FSK center, isolate channel ─────────────────────────────────────
N = 8192; w = np.hanning(N); psd = np.zeros(N)
for i in range(x.size // N):
    psd += np.abs(np.fft.fftshift(np.fft.fft(x[i*N:(i+1)*N]*w)))**2
f = np.fft.fftshift(np.fft.fftfreq(N, 1/Fs))
def p_at(fr): return psd[np.argmin(np.abs(f - fr))]
fc_off, best_s = 0.0, -1.0
for fcx in np.arange(-1.5e6, 1.5e6, 5e3):
    s = p_at(fcx - 0.5e6) + p_at(fcx + 0.5e6)
    if s > best_s: best_s, fc_off = s, fcx
if not FILE_MODE:
    print(f"FSK center offset = {fc_off/1e3:+.1f} kHz  => RF center {(Fc+fc_off)/1e6:.4f} MHz, "
          f"tones {(Fc+fc_off-0.5e6)/1e6:.3f}/{(Fc+fc_off+0.5e6)/1e6:.3f} MHz")

nmix = np.arange(x.size)
xb = x * np.exp(-1j*2*np.pi*fc_off/Fs*nmix)
X = np.fft.fft(xb); fr = np.fft.fftfreq(x.size, 1/Fs)
X[np.abs(fr) > 0.7e6] = 0
xf = np.fft.ifft(X).astype(np.complex64)

# ── Envelope health (Aaronia TX underflow detector) ──────────────────────────
env = np.abs(xf)
dropout = float(np.mean(env < 0.3*env.mean()) * 100)
print(f"envelope std/mean={env.std()/env.mean():.2f}  dropouts(<30%)={dropout:.1f}%"
      + ("   <-- TX UNDERFLOW (gaps)!" if dropout > 5 else "   (clean)"))

# Pick the demod region: whole signal if clean, else the longest gap-free run
if dropout > 5:
    good = env > 0.45*env.mean()
    idx = np.where(good)[0]
    if idx.size == 0:
        print("no usable signal"); sys.exit(1)
    runs = np.split(idx, np.where(np.diff(idx) > 5)[0] + 1)
    run = max(runs, key=len)
    a, b_end = int(run[0]), int(run[-1])
    seg = xf[a:b_end]
    print(f"TX gapping -> demod longest clean run: {(b_end-a)/Fs*1e3:.1f} ms ({b_end-a} samples)")
else:
    seg = xf

# ── FM discriminator + matched filter ─────────────────────────────────────────
d = seg[1:] * np.conj(seg[:-1])
inst = np.angle(d) * (Fs / (2*np.pi))           # NO mean/median centering (biased on 0xAB)
k = max(1, int(round(SPS_NOM)))
mf = np.convolve(inst, np.ones(k)/k, mode='same')

ref = PATT_BITS
def slice_and_ber(sps, phfrac):
    nsym = int((len(mf) - sps) / sps)
    if nsym < 16: return (1.0, 0, 0, np.zeros(0, np.int8), 0.0)
    idx = (np.arange(nsym)*sps + sps*phfrac).astype(np.int64)
    idx = idx[idx < len(mf)]
    sym = mf[idx]
    thr = (np.percentile(sym, 15) + np.percentile(sym, 85)) / 2   # midpoint of 2 clusters
    bits = (sym > thr).astype(np.int8)
    best = (1.0, 0, 0)
    for inv in (0, 1):
        bb = bits ^ inv
        for phase in range(8):
            rep = np.tile(np.roll(ref, -phase), len(bb)//8 + 1)[:len(bb)]
            ber = np.mean(bb != rep)
            if ber < best[0]: best = (ber, inv, phase)
    return best[0], best[1], best[2], bits, thr

# ── Timing recovery: minimise BER vs known 0xAB ──────────────────────────────
best = None
for sps in np.arange(SPS_NOM-1.5, SPS_NOM+1.5+1e-9, 0.01):
    for phfrac in np.arange(0.1, 1.0, 0.1):
        ber, inv, phase, bits, thr = slice_and_ber(sps, phfrac)
        if best is None or ber < best[0]:
            best = (ber, sps, phfrac, inv, phase, bits, thr)
ber, sps, phfrac, inv, phase, bits, thr = best
print(f"timing: sps={sps:.3f} (nom {SPS_NOM:.1f})  phase={phfrac:.1f}  "
      f"thr={thr/1e3:+.0f}kHz  nbits={len(bits)}")
print(f"BER = {ber*100:.3f}%   (polarity_inverted={inv}, byte_phase={phase})")

b = (bits ^ inv)[(8-phase) % 8:]
b = b[:len(b)//8*8].reshape(-1, 8)
vals_bytes = b.dot(1 << np.arange(7, -1, -1))
uniq, cnts = np.unique(vals_bytes, return_counts=True)
topi = np.argsort(cnts)[::-1][:4]
print("most common decoded bytes: " +
      ", ".join(f"0x{uniq[i]:02X}×{cnts[i]}" for i in topi))
ok = ber < 0.02 and len(uniq) and uniq[topi[0]] == PATTERN
print(f"RESULT: {'PASS' if ok else 'FAIL'}  "
      f"(expected dominant byte 0x{PATTERN:02X})")
