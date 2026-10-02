# siggen_sweep — Coverage-painting helper for SCAN + Max-Hold

Drives the **Aaronia Spectran V6B internal signal generator** across the band so the web app's
**SCAN mode with MAX HOLD** builds a full-band coverage picture. A manual test/demo utility for
`web_spectrum` — it only talks to the Aaronia; you watch the result in the web UI.

## Prerequisite

RTSA running a mission with the V6B in **TX = Signal Generator** mode (load `siggen_sweep.rmix`,
or set it in the GUI).

## What it does

1. Sets the generator to **Full Sweep** (best-effort, via `PUT /remoteconfig`).
2. Steps the V6B center frequency from `--start` to `--stop`, dwelling `--dwell` s at each.

Meanwhile, in the Fobos web app: **SCAN** over the same range with **MAX HOLD** on — the dwelling
generator energy paints the coverage.

## Run

```sh
python3 siggen_sweep.py [--start 70] [--stop 6000] [--step 40] [--dwell 5] \
                        [--attn -30] [--loop] [--no-gen]
```
Edit `HOST` / the control URL near the top of the script for your Aaronia's IP and port.

Python deps: standard library only.
