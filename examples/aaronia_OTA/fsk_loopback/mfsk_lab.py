#!/usr/bin/env python3
"""
M-ary FSK lab: generator + non-coherent demodulator + offline validation.

- gen_mfsk: continuous-phase M-FSK (M=2/4/8), orthogonal tone spacing.
- demod_mfsk: per-symbol bank of M matched filters (argmax) with timing search.
- test pattern: fixed pseudo-random byte sequence (deterministic), aligned by
  correlation against the known bits to measure BER.

Offline self-test sweeps symbol rates {100k,500k,1M,2M,5M} x M{2,4,8}.
"""
import sys, numpy as np

# ── Test pattern ──────────────────────────────────────────────────────────────
def test_bytes(nbytes=200, seed=1234):
    return np.random.RandomState(seed).randint(0, 256, nbytes).astype(np.uint8)

def bytes_to_bits(b):
    return np.unpackbits(np.asarray(b, dtype=np.uint8))

# ── Generator ─────────────────────────────────────────────────────────────────
def gen_mfsk(bits, M, symrate, Fs, spacing, amp=0.9, repeats=1):
    bps  = int(np.log2(M))
    bits = np.tile(bits, repeats)
    nsym = len(bits) // bps
    b    = bits[:nsym*bps].reshape(nsym, bps)
    syms = b.dot(1 << np.arange(bps-1, -1, -1))          # MSB-first within symbol
    tones = (syms - (M-1)/2.0) * spacing                  # Hz, symmetric about 0
    n     = int(round(nsym * Fs / symrate))
    sidx  = np.minimum((np.arange(n) * symrate / Fs).astype(np.int64), nsym-1)
    finst = tones[sidx]
    phase = 2*np.pi*np.cumsum(finst)/Fs
    iq    = (amp*np.exp(1j*phase)).astype(np.complex64)
    return iq, syms

# ── Demodulator (vectorized non-coherent matched-filter bank) ─────────────────
def demod_exact(x, M, sps, freqs, Fs, off=0):
    """Vectorized M-FSK demod at known integer sps and offset -> symbol indices."""
    k = int(round(sps))
    nsym = (len(x) - off) // k
    xx = x[off:off+nsym*k].reshape(nsym, k)
    t  = np.arange(k) / Fs
    refs = np.exp(-1j*2*np.pi*np.outer(freqs, t))          # (M, k) = conj-tones
    sc = np.abs(xx @ refs.T)                                # matched filter (nsym, M)
    return np.argmax(sc, axis=1)

def demod_ota(x, M, symrate, Fs, spacing, known_syms):
    """Robust M-FSK demod for real RF: corrects center offset, searches symbol
    timing (clock drift) + phase, aligns to known pattern. Returns (ber,info)."""
    # Center estimate: mean instantaneous frequency (coarse) + a small refine that
    # is chosen by the min-BER search below (robust for tight AND wide spacing,
    # using the known pattern as ground truth).
    freqs = (np.arange(M)-(M-1)/2.0)*spacing
    n = np.arange(len(x))
    sps0 = Fs/symrate
    half = (M-1)/2.0*spacing + symrate
    fr = np.fft.fftfreq(len(x), 1/Fs)
    # Coarse center via power-weighted spectral centroid of the occupied band.
    # (Mean instantaneous frequency wraps to a meaningless value for wide tone
    # spacing: averaging two far-apart tones aliases past +/-Fs/2, e.g. ~12.5 MHz
    # for 16 MHz spacing — the freq search then can't recover. The centroid of the
    # symmetric M-FSK band gives the true center for any spacing.)
    P = np.abs(np.fft.fft(x))**2
    roi = np.abs(fr) < 1.5*half
    bandm = roi & (P > 0.1*P[roi].max())
    coarse = float(np.sum(fr[bandm]*P[bandm])/np.sum(P[bandm])) if bandm.any() else 0.0
    best = None
    for df in coarse + np.linspace(-symrate, symrate, 7):
        xb = x*np.exp(-1j*2*np.pi*df/Fs*n)
        X = np.fft.fft(xb); X[np.abs(fr) > half] = 0
        xf = np.fft.ifft(X).astype(np.complex64)
        # FINE symbol-timing search: real Aaronia<->Fobos clock offset is ~0.5%+,
        # WIDER than the old +/-0.3% window (which missed the true sps entirely) and
        # needs fine resolution so a small sps error doesn't drift into failure.
        # Captures are bounded to ~4000 symbols upstream to keep this resolution enough.
        for sps in np.arange(sps0*0.99, sps0*1.01 + 1e-9, max(sps0*1e-4, 0.002)):
            k = int(round(sps))
            for ph in np.linspace(0, sps, 4, endpoint=False):
                nsym = int((len(xf)-ph)/sps)
                if nsym < 32: continue
                idx = (np.arange(nsym)*sps+ph).astype(np.int64)
                cols = np.clip(idx[:,None]+np.arange(k)[None,:], 0, len(xf)-1)
                xx = xf[cols]
                t = np.arange(k)/Fs
                refs = np.exp(-1j*2*np.pi*np.outer(freqs,t))
                syms = np.argmax(np.abs(xx @ refs.T), axis=1)
                for flip in (0,1):
                    ds = (M-1-syms) if flip else syms
                    ber, sh = _align_ber(ds, known_syms, M)
                    if best is None or ber < best[0]:
                        best = (ber, {'sps':sps,'ph':ph,'flip':flip,'shift':sh,
                                      'foff_khz':df/1e3,'nsym':nsym})
                        if ber == 0: return best
    return best

