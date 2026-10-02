#!/usr/bin/env python3
"""Gain-combo characterization: for every LNA(0-3) x VGA(0-31), sweep the V6B
signal generator and the Fobos together across 70-6000 MHz, accumulate the
max-hold panorama, and render it (web-UI style) to reports/sweep/sweep_LNA_l_VGA_v.png.

Synchronized sweep: sig-gen and Fobos are stepped to the SAME center per step, so
one clean pass = a complete panorama (no slow two-sweep alignment). Uses the web
app's FFT capture (/api/params + /api/spectrum) and the Aaronia API for the sig-gen.

Usage: gain_sweep_capture.py            # all LNA x VGA combos
       gain_sweep_capture.py L V        # single combo (test)
"""
import sys, os, time, json, urllib.request, numpy as np
from PIL import Image, ImageDraw

WEB     = "http://localhost:8080"
AARONIA = "http://10.10.1.74:54664/remoteconfig"
OUTDIR  = "/home/pi5/dev/fobosone/reports/sweep"

# Gain grid, per fobos_sdr.h: LNA 0..3, VGA 0..31.
N_LNA, N_VGA = 4, 32
N_COMBOS     = N_LNA * N_VGA
RATE    = 50e6
STEP    = RATE * 0.8                      # contiguous tiling
FROM, TO = 70e6, 6000e6
FFT     = 1024
CENTERS = np.arange(FROM + STEP/2, TO, STEP)
_req = [200]

def _post(path, body=None):
    d = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(WEB+path, data=d, method="POST",
        headers={'Content-Type':'application/json'} if body is not None else {})
    return json.loads(urllib.request.urlopen(r, timeout=10).read())

def _get(path):
    return json.loads(urllib.request.urlopen(WEB+path, timeout=10).read())

def set_siggen(hz):
    _req[0] += 1
    body = {"request":_req[0],"receiverName":"Block_Spectran_V6B_0","config":{"type":"group",
            "items":[{"type":"group","name":"main","items":[{"type":"float","name":"centerfreq","value":float(hz)}]}]}}
    rq = urllib.request.Request(AARONIA, data=json.dumps(body).encode(), method="PUT")
    try: urllib.request.urlopen(rq, timeout=6).read()
    except Exception: pass

def sweep_panorama(lna, vga):
    """Step sig-gen+Fobos across the band; return (freqs_MHz, power_dB) panorama."""
    # start FFT mode at the first center with this gain
    _post('/api/start', {'mode':'fft','freq':float(CENTERS[0]),'rate':RATE,
                         'lna':int(lna),'vga':int(vga),'fft_size':FFT,'accum_n':4})
    time.sleep(0.4)
    keep = int(round(STEP/RATE*FFT)); lo=(FFT-keep)//2; hi=lo+keep
    pf, pp = [], []
    for fc in CENTERS:
        set_siggen(fc)
        _post('/api/params', {'freq':float(fc)})
        last = 0.0
        try:
            r = _get(f'/api/spectrum?wait=true&timeout=3&after={last}')
            # ensure it's the retuned frame (center close to fc)
            for _ in range(6):
                if abs(r.get('center',0) - fc) < RATE*0.1: break
                last = r['ts']; r = _get(f'/api/spectrum?wait=true&timeout=3&after={last}')
            power = np.array(r['power']); freqs = np.array(r['freqs'])
        except Exception:
            power = np.full(FFT, -120.0); freqs = fc + np.linspace(-RATE/2, RATE/2, FFT)
        pf.extend((freqs[lo:hi]/1e6).tolist()); pp.extend(power[lo:hi].tolist())
    return np.array(pf), np.array(pp)

def render(pf, pp, lna, vga, path):
    W,H = 1700, 480; L,Rm,T,B = 70,20,60,40
    pw, ph = W-L-Rm, H-T-B
    img = Image.new('RGB',(W,H),(8,8,16)); d = ImageDraw.Draw(img)
    ref = 5.0; rng = 90.0; mn = ref-rng                       # dB display window
    fmin,fmax = 0.0, 6000.0
    # grid + axes
    for db in range(int(mn)//10*10, int(ref)+1, 10):
        y = T+ph*(1-(db-mn)/rng)
        if T<=y<=T+ph:
            d.line([(L,y),(W-Rm,y)],fill=(40,52,80)); d.text((6,y-6),f"{db}",fill=(150,175,200))
    for f in range(0,6001,500):
        x = L+pw*(f-fmin)/(fmax-fmin)
        d.line([(x,T),(x,T+ph)],fill=(40,52,80)); d.text((x-10,T+ph+6),f"{f}",fill=(150,175,200))
    # max-hold trace (yellow)
    pts=[]
    for f,p in zip(pf,pp):
        x=L+pw*(f-fmin)/(fmax-fmin); y=T+ph*(1-(max(min(p,ref),mn)-mn)/rng); pts.append((x,y))
    if len(pts)>1: d.line(pts, fill=(255,213,68), width=1)
    # peak
    pk=int(np.argmax(pp)); d.text((L+pw*(pf[pk]-fmin)/(fmax-fmin)-30, T+8),
        f"peak {pp[pk]:.1f}dB @ {pf[pk]:.0f}MHz", fill=(255,140,70))
    # header / settings (web-UI style)
    d.text((L,8), "FOBOSONE SPECTRUM  -  MAX HOLD (gain characterization)", fill=(80,238,255))
    d.text((L,28), f"LNA={lna}  VGA={vga}   |   {int(FROM/1e6)}-{int(TO/1e6)} MHz   "
                   f"rate {RATE/1e6:.0f} MHz   step {STEP/1e6:.0f} MHz   floor~{np.median(pp):.0f} dB",
           fill=(205,221,234))
    img.save(path)

def main():
    os.makedirs(OUTDIR, exist_ok=True)
    if len(sys.argv) == 3:
        combos = [(int(sys.argv[1]), int(sys.argv[2]))]
    else:
        combos = [(l,v) for l in range(N_LNA) for v in range(N_VGA)]
    print(f"{len(combos)} combo(s); {len(CENTERS)} steps each")
    for i,(lna,vga) in enumerate(combos):
        t0=time.time()
        pf,pp = sweep_panorama(lna,vga)
        path = os.path.join(OUTDIR, f"sweep_LNA_{lna}_VGA_{vga}.png")
        render(pf,pp,lna,vga,path)
        print(f"[{i+1}/{len(combos)}] LNA={lna} VGA={vga} -> {path}  "
              f"peak {pp.max():.1f}dB ({time.time()-t0:.0f}s)", flush=True)
    print("DONE ->", OUTDIR)

if __name__ == '__main__':
    main()
