# psk_loopback — PSK Over-The-Air Loopback Test / Lab

An end-to-end **PSK modem test/lab** (sibling of [`../fsk_loopback/`](../fsk_loopback/)): an
Aaronia Spectran V6B transmits a generated PSK signal, a FobosOne receives and demodulates it,
and the scripts sweep parameters and report symbol/bit error rate (SER/BER). Includes DQPSK and
carrier-PLL diagnostics.

## Hardware setup

- **Aaronia Spectran V6B** (power/TX USB) running **RTSA-Suite PRO**, driven over its HTTP
  remote-config API.
- **FobosOne SDR** on USB.
- An **RF cable** between the V6B TX and the FobosOne RX.

## Signal path

```
gen_psk → IQ / .rtsa → Aaronia FileReader → V6B TX
        → RF cable → FobosOne RX capture → demod → SER/BER
```

## Build (C helpers)

```sh
cd examples/aaronia_OTA/psk_loopback/c
make            # builds: capture, gen_psk, demod_psk_file
```
Python deps: `numpy`, `scipy`.

## Run

```sh
python3 psk_sweep.py          # coherent M-PSK OTA sweep → SER table
python3 ota_sweep_psk.py      # OTA sweep variant
python3 dqpsk_sweep.py        # differential QPSK sweep
python3 pll_probe.py          # carrier-PLL lock diagnostics
python3 restart_rtsa.py       # restart the Aaronia RTSA over its API (helper)
```
**Success:** each run prints an SER/BER table; the link works reliably at sample rates
**≥ ~1 MS/s** (below that the carrier phase decorrelates within a symbol, so ~100 kS/s fails).

> Lab scripts — adjust host IPs / paths for your bench.

## Scripts

| File | Purpose |
|---|---|
| `psk_sweep.py` | coherent M-PSK OTA parameter sweep |
| `ota_sweep_psk.py` | OTA sweep variant |
| `psk_loopback.py` | wired/loopback PSK variant |
| `dqpsk_ota.py`, `dqpsk_sweep.py` | differential QPSK OTA / sweep |
| `mpsk_lab.py` | M-PSK experimentation lab |
| `pll_probe.py` | Costas/PLL carrier-recovery diagnostics |
| `restart_rtsa.py` | restart the Aaronia RTSA-Suite over its API |
| `c/gen_psk`, `c/capture`, `c/demod_psk_file` | C generator / capture / file-demod tools |

## See also

[`../demod_psk/`](../../abstract_demod/demod_psk/) — the standalone PSK demodulator example.
