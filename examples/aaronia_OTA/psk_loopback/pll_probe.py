#!/usr/bin/env python3
"""Probe: can a decision-directed PLL recover the OTA PSK carrier (phase wander)?
Captures the live BPSK@100k that RTSA is currently transmitting, then compares
block carrier recovery vs a 2nd-order DD-PLL across loop bandwidths."""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'fsk_loopback'))
import mpsk_lab as p
from ota_capture import capture
import ota_sweep as fk

M   = int(sys.argv[1]) if len(sys.argv) > 1 else 2
Rs  = float(sys.argv[2]) if len(sys.argv) > 2 else 100e3
BETA = 0.35
Fs = min(50e6, max(8e6, 16*Rs))   # oversample 16x so the chain's rate scaling still leaves adequate sps
dur = max(0.04, 4000/Rs)          # ~>=4000 symbols
bits = p.fl.bytes_to_bits(p.fl.test_bytes())
_, syms, _ = p.gen_mpsk(bits, M, Rs, 8*Rs, beta=BETA)

def dd_pll(s, M, alpha):
    """2nd-order decision-directed loop. alpha = proportional gain (loop BW)."""
    beta = alpha*alpha/4.0
    out = np.empty(len(s), complex)
    ph = 0.0; fr = 0.0; step = 2*np.pi/M
    for i in range(len(s)):
        v = s[i]*np.exp(-1j*ph); out[i] = v
        d = round(np.angle(v)/step)*step          # nearest constellation phase
        err = np.angle(v*np.exp(-1j*d))           # phase error
        fr += beta*err; ph += fr + alpha*err
    return out

def carrier_freq(yc):
    """Sharp carrier-frequency estimate from the M-th-power FFT peak (clean: the
    M-th power collapses the M PSK phases to a single tone at M*fcarrier)."""
    Y = np.abs(np.fft.fft(yc**M)); fr = np.fft.fftfreq(len(yc), 1/Fs)
    return fr[np.argmax(Y)]/M

def symstreams(x):
    """matched filter -> coarse centroid -> SHARP M-th-power FFT carrier removal,
    then yield (sps_t,ph0,s) timing candidates. Residual phase WANDER is left for
    the PLL to track (that is the whole point: acquire freq, track phase)."""
    sps = int(round(Fs/Rs)); h = p.rrc(BETA, sps, 8)
    yc = np.convolve(x, h).astype(np.complex128); gd = 8*sps; n = np.arange(len(yc))
    P = np.abs(np.fft.fft(yc))**2
    frq = np.fft.fftshift(np.fft.fftfreq(len(yc), 1/Fs))
    binw = Fs/len(yc); w = max(1, int(round(Rs/binw)))
    Ps = np.convolve(np.fft.fftshift(P), np.ones(w), 'same')
    band = (Ps > 0.25*Ps.max()) & (np.abs(frq) < 2*Rs)
    coarse = np.sum(frq[band]*Ps[band])/np.sum(Ps[band])
    yc = yc*np.exp(-1j*2*np.pi*coarse/Fs*n)
    fcar = carrier_freq(yc)                       # sharp residual carrier
    yc = yc*np.exp(-1j*2*np.pi*fcar/Fs*n)         # now near-baseband; PLL tracks the rest
    sps_f = Fs/Rs                       # FLOAT sps: search around this, not the rounded int
    for sps_t in np.arange(sps_f*0.97, sps_f*1.03+1e-9, max(sps_f*0.0005, 0.01)):
        for ph0 in np.linspace(0, sps_t, 8, endpoint=False):
            idx = (np.arange(int((len(yc)-2*gd)/sps_t))*sps_t + ph0 + gd).astype(np.int64)
            idx = idx[(idx >= 0) & (idx < len(yc))]
            if len(idx) < 64: continue
            yield sps_t, ph0, yc[idx]