def _align_ber(dsyms, known, M):
    """Fast alignment via FFT circular cross-correlation of exp(2pi*sym/M)."""
    L = len(known); n = len(dsyms)
    if n < 16: return (1.0, 0)
    kt = np.tile(known, n//L + 2)[:n]
    a = np.exp(2j*np.pi*dsyms/M); b = np.exp(2j*np.pi*kt/M)
    corr = np.abs(np.fft.ifft(np.fft.fft(a) * np.conj(np.fft.fft(b))))
    sh = int(np.argmax(corr))
    best = (1.0, 0)
    for cand in {sh, (n-sh) % n, (sh+1) % n, (sh-1) % n}:
        ber = syms_to_ber(dsyms, np.roll(kt, cand), M)
        if ber < best[0]: best = (ber, cand)
    return best

def syms_to_ber(dsyms, ref_syms, M):
    bps = int(np.log2(M))
    n = min(len(dsyms), len(ref_syms))
    db = np.unpackbits(dsyms[:n].astype(np.uint8)[:,None], axis=1)[:, -bps:]
    rb = np.unpackbits(ref_syms[:n].astype(np.uint8)[:,None], axis=1)[:, -bps:]
    return np.mean(db != rb)

# ── Offline validation matrix ─────────────────────────────────────────────────
def write_iqraw(iq, Fs, center, path):
    """Write interleaved float32 .iq + RTSA_RAW_IQ_File .iq.xml sidecar."""
    import os, time
    inter = np.empty(2*len(iq), dtype=np.float32)
    inter[0::2] = iq.real; inter[1::2] = iq.imag
    inter.tofile(path)
    name = os.path.basename(path)
    xml = (f'<?xml version="1.0" encoding="UTF-8"?>\n<RTSA_RAW_IQ_File>\n'
           f'    <Samples>0</Samples>\n'
           f'    <Clock unit="Hz">{Fs:.6f}</Clock>\n'
           f'    <CenterFrequency unit="Hz">{center:.6f}</CenterFrequency>\n'
           f'    <SampleStartTime unit="Epoch time">{time.time():.6f}</SampleStartTime>\n'
           f'    <Format>complex</Format>\n    <DataType>float32</DataType>\n'
           f'    <DataFilename>{name}</DataFilename>\n</RTSA_RAW_IQ_File>\n')
    open(path + '.xml', 'w').write(xml)

def genfile(M, Rs, center=385e6, outdir='/tmp/mfsk', repeats=6):
    import os
    os.makedirs(outdir, exist_ok=True)
    spacing = Rs
    bw = (M-1)*spacing + Rs
    Fs = max(2e6, _nice_rate(1.6*bw))            # file sample rate (IQModulator upsamples)
    bits = bytes_to_bits(test_bytes())
    iq, syms = gen_mfsk(bits, M, Rs, Fs, spacing, repeats=repeats)
    name = f"mfsk_M{M}_R{int(Rs)}"
    path = os.path.join(outdir, name + '.iq')
    write_iqraw(iq, Fs, center, path)
    np.save(os.path.join(outdir, name + '.syms.npy'), syms)
    print(f"{name}.iq  Fs={Fs/1e6:.3f}M bw={bw/1e6:.3f}M sps={Fs/Rs:.1f} "
          f"{len(iq)} samp ({len(iq)*8/1e6:.1f}MB)")
    return path, Fs, syms

def _nice_rate(x):
    for r in [2e6,4e6,8e6,16e6,32e6,64e6]:
        if r >= x: return r
    return 64e6

def impair(x, Fs, snr_db, foff_hz, ppm):
    if ppm:                                          # sample-clock drift via resample
        nlen = int(len(x)/(1+ppm*1e-6))
        x = np.interp(np.arange(nlen)*(1+ppm*1e-6), np.arange(len(x)), x.real) + \
            1j*np.interp(np.arange(nlen)*(1+ppm*1e-6), np.arange(len(x)), x.imag)
    n = np.arange(len(x))
    x = x*np.exp(1j*2*np.pi*foff_hz/Fs*n)            # carrier offset
    p = np.mean(np.abs(x)**2)
    npow = p/(10**(snr_db/10))
    x = x + np.sqrt(npow/2)*(np.random.randn(len(x))+1j*np.random.randn(len(x)))
    return x.astype(np.complex64)

if __name__ == '__main__':
    data = test_bytes()
    bits = bytes_to_bits(data)
    rates = [100e3, 500e3, 1e6, 2e6, 5e6]
    Ms    = [2, 4, 8]

    if len(sys.argv) > 1 and sys.argv[1] == 'ota':
        print("OTA-robust demod vs impaired channel (SNR=15dB, foff, 40ppm drift)")
        print(f"{'symrate':>9} {'M':>2} {'BER':>9}  info")
        for sr in rates:
            for M in Ms:
                spacing = sr; sps = max(16,4*M); Fs = sr*sps
                iq, syms = gen_mfsk(bits, M, sr, Fs, spacing, repeats=2)
                y = impair(iq, Fs, 15.0, 0.03*sr, 40)
                ber, info = demod_ota(y, M, sr, Fs, spacing, syms)
                tag = 'OK' if ber < 0.02 else ('marg' if ber<0.1 else 'FAIL')
                print(f"{sr/1e3:8.0f}k {M:2d} {ber*100:8.3f}%  {tag} sps={info['sps']:.2f} flip={info['flip']} foff={info['foff_khz']:.0f}k")
        sys.exit(0)

    print(f"pattern: {len(data)} bytes ({len(bits)} bits), seed=1234")
    print(f"{'symrate':>9} {'M':>2} {'spacing':>9} {'Fs(Msps)':>9} {'sps':>5} {'BER':>9}")
    allok = True
    for sr in rates:
        for M in Ms:
            spacing = sr                              # orthogonal tone spacing
            sps = max(16, 4*M)
            Fs  = sr * sps
            iq, syms = gen_mfsk(bits, M, sr, Fs, spacing, repeats=1)
            freqs = (np.arange(M) - (M-1)/2.0) * spacing
            d = demod_exact(iq, M, sps, freqs, Fs, off=0)
            ber = syms_to_ber(d, syms, M)
            ok = ber < 1e-9; allok &= ok
            print(f"{sr/1e3:8.0f}k {M:2d} {spacing/1e3:7.0f}k {Fs/1e6:8.2f} {sps:5d} {ber*100:8.4f}% {'OK' if ok else 'FAIL'}")
    print("ALL PASS" if allok else "SOME FAILED")
