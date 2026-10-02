#!/usr/bin/env python3
"""OTA M-PSK rate sweep. For each (M,Rs): generate RRC-PSK file -> overwrite
~/Aaronia/test.iq -> restart RTSA -> detach web app -> probe (auto-detects the
chain's rate scaling). Maps BER vs symbol rate to expose the phase-noise floor.

Prereq: the mission must be SAVED with the working (385 MHz) IQ Modulator, since
restart_rtsa.py reloads the saved mission. File rate kept <=8 MS/s (IQ Mod ceiling),
amp 0.3 (avoid TX clipping).
"""
import sys, os, re, subprocess, numpy as np
sys.path.insert(0, '.'); sys.path.insert(0, '../fsk_loopback')
import mpsk_lab as p, ota_sweep as fk
import restart_rtsa

HOST = "vertical@10.10.1.74"

def gen_push(M, Rs, amp=0.3):
    FS = 8e6                                   # <= IQ Modulator input ceiling
    bits = p.fl.bytes_to_bits(p.fl.test_bytes())
    bps = int(np.log2(M)); per = (len(bits)//bps)*int(round(FS/Rs))
    reps = max(1, int(round(3.0*FS/per)))
    x, syms, sps = p.gen_mpsk(bits, M, Rs, FS, beta=0.35, amp=amp, repeats=reps)
    base = "/tmp/psk_native/test"; fk.write_fs_files(x, FS, fk.FC, base)
    t = open(base+".iq.xml").read()
    open(base+".iq.xml", "w").write(re.sub(r'<DataFilename>.*?</DataFilename>',
                                           '<DataFilename>test.iq</DataFilename>', t))
    subprocess.run(["scp","-o","ConnectTimeout=25","-o","BatchMode=yes",
                    base+".iq", base+".iq.xml", f"{HOST}:Aaronia/"], check=True,
                   capture_output=True)
    return FS, sps

def probe(M, Rs):
    subprocess.run(["curl","-s","-X","POST","http://localhost:8080/api/detach"],
                   capture_output=True)
    r = subprocess.run(["python3","pll_probe.py",str(M),str(int(Rs))],
                       capture_output=True, text=True, timeout=200)
    best = 1.0; rate = None
    for ln in r.stdout.splitlines():
        if "on-air Rs" in ln:
            m = re.search(r'Rs = ([\d.]+) M', ln); rate = float(m.group(1)) if m else None
        for key in ('block','pll0.05','pll0.1','diff'):
            if ln.strip().startswith(key) and 'BER=' in ln:
                best = min(best, float(re.search(r'BER=\s*([\d.]+)', ln).group(1)))
    return best, rate

def main():
    configs = [(2,700e3),(2,1e6),(2,1.5e6),(2,2e6),(4,1e6),(4,1.5e6),(4,2e6)]
    os.makedirs("/tmp/psk_native", exist_ok=True)
    print(f"{'mod':>4} {'Rs_nom':>8} {'Rs_air':>8} {'bestBER':>9}")
    rows = []
    for M, Rs in configs:
        gen_push(M, Rs)
        restart_rtsa.restart()
        try:
            ber, air = probe(M, Rs)
        except Exception as e:
            ber, air = None, None; print("  probe err:", str(e)[:60])
        mm = {2:'BPSK',4:'QPSK',8:'8PSK'}[M]
        line = f"{mm:>4} {Rs/1e6:7.2f}M {('%.2fM'%air) if air else '--':>8} {('%.2f%%'%ber) if ber is not None else 'FAIL':>9}"
        print(line); rows.append((mm,Rs,air,ber))
    open("/tmp/psk_native/sweep_results.txt","w").write(
        "\n".join(f"{m} {r} {a} {b}" for m,r,a,b in rows))
    print("DONE -> /tmp/psk_native/sweep_results.txt")

if __name__ == '__main__':
    main()
