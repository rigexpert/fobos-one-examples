#!/usr/bin/env python3
"""
OTA M-FSK sweep: generate signal -> swap into Aaronia File Source -> TX -> Fobos
capture -> demod -> BER. Sweeps symbol rate x M x tone-spacing(narrow/mid/large).

Per-test mechanism (File Source only reloads on RTSA restart, file locked while running):
  Stop RTSA -> scp overwrite File Source .iq+.xml -> launch RTSA
  -> 54666 V6B connect+run -> 54666 Filesource playbutton -> Fobos capture.
"""
import sys, os, time, base64, subprocess, numpy as np
import mfsk_lab as mf
from ota_capture import capture

HOST   = "vertical@10.10.1.74"
SSHOPT = ["-o","ConnectTimeout=10","-o","BatchMode=yes"]
FS_REMOTE = "Aaronia/fsk_386mhz_100kbd.iq"          # File Source's file (scp dest, ~home)
FC = 385e6
FOBOS_RATES = [8,10,12.5,16,20,25,32,40,50,64]      # MHz

def _b64(d): return base64.b64encode(d).decode()
PUT_V6  = _b64(b'{"request":1,"receiverName":"Block_Spectran_V6B_0","config":{"type":"group","items":[{"type":"group","name":"main","items":[{"type":"bool","name":"connect","value":true},{"type":"bool","name":"run","value":true}]}]}}')
PUT_PB  = _b64(b'{"request":2,"receiverName":"Block_Filesource_0","config":{"type":"group","items":[{"type":"bool","name":"playbutton","value":true}]}}')

def ssh(cmd, t=60):
    return subprocess.run(["ssh",*SSHOPT,HOST,cmd], capture_output=True, text=True, timeout=t)

def scp(local, remote, t=120):
    return subprocess.run(["scp",*SSHOPT,local,f"{HOST}:{remote}"], capture_output=True, text=True, timeout=t)

def pick_rate(bw):
    for r in FOBOS_RATES:
        if r*1e6 >= 1.35*bw: return r*1e6
    return 64e6

def write_fs_files(iq, Fs_file, center, base):
    inter = np.empty(2*len(iq), dtype=np.float32); inter[0::2]=iq.real; inter[1::2]=iq.imag
    inter.tofile(base+".iq")
    xml = (f'<?xml version="1.0" encoding="UTF-8"?>\n<RTSA_RAW_IQ_File>\n<Samples>0</Samples>\n'
           f'<Clock unit="Hz">{Fs_file:.6f}</Clock>\n<CenterFrequency unit="Hz">{center:.6f}</CenterFrequency>\n'
           f'<SampleStartTime unit="Epoch time">{time.time():.6f}</SampleStartTime>\n'
           f'<Format>complex</Format>\n<DataType>float32</DataType>\n'
           f'<DataFilename>fsk_386mhz_100kbd.iq</DataFilename>\n</RTSA_RAW_IQ_File>\n')
    open(base+".iq.xml","w").write(xml)

def swap_and_start(local_base):
    ssh('Stop-Process -Name "*Aaronia*" -Force -EA SilentlyContinue; Start-Sleep 4; echo done', t=30)
    scp(local_base+".iq",     FS_REMOTE)
    scp(local_base+".iq.xml", FS_REMOTE+".xml")
    ssh('schtasks /run /tn AaroniaGUI', t=30)
    # wait for RTSA, then connect V6B + start playback
    ssh(f"Start-Sleep 42; [IO.File]::WriteAllBytes($env:TEMP+'\\v6.json',[Convert]::FromBase64String('{PUT_V6}')); "
        f"curl.exe -s --max-time 8 -X PUT --data ('@'+$env:TEMP+'\\v6.json') http://localhost:54666/remoteconfig | Out-Null; "
        f"Start-Sleep 13; [IO.File]::WriteAllBytes($env:TEMP+'\\pb.json',[Convert]::FromBase64String('{PUT_PB}')); "
        f"curl.exe -s --max-time 6 -X PUT --data ('@'+$env:TEMP+'\\pb.json') http://localhost:54666/remoteconfig | Out-Null; "
        f"Start-Sleep 2; echo started", t=90)

