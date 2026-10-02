#!/usr/bin/env python3
"""Cycle the Spectran V6B internal Signal Generator across the band so a Fobos
scan with Max Hold builds a full-band signal picture.

Prereq: RTSA running a mission with V6B **TX mode = Signal Generator** (load
siggen_sweep.rmix, or set it in the GUI). This script then:
  1. sets the generator to **Full Sweep** (covers the instantaneous span at each
     center) — best effort via PUT; only applies once the V6B is in sig-gen mode,
  2. steps the V6B center frequency from START to STOP, dwelling DWELL s on each
     (the 'main' group centerfreq applies live via PUT /remoteconfig).

Meanwhile, on the Fobos web app: SCAN mode over the same 70-6000 MHz range with
MAX HOLD on -> the dwelling sig-gen energy paints the coverage picture.

Usage: siggen_sweep.py [--start 70] [--stop 6000] [--step 40] [--dwell 5]
                       [--attn -30] [--loop] [--no-gen]
"""
import sys, time, json, argparse, urllib.request

HOST = "10.10.1.74"
URL  = f"http://{HOST}:54664/remoteconfig"   # control HTTP server (was 54666 in older mission)
_req = [100]

def _put(items):
    """PUT a nested config under Block_Spectran_V6B_0."""
    _req[0] += 1
    body = {"request": _req[0], "receiverName": "Block_Spectran_V6B_0",
            "config": {"type": "group", "items": items}}
    data = json.dumps(body).encode()
    r = urllib.request.Request(URL, data=data, method="PUT")
    try:
        urllib.request.urlopen(r, timeout=6).read()
        return True
    except Exception as e:
        print("  PUT failed:", str(e)[:80]); return False

def set_centerfreq(hz):
    return _put([{"type": "group", "name": "main", "items": [
        {"type": "float", "name": "centerfreq", "value": float(hz)}]}])

def set_attn(db):
    return _put([{"type": "group", "name": "main", "items": [
        {"type": "float", "name": "transattn", "value": float(db)}]}])

def set_generator_fullsweep(start_hz, stop_hz, step_hz, dur):
    """type=6 == Full Sweep (see Generator Type enum). Applies only when the V6B
    is already in Signal Generator TX mode (device-group param)."""
    return _put([{"type": "group", "name": "device", "items": [
        {"type": "group", "name": "generator", "items": [
            {"type": "enum",  "name": "type",      "value": 6},
            {"type": "float", "name": "startfreq", "value": float(start_hz)},
            {"type": "float", "name": "stopfreq",  "value": float(stop_hz)},
            {"type": "float", "name": "stepfreq",  "value": float(step_hz)},
            {"type": "float", "name": "duration",  "value": float(dur)},
        ]}]}])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--start", type=float, default=70)     # MHz
    ap.add_argument("--stop",  type=float, default=6000)   # MHz
    ap.add_argument("--step",  type=float, default=40)     # MHz (~instantaneous span)
    ap.add_argument("--dwell", type=float, default=5)      # s per center
    ap.add_argument("--attn",  type=float, default=None)   # transmitter gain (dB), e.g. -30
    ap.add_argument("--loop",  action="store_true")        # repeat forever
    ap.add_argument("--no-gen", action="store_true")       # skip generator config (set in GUI)
    a = ap.parse_args()

    if a.attn is not None:
        print(f"set transmitter gain = {a.attn} dB"); set_attn(a.attn)
    if not a.no_gen:
        # Full Sweep covering the instantaneous span around each center (the script
        # re-issues per step below; here we set the type/duration once).
        print("set generator = Full Sweep")
        set_generator_fullsweep(a.start*1e6, a.stop*1e6, a.step*1e6, a.dwell)

    n = int((a.stop - a.start) / a.step) + 1
    print(f"sweeping {a.start:.0f}->{a.stop:.0f} MHz, {a.step:.0f} MHz steps, "
          f"{a.dwell:.0f}s dwell ({n} steps, ~{n*a.dwell/60:.1f} min/pass)")
    try:
        while True:
            f = a.start
            while f <= a.stop + 1e-6:
                ok = set_centerfreq(f*1e6)
                print(f"  {f:7.1f} MHz {'ok' if ok else 'ERR'}", flush=True)
                time.sleep(a.dwell)
                f += a.step
            if not a.loop:
                break
            print("--- pass complete, looping ---")
    except KeyboardInterrupt:
        print("\nstopped.")

if __name__ == "__main__":
    main()
