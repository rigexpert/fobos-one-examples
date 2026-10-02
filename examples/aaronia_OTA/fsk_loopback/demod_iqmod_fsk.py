#!/usr/bin/env python3
"""
Demodulate 2-FSK received from the IQ Modulator → V6B TX chain.

Expected signal (after IQ Modulator targetfreq=379.5 MHz, V6B center=380.5 MHz):
  Tone 0: RF ≈ 380.0 MHz  →  Fobos baseband (at 380.5 MHz center) ≈ -500 kHz
  Tone 1: RF ≈ 381.5 MHz  →  Fobos baseband ≈ +1000 kHz

Usage:
  python3 demod_iqmod_fsk.py [iq_bin] [fobos_center_hz] [fobos_rate_hz]

Reads expected TX symbols from /tmp/fsk_iqmod_syms.json to compute BER.
"""
import sys, json, struct
import numpy as np

# ── Args ─────────────────────────────────────────────────────────────────────
iq_file    = sys.argv[1] if len(sys.argv) > 1 else '/tmp/fsk_iqmod_rx.bin'
Fc         = float(sys.argv[2]) if len(sys.argv) > 2 else 380_500_000.0
Fs         = float(sys.argv[3]) if len(sys.argv) > 3 else 5_000_000.0

# ── Known TX parameters ───────────────────────────────────────────────────────
# IQ Modulator: DC → 379.5 MHz.  Tones in .rtsa: +500 kHz, +2000 kHz from DC
# After modulation: 379.5+0.5=380.0 MHz, 379.5+2.0=381.5 MHz
# FobosOne at 380.5 MHz: tone0=-500 kHz, tone1=+1000 kHz
F0_BB      = -500_000.0    # Fobos baseband Hz
F1_BB      = +1_000_000.0  # Fobos baseband Hz
SYMBOL_RATE = 100_000
K           = round(Fs / SYMBOL_RATE)
M           = 2
PREAMBLE    = 8
NOISE_GAP   = 24

print(f"Source:      {iq_file}")
print(f"Fobos:       center={Fc/1e6:.3f} MHz  rate={Fs/1e6:.1f} Msps  K={K}")
print(f"Tone 0 BB:   {F0_BB/1e3:+.0f} kHz  (RF≈{(Fc+F0_BB)/1e6:.3f} MHz)")
print(f"Tone 1 BB:   {F1_BB/1e3:+.0f} kHz  (RF≈{(Fc+F1_BB)/1e6:.3f} MHz)")

# ── Load IQ ──────────────────────────────────────────────────────────────────
raw = np.frombuffer(open(iq_file, 'rb').read(), dtype=np.float32)
iq  = raw[0::2] + 1j * raw[1::2]
n   = len(iq)
print(f"Loaded {n} samples ({n/Fs:.2f}s)")

# ── Spectrum check ────────────────────────────────────────────────────────────
fft_size = 8192
n_blk = n // fft_size
psd = np.abs(np.fft.fftshift(np.fft.fft(
    iq[:n_blk*fft_size].reshape(n_blk, fft_size), axis=1)))**2
avg = psd.mean(0)
avg_db = 10*np.log10(avg + 1e-30)
freqs = np.fft.fftshift(np.fft.fftfreq(fft_size, 1/Fs))
noise = np.median(avg_db)
print(f"\nSpectrum noise floor: {noise:.1f} dB")
top = np.argsort(avg_db)[::-1]
shown, prev_f = 0, -1e9
for i in top:
    if abs(freqs[i] - prev_f) > 50_000 and abs(freqs[i]) > 50_000:
        dBc = avg_db[i] - noise
        if dBc < 5: break
        print(f"  f={freqs[i]/1e3:+7.0f} kHz  RF={(Fc+freqs[i])/1e6:.3f} MHz  {dBc:+.1f} dBc")
        prev_f = freqs[i]
        shown += 1
    if shown > 8: break

# ── DFT energy demod ─────────────────────────────────────────────────────────
f0_norm = F0_BB / Fs
f1_norm = F1_BB / Fs
t_k     = np.arange(K)
e0      = np.exp(-1j * 2 * np.pi * f0_norm * t_k)
e1      = np.exp(-1j * 2 * np.pi * f1_norm * t_k)

# Scan best symbol offset
best_snr, best_off = -999, 0
for off in range(K):
    seg = iq[off:off + 5000*K]
    if len(seg) < 5000*K: break
    blk = seg.reshape(5000, K)
    E0  = np.abs(blk @ e0)**2
    E1  = np.abs(blk @ e1)**2
    snr = 10*np.log10(np.mean(np.maximum(E0,E1)) / (np.mean(np.minimum(E0,E1)) + 1e-30))
    if snr > best_snr:
        best_snr, best_off = snr, off

print(f"\nBest symbol offset: {best_off}  SNR: {best_snr:.1f} dB")

n_syms = (n - best_off) // K
blk    = iq[best_off:best_off + n_syms*K].reshape(n_syms, K)
E0     = np.abs(blk @ e0)**2
E1     = np.abs(blk @ e1)**2
bits   = (E1 > E0).astype(int)

margin = 10*np.log10((np.maximum(E0,E1) + 1e-30) / (np.minimum(E0,E1) + 1e-30))
print(f"Margin: mean={margin.mean():.1f} dB  p5={np.percentile(margin,5):.1f} dB  <3dB:{np.mean(margin<3)*100:.0f}%")

# ── Burst detection ──────────────────────────────────────────────────────────
# Use energy to detect bursts (high energy = signal present)
energy = np.maximum(E0, E1)
energy_smooth = np.convolve(energy, np.ones(PREAMBLE)/PREAMBLE, 'same')
noise_e = np.percentile(energy_smooth, 30)
sig_e   = np.percentile(energy_smooth, 70)
threshold_e = (noise_e + sig_e) / 2

in_burst   = energy_smooth > threshold_e
transitions = np.where(np.diff(in_burst.astype(int)) > 0)[0]

print(f"\nBurst starts (leading edges): {len(transitions)}")
bursts_found = []
for start in transitions[:20]:
    # Skip preamble, extract data symbols
    data_start = start + PREAMBLE
    data_end   = data_start + 64  # DATA_SYMS
    if data_end > n_syms: continue
    burst_bits = bits[data_start:data_end]
    bursts_found.append(burst_bits)

print(f"Captured {len(bursts_found)} burst(s) of 64 data symbols")

# ── BER vs known TX ──────────────────────────────────────────────────────────
try:
    with open('/tmp/fsk_iqmod_syms.json') as f:
        meta = json.load(f)
    tx_syms = meta['tx_syms']
    print(f"\nBER vs known TX pattern:")
    for b_idx, (tx, rx) in enumerate(zip(tx_syms, bursts_found)):
        tx_arr = np.array(tx[:len(rx)])
        rx_arr = np.array(rx[:len(tx)])
        n_cmp  = min(len(tx_arr), len(rx_arr))
        ber    = np.mean(tx_arr[:n_cmp] != rx_arr[:n_cmp])
        ber_inv = np.mean(tx_arr[:n_cmp] != (1-rx_arr[:n_cmp]))
        actual_ber = min(ber, ber_inv)
        print(f"  Burst {b_idx}: BER={actual_ber:.4f}  ({int(actual_ber*n_cmp)}/{n_cmp} errors)"
              f"{'  [inverted polarity]' if ber_inv < ber else ''}")
except FileNotFoundError:
    print("  /tmp/fsk_iqmod_syms.json not found — cannot compute BER vs known pattern")
