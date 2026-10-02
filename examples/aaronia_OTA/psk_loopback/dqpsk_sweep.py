#!/usr/bin/env python3
"""DQPSK OTA BER vs symbol rate over the Aaronia->Fobos link (free-running LOs).

Recipe (hard-won, see memory aaronia-rtsa-api):
  - generate each IQ file at the chain's NATIVE 46.08 MS/s -> transmits 1:1, NO rate
    scaling, so on-air Rs == generated Rs (verified by differential-autocorr period).
  - unique filename per rate -> File Source reloads (no buffer-cache trap).
  - one gentle capture per rate (hammering wedges the Fobos USB off the bus).
  - resample capture to integer sps=8 at the true Rs, then demod_dpsk_ota.

Usage: dqpsk_sweep.py            # default rate set
"""
import sys, os, re, time, json, subprocess, urllib.request, numpy as np
from fractions import Fraction
sys.path.insert(0,'.'); sys.path.insert(0, os.path.join('..','fsk_loopback'))
import mpsk_lab as p, ota_sweep as fk
from scipy.signal import resample_poly
from ota_capture import capture

HOST="vertical@10.10.1.74"; AAR="http://10.10.1.74:54665/remoteconfig"
FS_FILE=46.08e6; M=4; FC=fk.FC
RATES=[0.5e6,1e6,2e6,2.88e6,4e6,6e6,8e6]
_req=[0]
def put(rcv,items):
    _req[0]+=1; b={"request":_req[0],"receiverName":rcv,"config":{"type":"group","items":items}}
    try:
        urllib.request.urlopen(urllib.request.Request(AAR,data=json.dumps(b).encode(),method="PUT"),timeout=8).read(); return True
    except Exception: return False

def cap_rate(Rs):                       # Fobos rate giving sps ~8-16, within 8..64
    for r in [16e6,20e6,25e6,32e6,40e6,50e6,64e6]:
        if r >= Rs*7: return r
    return 64e6

def gen_push(Rs, idx):
    bits=p.fl.bytes_to_bits(p.fl.test_bytes())
    bps=int(np.log2(M)); per=(len(bits)//bps)*int(round(FS_FILE/Rs))
    reps=max(1,int(round(0.12*FS_FILE/max(per,1))))
    x,data,sps=p.gen_dpsk(bits,M,Rs,FS_FILE,beta=0.35,amp=0.3,repeats=reps)
    base=f"/tmp/dqpsk_sw/test_{idx}"; os.makedirs("/tmp/dqpsk_sw",exist_ok=True)
    fk.write_fs_files(x,FS_FILE,FC,base)
    t=open(base+".iq.xml").read()
    open(base+".iq.xml","w").write(re.sub(r'<DataFilename>.*?</DataFilename>',f'<DataFilename>test_{idx}.iq</DataFilename>',t))
    subprocess.run(["scp","-o","ConnectTimeout=25","-o","BatchMode=yes",base+".iq",base+".iq.xml",f"{HOST}:Aaronia/"],check=True,capture_output=True)
    actual_Rs = FS_FILE/sps                 # gen rounds sps to int -> true rate may differ
    return f"C:/Users/vertical/Aaronia/test_{idx}.iq", actual_Rs

def start_tx(remote_path):
    put("Block_Filesource_0",[{"type":"bool","name":"playbutton","value":False}]); time.sleep(0.3)
    put("Block_Filesource_0",[{"type":"string","name":"filename","value":remote_path}]); time.sleep(0.4)
    put("Block_Filesource_0",[{"type":"enum","name":"playbackmode","value":2}])
    put("Block_IQModulator_0",[{"type":"group","name":"main","items":[{"type":"float","name":"targetfreq","value":FC}]}])
    put("Block_Spectran_V6B_0",[{"type":"group","name":"main","items":[{"type":"float","name":"transattn","value":-20.0},{"type":"bool","name":"connect","value":True},{"type":"bool","name":"run","value":True}]}]); time.sleep(0.4)
    put("Block_Filesource_0",[{"type":"bool","name":"playbutton","value":True}]); time.sleep(0.6)

def grab(Fs_cap):
    subprocess.run(["curl","-s","-X","POST","http://localhost:8080/api/attach"],capture_output=True); time.sleep(1.5)
    subprocess.run(["curl","-s","-X","POST","http://localhost:8080/api/detach"],capture_output=True); time.sleep(1.5)
    for _ in range(2):
        try:
            x=capture(FC,Fs_cap,0.06,2,12)
            if len(x)>10000 and np.mean(np.abs(x))>0.01: return x
        except Exception: pass
        time.sleep(1)
    return None

def demod(x, Rs, Fs_cap):
    fr=Fraction(int(Rs*8),int(Fs_cap)).limit_denominator(2000)
    xr=resample_poly(x,fr.numerator,fr.denominator); Fs2=Fs_cap*fr.numerator/fr.denominator
    xr=xr[:int(30000*8)]
    bits=p.fl.bytes_to_bits(p.fl.test_bytes()); _,data,_=p.gen_dpsk(bits,M,Rs,8*Rs,beta=0.35)
    ber,info=p.demod_dpsk_ota(xr,M,Rs,Fs2,0.35,data)
    return ber,info

def main():
    print(f"{'Rs_req':>7} {'Rs_air':>7} {'Fs_cap':>7} {'mean':>6} {'BER%':>7}")
    rows=[]
    for i,Rs in enumerate(RATES):
        rp,Rs_air=gen_push(Rs,i); start_tx(rp)
        Fs_cap=cap_rate(Rs_air)
        x=grab(Fs_cap)
        if x is None: print(f"{Rs/1e6:7.2f} {Rs_air/1e6:7.3f} {Fs_cap/1e6:7.0f} {'--':>6} {'NOSIG':>7}"); rows.append((Rs_air,None)); continue
        m=np.mean(np.abs(x))
        try: ber,info=demod(x,Rs_air,Fs_cap)
        except Exception as e: print(f"{Rs/1e6:7.2f} demod err {str(e)[:40]}"); rows.append((Rs_air,None)); continue
        print(f"{Rs/1e6:7.2f} {Rs_air/1e6:7.3f} {Fs_cap/1e6:7.0f} {m:6.3f} {ber*100:7.2f}", flush=True)
        rows.append((Rs_air,ber))
    print("\nDQPSK OTA BER vs symbol rate:")
    for Rs,ber in rows:
        print(f"  {Rs/1e6:5.2f} MS/s : {('%.2f%%'%(ber*100)) if ber is not None else 'FAIL'}")
    open("/tmp/dqpsk_sw/results.txt","w").write("\n".join(f"{r/1e6} {b}" for r,b in rows))

if __name__=='__main__':
    main()
