#!/usr/bin/env python3
"""DQPSK over the Aaronia->Fobos link (independent free-running LOs).

WHY: coherent QPSK needs an absolute carrier-phase reference, which the unlocked
TX/RX oscillators don't provide (the phase WANDERS). DQPSK carries data in the phase
TRANSITION between symbols, so the wandering common phase cancels in the demod -- no
shared clock required (this is what real OTA links use, plus pilots/FEC).

Modes:
  dqpsk_ota.py offline      # simulate phase wander: coherent QPSK vs DQPSK (proof)
  dqpsk_ota.py ota [M Rs]   # generate DQPSK .iq -> Aaronia File Source -> Fobos -> BER
"""
import sys, os, re, time, subprocess, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'fsk_loopback'))
import mpsk_lab as p
import ota_sweep as fk

HOST = "vertical@10.10.1.74"


# ── phase-wander impairment (Wiener / random-walk phase, like free-running LOs) ──
def add_wander(x, rad_per_sample, foff_hz=0.0, Fs=1.0, snr_db=30.0):
    n = np.arange(len(x))
    ph = np.cumsum(np.random.randn(len(x)) * rad_per_sample)   # random-walk phase
    y = x * np.exp(1j*(ph + 2*np.pi*foff_hz/Fs*n))
    pw = np.mean(np.abs(y)**2); npw = pw/(10**(snr_db/10))
    y = y + np.sqrt(npw/2)*(np.random.randn(len(y))+1j*np.random.randn(len(y)))
    return y.astype(np.complex64)


def offline():
    """Show DQPSK beating coherent QPSK as the per-symbol carrier wander grows."""
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    M, Rs, Fs = 4, 1e6, 8e6
    sps = int(round(Fs/Rs))
    print(f"OFFLINE QPSK vs DQPSK under random-walk carrier wander  (M={M}, Rs={Rs/1e6:.0f}M, sps={sps}, SNR=30 dB)")
    print(f"{'wander/symbol':>14} {'coherent QPSK':>14} {'DQPSK':>10}")
    # absolute-encoded (for coherent) and differential-encoded (for DQPSK) signals
    xa, sa, _ = p.gen_mpsk(bits, M, Rs, Fs, beta=0.35, repeats=2)
    xd, sd, _ = p.gen_dpsk(bits, M, Rs, Fs, beta=0.35, repeats=2)
    for deg in (0, 2, 5, 10, 20, 40):
        # rad/symbol -> rad/sample so the random walk std per symbol = deg degrees
        rps = np.deg2rad(deg) / np.sqrt(sps)
        ya = add_wander(xa, rps, Fs=Fs); yd = add_wander(xd, rps, Fs=Fs)
        bc, _ = p.demod_mpsk_ota(ya, M, Rs, Fs, 0.35, sa)
        bd, _ = p.demod_dpsk_ota(yd, M, Rs, Fs, 0.35, sd)
        print(f"{deg:11d} deg {bc*100:12.2f}% {bd*100:8.2f}%")
    print("\n(coherent degrades as wander grows; DQPSK stays low -> it ignores the "
          "common wandering phase. This is the OTA-without-shared-clock case.)")


