#!/usr/bin/env python3
"""
2-FSK demodulator — Fobos SDR RX, fractional symbol timing + frame sync.

Measures the true Aaronia symbol period (K_actual) from the received energy
autocorrelation, then extracts each symbol block at the correct fractional
sample offset.  This compensates for clock mismatch between the Aaronia
transmitter and the Fobos ADC.

Usage:
  python3 demod_fobos_fsk_burst.py [center_hz] [samplerate_hz] [capture_s] [lna] [vga] [pattern_hex]
  Defaults: 385e6 Hz, 10e6 sps, 5.0 s, LNA=2, VGA=8, pattern=ab
"""
import sys, ctypes, os
import numpy as np

Fc          = float(sys.argv[1]) if len(sys.argv) > 1 else 385_000_000.0
Fs          = float(sys.argv[2]) if len(sys.argv) > 2 else 10_000_000.0
CAPTURE_S   = float(sys.argv[3]) if len(sys.argv) > 3 else 5.0
LNA         = int(sys.argv[4])   if len(sys.argv) > 4 else 2
VGA         = int(sys.argv[5])   if len(sys.argv) > 5 else 8
PATTERN_HEX = sys.argv[6]        if len(sys.argv) > 6 else 'ab'

SYMBOL_RATE = 100_000
K           = round(Fs / SYMBOL_RATE)    # = 100 (nominal block size)
FRAME_DATA  = 16
FRAME_GAP   = 16
FRAME_LEN   = FRAME_DATA + FRAME_GAP     # = 32 symbols (nominal)
BUF_LEN     = 65536

print(f"Fobos center : {Fc/1e6:.3f} MHz  Fs={Fs/1e6:.3f} Msps  K={K}")
print(f"LNA={LNA}  VGA={VGA}  Pattern={PATTERN_HEX}")

# ── Fobos capture ─────────────────────────────────────────────────────────────
LIB = os.path.realpath(os.path.join(os.path.dirname(__file__),
    '../../libfobos-sdr-agile/build/libfobos_sdr.so'))
lib = ctypes.CDLL(LIB)
for fn, rt in [('fobos_sdr_open',ctypes.c_int),('fobos_sdr_set_frequency',ctypes.c_int),
               ('fobos_sdr_set_samplerate',ctypes.c_int),('fobos_sdr_set_lna_gain',ctypes.c_int),
               ('fobos_sdr_set_vga_gain',ctypes.c_int),('fobos_sdr_start_sync',ctypes.c_int),
               ('fobos_sdr_read_sync',ctypes.c_int),('fobos_sdr_stop_sync',ctypes.c_int),
               ('fobos_sdr_close',ctypes.c_int)]:
    getattr(lib, fn).restype = rt

dev = ctypes.c_void_p()
assert lib.fobos_sdr_open(ctypes.byref(dev), 0) == 0
lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(LNA))
lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(VGA))
lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(Fs))
lib.fobos_sdr_set_frequency(dev, ctypes.c_double(Fc))
assert lib.fobos_sdr_start_sync(dev, ctypes.c_uint32(BUF_LEN)) == 0

n_target = int(CAPTURE_S * Fs)
chunks   = []
raw_buf  = (ctypes.c_float * (BUF_LEN * 2))()
captured = 0
print(f"Capturing {CAPTURE_S:.1f}s...", end='', flush=True)
while captured < n_target:
    actual = ctypes.c_uint32(BUF_LEN * 2)
    if lib.fobos_sdr_read_sync(dev, raw_buf, ctypes.byref(actual)) != 0: break
    raw = np.frombuffer(raw_buf, dtype=np.float32, count=actual.value)
    chunks.append((raw[0::2] + 1j*raw[1::2]).astype(np.complex64))
    captured += actual.value // 2
    print('.', end='', flush=True)
lib.fobos_sdr_stop_sync(dev)
lib.fobos_sdr_close(dev)
print(f" done ({captured})\n")

iq = np.concatenate(chunks)[:n_target]

