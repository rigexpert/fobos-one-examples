#!/usr/bin/env python3
"""
Live 2-FSK demodulator reading IQ from Aaronia RTSA V6B (port 54666).

Tones (at Aaronia IQ center 383.8125 MHz, Fs=7.68 Msps, 100 kbaud):
  Tone 0 (upper, symbol 0): RF ~382.81 MHz  BB ≈ -1000 kHz
  Tone 1 (lower, symbol 1): RF ~380.81 MHz  BB ≈ -3000 kHz

Usage:
  python3 demod_aaronia_fsk.py [aaronia_host] [port]
"""
import sys, json, time, urllib.request
import numpy as np

AARONIA_HOST = sys.argv[1] if len(sys.argv) > 1 else '10.200.0.125'
IQ_PORT      = int(sys.argv[2]) if len(sys.argv) > 2 else 54666
SYMBOL_RATE  = 100_000

# ── Auto-detect tone frequencies from first N packets ─────────────────────────
DETECT_PKTS = 2   # packets to average for tone detection
MIN_DBC     = 15  # minimum dBc for a tone to be detected

def detect_tones(pkts):
    """Return (Fc, Fs, F0_BB, F1_BB) from a list of IQ packets."""
    all_iq = []
    Fc = pkts[-1].get('startFrequency', 0)
    Fs = pkts[-1].get('sampleFrequency', 7.68e6)
    for pkt in pkts:
        flat = np.array(pkt['samples'], dtype=np.float32)
        all_iq.append(flat[0::2] + 1j*flat[1::2])
    iq = np.concatenate(all_iq)

    fft_size = 8192
    n_blk = len(iq) // fft_size
    if n_blk < 1:
        return Fc, Fs, None, None
    psd = np.abs(np.fft.fftshift(np.fft.fft(
        iq[:n_blk*fft_size].reshape(n_blk, fft_size), axis=1)))**2
    avg  = psd.mean(0)
    avgdb = 10*np.log10(avg + 1e-30)
    freqs = np.fft.fftshift(np.fft.fftfreq(fft_size, 1/Fs))
    noise = np.percentile(avgdb, 25)

    # Find two strongest well-separated peaks (>50 kHz apart, >MIN_DBC above noise)
    peaks = []
    top = np.argsort(avgdb)[::-1]
    prev = -1e9
    for i in top:
        f = freqs[i]
        if abs(f) < 30e3:
            continue  # skip DC
        if abs(f - prev) > 300e3:
            dbc = avgdb[i] - noise
            if dbc < MIN_DBC:
                break
            peaks.append((f, dbc))
            prev = f
        if len(peaks) >= 2:
            break

    if len(peaks) < 2:
        return Fc, Fs, None, None

    f_hi = max(peaks, key=lambda x: x[0])[0]
    f_lo = min(peaks, key=lambda x: x[0])[0]
    return Fc, Fs, f_hi, f_lo   # F0=upper(symbol 0), F1=lower(symbol 1)

# ── DFT energy demodulator ────────────────────────────────────────────────────

