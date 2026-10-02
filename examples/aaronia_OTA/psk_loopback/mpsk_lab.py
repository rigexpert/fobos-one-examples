#!/usr/bin/env python3
"""
M-ary PSK lab: RRC-shaped generator + coherent demodulator + offline validation.

- gen_mpsk: M-PSK (BPSK/QPSK/8PSK), root-raised-cosine pulse shaping.
- demod_mpsk_ota: matched filter -> coarse freq (M-th power) -> symbol timing
  -> phase (M-th power) -> decision -> align to known pattern (M rotations x shift).
- Validated offline: rates {100k,500k,1M,2M,5M} x M{2,4,8}, clean & impaired.

Reuses test pattern / helpers from the FSK lab.
"""
import sys, os, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'fsk_loopback'))
import mfsk_lab as fl   # test_bytes, bytes_to_bits, syms_to_ber, impair

def rrc(beta, sps, span=8):
    N = span*sps
    t = (np.arange(N+1) - N/2)/sps
    h = np.zeros_like(t)
    for i, ti in enumerate(t):
        if abs(ti) < 1e-8:
            h[i] = 1 - beta + 4*beta/np.pi
        elif beta > 0 and abs(abs(ti) - 1/(4*beta)) < 1e-8:
            h[i] = beta/np.sqrt(2)*((1+2/np.pi)*np.sin(np.pi/(4*beta)) +
                                    (1-2/np.pi)*np.cos(np.pi/(4*beta)))
        else:
            h[i] = (np.sin(np.pi*ti*(1-beta)) + 4*beta*ti*np.cos(np.pi*ti*(1+beta))) / \
                   (np.pi*ti*(1 - (4*beta*ti)**2))
    return h/np.sqrt(np.sum(h**2))

def gen_mpsk(bits, M, Rs, Fs, beta=0.35, amp=0.9, repeats=1, span=8):
    bps = int(np.log2(M)); bits = np.tile(bits, repeats)
    nsym = len(bits)//bps
    syms = bits[:nsym*bps].reshape(nsym, bps).dot(1 << np.arange(bps-1, -1, -1))
    const = np.exp(1j*2*np.pi*syms/M)                  # binary->phase mapping
    sps = int(round(Fs/Rs))
    up = np.zeros(nsym*sps, dtype=complex); up[::sps] = const
    h = rrc(beta, sps, span)
    x = np.convolve(up, h)                              # full conv (group delay = N/2)
    x = x/np.max(np.abs(x)) * amp
    return x.astype(np.complex64), syms, sps

def demod_mpsk_ota(x, M, Rs, Fs, beta, known_syms, span=8):
    sps = int(round(Fs/Rs))
    h = rrc(beta, sps, span)
    yc = np.convolve(x, h).astype(np.complex128)        # matched filter
    gd = span*sps; n = np.arange(len(yc))
    # coarse center: PSK is a contiguous band, so the Rs-wide window of max energy
    # gives the band center (handles large OTA carrier offsets that the per-symbol
    # phase-slope can't, due to unwrap aliasing). Fine residual handled below.
    P = np.abs(np.fft.fft(yc))**2
    frq = np.fft.fftshift(np.fft.fftfreq(len(yc), 1/Fs))
    binw = Fs/len(yc); w = max(1, int(round(Rs/binw)))
    Ps = np.convolve(np.fft.fftshift(P), np.ones(w), 'same')
    # power-weighted centroid over the band (above threshold, within +/-2*Rs) — far
    # more accurate than argmax, so the residual is small enough for the phase-slope.
    band = (Ps > 0.25*Ps.max()) & (np.abs(frq) < 2*Rs)
    coarse = np.sum(frq[band]*Ps[band])/np.sum(Ps[band]) if band.any() else frq[np.argmax(Ps)]
    yc = yc*np.exp(-1j*2*np.pi*coarse/Fs*n)
    best = None
    # FINE symbol-timing search: a small fixed-sps error drifts over the capture and
    # (PSK being phase-sensitive) wrecks the decision. Real Aaronia<->Fobos clock
    # offset is ~0.5%+, so search +/-1% with ~0.01% resolution.
    for sps_t in np.arange(sps*0.99, sps*1.01 + 1e-9, max(sps*0.0001, 0.002)):
        for ph in np.linspace(0, sps_t, 4, endpoint=False):
            idx = (np.arange(int((len(yc)-2*gd)/sps_t)) * sps_t + ph + gd).astype(np.int64)
            idx = idx[(idx >= 0) & (idx < len(yc))]
            if len(idx) < 32: continue
            s = yc[idx]
            kk = np.arange(len(s))
            # fine residual carrier freq from the symbol-point M-th-power phase slope
            slope = np.polyfit(kk, np.unwrap(np.angle(s**M)), 1)[0]
            resid_khz = slope/M * Rs/(2*np.pi) / 1e3
            s = s*np.exp(-1j*(slope/M)*kk)
            phi = np.angle(np.mean(s**M))/M             # residual constant phase
            s = s*np.exp(-1j*phi)
            est = np.round(np.angle(s)/(2*np.pi/M)).astype(np.int64) % M
            ber, info = _align_psk(est, known_syms, M)
            if best is None or ber < best[0]:
                best = (ber, {'resid_khz':resid_khz,'sps':sps_t,'ph':ph,'rot':info[0],'shift':info[1]})
                if ber == 0: return best
    return best

