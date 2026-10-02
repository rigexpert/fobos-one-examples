#!/usr/bin/env python3
"""FobosOne absolute-power calibration sweep (factored).

Aaronia V6B in Signal-Generator / Relative-Tone mode emits a CW tone at
(centerfreq + 200 kHz) at a known TX power. The Fobos (driven via the web-spectrum
app's robust FFT REST path) reads the tone power. With the known cable loss
(CBL-1M-SMSM+) the true power at the RX port = TX_power - cable_loss(f), so the
calibration offset is  C(f,gain) = expected_dBm - R_dBFS(f,gain).

Factored model (keeps it to ~an hour instead of days):
  Part A  fine 100 kHz frequency response at ONE reference gain  -> C_ref(f)
  Part B  full LNA x VGA grid at coarse frequencies              -> delta(lna,vga,f)
  full:   C(f,lna,vga) = C_ref(f) + delta(lna,vga,f)
3 TX powers (-50/-40/-30) per point check linearity (R must track TX 1:1).

Reads via the web app => no fragile standalone capture. Writes fobos_calibration.json.

Usage: calib_measure.py test     # small range, quick end-to-end validation
       calib_measure.py full     # 70-6000 MHz @ 100 kHz fine + coarse gain grid
"""
import sys, os, time, json, urllib.request, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from cable_loss import cable_loss_db, datasheet_points

WEB   = "http://localhost:8080"
AAR   = "http://10.10.1.74:54665/remoteconfig"
V6B   = "Block_Spectran_V6B_0"
OUT   = os.path.join(os.path.dirname(__file__), "fobos_calibration.json")

TONE_OFFSET = 200e3            # relative-tone offset from center (Hz)
FS_CAP      = 10e6             # Fobos sample rate for the measurement
FFT         = 8192            # RBW = 10 MHz / 8192 ~ 1.22 kHz -> negligible scalloping
ACCUM       = 4               # FFTs averaged per read (steady tone power)
POWERS      = [-50.0, -40.0, -30.0]    # Part A linearity check at the reference gain
# Gain grid, per fobos_sdr.h: LNA 0..3 (0,1: 0 dB, 2: +16 dB, 3: +33 dB),
# VGA 0..31 (0..+62 dB, 2 dB per step).
# Steps 0 and 1 are both 0 dB, so only three LNA settings are electrically
# distinct. Measure those (3x32 = 96 combos, not 128) and alias 1 -> 0 when the
# table is written, so the file still carries a row for every selectable step.
# The UI's lookup falls back to the LAST row when a step is missing, which would
# apply the +33 dB correction to a 0 dB setting.
N_LNA, N_VGA = 4, 32
MEAS_LNAS = [0, 2, 3]
LNA_ALIAS = {1: 0}
REF_LNA, REF_VGA = 1, 8        # reference gain for the fine frequency response
TONE_WIN    = 40e3            # +/- search window around the tone for the peak (Hz)
LIN_CEIL    = 14.0            # dBFS: above this the front-end compresses/clips
# Part B auto-power ladder (high->low): pick the highest NON-clipping power per gain so
# each LNA/VGA stays linear (gains span more range than one TX power covers).
B_POWERS    = [-30.0, -40.0, -50.0, -60.0, -70.0, -80.0]

# ── Aaronia signal generator ───────────────────────────────────────────────────
_req=[0]
def _put(items):
    _req[0]+=1
    body={"request":_req[0],"receiverName":V6B,"config":{"type":"group","items":items}}
    try:
        urllib.request.urlopen(urllib.request.Request(AAR,data=json.dumps(body).encode(),method="PUT"),timeout=8).read()
        return True
    except Exception:
        return False
def gen_setup():
    # relative tone 200 kHz, connect + run; transmittermode/type already = 4/0 (set by user)
    _put([{"type":"group","name":"device","items":[{"type":"group","name":"generator","items":[
        {"type":"float","name":"offsetfreq","value":TONE_OFFSET}]}]}])
    _put([{"type":"group","name":"main","items":[{"type":"bool","name":"connect","value":True},
        {"type":"bool","name":"run","value":True}]}])
