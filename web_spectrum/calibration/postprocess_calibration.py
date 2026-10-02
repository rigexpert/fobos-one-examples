#!/usr/bin/env python3
"""Clean ambient-interference corruption out of the measured frequency-response curve.

The Fobos+cable response is physically a smooth curve over the whole 70-6000 MHz span.
Wideband ambient signals (notably 2.4 GHz Wi-Fi/BT, ~200 MHz of the band) make the tone
peak-search latch onto interference, producing a large localized anomaly. We fit a robust
smooth polynomial (iteratively rejecting outliers), flag points that deviate from it, and
replace them by interpolating across from the clean neighbours. Genuine fine detail in
clean regions is preserved. Re-saves fobos_calibration.json (raw kept as .raw).
"""
import json, os, shutil, numpy as np

OUT = os.path.join(os.path.dirname(__file__), "fobos_calibration.json")
# The real RX+cable response offset is always negative (~-54..-32 dB). Ambient interference
# (mainly the 2.4 GHz ISM band) makes the tone search read a non-physical value (up to +25 dB).
# Strategy: flag gross outliers (C > HI_THRESH), DILATE the flagged region by EDGE_MHz to
# also drop the partially-corrupted band edges, and below VALID_MIN (the Aaronia sig-gen's
# reach) hold flat. Then interpolate every dropped point across from the clean neighbours.
HI_THRESH = -28.0
EDGE_MHZ  = 80.0        # dilate flagged interference bands by this on each side
VALID_MIN = 200e6       # below this the Aaronia can't generate the tone -> extrapolate flat

def robust_clean(f, y):
    from scipy.ndimage import binary_dilation
    step = f[1] - f[0]
    bad = y > HI_THRESH
    bad = binary_dilation(bad, iterations=int(EDGE_MHZ*1e6/step))   # cover corrupted band edges
    bad |= (f < VALID_MIN)                                          # low-freq: extrapolate flat
    out = y.copy()
    good = np.where(~bad)[0]
    if len(good): out[bad] = np.interp(np.where(bad)[0], good, y[good])
    return out, bad

def main():
    raw = OUT + ".raw"
    src = raw if os.path.exists(raw) else OUT
    d = json.load(open(src))
    f = np.array(d['freq_response']['freqs_hz']); off = np.array(d['freq_response']['offset_db'])
    cleaned, bad = robust_clean(f, off)
    # report contiguous rejected bands
    idx = np.where(bad)[0]; bands=[]
    if len(idx):
        s=idx[0]
        for k in range(1,len(idx)+1):
            if k==len(idx) or idx[k]!=idx[k-1]+1:
                bands.append((f[s]/1e6, f[idx[k-1]]/1e6)); s=idx[k] if k<len(idx) else s
    print(f"rejected {int(bad.sum())}/{len(off)} interference points in {len(bands)} band(s):")
    for a,b in bands[:30]: print(f"   {a:.1f} - {b:.1f} MHz")
    if not os.path.exists(raw): shutil.copy(OUT, raw)
    d['freq_response']['offset_db'] = cleaned.tolist()

    # ── clean the gain grid: drop corrupted coarse columns (the ref-gain delta must be
    # ~0 at every coarse freq; a large value flags an interference/low-freq column) ──
    gd = d['gain_delta']; cf = np.array(gd['freqs_hz']); dl = np.array(gd['delta_db'])  # [lna][vga][coarse]
    ri = gd['lnas'].index(d['meta']['ref_lna']); vj = gd['vgas'].index(d['meta']['ref_vga'])
    badcol = np.abs(dl[ri, vj, :]) > 5.0
    goodc = np.where(~badcol)[0]
    if len(goodc):
        for li in range(dl.shape[0]):
            for vi in range(dl.shape[1]):
                dl[li, vi, badcol] = np.interp(np.where(badcol)[0], goodc, dl[li, vi, goodc])
    gd['delta_db'] = dl.tolist()
    print(f"gain grid: replaced {int(badcol.sum())} corrupted coarse column(s) at "
          f"{[int(cf[i]/1e6) for i in np.where(badcol)[0]]} MHz")

    d['meta']['postprocess'] = (f"non-physical threshold C_ref>{HI_THRESH}dB + dilate {EDGE_MHZ}MHz, "
                                f"<{VALID_MIN/1e6:.0f}MHz flat; {int(bad.sum())} freq-resp pts and "
                                f"{int(badcol.sum())} gain-grid columns replaced (ambient interference / sig-gen reach)")
    json.dump(d, open(OUT,"w"), indent=1)
    print(f"cleaned Cref range {cleaned.min():.1f}..{cleaned.max():.1f} dB (was {off.min():.1f}..{off.max():.1f})")

if __name__ == "__main__":
    main()