def demod_stream(Fc, Fs, F0_BB, F1_BB):
    K = round(Fs / SYMBOL_RATE)
    f0n = F0_BB / Fs
    f1n = F1_BB / Fs
    t_k = np.arange(K)
    e0  = np.exp(-1j * 2 * np.pi * f0n * t_k)
    e1  = np.exp(-1j * 2 * np.pi * f1n * t_k)

    print(f"\n{'─'*60}")
    print(f"Aaronia IQ center : {Fc/1e6:.4f} MHz")
    print(f"Sample rate       : {Fs/1e6:.4f} Msps")
    print(f"K (samples/symbol): {K}")
    print(f"Tone 0 (sym=0)    : BB {F0_BB/1e3:+.1f} kHz  RF {(Fc+F0_BB)/1e6:.4f} MHz")
    print(f"Tone 1 (sym=1)    : BB {F1_BB/1e3:+.1f} kHz  RF {(Fc+F1_BB)/1e6:.4f} MHz")
    print(f"{'─'*60}\n")

    buf  = np.zeros(0, dtype=np.complex64)
    syms = []
    margins = []
    n_total = 0
    t_start = time.time()

    url = f'http://{AARONIA_HOST}:{IQ_PORT}/stream'
    with urllib.request.urlopen(url, timeout=30) as resp:
        raw_buf = b''
        while True:
            chunk = resp.read(262144)
            if not chunk:
                break
            raw_buf += chunk
            while b'\n' in raw_buf:
                line, raw_buf = raw_buf.split(b'\n', 1)
                line = line.strip()
                if not line:
                    continue
                try:
                    obj = json.loads(line)
                except Exception:
                    continue
                if 'samples' not in obj:
                    continue

                flat  = np.array(obj['samples'], dtype=np.float32)
                chunk_iq = flat[0::2] + 1j*flat[1::2]
                buf = np.concatenate([buf, chunk_iq.astype(np.complex64)])

                # Demodulate all complete symbols
                new_syms = 0
                while len(buf) >= K:
                    seg = buf[:K]
                    buf = buf[K:]
                    E0 = abs(np.dot(seg, e0))**2
                    E1 = abs(np.dot(seg, e1))**2
                    sym = 0 if E0 > E1 else 1
                    m   = 10*np.log10((max(E0,E1)+1e-30)/(min(E0,E1)+1e-30))
                    syms.append(sym)
                    margins.append(m)
                    new_syms += 1

                n_total += len(chunk_iq)

                if new_syms == 0:
                    continue

                # Print every 100 symbols
                if len(syms) % 100 < new_syms:
                    elapsed = time.time() - t_start
                    last = syms[-min(64, len(syms)):]
                    bits = ''.join(str(b) for b in last)
                    # Group into bytes
                    byte_strs = []
                    for i in range(0, len(last)-7, 8):
                        byte_strs.append(f"0x{int(''.join(str(x) for x in last[i:i+8]),2):02X}")
                    mar = np.mean(margins[-100:]) if len(margins) >= 100 else np.mean(margins)
                    n0 = syms[-100:].count(0) if len(syms) >= 100 else syms.count(0)
                    n1 = 100 - n0 if len(syms) >= 100 else syms.count(1)
                    print(f"[{elapsed:6.1f}s] syms={len(syms):6d}  margin={mar:.1f} dB  "
                          f"0s:{n0}% 1s:{n1}%  last={bits[:32]}{'...' if len(bits)>32 else ''}")
                    if byte_strs:
                        print(f"           bytes: {' '.join(byte_strs[:8])}")

# ── Main ──────────────────────────────────────────────────────────────────────

print(f"Connecting to Aaronia IQ at {AARONIA_HOST}:{IQ_PORT} ...")

# Collect detection packets
url = f'http://{AARONIA_HOST}:{IQ_PORT}/stream'
detect_pkts = []
print(f"Collecting {DETECT_PKTS} packets for tone detection...")
with urllib.request.urlopen(url, timeout=15) as resp:
    raw_buf = b''
    while len(detect_pkts) < DETECT_PKTS:
        chunk = resp.read(262144)
        if not chunk:
            break
        raw_buf += chunk
        while b'\n' in raw_buf:
            line, raw_buf = raw_buf.split(b'\n', 1)
            try:
                obj = json.loads(line.strip())
                if 'samples' in obj:
                    detect_pkts.append(obj)
                    print(f"  packet {len(detect_pkts)}: {len(obj['samples'])//2} IQ samples")
            except:
                pass

Fc, Fs, F0_BB, F1_BB = detect_tones(detect_pkts)
if F0_BB is None:
    print("ERROR: Could not detect two FSK tones in spectrum. Check signal.")
    sys.exit(1)

spacing = abs(F0_BB - F1_BB)
print(f"\nDetected tones:")
print(f"  Tone 0 (sym 0): BB {F0_BB/1e3:+.1f} kHz  RF {(Fc+F0_BB)/1e6:.4f} MHz")
print(f"  Tone 1 (sym 1): BB {F1_BB/1e3:+.1f} kHz  RF {(Fc+F1_BB)/1e6:.4f} MHz")
print(f"  Spacing: {spacing/1e6:.3f} MHz")

print("\nStarting demodulation (Ctrl-C to stop)...\n")
try:
    demod_stream(Fc, Fs, F0_BB, F1_BB)
except KeyboardInterrupt:
    print("\nStopped.")