NSYM_DEMOD = 4000               # bound demod window: keeps fine-sps timing resolution sufficient

def run_config(M, Rs, spacing, dur=None, lna=0, vga=4, repeats=8):
    bw = (M-1)*spacing + Rs
    Fs_file = pick_rate(bw)                          # file rate (IQModulator upsamples to 61M)
    bits = mf.bytes_to_bits(mf.test_bytes())
    iq, syms = mf.gen_mfsk(bits, M, Rs, Fs_file, spacing, repeats=repeats)
    base = "/tmp/mfsk/fs"
    os.makedirs("/tmp/mfsk", exist_ok=True)
    write_fs_files(iq, Fs_file, FC, base)
    swap_and_start(base)
    Fs_cap = pick_rate(bw)
    if dur is None:                                  # capture a bit more than we demod
        dur = max(1.5*NSYM_DEMOD/Rs, 0.006)
    x = capture(FC, Fs_cap, dur, lna, vga)
    m = np.mean(np.abs(x))
    if m < 0.003:
        return None, {'note':'no signal','mean':m}, Fs_cap
    nmax = int(NSYM_DEMOD*Fs_cap/Rs)                 # steady middle chunk (skips TX startup)
    if len(x) > nmax:
        s0 = (len(x)-nmax)//2; x = x[s0:s0+nmax]
    ber, info = mf.demod_ota(x, M, Rs, Fs_cap, spacing, syms)
    info['mean']=m; info['Fs_cap']=Fs_cap; info['bw']=bw
    return ber, info, Fs_cap

def sweep():
    rates=[100e3,500e3,1e6,2e6,5e6]; Ms=[2,4,8]
    path='/tmp/mfsk/fsk_results.txt'; os.makedirs('/tmp/mfsk',exist_ok=True)
    out=open(path,'w')
    hdr=f"{'M':>2} {'Rs':>8} {'spacing':>9} {'label':>6} {'bw':>7} {'Fs':>5} {'BER':>9} {'mean':>6} {'foff':>7}"
    print(hdr); out.write(hdr+'\n'); out.flush()
    for Rs in rates:
        for M in Ms:
            dfmax = (48e6-Rs)/(M-1) if M>1 else 48e6
            cands=[('narrow',Rs),('mid',min(4*Rs,dfmax)),('large',min(16*Rs,dfmax))]
            seen=set()
            for label,sp in cands:
                sp=max(Rs,sp); key=round(sp/1e3)
                if key in seen: continue
                seen.add(key)
                t0=time.time()
                try:
                    ber,info,fs=run_config(M,Rs,sp)
                except Exception as e:
                    ber=None; info={'note':str(e)[:50]}; fs=0
                if ber is None:
                    line=f"{M:2d} {Rs/1e3:7.0f}k {sp/1e3:8.0f}k {label:>6} {'--':>7} {'--':>5} {'NOSIG':>9} {info.get('mean',0):.3f} {info.get('note','')}"
                else:
                    line=f"{M:2d} {Rs/1e3:7.0f}k {sp/1e3:8.0f}k {label:>6} {info['bw']/1e6:6.2f}M {fs/1e6:4.0f}M {ber*100:8.3f}% {info['mean']:.3f} {info['foff_khz']:6.0f}k"
                print(line+f"  ({time.time()-t0:.0f}s)"); out.write(line+'\n'); out.flush()
    out.close(); print("SWEEP DONE -> "+path)

if __name__ == '__main__':
    if sys.argv[1]=='sweep':
        sweep(); sys.exit(0)
    # quick single-config test: M Rs spacing
    M=int(sys.argv[1]); Rs=float(sys.argv[2]); sp=float(sys.argv[3])
    ber,info,fs = run_config(M,Rs,sp)
    if ber is None: print(f"M{M} Rs{Rs/1e3:.0f}k sp{sp/1e3:.0f}k -> {info}")
    else: print(f"M{M} Rs{Rs/1e3:.0f}k sp{sp/1e3:.0f}k bw{info['bw']/1e6:.2f}M Fs{fs/1e6:.0f}M "
                f"BER={ber*100:.3f}% mean={info['mean']:.3f} foff={info['foff_khz']:.0f}k")
