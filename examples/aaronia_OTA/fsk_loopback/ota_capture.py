#!/usr/bin/env python3
"""Capture from Fobos and M-FSK demodulate a config. Used by the OTA sweep.
Usage: ota_capture.py M Rs Fc Fs dur lna vga [symsdir]
"""
import sys, ctypes, os, numpy as np
import mfsk_lab as mf

LIB = os.path.realpath(os.path.join(os.path.dirname(__file__),
    '../../libfobos-sdr-agile/build/libfobos_sdr.so'))

def capture(Fc, Fs, dur, lna, vga):
    lib = ctypes.CDLL(LIB)
    for fn in ('fobos_sdr_open','fobos_sdr_close','fobos_sdr_set_frequency',
               'fobos_sdr_set_samplerate','fobos_sdr_set_lna_gain','fobos_sdr_set_vga_gain',
               'fobos_sdr_read_async','fobos_sdr_cancel_async'):
        getattr(lib, fn).restype = ctypes.c_int
    CB = ctypes.CFUNCTYPE(None, ctypes.POINTER(ctypes.c_float), ctypes.c_uint32,
                          ctypes.c_void_p, ctypes.c_void_p)
    dev = ctypes.c_void_p(); lib.fobos_sdr_open(ctypes.byref(dev), 0)
    lib.fobos_sdr_set_lna_gain(dev, ctypes.c_uint(lna))
    lib.fobos_sdr_set_vga_gain(dev, ctypes.c_uint(vga))
    lib.fobos_sdr_set_samplerate(dev, ctypes.c_double(Fs))
    lib.fobos_sdr_set_frequency(dev, ctypes.c_double(Fc))
    need = int(dur*Fs); ch = []; got = [0]
    def cb(p, n, d, u):
        if got[0] >= need: lib.fobos_sdr_cancel_async(dev); return
        a = ctypes.cast(p, ctypes.c_void_p).value
        ch.append(np.frombuffer((ctypes.c_float*(n*2)).from_address(a),
                                dtype=np.float32, count=n*2).copy()); got[0]+=n
        if got[0] >= need: lib.fobos_sdr_cancel_async(dev)
    lib.fobos_sdr_read_async(dev, CB(cb), None, 16, 65536); lib.fobos_sdr_close(dev)
    raw = np.concatenate(ch)[:need*2]
    return (raw[0::2] + 1j*raw[1::2]).astype(np.complex64)

if __name__ == '__main__':
    M=int(sys.argv[1]); Rs=float(sys.argv[2]); Fc=float(sys.argv[3])
    Fs=float(sys.argv[4]); dur=float(sys.argv[5]); lna=int(sys.argv[6]); vga=int(sys.argv[7])
    sdir=sys.argv[8] if len(sys.argv)>8 else '/tmp/mfsk'
    x = capture(Fc, Fs, dur, lna, vga)
    m = np.mean(np.abs(x))
    # coarse spectrum to show where energy is
    N=8192; w=np.hanning(N); psd=np.zeros(N)
    for i in range(x.size//N): psd += np.abs(np.fft.fftshift(np.fft.fft(x[i*N:(i+1)*N]*w)))**2
    f=np.fft.fftshift(np.fft.fftfreq(N,1/Fs)); pk=f[np.argmax(psd)]
    print(f"mean|x|={m:.4f}  peak@{(Fc+pk)/1e6:.4f}MHz  ({'SIGNAL' if m>0.003 else 'NOISE'})")
    if m <= 0.003:
        sys.exit(0)
    syms = np.load(os.path.join(sdir, f"mfsk_M{M}_R{int(Rs)}.syms.npy"))
    ber, info = mf.demod_ota(x, M, Rs, Fs, Rs, syms)
    print(f"M={M} Rs={Rs/1e3:.0f}k  BER={ber*100:.3f}%  "
          f"foff={info['foff_khz']:.0f}k sps={info['sps']:.2f} flip={info['flip']}")
