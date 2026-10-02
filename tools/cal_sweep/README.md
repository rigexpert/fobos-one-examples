# cal_sweep — Receive-calibration sweep (standalone)

Steps an Aaronia Spectran V6 PLUS CW tone across a frequency range while the Fobos SDR measures
received peak power for **every (LNA, VGA) gain combination**, producing a CSV and a matplotlib
plot of all 128 gain curves (LNA 0–3 × VGA 0–31).

> Standalone tool. It measures **relative dBFS** per gain — it is **not** the absolute-dBm
> calibration used by the web app (that lives in `web_spectrum/calibration/`).

## Gain grid: why 128 combos here and 96 in `web_spectrum/calibration/`

The driver accepts LNA 0–3 and VGA 0–31, but **LNA steps 0 and 1 are both 0 dB** (2 is +16 dB,
3 is +33 dB). Only three LNA settings are electrically distinct, so the two tools deliberately
disagree on how many combinations to measure:

| | Combos | Why |
|---|---|---|
| `tools/cal_sweep` (this tool) | **128** = 4 × 32 | Characterization — measures the response surface |
| `web_spectrum/calibration` | **96** = 3 × 32 | Calibration — skips the duplicate, aliases 1 → 0 |

This tool sweeps the **full** range on purpose. Its job is to plot what the hardware actually
does, so measuring LNA 0 and 1 separately *confirms* they are identical rather than assuming the
datasheet. The duplicate curve is the evidence, not waste — if the two curves ever diverge, that
is a finding. It costs one extra LNA row — 32 measurements per frequency, 25% of the run — and
draws one redundant curve per plot; both are acceptable for a diagnostic run.

The web app's calibration makes the opposite trade: it is run to produce a correction table, not
to learn anything, so re-measuring a known-identical state is 32 wasted sweeps. It measures LNA
`[0, 2, 3]` and aliases step 1 onto step 0 when writing the table, so the emitted file still
carries a row for every selectable step.

> If you would rather this tool also skipped the duplicate, iterate an explicit `{0, 2, 3}` in
> `c/measure.c` rather than lowering `LNA_MAX` — the redundant step is 1, not the top of the
> range, so a lower ceiling would drop the +33 dB step instead. Note that skipping it means a
> divergence between steps 0 and 1 would go unnoticed, which is the one thing this tool exists
> to catch.

## Hardware setup

- **Aaronia Spectran V6 PLUS** as a CW generator, driven over its HTTP API (host/port set via
  `--host` / `--port`).
- **Fobos SDR** on USB.
- An **RF cable** between the generator and the Fobos.

## Signal path

```
Aaronia TX (CW) --RF cable--> Fobos RX --> measure (FFT peak dBFS) --> cal_sweep.py aggregates
```

## Build

```sh
cd tools/cal_sweep/c
make            # builds ./measure  (needs libfftw3 + installed libfobos-sdr-agile)
```
Python deps: `numpy`, `matplotlib`.

## Run

```sh
python3 cal_sweep.py                      # defaults: 70 MHz → 6 GHz, 10 MHz step
python3 cal_sweep.py --freq-start 400e6 --freq-stop 500e6 --freq-step 10e6 --tx-power -20
```
**Outputs:** `cal_sweep.csv` + `cal_sweep.png` (128 gain curves); `spot_check.*` for a focused
re-check. **Success:** the plot shows 128 monotonic gain curves and the CSV has one row per
(freq, LNA, VGA).

## Options

| Flag | Default | Description |
|---|---|---|
| `--host` | (script) | Aaronia RTSA host |
| `--port` | 54665 | Aaronia FFT/HTTP port |
| `--freq-start` / `--freq-stop` / `--freq-step` | 70e6 / 6000e6 / 10e6 | Sweep range (Hz) |
| `--tx-power` | -20 | Generator output (dBm) |
| `--rate` | 2500000 | Fobos sample rate (Hz) |
| `--n-fft` | 65536 | FFT size for `measure` |
| `--measure` | `./c/measure` | Path to the measure binary |

(Run `python3 cal_sweep.py --help` for the authoritative list.)

## Files

- `cal_sweep.py` — orchestration, CSV, plotting.
- `c/measure.c` — per-frequency FFT-peak meter (opens the Fobos directly via libfobos).
- `c/fobos.c` / `c/fobos.h`, `c/Makefile` — the measure tool's device wrapper + build.
