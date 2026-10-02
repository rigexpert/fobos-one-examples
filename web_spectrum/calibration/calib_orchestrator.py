#!/usr/bin/env python3
"""Config-driven FobosOne absolute-power calibration, launched by the web app's CAL tab.

Reads a JSON config (path as argv[1]), drives the Aaronia signal generator + the running
C++ web backend (localhost:8080) REST API, and writes fobos_calibration.json in the schema
the main app's CAL dBm toggle loads. Emits progress as JSON lines on stdout:
  {"pct":..,"freq_hz":..,"eta_s":..,"msg":..}         progress
  {"done":true,"ok":true|false,"msg":..}              terminal

The Fobos tone power is read from /api/spectrum META (peak_db = the tone bin, avg_db = floor;
the backend returns meta only, no arrays). Model (factored, from calib_measure.py):
  K = R_dBFS - P_dBm       (linear-region, power-independent)
  C(f,lna,vga) = -cable_loss(f) - K_ref(f) + delta(lna,vga,f)
  displayed_dBm = raw_dBFS + C

--dry-run (or "dry_run":true in config) synthesizes readings for UI/plumbing tests.
"""
import sys, os, time, json, urllib.request, numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from cable_loss import cable_loss_db, datasheet_points

CFG_PATH = sys.argv[1]
CFG = json.load(open(CFG_PATH))
DRY = CFG.get('dry_run', False) or '--dry-run' in sys.argv

WEB = "http://localhost:8080"
AAR = f"http://{CFG.get('gen_ip','10.10.1.74')}:{CFG.get('gen_port',54664)}/remoteconfig"
V6B = "Block_Spectran_V6B_0"
OUT = os.path.join(os.path.dirname(__file__), "fobos_calibration.json")

TONE_OFFSET = float(CFG.get('tone_offset_hz', 200e3))
FS_CAP      = float(CFG.get('rate_hz', 10e6))
FFT         = int(CFG.get('fft_size', 8192))
ACCUM       = int(CFG.get('accum', 4))
SETTLE      = float(CFG.get('settle_ms', 250)) / 1000.0
POWERS      = [float(x) for x in CFG.get('powers_dbm', [-40.0])]
# Gain grid, per fobos_sdr.h: LNA 0..3, VGA 0..31.
N_LNA, N_VGA = 4, 32
LNA_DB          = [0.0, 0.0, 16.0, 33.0]   # dB per LNA step
VGA_DB_PER_STEP = 2.0                      # VGA spans 0..+62 dB in 2 dB steps
# Steps 0 and 1 are both 0 dB, so only three LNA settings are electrically
# distinct. Measure those (3x32 = 96 combos, not 128) and alias 1 -> 0 when the
# table is written, so the file still carries a row for every selectable step.
# The UI's lookup falls back to the LAST row when a step is missing, which would
# apply the +33 dB correction to a 0 dB setting.
MEAS_LNAS = [0, 2, 3]
LNA_ALIAS = {1: 0}
REF_LNA     = min(N_LNA-1, max(0, int(CFG.get('ref_lna', 1))))
REF_VGA     = min(N_VGA-1, max(0, int(CFG.get('ref_vga', 8))))
LIN_CEIL    = 14.0
B_POWERS    = sorted(set(POWERS + [-30., -40., -50., -60., -70., -80.]), reverse=True)

def emit(**kw): print(json.dumps(kw), flush=True)
def log(m): emit(msg=m)

# ── Aaronia generator ───────────────────────────────────────────────────────
_req = [0]
def _put(items):
    if DRY: return True
    _req[0] += 1
    body = {"request": _req[0], "receiverName": V6B,
            "config": {"type": "group", "items": items}}
    try:
        urllib.request.urlopen(urllib.request.Request(
            AAR, data=json.dumps(body).encode(), method="PUT"), timeout=8).read()
        return True
    except Exception:
        return False
def gen_setup():
    _put([{"type":"group","name":"device","items":[{"type":"group","name":"generator","items":[
        {"type":"float","name":"offsetfreq","value":TONE_OFFSET}]}]}])
    _put([{"type":"group","name":"main","items":[{"type":"bool","name":"connect","value":True},
        {"type":"bool","name":"run","value":True}]}])
def gen_center(hz): _put([{"type":"group","name":"main","items":[{"type":"float","name":"centerfreq","value":float(hz)}]}])
def gen_power(dbm): _put([{"type":"group","name":"main","items":[{"type":"float","name":"transattn","value":float(dbm)}]}])