# ── Measure fractional symbol period K_actual via block-energy ACF ────────────
# Each frame occupies N_FRAME Aaronia symbols = N_FRAME * K_actual Fobos samples.
# We measure the frame period in K-blocks via autocorrelation of block energy,
# then back out K_actual.  With perfect clocks K_actual = K = 100; any clock
# mismatch between the Aaronia TX and Fobos ADC gives K_actual ≠ 100.
_n_s_acf = len(iq) // K
_E_acf   = (np.abs(iq[:_n_s_acf*K].reshape(_n_s_acf, K))**2).sum(axis=1).astype(np.float64)
_E_acf  -= _E_acf.mean()
_acf_lags = np.arange(100, 220)
_acf_vals = np.array([float(np.dot(_E_acf[:_n_s_acf-l], _E_acf[l:])) for l in _acf_lags])
_pi = int(np.argmax(_acf_vals))
if 0 < _pi < len(_acf_vals) - 1:
    _y0, _y1, _y2 = _acf_vals[_pi-1], _acf_vals[_pi], _acf_vals[_pi+1]
    _frac = (_y0 - _y2) / (2 * (_y0 - 2*_y1 + _y2))
    _frame_blocks = float(_acf_lags[_pi]) + _frac
else:
    _frame_blocks = float(_acf_lags[_pi])
N_FRAME  = round(_frame_blocks)          # frame period in symbols (e.g. 142)
K_actual = _frame_blocks * K / N_FRAME   # fractional Fobos samples per Aaronia symbol
drift_ppm = (K_actual - K) / K * 1e6
print(f"Clock meas: frame={_frame_blocks:.4f} K-blks  N_FRAME={N_FRAME}  "
      f"K_actual={K_actual:.5f}  drift={drift_ppm:.0f} ppm")
del _E_acf, _acf_vals

# Base start positions for offset=0, using K_actual spacing
# starts_base[n] = round(n * K_actual), so symbol n is extracted from
# iq[starts_base[n] + D : starts_base[n] + D + K] for initial phase D.
n_syms_f     = int((len(iq) - K) / K_actual)
starts_base  = np.round(np.arange(n_syms_f) * K_actual).astype(np.int32)

