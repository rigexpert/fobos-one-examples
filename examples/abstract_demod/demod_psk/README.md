# demod_psk — M-PSK Demodulator

Coherent M-PSK demodulation (M = 2…64, power of 2) of a signal received from the Fobos SDR.
Output is a raw bit stream packed MSB-first into bytes.

**Requires:** [liquid-dsp](https://github.com/jgaeddert/liquid-dsp).

> ⚠ **Baseline example — limited for real signals.** The Costas loop locks to *a* carrier phase,
> but PSK has an **M-fold phase ambiguity** — resolve it with **differential encoding (DPSK/DQPSK)**
> or a known pilot/preamble. Real bursts also need **preamble-based timing synchronization**, which
> this example does not implement. For an OTA-proven PSK/DQPSK receiver see
> [`../../aaronia_OTA/psk_loopback/`](../../aaronia_OTA/psk_loopback/) and the group note in
> [`../README.md`](../README.md).

## Overview

```
[Fobos SDR] → DDC (optional -O) → rational resampler
           → symbol timing recovery (symsync_crcf: Gardner TED + polyphase RRC)
           → carrier recovery (Costas loop: nco_crcf + modemcf)
           → symbol → bits (MSB-first) → byte output
```

## Build

```sh
# liquid-dsp (once):
git clone https://github.com/jgaeddert/liquid-dsp
cd liquid-dsp && ./bootstrap.sh && ./configure && make && sudo make install && sudo ldconfig
cd -

# the example (needs the installed libfobos-sdr-agile too):
cd examples/abstract_demod/demod_psk/c
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```
This builds `demod_psk` and the self-test binaries `test_demod_psk` and `test_psk_e2e`.

## Run / Test

**Functional self-test (no hardware):** synthesises a modulated signal and checks the recovered
bits — run the built test binaries:

```sh
./test_demod_psk      # unit-level checks
./test_psk_e2e        # end-to-end generate → demod, expect PASS / low error rate
```

**Live off-air (needs a Fobos SDR + a PSK transmitter):**

```sh
# BPSK at 9600 baud, 433 MHz, 2 MSPS
./demod_psk -c 433000000 -r 2000000 -b 9600 -M 2 -o bits.bin

# QPSK at 19200 baud
./demod_psk -c 433000000 -r 2000000 -b 19200 -M 4 -o bits.bin
```
For a controlled loopback test (Aaronia generator → Fobos), see
[`../psk_loopback/`](../../aaronia_OTA/psk_loopback/).

## Options

| Flag | Description |
|---|---|
| `-c <Hz>` | SDR centre frequency |
| `-r <Hz>` | IQ sample rate |
| `-b <baud>` | Symbol rate |
| `-M <N>` | Constellation order (2=BPSK, 4=QPSK, …) |
| `-O <Hz>` | DDC offset (shift signal to DC) |
| `-o <file>` | Output bit stream (default stdout) |

(Run `./demod_psk -h` for the full, authoritative list.)

## Files

- `c/main.c` — CLI + capture + demod pipeline.
- `c/test_demod_psk.c`, `c/test_psk_e2e.c` — functional self-tests.
- Shared: [`../shared/`](../shared/), [`../fobos/`](../fobos/).