def gen_center(hz): return _put([{"type":"group","name":"main","items":[{"type":"float","name":"centerfreq","value":float(hz)}]}])
def gen_power(dbm):  return _put([{"type":"group","name":"main","items":[{"type":"float","name":"transattn","value":float(dbm)}]}])

# ── Fobos via web app REST ─────────────────────────────────────────────────────
def _post(path, body=None):
    d=json.dumps(body).encode() if body is not None else None
    r=urllib.request.Request(WEB+path,data=d,method="POST",headers={'Content-Type':'application/json'} if body is not None else {})
    return json.loads(urllib.request.urlopen(r,timeout=10).read())
def _spec(after=0.0):
    return json.loads(urllib.request.urlopen(WEB+f"/api/spectrum?wait=true&timeout=4&after={after}",timeout=8).read())
def fobos_start(freq):
    _post("/api/attach")
    _post("/api/start",{"mode":"fft","freq":float(freq),"rate":FS_CAP,"fft_size":FFT,
                        "accum_n":ACCUM,"lna":REF_LNA,"vga":REF_VGA})
def fobos_tune(freq,lna,vga):
    _post("/api/params",{"freq":float(freq),"lna":int(lna),"vga":int(vga)})

def read_tone(center, settle=0.25, frames=2):
    """Peak tone power (dBFS) within +/-TONE_WIN of center+200kHz, + noise floor."""
    time.sleep(settle)                       # let retune + a fresh averaged frame flow
    d=None; last=0.0
    for _ in range(frames):
        d=_spec(after=last); last=d.get('ts',0.0)
    f=np.array(d['freqs']); p=np.array(d['power'])
    tone_f = center + TONE_OFFSET
    sel = np.abs(f - tone_f) <= TONE_WIN
    if not sel.any(): return None, None, None
    R = float(p[sel].max())
    # noise floor: median away from the tone and away from DC
    nz = (np.abs(f - tone_f) > 4*TONE_WIN) & (np.abs(f - center) > 5*TONE_WIN)
    noise = float(np.median(p[nz])) if nz.any() else float(np.median(p))
    return R, noise, R-noise

# ── measurement primitives ─────────────────────────────────────────────────────
# Work in the linear-region offset K = R - P (slope-1 verified): power-independent, so
# C(f,gain) = -cable_loss(f) - K(f,gain). This lets every gain be measured at whatever
# TX power keeps it linear, then compared on equal footing.

def K_linear(F, lna, vga):
    """Highest non-clipping reading at this gain. Returns (K=R-P, P, R, snr) or Nones."""
    fobos_tune(F, lna, vga)
    for P in B_POWERS:                       # high -> low; first non-clipping = best SNR
        gen_power(P)
        R,noise,snr = read_tone(F)
        if R is not None and R < LIN_CEIL and snr > 10:
            return R - P, P, R, snr
    return None, None, None, None            # all clip or all noise

FINE_POWER = -40.0   # single linear-region TX power for the fast fine sweep (ref gain)