# ── Spectrum + tone auto-detection ────────────────────────────────────────────
fft_size = 16384
n_avg    = min(len(iq) // fft_size, 64)
spec_acc = np.zeros(fft_size)
for bi in range(n_avg):
    spec_acc += np.abs(np.fft.fft(iq[bi*fft_size:(bi+1)*fft_size]))**2
spec  = np.fft.fftshift(spec_acc / n_avg)
pdb   = 10*np.log10(spec + 1e-30)
fbin  = np.fft.fftshift(np.fft.fftfreq(fft_size, 1/Fs))
noise = np.percentile(pdb, 25)
print(f"\nSpectrum: noise={noise:.1f} dB  peak={pdb.max():.1f} dBfs  SNR={pdb.max()-noise:.1f} dBc")

tones = []
for i in np.argsort(pdb)[::-1]:
    f, dbc = fbin[i], pdb[i]-noise
    if abs(f) < 50e3 or dbc < 8: break
    if all(abs(f-tf) > 300e3 for tf, _ in tones):
        tones.append((f, dbc))
        print(f"  BB {f/1e3:+7.0f} kHz  RF {(Fc+f)/1e6:.3f} MHz  +{dbc:.0f} dBc")
    if len(tones) >= 4: break

if len(tones) >= 2:
    best = None; best_sc = -1e9
    for ia in range(len(tones)):
        for ib in range(ia+1, len(tones)):
            sp = abs(tones[ia][0] - tones[ib][0])
            sc = -(abs(sp - 2e6)/1e6) - abs(tones[ia][1]-tones[ib][1])/20
            if sc > best_sc: best_sc = sc; best = (ia, ib)
    ia, ib  = best
    F0_BB   = max(tones[ia][0], tones[ib][0])   # upper → sym 0
    F1_BB   = min(tones[ia][0], tones[ib][0])   # lower → sym 1
    print(f"\n  Tone0 (sym=0): {F0_BB/1e3:+.0f} kHz  RF {(Fc+F0_BB)/1e6:.3f} MHz")
    print(f"  Tone1 (sym=1): {F1_BB/1e3:+.0f} kHz  RF {(Fc+F1_BB)/1e6:.3f} MHz")
    print(f"  Spacing: {(F0_BB-F1_BB)/1e3:.0f} kHz")
else:
    F0_BB, F1_BB = +1_000_000.0, -1_000_000.0
    print("  WARNING: tone auto-detect failed — using ±1 MHz defaults")

# ── DFT reference phasors ─────────────────────────────────────────────────────
t_k  = np.arange(K)
e0   = np.exp(-1j * 2 * np.pi * F0_BB / Fs * t_k).astype(np.complex64)
e1   = np.exp(-1j * 2 * np.pi * F1_BB / Fs * t_k).astype(np.complex64)
k_blk = np.arange(K, dtype=np.int32)   # integer offsets for block extraction

# ── Pattern ────────────────────────────────────────────────────────────────────
pat_bytes = bytes.fromhex(PATTERN_HEX)
N_PAT     = len(pat_bytes) * 8

def bytes_to_bits(b, msb_first=True):
    bits = []
    for byte in b:
        for i in (range(7,-1,-1) if msb_first else range(8)):
            bits.append((byte >> i) & 1)
    return bits

# ── Fractional-timing global sweep ────────────────────────────────────────────
# For each trial initial phase offset D (0..K-1), place symbol n at
#   iq[starts_base[n] + D : starts_base[n] + D + K].
# This tracks the true Aaronia symbol boundaries throughout the capture,
# eliminating the clock-drift-induced BER floor that afflicts a fixed K-block grid.
#
# Score each offset by the number of exact pattern matches (corr == N_PAT) in the
# symbol stream.  The correct phase has ~N_bursts matches; wrong phases have ~0.
# Use only first SWEEP_SYMS symbols to keep sweep fast (~1 second of data).
SWEEP_SYMS = min(n_syms_f, max(50_000, n_syms_f // 4))
CHUNK      = 10_000    # process this many symbols at a time (≈ 8 MB working set)

print(f"\nFractional timing sweep (K_actual={K_actual:.4f}, {SWEEP_SYMS} symbols)...",
      end='', flush=True)

best_score = -1; best_go = 0; best_msb = True; best_gidx = 0

for msb in (True, False):
    p_bp = np.array(bytes_to_bits(pat_bytes, msb_first=msb), dtype=np.float32) * 2 - 1
    for offset in range(K):
        starts = (starts_base[:SWEEP_SYMS] + offset).astype(np.int32)
        valid  = starts + K <= len(iq)
        starts = starts[valid]
        n_sw   = len(starts)
        if n_sw < N_PAT: continue

        E0 = np.empty(n_sw, np.float32)
        E1 = np.empty(n_sw, np.float32)
        for ci in range(0, n_sw, CHUNK):
            ce    = min(ci + CHUNK, n_sw)
            s_c   = starts[ci:ce]
            idx_c = s_c[:, None] + k_blk[None, :]   # (chunk, K)
            blk_c = iq[idx_c]
            E0[ci:ce] = np.abs(blk_c @ e0)**2
            E1[ci:ce] = np.abs(blk_c @ e1)**2

        E_env_sw = E0 + E1
        E_pk_sw  = float(E_env_sw.max())
        hi_sw    = E_env_sw > E_pk_sw * 0.15  # only count matches in high-energy region

        syms   = (E1 > E0).astype(np.float32) * 2 - 1
        corr   = np.correlate(syms, p_bp, 'valid')
        n_c    = len(corr)
        hi_c   = hi_sw[:n_c]
        n_perf = int(((corr == N_PAT) & hi_c).sum())

        if n_perf > best_score:
            best_score = n_perf; best_go = offset; best_msb = msb
            perf_pos   = np.where((corr == N_PAT) & hi_c)[0]
            best_gidx  = int(perf_pos[0]) if len(perf_pos) > 0 else int(corr.argmax())
    print('.', end='', flush=True)
print(f" done\n")

p_bits = bytes_to_bits(pat_bytes, msb_first=best_msb)
p_bp   = np.array(p_bits, dtype=np.float32) * 2 - 1
print(f"Best: offset={best_go}/{K}  n_perfect={best_score}  "
      f"{'MSB' if best_msb else 'LSB'}-first  first_match_at_sym={best_gidx}")

# ── Full-capture symbol extraction at best offset ─────────────────────────────
starts_best = (starts_base + best_go).astype(np.int32)
valid_best  = starts_best + K <= len(iq)
starts_best = starts_best[valid_best]
n_syms_full = len(starts_best)

best_E0 = np.empty(n_syms_full, np.float32)
best_E1 = np.empty(n_syms_full, np.float32)
print(f"Extracting {n_syms_full} symbols with fractional timing...", end='', flush=True)
for ci in range(0, n_syms_full, CHUNK):
    ce    = min(ci + CHUNK, n_syms_full)
    s_c   = starts_best[ci:ce]
    idx_c = s_c[:, None] + k_blk[None, :]
    blk_c = iq[idx_c]
    best_E0[ci:ce] = np.abs(blk_c @ e0)**2
    best_E1[ci:ce] = np.abs(blk_c @ e1)**2
print(" done")

sym = (best_E1 > best_E0).astype(np.int8)

n0 = np.sum(sym==0); n1 = np.sum(sym==1)
print(f"Symbols: {len(sym)}  0s:{n0}({100*n0/len(sym):.0f}%)  1s:{n1}({100*n1/len(sym):.0f}%)")

# ── Frame extraction ──────────────────────────────────────────────────────────
x     = sym.astype(np.float32) * 2 - 1
corr  = np.correlate(x, p_bp, 'valid')

# ── Raw symbol dump around first match ───────────────────────────────────────
first_frame = best_gidx
dump_start  = max(0, first_frame - 5)
dump_end    = min(len(sym), first_frame + 60)
# Re-extract raw block energies for this window using fractional starts
s_d   = starts_best[dump_start:dump_end]
v_d   = s_d + K <= len(iq)
blk_d = iq[(s_d[v_d])[:, None] + k_blk[None, :]]
E0_d  = np.abs(blk_d @ e0)**2
E1_d  = np.abs(blk_d @ e1)**2
E_d   = E0_d + E1_d
E_max_d = E_d.max() if len(E_d) else 1.0
print(f"\nSymbol dump [{dump_start}..{dump_end}]  (E shown as fraction of max={E_max_d:.2e}):")
print("  idx  bit  E/Emax  bar")
for i, (s, e) in enumerate(zip(sym[dump_start:dump_end], E_d)):
    frac = e / E_max_d
    bar  = '#' * int(frac * 40)
    mark = '<<<' if dump_start + i == first_frame else ''
    print(f"  {dump_start+i:5d}  {s}  {frac:.3f}  |{bar}{mark}")

# ── Energy envelope: find actual burst period ──────────────────────────────────
E_env  = (best_E0 + best_E1).astype(np.float32)
E_peak = E_env.max()
E_thr  = E_peak * 0.30
active = (E_env > E_thr).astype(np.int8)
duty   = 100 * active.mean()
print(f"\nEnergy envelope: peak={E_peak:.3e}  thr={E_thr:.3e}  active%={duty:.1f}%")

edges = np.where(np.diff(active.astype(np.int16)) > 0)[0] + 1
print(f"Rising edges (burst starts): {len(edges)}")

if len(edges) >= 2:
    spacings = np.diff(edges)
    uniq, cnt = np.unique(spacings, return_counts=True)
    top5 = np.argsort(cnt)[::-1][:5]
    print("Burst period candidates:")
    for ti in top5:
        print(f"  {uniq[ti]} sym  ({uniq[ti]/SYMBOL_RATE*1e3:.2f} ms)  ×{cnt[ti]}")
    detected_period = int(uniq[top5[0]])
    burst_first_sym = int(edges[0])
    print(f"Energy-detected: first_burst={burst_first_sym}  period={detected_period}")
else:
    detected_period = FRAME_LEN
    burst_first_sym = first_frame
    print("Not enough edges — assuming continuous TX")

# ── Scan for frame period via correlation lags ─────────────────────────────────
MAX_LAG = min(2048, len(corr) - first_frame - 2)
lags  = np.arange(1, MAX_LAG)
cvals = np.array([float(corr[first_frame + lag]) for lag in lags
                  if first_frame + lag < len(corr)])
lags  = lags[:len(cvals)]

skip = N_PAT
cvals_skip = cvals.copy(); cvals_skip[:skip] = -999
best_lag = int(lags[cvals_skip.argmax()])
best_cv  = cvals[cvals_skip.argmax()]

print(f"\nLag scan (first_frame={first_frame}, searching 1..{MAX_LAG}):")
print("  lag  corr")
shown = set()
ranges = list(range(1, N_PAT+6)) + list(range(85, 125)) + list(range(135, 155)) + [best_lag]
for show_lag in ranges:
    if show_lag in shown or show_lag >= len(lags): continue
    shown.add(show_lag)
    ci = show_lag - 1
    if ci < len(cvals):
        bar = '#' * int(abs(cvals[ci])) if cvals[ci] >= 0 else '.' * int(abs(cvals[ci]))
        marker = ' <-- PEAK' if show_lag == best_lag else ''
        print(f"  {show_lag:4d}  {cvals[ci]:+5.0f}  {'|'+bar if cvals[ci]>=0 else bar+'|'}{marker}")

corr_frame_period = best_lag if best_cv >= N_PAT * 0.6 else FRAME_LEN
print(f"\nCorr-detected frame period: {corr_frame_period} sym  corr={best_cv:.0f}/{N_PAT}")

if len(edges) >= 2:
    frame_period = detected_period
    first_frame  = burst_first_sym
    print(f"Using energy-detected period={frame_period} sym, first_frame={first_frame}")
else:
    frame_period = corr_frame_period
    print(f"Using corr-detected period={frame_period} sym, first_frame={first_frame}")

# ── Filter burst edges ─────────────────────────────────────────────────────────
if len(edges) >= 2:
    min_gap  = int(frame_period * 0.6)
    filtered = [edges[0]]
    for e in edges[1:]:
        if e - filtered[-1] >= min_gap:
            filtered.append(int(e))
    burst_edges = np.array(filtered)
else:
    burst_edges = np.array([first_frame])

print(f"\nBurst edges after gap-filter: {len(burst_edges)}  "
      f"(min_gap={min_gap if len(edges)>=2 else 0} sym)")

# ── Burst structure diagnostic: K-block energy trace for first burst ──────────
# For the first two detected edges, dump E0 and E1 per K-block in SAMPLE space
# (independent of fractional timing / D errors) to reveal preamble structure.
if len(burst_edges) >= 1:
    print(f"\nRaw K-block energy trace for first 2 bursts:")
    for _bi, _be in enumerate(burst_edges[:2]):
        _s_edge = int(starts_best[min(int(_be), len(starts_best)-1)])
        _scan_start = max(0, _s_edge - 2*K)
        _n_blk = min(20, (len(iq) - _scan_start) // K)
        print(f"\n  burst#{_bi} s_edge={_s_edge} (scanning blocks {-2}..{_n_blk-3}):")
        print(f"  {'blk':>4}  {'s':>7}  {'E0':>7}  {'E1':>7}  tone  bits?")
        for _bi2 in range(_n_blk):
            _ss = _scan_start + _bi2 * K
            if _ss + K > len(iq): break
            _blk = iq[_ss:_ss+K]
            _e0 = float(np.abs(_blk @ e0)**2)
            _e1 = float(np.abs(_blk @ e1)**2)
            _rel = _bi2 - 2   # relative to edge
            _tone = 'F1' if _e1 > _e0 else 'F0'
            print(f"  {_rel:>4}  {_ss:>7}  {_e0:>7.2f}  {_e1:>7.2f}  {_tone}")

print(f"\n{'─'*60}")
print(f"Decoding per burst (energy-detected edges, per-burst fine-align)")
print(f"Expected: {''.join(str(b) for b in p_bits)}")

# ── Per-burst raw-sample scan ─────────────────────────────────────────────────
# Scan raw sample offsets relative to each energy-detected burst edge using
# integer K=100 blocks (fractional drift <1 sample within 8 symbols → negligible).
# N_DATA: number of data bits to match per burst.  Cap at 8 to keep the
# vectorised scan tensor (max_d × N_DATA × K) within ~200 MB.
N_DATA    = min(len(p_bits), 8)
p_data_bits = p_bits[:N_DATA]
p_data_bp   = np.array(p_data_bits, dtype=np.float32) * 2 - 1   # (N_DATA,)

SCAN_BACK = K        # scan K samples BEFORE burst edge (late-detection guard)
SCAN_LEN  = 1500     # forward scan range in samples
MIN_CV_D  = N_DATA - 1   # tolerate 1 bit error (= N_DATA-1 for clean decode)

n_fr = 0; n_ok = 0
errs_per_bit = np.zeros(N_DATA, dtype=int)
k_arr_ps     = np.arange(N_DATA, dtype=np.int32)
j_arr_ps     = np.arange(K,     dtype=np.int32)

E_PEAK_GLOBAL = float((best_E0 + best_E1).max())
MIN_E_RATIO   = 0.01   # require block energy >= 1% of peak to count as real signal

print(f"\nPer-burst raw scan  (N_DATA={N_DATA}, back={SCAN_BACK}, fwd={SCAN_LEN}, "
      f"{len(burst_edges)} bursts):")
print(f"Expected ({N_DATA} bits): {''.join(str(b) for b in p_data_bits)}")
print()

for bi_burst, burst_edge in enumerate(burst_edges):
    s_edge  = int(starts_best[min(int(burst_edge), len(starts_best) - 1)])
    s_start = max(0, s_edge - SCAN_BACK)
    max_d   = min(SCAN_BACK + SCAN_LEN, len(iq) - s_start - N_DATA * K - K)
    if max_d <= 0:
        continue

    d_arr = np.arange(max_d, dtype=np.int32)
    idx   = (s_start
             + d_arr[:, None, None]
             + k_arr_ps[None, :, None] * K
             + j_arr_ps[None, None, :])          # (max_d, N_DATA, K)
    blk    = iq[idx]                             # complex64  (max_d, N_DATA, K)
    E0_all = np.abs(blk @ e0)**2                 # (max_d, N_DATA)
    E1_all = np.abs(blk @ e1)**2
    E_avg_all = (E0_all + E1_all).mean(axis=1)   # (max_d,) mean block energy

    sym_all = (E1_all > E0_all).astype(np.float32) * 2 - 1
    cv_all  = sym_all @ p_data_bp                # (max_d,)

    E_thr_gate = E_PEAK_GLOBAL * MIN_E_RATIO
    cv_gated   = np.where(E_avg_all > E_thr_gate, cv_all, float(-N_DATA - 1))

    best_d  = int(np.argmax(cv_gated))
    best_cv = float(cv_all[best_d])

    if bi_burst < 8:
        top5_cv = np.sort(cv_gated[cv_gated > -N_DATA])[-5:][::-1] \
                  if (cv_gated > -N_DATA).any() else []
        E_b = float(E_avg_all[best_d])
        print(f"  burst#{bi_burst} e={int(burst_edge):6d} s_edge={s_edge} "
              f"best_d={best_d}(s={s_start+best_d}) cv={best_cv:+.1f}  "
              f"E_avg={E_b:.2e}  top{min(5,len(top5_cv))}={[int(x) for x in top5_cv]}")
        sym_line = ' '.join(
            f"s{k}:{'F1' if E1_all[best_d,k]>E0_all[best_d,k] else 'F0'}"
            f"({E0_all[best_d,k]:.1f}/{E1_all[best_d,k]:.1f})"
            for k in range(N_DATA))
        print(f"    [{sym_line}]")

    if best_cv < MIN_CV_D:
        continue

    sym_f = (E1_all[best_d] > E0_all[best_d]).astype(np.int8)
    ok = all(int(sym_f[i]) == p_data_bits[i] for i in range(N_DATA))

    for bii in range(N_DATA):
        if int(sym_f[bii]) != p_data_bits[bii]:
            errs_per_bit[bii] += 1

    n_fr += 1
    if ok: n_ok += 1
    if n_fr <= 40:
        bits_str = ''.join(str(int(b)) for b in sym_f)
        print(f"  [e={int(burst_edge):6d}+{best_d}samp cv={best_cv:+.0f}] "
              f"{bits_str}  {'OK' if ok else 'ERR'}")

total_bits = n_fr * N_DATA
bit_errors = int(errs_per_bit.sum())
print(f"\nFrames: {n_fr}  OK: {n_ok}  Frame-BER: {100*(n_fr-n_ok)/max(n_fr,1):.1f}%  "
      f"Bit-BER: {100*bit_errors/max(total_bits,1):.2f}%  ({bit_errors}/{total_bits})")
bad = [(i, errs_per_bit[i]) for i in range(N_DATA) if errs_per_bit[i] > n_fr * 0.1]
if bad:
    print(f"Persistent errors at bits: {[(b, f'{e}/{n_fr}') for b, e in bad]}")
else:
    print("All bits clean.")