# ── Fobos via C++ backend REST ──────────────────────────────────────────────
def _post(path, body=None):
    if DRY: return {"ok": True}
    d = json.dumps(body).encode() if body is not None else None
    r = urllib.request.Request(WEB+path, data=d, method="POST",
        headers={'Content-Type':'application/json'} if body is not None else {})
    return json.loads(urllib.request.urlopen(r, timeout=10).read())
def _spec(after=0.0):
    return json.loads(urllib.request.urlopen(
        WEB+f"/api/spectrum?wait=true&timeout=4&after={after}", timeout=8).read())
def fobos_start(freq):
    _post("/api/attach")
    _post("/api/start", {"mode":"fft","freq":float(freq),"rate":FS_CAP,"fft_size":FFT,
                         "accum_n":ACCUM,"lna":REF_LNA,"vga":REF_VGA})
def fobos_tune(freq, lna, vga):
    _post("/api/params", {"freq":float(freq),"lna":int(lna),"vga":int(vga)})

def _synth(center, P, lna, vga):
    """Dry-run: plausible R_dBFS = P - cable_loss - rolloff + gain_term (for plumbing tests)."""
    f = center
    cl = float(cable_loss_db(np.array([f]))[0])
    rolloff = 3.0 * (f/6e9)
    gain = (LNA_DB[lna]-LNA_DB[REF_LNA]) + (vga-REF_VGA)*VGA_DB_PER_STEP
    R = P - cl - rolloff + gain + 40.0   # +40 ~ front-end gain to land near dBFS scale
    return R, R-25.0, 25.0, R-2.0

def read_tone(center, P, lna, vga, settle=None):
    """Return (R_dBFS_peak, noise, snr, carrier_dbfs). Tone = strongest bin -> peak_db."""
    if DRY:
        return _synth(center, P, lna, vga)
    time.sleep(SETTLE if settle is None else settle)
    d = _spec()
    R = d.get('peak_db'); noise = d.get('avg_db')
    snr = (R - noise) if (R is not None and noise is not None) else None
    return R, noise, snr, d.get('carrier_dbfs')

# ── cable loss ──────────────────────────────────────────────────────────────
def parse_touchstone(text):
    """Parse .s1p/.s2p -> (freqs_hz array, insertion_loss_dB array). Uses S21 for 2-port
    (passive reciprocal cable), or the single S value for 1-port. Handles the
    '# <unit> S <DB|MA|RI> R <z0>' option line."""
    unit = {'HZ':1.,'KHZ':1e3,'MHZ':1e6,'GHZ':1e9}; umul=1e9; fmt='MA'; nports=None
    fs=[]; loss=[]
    for raw in text.splitlines():
        line = raw.split('!',1)[0].strip()
        if not line: continue
        if line.startswith('#'):
            toks = line[1:].upper().split()
            for i,tk in enumerate(toks):
                if tk in unit: umul = unit[tk]
                if tk in ('DB','MA','RI'): fmt = tk
            continue
        vals = [float(x) for x in line.replace(',',' ').split()]
        if len(vals) < 3: continue
        f = vals[0]*umul
        # 2-port DB/MA row: f S11m S11a S21m S21a ...  -> S21 is cols 3,4
        if len(vals) >= 9: mag_i = 3     # s2p: use S21
        else:              mag_i = 1     # s1p: single S param
        if fmt == 'DB':
            s_db = vals[mag_i]                       # already 20log10|S|
        elif fmt == 'RI':
            re,im = vals[mag_i], vals[mag_i+1]
            s_db = 20*np.log10(max((re*re+im*im)**0.5, 1e-12))
        else:  # MA
            s_db = 20*np.log10(max(vals[mag_i], 1e-12))
        fs.append(f); loss.append(-s_db)             # insertion loss = -S21_dB (>=0)
    if not fs: raise ValueError("no data rows parsed from Touchstone file")
    return np.array(fs), np.array(loss)

def make_cable_loss():
    c = CFG.get('cable','cbl-1m-smsm')
    if c == 'none':
        return lambda f: np.zeros_like(np.asarray(f, float)), "none (0 dB)", [], []
    if c == 'custom':
        cf, cl = parse_touchstone(CFG['touchstone'])
        log(f"custom cable: {len(cf)} Touchstone points, {cf[0]/1e6:.0f}-{cf[-1]/1e6:.0f} MHz, "
            f"loss {cl.min():.1f}..{cl.max():.1f} dB")
        return (lambda f: np.interp(np.asarray(f,float), cf, cl)), "custom (Touchstone S21)", cf.tolist(), cl.tolist()
    dcf, dcl = datasheet_points()
    return cable_loss_db, "CBL-1M-SMSM+", dcf, dcl