# ── sweeps ─────────────────────────────────────────────────────────────────────
def part_A(fine_freqs, coarse_freqs, log):
    """Fine frequency response at the reference gain (ONE power for speed: 100 kHz x 3
    powers would be ~25 h). The 3-power linearity is checked at the coarse grid only.
    Returns K_ref(fine), and the linearity residual interpolated onto the fine grid."""
    log(f"Part A: fine response @ ref gain LNA={REF_LNA} VGA={REF_VGA}, {len(fine_freqs)} freqs @ {FINE_POWER} dBm")
    Kref=np.full(len(fine_freqs), np.nan); t0=time.time()
    gen_power(FINE_POWER)
    for i,F in enumerate(fine_freqs):
        gen_center(F); fobos_tune(F, REF_LNA, REF_VGA)
        R,noise,snr = read_tone(F, settle=0.13, frames=1)
        if R is not None and R < LIN_CEIL and snr>8: Kref[i] = R - FINE_POWER
        if i%500==0 or i==len(fine_freqs)-1:
            el=time.time()-t0; eta=el/(i+1)*(len(fine_freqs)-i-1)
            log(f"  A {i+1}/{len(fine_freqs)} {F/1e6:.1f}MHz Kref={Kref[i]:.1f} SNR~{snr if snr else 0:.0f} ETA {eta/60:.0f}m")
    # 3-power linearity check at the coarse grid (smooth, so coarse is enough)
    log(f"  A-lin: 3-power linearity @ ref gain, {len(coarse_freqs)} coarse freqs")
    lin_c=[]
    for F in coarse_freqs:
        gen_center(F); fobos_tune(F, REF_LNA, REF_VGA)
        Ks=[]
        for P in POWERS:
            gen_power(P); R,noise,snr=read_tone(F, settle=0.3, frames=2)
            Ks.append(R-P if (R is not None and R<LIN_CEIL and snr>8) else np.nan)
        Ks=np.array(Ks); lin_c.append((np.nanmax(Ks)-np.nanmin(Ks)) if np.isfinite(Ks).any() else np.nan)
    resid=np.interp(np.array(fine_freqs,float), np.array(coarse_freqs,float),
                    np.nan_to_num(np.array(lin_c)))
    gen_power(FINE_POWER)
    return Kref, resid

def part_B(coarse_freqs, lnas, vgas, log):
    """Gain grid at coarse freqs. Returns K[lna][vga][f] (linear-region offset)."""
    log(f"Part B: gain grid {len(lnas)}x{len(vgas)} @ {len(coarse_freqs)} coarse freqs (auto-power per gain)")
    Kg = np.full((len(lnas),len(vgas),len(coarse_freqs)), np.nan)
    for fi,F in enumerate(coarse_freqs):
        gen_center(F)
        for li,lna in enumerate(lnas):
            for vi,vga in enumerate(vgas):
                K,P,R,snr = K_linear(F,lna,vga)
                if K is not None: Kg[li,vi,fi]=K
        log(f"  B {fi+1}/{len(coarse_freqs)} {F/1e6:.0f}MHz done")
    return Kg

# ── build calibration ──────────────────────────────────────────────────────────
def _fillnan(a):
    """Fill NaNs by interpolating valid points; edges extrapolate to the nearest valid
    value (np.interp clamps). Used so unmeasured bands (e.g. <~200 MHz where the Aaronia
    sig-gen can't reach) extrapolate to the nearest calibrated point, not 0."""
    a=np.array(a,float); idx=np.where(~np.isnan(a))[0]
    if len(idx)==0: return np.zeros_like(a)
    return np.interp(np.arange(len(a)), idx, a[idx])

def expand_lna_aliases(lnas, rows):
    """Give each LNA step skipped as an electrical duplicate its own table row.

    Returns (lnas_out, rows_out) sorted by step. A grid that never measured the
    aliased source passes through untouched."""
    out = list(zip(lnas, rows))
    for step, src in LNA_ALIAS.items():
        if step not in lnas and src in lnas:
            out.append((step, rows[lnas.index(src)]))
    out.sort(key=lambda kv: kv[0])
    return [k for k, _ in out], [v for _, v in out]

