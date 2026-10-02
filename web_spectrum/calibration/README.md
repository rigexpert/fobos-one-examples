# calibration — Absolute-power (dBm) calibration for web_spectrum

The measurement tooling behind the web app's **CAL** tab. It produces
`fobos_calibration.json`, which the app loads (`/api/calibration`) so the spectrum/peak read in
**absolute dBm**.

## How it fits in

The C++/Python backend **spawns `calib_orchestrator.py`** when you click *Start* in the CAL tab
and streams its JSON progress lines back to the browser over the WebSocket (`cal_progress`).

## Method

An **Aaronia Spectran V6** signal generator emits a relative-tone CW (+200 kHz) at a known TX
power; the FobosOne (driven via the backend's REST API) measures the received tone power. With
the known RF-cable insertion loss, the true power at the RX port is `TX_power − cable_loss(f)`,
so the calibration offset is `C(f, lna, vga) = expected_dBm − R_dBFS`.

- **Factored model** (keeps it to ~an hour): a fine frequency sweep at one reference gain, plus a
  coarse LNA×VGA gain grid; multiple TX powers verify linearity.
- Ambient-interference cleanup and cable-loss correction are applied.

### Gain grid: 96 combos, not 128

The driver accepts LNA 0–3, but steps 0 and 1 are both 0 dB (2 is +16 dB, 3 is +33 dB), so the
grid measures LNA `[0, 2, 3]` × VGA 0–31 — re-measuring a known-identical state would only spend
32 sweeps to learn nothing.

The **emitted table still carries all four rows**: step 1 is aliased onto step 0 when the JSON is
written. This is required, not cosmetic — `calOffsetAt()` in `templates/index.html` falls back to
`lnas[length-1]` when a step is missing, so shipping only `[0, 2, 3]` would silently apply the
+33 dB correction to LNA=1, a 0 dB setting. Aliasing also keeps the default `ref_lna = 1` valid
for the `gd['lnas'].index(ref_lna)` lookup in `postprocess_calibration.py`.

`tools/cal_sweep/` deliberately sweeps all 128 — it is a characterization tool, where measuring
both 0 dB steps confirms they are identical instead of assuming it. See its README.

## Files

| File | Purpose |
|---|---|
| `calib_orchestrator.py` | config-driven sweep launched by the CAL tab (drives the Aaronia + the app REST); emits JSON progress; writes `fobos_calibration.json` |
| `cable_loss.py` | Mini-Circuits CBL-1M-SMSM+ insertion-loss table + interpolation (or a custom Touchstone `.s1p/.s2p`) |
| `postprocess_calibration.py` | ambient-interference rejection / curve cleanup |
| `calib_measure.py` | earlier standalone CLI version of the sweep |
| `fobos_calibration.json` | the calibration output the app consumes (generated) |

## Run standalone (advanced)

Normally you run it from the CAL tab. It can also be invoked directly with a JSON config file:

```sh
python3 calib_orchestrator.py <config.json>     # add "dry_run": true to test without hardware
```
Config keys: `from_hz`, `to_hz`, `step_hz`, `gen_ip`, `gen_port`, `powers_dbm`, `gain_mode`
(`single`/`factored`/`full`), `ref_lna`, `ref_vga`, `coarse_step_hz`, `cable`
(`cbl-1m-smsm`/`custom`/`none`), `touchstone`, `exclude_hz`, `rate_hz`, `fft_size`, `accum`,
`settle_ms`, `tone_offset_hz`, `clean`.

Python deps: `numpy`, `scipy`.