# ── frequency plan ──────────────────────────────────────────────────────────
def in_exclude(f):
    for a,b in CFG.get('exclude_hz', []):
        if a <= f <= b: return True
    return False

def build_freqs():
    fine = np.arange(CFG['from_hz'], CFG['to_hz']+1, CFG['step_hz'])
    mode = CFG.get('gain_mode','factored')
    if mode == 'single':
        lnas, vgas, coarse = [REF_LNA], [REF_VGA], np.array([])
    elif mode == 'full':
        lnas, vgas = list(MEAS_LNAS), list(range(N_VGA))
        coarse = np.arange(CFG['from_hz'], CFG['to_hz']+1, CFG['step_hz'])   # grid at fine res
    else:  # factored
        lnas, vgas = list(MEAS_LNAS), list(range(N_VGA))
        cs = float(CFG.get('coarse_step_hz', 200e6))
        coarse = np.arange(CFG['from_hz'], CFG['to_hz']+1, cs)
    return fine, coarse, lnas, vgas

# ── main ────────────────────────────────────────────────────────────────────
def _fillnan(a):
    a=np.array(a,float); idx=np.where(~np.isnan(a))[0]
    if len(idx)==0: return np.zeros_like(a)
    return np.interp(np.arange(len(a)), idx, a[idx])

def expand_lna_aliases(lnas, rows):
    """Give each LNA step skipped as an electrical duplicate its own table row.

    Returns (lnas_out, rows_out) sorted by step. A grid that never measured the
    aliased source -- gain_mode 'single', say -- passes through untouched."""
    out = list(zip(lnas, rows))
    for step, src in LNA_ALIAS.items():
        if step not in lnas and src in lnas:
            out.append((step, rows[lnas.index(src)]))
    out.sort(key=lambda kv: kv[0])
    return [k for k, _ in out], [v for _, v in out]

def main():
    fine, coarse, lnas, vgas = build_freqs()
    cable_loss_f, cable_name, cab_f, cab_l = make_cable_loss()
    n_fine = len(fine); n_grid = len(coarse)*len(lnas)*len(vgas)
    total = n_fine + n_grid + len(coarse)*max(0,len(POWERS)-1)
    log(f"calibration: {n_fine} fine freqs @ ref gain, gain grid {len(lnas)}x{len(vgas)} @ {len(coarse)} coarse, "
        f"cable={cable_name}, powers={POWERS}, {'DRY-RUN' if DRY else 'LIVE'}")
    done=[0]; t0=time.time()
    def tick(freq_hz):
        done[0]+=1
        el=time.time()-t0; rate=done[0]/max(el,1e-6)
        eta=(total-done[0])/max(rate,1e-6)
        emit(pct=100.0*done[0]/max(total,1), freq_hz=float(freq_hz), eta_s=eta)

    if not DRY:
        gen_setup(); fobos_start(fine[0]); time.sleep(2)

    # Part A: fine response at reference gain
    FINE_POWER = POWERS[0]
    if not DRY: gen_power(FINE_POWER)
    Kref = np.full(n_fine, np.nan)
    for i,F in enumerate(fine):
        if not in_exclude(F):
            if not DRY: gen_center(F); fobos_tune(F, REF_LNA, REF_VGA)
            R,noise,snr,car = read_tone(F, FINE_POWER, REF_LNA, REF_VGA, settle=(0.13 if not DRY else 0))
            if R is not None and R < LIN_CEIL and (snr or 0) > 8: Kref[i] = R - FINE_POWER
        tick(F)
        if ABORT[0]: return finish(False, "aborted")

    # linearity residual (multi-power) at coarse grid
    resid = np.zeros(n_fine)
    if len(POWERS) > 1 and len(coarse):
        lin_c=[]
        for F in coarse:
            Ks=[]
            for P in POWERS:
                if not DRY: gen_center(F); gen_power(P); fobos_tune(F,REF_LNA,REF_VGA)
                R,noise,snr,car = read_tone(F,P,REF_LNA,REF_VGA)
                Ks.append(R-P if (R is not None and R<LIN_CEIL and (snr or 0)>8) else np.nan)
                tick(F)
                if ABORT[0]: return finish(False,"aborted")
            Ks=np.array(Ks); lin_c.append(np.nanmax(Ks)-np.nanmin(Ks) if np.isfinite(Ks).any() else np.nan)
        resid=np.interp(fine.astype(float), coarse.astype(float), np.nan_to_num(lin_c))

    # Part B: gain grid
    Kg = np.full((len(lnas),len(vgas),len(coarse)), np.nan)
    for fi,F in enumerate(coarse):
        if in_exclude(F):
            for li in range(len(lnas)):
                for vi in range(len(vgas)): tick(F)
            continue
        if not DRY: gen_center(F)
        for li,lna in enumerate(lnas):
            for vi,vga in enumerate(vgas):
                K=None
                for P in B_POWERS:
                    if not DRY: fobos_tune(F,lna,vga); gen_power(P)
                    R,noise,snr,car = read_tone(F,P,lna,vga)
                    if R is not None and R<LIN_CEIL and (snr or 0)>10: K=R-P; break
                if K is not None: Kg[li,vi,fi]=K
                tick(F)
                if ABORT[0]: return finish(False,"aborted")

    write_json(fine, Kref, resid, coarse, lnas, vgas, Kg, cable_loss_f, cable_name, cab_f, cab_l)
    if not DRY:
        _post("/api/stop"); _post("/api/detach")
    if CFG.get('clean', True):
        try:
            import postprocess_calibration as pp; pp.main()
            log("interference cleanup done")
        except Exception as e:
            log(f"postprocess skipped: {str(e)[:80]}")
    finish(True, "calibration written")

