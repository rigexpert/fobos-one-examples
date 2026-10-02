#!/usr/bin/env python3
"""Gain-combo characterization via the REAL web UI (Playwright headless screenshot).

Fobos stays in SCAN mode over 70-6000 with MAX HOLD on. For every LNA(0-3) x
VGA(0-31): set the gains in the UI, reset max-hold, step the Aaronia signal
generator one pass across 70-6000 (the scan's max-hold accumulates the comb),
then screenshot the whole web page to reports/sweep/sweep_LNA_l_VGA_v.png.
"""
import time, json, os, urllib.request
from playwright.sync_api import sync_playwright

WEB     = "http://localhost:8080"
AARONIA = "http://10.10.1.74:54664/remoteconfig"
OUT     = "/home/pi5/dev/fobosone/reports/sweep"
FROM_M, TO_M = 70, 6000          # MHz scan range
SG_STEP = 38e6                   # generator step (Hz) = measured full-sweep coverage -> no gaps
DWELL   = 3.0                    # s per generator step (~3 scan passes at accum_n=4) -> solid fill
NUM_PASSES = 1                   # passes per combo
ACCUM_N = 4                      # FFTs averaged per scan step; low => fast scan refresh => good fill
RUN_TS  = time.strftime("%Y%m%d-%H%M%S")   # run timestamp, embedded in every filename

# Gain grid, per fobos_sdr.h: LNA 0..3, VGA 0..31.
N_LNA, N_VGA = 4, 32
N_COMBOS     = N_LNA * N_VGA
_req = [300]

def _put_v6(items):
    _req[0] += 1
    body = {"request":_req[0],"receiverName":"Block_Spectran_V6B_0",
            "config":{"type":"group","items":items}}
    try: urllib.request.urlopen(urllib.request.Request(AARONIA, data=json.dumps(body).encode(), method="PUT"), timeout=6).read()
    except Exception: pass

def set_siggen(hz):
    _put_v6([{"type":"group","name":"main","items":[{"type":"float","name":"centerfreq","value":float(hz)}]}])

def gen_run():           # ensure the V6B generator is transmitting
    _put_v6([{"type":"group","name":"main","items":[{"type":"bool","name":"connect","value":True},{"type":"bool","name":"run","value":True}]}])

def gen_pass(log):
    for _ in range(NUM_PASSES):
        f = FROM_M*1e6
        while f <= TO_M*1e6 + 1:
            set_siggen(f); time.sleep(DWELL); f += SG_STEP

import glob

def newest_png(lna, vga):
    """Newest screenshot for this combo across ALL runs (timestamps), or None."""
    fs = sorted(glob.glob(f"{OUT}/sweep_LNA_{lna}_VGA_{vga}_*.png"))
    return fs[-1] if fs else None

def build_pdf(combos, log):
    """Combine the newest screenshot per combo into one annotated PDF."""
    from PIL import Image, ImageDraw
    pages = []
    for lna, vga in combos:
        png = newest_png(lna, vga)
        if not png:
            continue
        img = Image.open(png).convert('RGB')
        peak = "?"
        csv = png[:-4] + ".csv"
        if os.path.exists(csv):
            try:
                vals = [float(l.split(',')[1]) for l in open(csv).read().splitlines()[1:] if ',' in l]
                if vals:
                    peak = f"{max(vals):.1f} dB  (floor ~{sorted(vals)[len(vals)//20]:.0f} dB)"
            except Exception:
                pass
        hdr = 34
        page = Image.new('RGB', (img.width, img.height+hdr), (255,255,255))
        d = ImageDraw.Draw(page)
        d.text((10,10), f"FobosOne gain sweep   LNA={lna}  VGA={vga}   |   peak {peak}   |   "
                        f"70-6000 MHz scan max-hold (auto-range)", fill=(0,0,0))
        page.paste(img, (0, hdr))
        pages.append(page)
    if pages:
        out = f"{OUT}/gain_sweep_{RUN_TS}.pdf"
        pages[0].save(out, save_all=True, append_images=pages[1:])
        log(f"PDF written: {out} ({len(pages)} pages)")

# JS helpers (module scope: reused by every per-combo page)
FORCE_ON = "() => { if(!maxHoldOn) toggleMaxHold(); if(!autoRange) toggleAutoRange(); }"
STATE = ("() => { const a=(maxPower||[]).filter(v=>isFinite(v));"
         "  if(!a.length) return {mh:maxHoldOn, ar:autoRange, n:0};"
         "  const mx=a.reduce((m,v)=>v>m?v:m,-1e9); const so=[...a].sort((x,y)=>x-y);"
         "  const fl=so[Math.floor(so.length*0.05)];"
         "  return {mh:maxHoldOn, ar:autoRange, n:a.length, peak:mx, floor:fl,"
         "          ref:parseFloat(document.getElementById('ref-db').value)}; }")