# ── Differential M-PSK (DBPSK/DQPSK/D8PSK) ─────────────────────────────────────
# Information is carried in the phase TRANSITION between consecutive symbols, not the
# absolute phase. The demod recovers each symbol from angle(r[n]*conj(r[n-1])), so a
# slowly-wandering common carrier phase (independent free-running TX/RX LOs) CANCELS.
# This is why DQPSK closes the link where coherent QPSK can't: no absolute phase
# reference is needed, only that the wander is small WITHIN one symbol period.
def _gray_tabs(M):
    g = np.array([d ^ (d >> 1) for d in range(M)])   # binary -> Gray (1 bit/transition err)
    inv = np.zeros(M, dtype=np.int64); inv[g] = np.arange(M)
    return g, inv

def gen_dpsk(bits, M, Rs, Fs, beta=0.35, amp=0.9, repeats=1, span=8):
    """Differential (Gray-coded) M-PSK generator. Returns (iq, data_syms, sps) where
    data_syms are the INFORMATION symbols (what the demod's BER is measured against)."""
    bps = int(np.log2(M)); bits = np.tile(bits, repeats)
    nsym = len(bits) // bps
    data = bits[:nsym*bps].reshape(nsym, bps).dot(1 << np.arange(bps-1, -1, -1))
    g, _ = _gray_tabs(M)
    abs_idx = np.cumsum(g[data]) % M                 # differential accumulation of increments
    const = np.exp(1j*2*np.pi*abs_idx/M)
    sps = int(round(Fs/Rs))
    up = np.zeros(nsym*sps, dtype=complex); up[::sps] = const
    h = rrc(beta, sps, span)
    x = np.convolve(up, h)
    x = x/np.max(np.abs(x)) * amp
    return x.astype(np.complex64), data, sps

def demod_dpsk_ota(x, M, Rs, Fs, beta, known_data, span=8):
    """Differential M-PSK demod robust to carrier phase WANDER. Removes the coarse band
    offset + sharp residual carrier (constant freq), then decodes phase TRANSITIONS;
    the residual slow phase wander cancels in the difference."""
    sps = int(round(Fs/Rs)); h = rrc(beta, sps, span)
    yc = np.convolve(x, h).astype(np.complex128); gd = span*sps; n = np.arange(len(yc))
    # coarse band centroid (handles large OTA carrier offset)
    P = np.abs(np.fft.fft(yc))**2
    frq = np.fft.fftshift(np.fft.fftfreq(len(yc), 1/Fs))
    binw = Fs/len(yc); w = max(1, int(round(Rs/binw)))
    Ps = np.convolve(np.fft.fftshift(P), np.ones(w), 'same')
    band = (Ps > 0.25*Ps.max()) & (np.abs(frq) < 2*Rs)
    coarse = np.sum(frq[band]*Ps[band])/np.sum(Ps[band]) if band.any() else frq[np.argmax(Ps)]
    yc = yc*np.exp(-1j*2*np.pi*coarse/Fs*n)
    # sharp residual carrier from the M-th-power FFT peak (constant freq only)
    Y = np.abs(np.fft.fft(yc**M)); fr = np.fft.fftfreq(len(yc), 1/Fs)
    fcar = fr[np.argmax(Y)]/M
    yc = yc*np.exp(-1j*2*np.pi*fcar/Fs*n)
    _, ginv = _gray_tabs(M)
    best = None
    for sps_t in np.arange(sps*0.99, sps*1.01 + 1e-9, max(sps*0.0001, 0.002)):
        for ph in np.linspace(0, sps_t, 4, endpoint=False):
            idx = (np.arange(int((len(yc)-2*gd)/sps_t)) * sps_t + ph + gd).astype(np.int64)
            idx = idx[(idx >= 0) & (idx < len(yc))]
            if len(idx) < 32: continue
            s = yc[idx]
            d = s[1:]*np.conj(s[:-1])                 # differential: common phase cancels
            off = np.angle(np.mean(d**M))/M           # residual constant rotation (freq leftover)
            didx = np.round(np.angle(d*np.exp(-1j*off))/(2*np.pi/M)).astype(np.int64) % M
            ber, info = _align_dpsk(didx, known_data, ginv, M)
            if best is None or ber < best[0]:
                best = (ber, {'sps':sps_t,'ph':ph,'fcar_khz':(coarse+fcar)/1e3,
                              'rot':info[0],'shift':info[1]})
                if ber == 0: return best
    return best