def build_json(fine_freqs, Kref, resid, coarse_freqs, lnas, vgas, Kg):
    fine=np.array(fine_freqs)
    valid = np.where(~np.isnan(Kref))[0]
    vmin = float(fine[valid[0]]) if len(valid) else float(fine[0])   # first measured freq
    Kref = _fillnan(Kref)                                       # extrapolate unmeasured bands
    # reference-gain absolute offset: C_ref(f) = -cable_loss(f) - K_ref(f)
    Cref = -cable_loss_db(fine) - Kref
    # gain delta vs the reference gain (per coarse freq): delta = K_ref(f) - K_gain(f)
    Kref_coarse = np.interp(np.array(coarse_freqs,float), fine, Kref)
    Kg = np.stack([[ _fillnan(Kg[li,vi,:]) for vi in range(len(vgas))] for li in range(len(lnas))])
    delta = Kref_coarse[None,None,:] - Kg                       # [lna][vga][coarse_f]
    lnas, delta_out = expand_lna_aliases(lnas, np.where(np.isnan(delta),0.0,delta).tolist())
    cf, cl = datasheet_points()
    obj = {
      "meta": {"created": time.strftime("%Y-%m-%d %H:%M:%S"),
               "method":"Aaronia sig-gen relative tone +200kHz; Fobos web-app FFT peak power; "
                        "linear-region offset K=R-P; C(f,lna,vga)=-cable_loss(f)-K_ref(f)+delta(lna,vga,f); "
                        "displayed_dBm = raw_dBFS + C",
               "fobos_rate_hz":FS_CAP,"fft_size":FFT,"accum_n":ACCUM,"rbw_hz":FS_CAP/FFT,
               "tone_offset_hz":TONE_OFFSET,"partA_powers_dbm":POWERS,"partB_powers_dbm":B_POWERS,
               "lin_ceil_dbfs":LIN_CEIL,"ref_lna":REF_LNA,"ref_vga":REF_VGA,
               "cable_model":"CBL-1M-SMSM+","linearity_resid_db_med":float(np.nanmedian(resid)),
               "valid_freq_min_hz":vmin,
               "note":"below valid_freq_min_hz the Aaronia sig-gen can't reach; those points are extrapolated"},
      "cable": {"freqs_hz":cf,"loss_db":cl},
      "freq_response": {"freqs_hz":fine.tolist(),"offset_db":Cref.tolist(),
                        "linearity_resid_db":_fillnan(resid).tolist()},
      "gain_delta": {"freqs_hz":list(map(float,coarse_freqs)),"lnas":lnas,"vgas":vgas,
                     "delta_db":delta_out},
    }
    json.dump(obj, open(OUT,"w"), indent=1)
    import shutil; shutil.copy(OUT, OUT+".raw")     # pristine raw; postprocess cleans OUT
    return obj

def main():
    mode = sys.argv[1] if len(sys.argv)>1 else "test"
    logf=open("/tmp/calib.log","a")
    def log(m): print(m,flush=True); logf.write(m+"\n"); logf.flush()
    if mode=="test":
        fine=np.arange(400e6,405e6+1,100e3)              # 51 freqs
        coarse=np.arange(400e6,402e6+1,1e6); lnas=list(MEAS_LNAS); vgas=[0,8,16,24]
    else:
        fine=np.arange(70e6,6000e6+1,100e3)              # 59,300 freqs
        coarse=np.arange(100e6,6000e6+1,300e6); lnas=list(MEAS_LNAS); vgas=list(range(N_VGA))
    log(f"=== calibration {mode}: {len(fine)} fine freqs, gain grid {len(lnas)}x{len(vgas)} @ {len(coarse)} coarse ===")
    gen_setup(); fobos_start(fine[0]); time.sleep(2)
    Kref,resid = part_A(fine, coarse, log)
    Kg         = part_B(coarse, lnas, vgas, log)
    gen_power(POWERS[0])                                   # leave at lowest power
    obj=build_json(fine, Kref, resid, coarse, lnas, vgas, Kg)
    log(f"wrote {OUT}  (median linearity residual {obj['meta']['linearity_resid_db_med']:.2f} dB)")
    _post("/api/stop"); _post("/api/detach")
    if mode != "test":
        log("post-processing (interference rejection)...")
        try:
            import postprocess_calibration as pp; pp.main()
        except Exception as e:
            log(f"postprocess error (run postprocess_calibration.py manually): {str(e)[:80]}")
    log("done")

if __name__=='__main__':
    main()