def setup_page(p):
    """Launch a FRESH browser + page and get it into scan / max-hold / auto-range.
    A fresh browser per combo avoids the multi-hour headless-Chromium crash that
    hung the long single-browser run. Device stays attached server-side across
    pages (a viewer disconnect no longer stops the scan)."""
    b  = p.chromium.launch(args=['--no-sandbox'])
    pg = b.new_page(viewport={'width':1850, 'height':960})
    pg.goto(WEB, wait_until='networkidle', timeout=20000)
    pg.wait_for_timeout(1200)
    pg.evaluate("() => { if(!attached) toggleDev(); }"); pg.wait_for_timeout(2500)
    pg.evaluate("setMode('scan')")
    pg.evaluate(f"() => {{ document.getElementById('scan-from').value='{FROM_M}'; document.getElementById('scan-to').value='{TO_M}'; }}")
    pg.evaluate("() => { const s=document.getElementById('rate-sel'); s.value='50000000'; s.dispatchEvent(new Event('change',{bubbles:true})); }")
    pg.evaluate(f"() => {{ document.getElementById('accum-n').value='{ACCUM_N}'; }}")
    pg.wait_for_timeout(300)
    pg.evaluate("() => { if(!running) toggleRx(); }"); pg.wait_for_timeout(3500)
    pg.evaluate(FORCE_ON)
    return b, pg

def run_combo(lna, vga, i, total, log):
    """One combo on its own fresh browser. Returns (st, good)."""
    t0 = time.time()
    path = f"{OUT}/sweep_LNA_{lna}_VGA_{vga}_{RUN_TS}.png"
    fcsv = f"{OUT}/sweep_LNA_{lna}_VGA_{vga}_{RUN_TS}.csv"
    st, good = {}, False
    with sync_playwright() as p:
        b = pg = None
        try:
            b, pg = setup_page(p)
            for attempt in range(3):
                try:
                    pg.evaluate("(v)=>{const e=document.getElementById('lna');e.value=v;e.dispatchEvent(new Event('input',{bubbles:true}));}", lna)
                    pg.evaluate("(v)=>{const e=document.getElementById('vga');e.value=v;e.dispatchEvent(new Event('input',{bubbles:true}));}", vga)
                    pg.wait_for_timeout(2500)            # gain change restarts the scan
                    pg.evaluate(FORCE_ON)
                    pg.evaluate("resetMaxHold()")
                    gen_pass(log)                        # generator one pass; browser max-holds
                    pg.wait_for_timeout(1500)
                    pg.evaluate(FORCE_ON)                # re-assert in case it toggled during the pass
                    pg.wait_for_timeout(900)
                    st = pg.evaluate(STATE)
                    good = (st.get('mh') and st.get('ar') and st.get('n',0) > 10
                            and st.get('peak') is not None
                            and (st['peak'] - st['floor']) > 6        # a real signal above floor
                            and st['ref'] >= st['peak'] - 3)          # signal within the plot
                    if good:
                        break
                    log(f"[{i+1}/{total}] LNA={lna} VGA={vga} attempt {attempt+1} BAD "
                        f"(mh={st.get('mh')} ar={st.get('ar')} n={st.get('n')} "
                        f"peak={st.get('peak')} floor={st.get('floor')} ref={st.get('ref')}) - retry")
                except Exception as e:
                    log(f"[{i+1}/{total}] LNA={lna} VGA={vga} attempt {attempt+1} ERR {str(e)[:90]}")
            try:
                pg.screenshot(path=path)
                data = pg.evaluate("() => getMaxHold()")
                with open(fcsv, "w") as cf:
                    cf.write("freq_hz,power_db\n")
                    for fr, pwv in zip(data.get('freqs', []), data.get('power', [])):
                        cf.write(f"{fr:.0f},{pwv:.2f}\n")
            except Exception as e:
                log(f"[{i+1}/{total}] LNA={lna} VGA={vga} save ERR {str(e)[:90]}")
        finally:
            try:
                if b: b.close()
            except Exception:
                pass
    tag = "OK" if good else "WARN(unverified)"
    log(f"[{i+1}/{total}] LNA={lna} VGA={vga} {tag} peak={st.get('peak')} floor={st.get('floor')} "
        f"ref={st.get('ref')} -> {path} ({time.time()-t0:.0f}s)")
    return st, good

def main():
    os.makedirs(OUT, exist_ok=True)
    import sys
    logf = open("/tmp/gain_browser.log", "a")
    def log(m): print(m, flush=True); logf.write(m+"\n"); logf.flush()
    test  = ('test' in sys.argv)
    force = ('--all' in sys.argv)            # redo combos even if a PNG already exists
    all_combos = [(0,0)] if test else [(l,v) for l in range(N_LNA) for v in range(N_VGA)]
    combos = all_combos if force else [c for c in all_combos if not newest_png(*c)]
    done = len(all_combos) - len(combos)
    log(f"sweep start: {done} combo(s) already done, {len(combos)} to do "
        f"(SG step {SG_STEP/1e6:.0f} MHz, dwell {DWELL}s, fresh browser/combo)")
    gen_run()
    for i,(lna,vga) in enumerate(combos):
        gen_run()                            # re-assert the generator is transmitting
        try:
            run_combo(lna, vga, i, len(combos), log)
        except Exception as e:
            log(f"[{i+1}/{len(combos)}] LNA={lna} VGA={vga} COMBO-ERR {str(e)[:120]}")
    build_pdf(all_combos, log)
    log("ALL DONE -> "+OUT)

if __name__ == '__main__':
    main()