def diff_ber(est_diff):
    """align differential symbols to the known differential sequence (shift only)."""
    ref = (syms - np.roll(syms, 1)) % M
    n = len(est_diff); L = len(ref)
    rt = np.tile(ref, n//L + 2)[:n]
    a = np.exp(2j*np.pi*est_diff/M); b = np.exp(2j*np.pi*rt/M)
    corr = np.abs(np.fft.ifft(np.fft.fft(a)*np.conj(np.fft.fft(b))))
    sh = int(np.argmax(corr)); best = 1.0
    for cand in {sh,(n-sh)%n,(sh+1)%n,(sh-1)%n}:
        best = min(best, p.fl.syms_to_ber(est_diff, np.roll(rt, cand), M))
    return best

def quality(s):
    """timing metric robust to phase WANDER: M-th-power magnitude concentration
    (block BER fails here because a wandering phase wrecks every absolute decision)."""
    return np.abs(np.mean(s**M))/np.mean(np.abs(s)**M)

def best_ber(x):
    """Pass 1: pick best timing by phase-wander-robust constellation quality.
    Pass 2: block / PLLs / differential on that one stream."""
    bs = None; bq = -1.0
    for sps_t, ph0, s in symstreams(x):
        q = quality(s)
        if q > bq: bq = q; bs = s
    s = bs
    phi = np.angle(np.mean(s**M))/M
    est = np.round(np.angle(s*np.exp(-1j*phi))/(2*np.pi/M)).astype(np.int64) % M
    res = {'block': p._align_psk(est, syms, M)[0]}
    for a, key in [(0.05,'pll0.05'),(0.1,'pll0.1')]:
        v = dd_pll(s, M, a)
        est = np.round(np.angle(v)/(2*np.pi/M)).astype(np.int64) % M
        res[key] = p._align_psk(est, syms, M)[0]
    ediff = np.round(np.angle(s[1:]*np.conj(s[:-1]))/(2*np.pi/M)).astype(np.int64) % M
    res['diff(DPSK)'] = diff_ber(ediff)
    return res, len(s)

def detect_rate(x, Rs_nom):
    """True on-air symbol rate from the data's repeat period (the chain may scale
    the rate, e.g. File Source replaying an 8 MHz file at 10 MHz -> 1.25x). Uses the
    differential symbol stream (immune to carrier wander) autocorrelated near the
    known pattern length."""
    sps = int(round(Fs/Rs_nom)); h = p.rrc(BETA, sps, 8)
    yc = np.convolve(x, h).astype(complex); nn = np.arange(len(yc))
    Y = np.abs(np.fft.fft(yc**M)); fr = np.fft.fftfreq(len(yc), 1/Fs); fc = fr[np.argmax(Y)]/M
    yc = yc*np.exp(-1j*2*np.pi*fc/Fs*nn); gd = 8*sps
    idx = (np.arange(int((len(yc)-2*gd)/sps))*sps + gd).astype(int); idx = idx[idx < len(yc)]
    s = yc[idx]; d = s[1:]*np.conj(s[:-1]); d = d/(np.abs(d)+1e-9)
    L = len(syms); dd = d - d.mean()
    ac = np.abs(np.correlate(dd, dd, 'full')); ac = ac[len(ac)//2:]
    lo, hi = int(L*0.6), min(int(L*1.5), len(ac)-1)
    if hi <= lo+2: return Rs_nom, 0.0
    pk = lo + int(np.argmax(ac[lo:hi]))
    return Rs_nom*L/pk, ac[pk]/ac[0]

if __name__ == '__main__':
    x = capture(fk.FC, Fs, dur, 0, 4); m = np.mean(np.abs(x))
    print(f"captured {len(x)} samp mean={m:.3f}")
    if m < 0.003: print("NO SIGNAL"); sys.exit(1)
    Rs_true, q = detect_rate(x, Rs)
    if abs(Rs_true-Rs)/Rs > 0.02:
        Rs = Rs_true            # symstreams/best_ber read this module global
        Fs = min(50e6, 16*Rs)   # exact integer sps=16 at the true rate
        print(f"auto-detected on-air Rs = {Rs/1e6:.4f} M (periodicity {q:.2f}); re-capturing @ Fs={Fs/1e6:.1f}M")
        x = capture(fk.FC, Fs, max(0.04, 4000/Rs), 0, 4); print(f"  re-captured mean={np.mean(np.abs(x)):.3f}")
    res, nb = best_ber(x)
    print(f"~{nb} symbols/stream")
    for k, v in res.items():
        print(f"  {k:>8}: BER={v*100:6.2f}%")