# ── OTA ────────────────────────────────────────────────────────────────────────
def gen_push(M, Rs, amp=0.3):
    """Generate a Gray-coded DQPSK file and push it to the Aaronia File Source (test.iq)."""
    FS = 8e6                                          # <= IQ Modulator input ceiling
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    bps = int(np.log2(M)); per = (len(bits)//bps)*int(round(FS/Rs))
    reps = max(1, int(round(3.0*FS/per)))
    x, data, sps = p.gen_dpsk(bits, M, Rs, FS, beta=0.35, amp=amp, repeats=reps)
    base = "/tmp/dqpsk/test"; os.makedirs("/tmp/dqpsk", exist_ok=True)
    fk.write_fs_files(x, FS, fk.FC, base)
    t = open(base+".iq.xml").read()
    open(base+".iq.xml", "w").write(re.sub(r'<DataFilename>.*?</DataFilename>',
                                           '<DataFilename>test.iq</DataFilename>', t))
    subprocess.run(["scp", "-o", "ConnectTimeout=25", "-o", "BatchMode=yes",
                    base+".iq", base+".iq.xml", f"{HOST}:Aaronia/"], check=True,
                   capture_output=True)
    return FS, data, sps


def detect_rate(x, M, Rs_nom, Fs):
    """True on-air symbol rate from the differential-symbol repeat period (immune to
    carrier wander). The chain can scale the rate (e.g. 8 MHz file replayed at 10 MHz)."""
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    _, data, _ = p.gen_dpsk(bits, M, Rs_nom, 8*Rs_nom, beta=0.35)
    L = len(data)
    sps = int(round(Fs/Rs_nom)); h = p.rrc(0.35, sps, 8)
    yc = np.convolve(x, h).astype(complex); nn = np.arange(len(yc))
    Y = np.abs(np.fft.fft(yc**M)); frq = np.fft.fftfreq(len(yc), 1/Fs); fc = frq[np.argmax(Y)]/M
    yc = yc*np.exp(-1j*2*np.pi*fc/Fs*nn); gd = 8*sps
    idx = (np.arange(int((len(yc)-2*gd)/sps))*sps + gd).astype(int); idx = idx[idx < len(yc)]
    s = yc[idx]; d = s[1:]*np.conj(s[:-1]); d = d/(np.abs(d)+1e-9); dd = d - d.mean()
    ac = np.abs(np.correlate(dd, dd, 'full')); ac = ac[len(ac)//2:]
    lo, hi = int(L*0.6), min(int(L*1.5), len(ac)-1)
    if hi <= lo+2: return Rs_nom, 0.0
    pk = lo + int(np.argmax(ac[lo:hi]))
    return Rs_nom*L/pk, ac[pk]/ac[0]


def ota(M, Rs):
    from ota_capture import capture
    print(f"DQPSK OTA: M={M} ({ {2:'DBPSK',4:'DQPSK',8:'D8PSK'}[M] }) Rs={Rs/1e6:.3f} M @ {fk.FC/1e6:.0f} MHz")
    gen_push(M, Rs)
    print("restart RTSA + play ...");
    import restart_rtsa; restart_rtsa.restart()
    subprocess.run(["curl","-s","-X","POST","http://localhost:8080/api/detach"],
                   capture_output=True)                # release Fobos from the web app
    Fs = min(50e6, max(8e6, 16*Rs))
    dur = max(0.04, 4000/Rs)
    x = capture(fk.FC, Fs, dur, 0, 4); m = np.mean(np.abs(x))
    print(f"captured {len(x)} samp mean={m:.3f}")
    if m < 0.003: print("NO SIGNAL"); return
    Rs_true, q = detect_rate(x, M, Rs, Fs)
    if abs(Rs_true-Rs)/Rs > 0.02:
        Rs = Rs_true; Fs = min(50e6, 16*Rs)
        print(f"auto-detected on-air Rs = {Rs/1e6:.4f} M (periodicity {q:.2f}); re-capturing")
        x = capture(fk.FC, Fs, max(0.04, 4000/Rs), 0, 4); print(f"  re-captured mean={np.mean(np.abs(x)):.3f}")
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    _, data, _ = p.gen_dpsk(bits, M, Rs, 8*Rs, beta=0.35)
    ber, info = p.demod_dpsk_ota(x, M, Rs, Fs, 0.35, data)
    print(f"DQPSK BER = {ber*100:.2f}%   (resid {info['fcar_khz']:.1f} kHz, rot {info['rot']}, shift {info['shift']})")
    print("PASS" if ber < 0.05 else "HIGH")


if __name__ == '__main__':
    mode = sys.argv[1] if len(sys.argv) > 1 else 'offline'
    if mode == 'offline':
        offline()
    else:
        M  = int(sys.argv[2]) if len(sys.argv) > 2 else 4
        Rs = float(sys.argv[3]) if len(sys.argv) > 3 else 1e6
        ota(M, Rs)
