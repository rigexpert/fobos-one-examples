#!/usr/bin/env python3
"""
OTA M-PSK sweep: generate RRC-PSK -> swap into Aaronia File Source -> RTSA restart
-> TX -> Fobos capture -> coherent demod -> BER. Reuses the FSK swap mechanism.
Sweeps symbol rate x M (BPSK/QPSK/8PSK). 15 configs.
Usage: ota_sweep_psk.py sweep [rates_khz_csv]   |   ota_sweep_psk.py M Rs
"""
import sys, os, time, numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'fsk_loopback'))
import ota_sweep as fk          # swap_and_start, write_fs_files, FC, FOBOS_RATES
from ota_capture import capture
import mpsk_lab as p

BETA = 0.35

def psk_cap_rate(Rs):           # need sps>=~6 at the capture for the RRC demod
    for r in fk.FOBOS_RATES:
        if r*1e6 >= 6*Rs: return r*1e6
    return 64e6

def run_psk(M, Rs, dur=None, lna=0, vga=4, repeats=8):
    if dur is None: dur = max(2500/Rs, 0.004)   # bound capture to ~2500 symbols (limits timing drift)
    Fs_file = 8*Rs
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    x, syms, sps = p.gen_mpsk(bits, M, Rs, Fs_file, beta=BETA, repeats=repeats)
    base = "/tmp/mpsk/fs"; os.makedirs("/tmp/mpsk", exist_ok=True)
    fk.write_fs_files(x, Fs_file, fk.FC, base)
    fk.swap_and_start(base)
    Fs_cap = psk_cap_rate(Rs)
    xc = capture(fk.FC, Fs_cap, dur, lna, vga); m = np.mean(np.abs(xc))
    if m < 0.003:
        return None, {'note':'no signal','mean':m}, Fs_cap
    ber, info = p.demod_mpsk_ota(xc, M, Rs, Fs_cap, BETA, syms)
    info['mean']=m; info['bw']=Rs*(1+BETA); return ber, info, Fs_cap

def sweep(rates):
    Ms=[2,4,8]; path='/tmp/mpsk/psk_results.txt'; os.makedirs('/tmp/mpsk',exist_ok=True)
    out=open(path,'w')
    hdr=f"{'M':>4} {'Rs':>8} {'bw':>7} {'Fs':>5} {'BER':>9} {'mean':>6}"
    print(hdr); out.write(hdr+'\n'); out.flush()
    for Rs in rates:
        for M in Ms:
            t0=time.time()
            try: ber,info,fs=run_psk(M,Rs)
            except Exception as e: ber=None; info={'note':str(e)[:50]}; fs=0
            mm={2:'BPSK',4:'QPSK',8:'8PSK'}[M]
            if ber is None:
                line=f"{mm:>4} {Rs/1e3:7.0f}k {'--':>7} {'--':>5} {'NOSIG':>9} {info.get('mean',0):.3f} {info.get('note','')}"
            else:
                line=f"{mm:>4} {Rs/1e3:7.0f}k {info['bw']/1e6:6.2f}M {fs/1e6:4.0f}M {ber*100:8.3f}% {info['mean']:.3f}"
            print(line+f"  ({time.time()-t0:.0f}s)"); out.write(line+'\n'); out.flush()
            if not fk.host_up() if hasattr(fk,'host_up') else False:
                out.write("ABORT: host unreachable\n"); out.close(); print("ABORT"); return
    out.close(); print("PSK SWEEP DONE -> "+path)

if __name__=='__main__':
    if sys.argv[1]=='sweep':
        rates=[float(r)*1e3 for r in sys.argv[2].split(',')] if len(sys.argv)>2 else [100e3,500e3,1e6,2e6,5e6]
        sweep(rates); sys.exit(0)
    M=int(sys.argv[1]); Rs=float(sys.argv[2])
    ber,info,fs=run_psk(M,Rs)
    print(f"{ {2:'BPSK',4:'QPSK',8:'8PSK'}[M] } Rs{Rs/1e3:.0f}k -> BER={None if ber is None else round(ber*100,3)}% {info}")