def write_json(fine, Kref, resid, coarse, lnas, vgas, Kg, cable_loss_f, cable_name, cab_f, cab_l):
    valid=np.where(~np.isnan(Kref))[0]
    vmin=float(fine[valid[0]]) if len(valid) else float(fine[0])
    Kref=_fillnan(Kref)
    Cref = -cable_loss_f(fine) - Kref
    if len(coarse):
        Kref_coarse=np.interp(coarse.astype(float), fine.astype(float), Kref)
        Kg=np.stack([[_fillnan(Kg[li,vi,:]) for vi in range(len(vgas))] for li in range(len(lnas))])
        delta=Kref_coarse[None,None,:]-Kg
        gd_freqs=list(map(float,coarse)); delta_out=np.where(np.isnan(delta),0.0,delta).tolist()
    else:
        gd_freqs=[float(fine[0]),float(fine[-1])]
        delta_out=[[[0.0,0.0] for _ in vgas] for _ in lnas]
    lnas, delta_out = expand_lna_aliases(lnas, delta_out)
    obj={
      "meta":{"created":time.strftime("%Y-%m-%d %H:%M:%S"),
              "method":"web-CAL orchestrator; Aaronia relative tone; peak_db; K=R-P; "
                       "C=-cable(f)-Kref(f)+delta(lna,vga,f); displayed_dBm=raw_dBFS+C",
              "fobos_rate_hz":FS_CAP,"fft_size":FFT,"accum_n":ACCUM,"rbw_hz":FS_CAP/FFT,
              "tone_offset_hz":TONE_OFFSET,"powers_dbm":POWERS,"lin_ceil_dbfs":LIN_CEIL,
              "ref_lna":REF_LNA,"ref_vga":REF_VGA,"gain_mode":CFG.get('gain_mode','factored'),
              "cable_model":cable_name,"exclude_hz":CFG.get('exclude_hz',[]),
              "linearity_resid_db_med":float(np.nanmedian(resid)) if np.isfinite(resid).any() else 0.0,
              "valid_freq_min_hz":vmin,"dry_run":DRY},
      "cable":{"freqs_hz":cab_f,"loss_db":cab_l},
      "freq_response":{"freqs_hz":fine.tolist(),"offset_db":Cref.tolist(),
                       "linearity_resid_db":_fillnan(resid).tolist()},
      "gain_delta":{"freqs_hz":gd_freqs,"lnas":lnas,"vgas":vgas,"delta_db":delta_out},
    }
    import shutil
    if DRY:
        json.dump(obj, open(OUT+".dryrun","w"), indent=1)   # NEVER touch the real file in dry-run
        return
    if os.path.exists(OUT):                                  # back up the previous real calibration
        shutil.copy(OUT, OUT + ".bak." + time.strftime("%Y%m%d-%H%M%S"))
    json.dump(obj, open(OUT,"w"), indent=1)
    shutil.copy(OUT, OUT+".raw")

ABORT=[False]
def finish(ok, msg):
    emit(done=True, ok=ok, msg=msg); sys.exit(0 if ok else 1)

if __name__=='__main__':
    import signal
    signal.signal(signal.SIGTERM, lambda *a: (ABORT.__setitem__(0,True)))
    try:
        main()
    except Exception as e:
        finish(False, f"error: {str(e)[:120]}")