def _align_dpsk(didx, known_data, ginv, M):
    """Resolve the constant differential-index bias (M rotations on didx, BEFORE Gray
    decode) and the circular shift, against the known data symbols. Min BER."""
    n = len(didx); L = len(known_data)
    if n < 16: return 1.0, (0, 0)
    kt = np.tile(known_data, n//L + 2)[:n]
    best = (1.0, (0, 0))
    for rot in range(M):
        data_hat = ginv[(didx + rot) % M]
        a = np.exp(2j*np.pi*data_hat/M); b = np.exp(2j*np.pi*kt/M)
        corr = np.abs(np.fft.ifft(np.fft.fft(a)*np.conj(np.fft.fft(b))))
        sh = int(np.argmax(corr))
        for cand in {sh, (n-sh)%n, (sh+1)%n, (sh-1)%n}:
            ber = fl.syms_to_ber(data_hat, np.roll(kt, cand), M)
            if ber < best[0]: best = (ber, (rot, cand))
    return best[0], best[1]

def _align_psk(est, known, M):
    """Try all M phase rotations x circular shift (FFT-correlation), min BER."""
    L = len(known); n = len(est)
    if n < 16: return 1.0, (0,0)
    kt = np.tile(known, n//L + 2)[:n]
    best = (1.0, (0,0))
    for rot in range(M):
        e = (est + rot) % M
        a = np.exp(2j*np.pi*e/M); b = np.exp(2j*np.pi*kt/M)
        corr = np.abs(np.fft.ifft(np.fft.fft(a)*np.conj(np.fft.fft(b))))
        sh = int(np.argmax(corr))
        for cand in {sh, (n-sh)%n, (sh+1)%n, (sh-1)%n}:
            ber = fl.syms_to_ber(e, np.roll(kt, cand), M)
            if ber < best[0]: best = (ber, (rot, cand))
    return best[0], best[1]

if __name__ == '__main__':
    data = fl.test_bytes(); bits = fl.bytes_to_bits(data)
    rates = [100e3,500e3,1e6,2e6,5e6]; Ms = [2,4,8]
    mode = sys.argv[1] if len(sys.argv) > 1 else 'clean'
    snr = 25.0
    print(f"PSK offline ({mode}{', SNR='+str(snr)+'dB +foff+drift' if mode=='impair' else ''}); beta=0.35 sps=8")
    print(f"{'symrate':>9} {'M':>3} {'BER':>9}")
    allok = True
    for Rs in rates:
        for M in Ms:
            Fs = 8*Rs
            x, syms, sps = gen_mpsk(bits, M, Rs, Fs, repeats=2)
            if mode == 'impair':
                x = fl.impair(x, Fs, snr, 0.02*Rs, 30)
            ber, info = demod_mpsk_ota(x, M, Rs, Fs, 0.35, syms)
            tag = 'OK' if ber < 0.005 else ('marg' if ber < 0.05 else 'FAIL')
            allok &= ber < 0.05
            mm = {2:'BPSK',4:'QPSK',8:'8PSK'}[M]
            print(f"{Rs/1e3:8.0f}k {mm:>4} {ber*100:8.3f}% {tag}")
    print("ALL OK" if allok else "SOME HIGH")
