# fsk_loopback — FSK Over-The-Air Loopback Test Harness

An end-to-end **FSK modem test**: an Aaronia Spectran V6B transmits a generated FSK burst, a
FobosOne receives and demodulates it, and the harness reports the symbol/bit error rate (SER/BER)
across parameter sweeps. This is a research/lab harness, not a single example program.

## Hardware setup

- **Aaronia Spectran V6B** with the **power/TX USB** connected, running **RTSA-Suite PRO**. It is
  driven over its HTTP remote-config API, and IQ files are `scp`'d to it for the FileReader → TX.
- **FobosOne SDR** on USB.
- An **RF cable** (or antennas) between the V6B TX and the FobosOne RX.

## Signal path

```
gen_fsk → resample 2.5 → 5.714 Msps → .rtsa file
        → scp to Aaronia → FileReader → V6B TX
        → RF cable → FobosOne RX capture → demod → SER/BER table
```

## Build (C helpers)

```sh
cd examples/aaronia_OTA/fsk_loopback/c
make            # builds: capture, gen_fsk, demod_fsk_file
```
Python deps: `numpy`, `scipy` (`pip install numpy scipy`).

## Run the test

```sh
# Full OTA loopback (auto-restarts RTSA for each case):
bash run_ota_test.sh
#   or, if the V6B is already connected and you hot-swap the .rtsa file:
bash run_ota_test.sh --no-restart --rtsa-wait 6

# Quick TX sanity check — expect a CW tone at 436 MHz (≥ -60 dBFS):
bash verify_tx.sh
```
**Success:** the run prints an SER/BER table per case; a working link typically shows **2-/4-FSK
at ~0–4 % BER**. `verify_tx.sh` should show a strong CW peak near the expected offset.

> These scripts contain host IPs / file paths for a specific bench — adjust them for your setup.

## Scripts

| File | Purpose |
|---|---|
| `run_ota_test.sh` | main OTA loopback runner (normal / `--no-restart` modes) |
| `ota_loopback.py` | the loopback engine (gen → .rtsa → TX → RX → demod → SER) |
| `ota_sweep.py` | sweep modulation params and collect results |
| `verify_tx.sh` | quick TX hardware check (CW tone) |
| `gen_fsk_py.py` | generate the FSK IQ / `.rtsa` burst |
| `demod_cont_fsk.py` | continuous-FSK demodulator |
| `demod_aaronia_fsk.py`, `demod_fobos_fsk_burst.py`, `demod_iqmod_fsk.py` | demod variants |
| `aaronia_stream_tx.py` | stream/TX helper for the Aaronia |
| `ota_capture.py` | capture-only helper |
| `mfsk_lab.py` | M-FSK experimentation lab |
| `diag_iq_sign.py` | IQ sign / spectrum diagnostics |
| `fsk_loopback.py` | wired/loopback variant |
| `c/gen_fsk`, `c/capture`, `c/demod_fsk_file` | C generator / capture / file-demod tools |

## See also

[`../demod_fsk/`](../../abstract_demod/demod_fsk/) — the standalone FSK demodulator example.
