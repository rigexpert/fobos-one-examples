# modem_loopback — OTA Modem Loopback Tests

End-to-end **over-the-air (OTA) modem tests** for FSK and PSK: an **Aaronia Spectran V6B**
transmits a generated signal, a **FobosOne** receives and demodulates it, and the harness reports
symbol/bit error rate (SER/BER) across parameter sweeps. Research/lab tooling — not part of the
`web_spectrum` app.

## Subprojects

| Dir | What it tests |
|---|---|
| [`fsk_loopback/`](fsk_loopback/) | FSK modem loopback **+ the shared OTA engine** (`ota_loopback.py`, `ota_sweep.py`, `ota_capture.py`) that both subprojects use |
| [`psk_loopback/`](psk_loopback/) | PSK / DQPSK modem loopback (imports the shared engine from `fsk_loopback/`) |

> The two are **coupled**: `psk_loopback` reuses the FSK subproject's Python modules
> (`sys.path.insert(0, '../fsk_loopback')`), and `fsk_loopback/ota_loopback.py` drives both
> modes. Keep them as siblings.

## How the loopback works

It is a **real RF** loopback, not a software file-in/file-out loopback:

```
generate IQ → .rtsa file → scp → Aaronia FileReader → V6B TX
                                                       │  RF cable   (real transmission)
                                                       ▼
                              FobosOne → capture (fresh IQ) → demod_*_file → SER/BER
```

- **TX** = a generated **`.rtsa` IQ file** is uploaded to the Aaronia and *played out* by its
  FileReader through the V6B transmitter (there is also an experimental live-stream path,
  `fsk_loopback/aaronia_stream_tx.py`).
- **RX** = the FobosOne captures a fresh block of the actual received signal, which is
  demodulated to bits and compared against the transmitted sequence.

## Hardware setup

- Aaronia Spectran V6B (power/TX USB) running RTSA-Suite PRO, driven over its HTTP remote-config
  API; IQ files are `scp`'d to it.
- A FobosOne SDR on USB.
- An RF cable between the V6B TX and the FobosOne RX.

## Quick start

```sh
# build the C helpers for each side
( cd fsk_loopback/c && make )      # capture, gen_fsk, demod_fsk_file
( cd psk_loopback/c && make )      # capture, gen_psk, demod_psk_file

# FSK OTA loopback (see fsk_loopback/README.md for options/modes)
bash fsk_loopback/run_ota_test.sh

# PSK OTA sweeps (see psk_loopback/README.md)
python3 psk_loopback/psk_sweep.py
```

See each subproject's README for the full script list, options, and expected results. Host IPs /
paths in the scripts are set for a specific bench — adjust them for your setup.
