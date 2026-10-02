# demod_fsk — M-FSK Demodulator

Non-coherent **M-FSK** demodulation (M any power of 2 ≥ 2) from a
[RigExpert Fobos SDR](https://rigexpert.com/fobos/). Decoded symbols are
converted to bits, packed **MSB-first** into bytes, and written to stdout or a
file — directly pipeable to `xxd`, `od`, or a protocol decoder.

> ⚠ **Baseline example — limited for real signals.** This demonstrates `fskdem` on a clean,
> continuously modulated stream. A real over-the-air burst needs **burst/symbol synchronization**
> (e.g. a **preamble** the receiver correlates against to recover timing) which this example does
> not implement. For an OTA-proven FSK receiver see
> [`../../aaronia_OTA/fsk_loopback/`](../../aaronia_OTA/fsk_loopback/) and the group note in
> [`../README.md`](../README.md).

## Overview

The full DSP chain runs in one real-time loop that pulls IQ from a dedicated
lock-free capture thread:

```
[Fobos SDR]  IQ float32 @ samplerate (e.g. 2 MSPS)
     │        capture thread (SCHED_FIFO, pinned to core 3)
     ▼
[receiver_read()]          pull IQ block from ring buffer
     ▼
[shift_unroll_process()]   optional DDC (-O): shift off-centre signal to DC
     │                     pre-computed LO table → NEON-vectorised multiply-add on ARMv8
     ▼
[msresamp_crcf]            rational resampler  samplerate → symbol_rate × k
     │                     (also the anti-alias filter, 60 dB stopband)
     ▼
[fskdem_demodulate()]      liquid-dsp non-coherent energy detector:
     │                     k complex samples → symbol index 0…M-1
     │                     (DFT energy in each of M tone bins, pick max — no phase reference)
     ▼
[symbol → bits]            optional Gray→natural mapping;
     │                     log2(M) bits shifted MSB-first into a byte accumulator
     ▼
[fwrite()]                 raw byte stream to stdout or -o file
```

There is no closed-loop symbol timing recovery: `fskdem` is fed exactly `k`
samples per symbol. For long transmissions with clock drift, add a
`symsync_crcf` stage after the resampler.

## Build

Run from `examples/abstract_demod/demod_fsk/c`. Two build systems are provided.

**Dependencies**

- [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) — built and installed
  from source:
  ```sh
  git clone https://github.com/jgaeddert/liquid-dsp
  cd liquid-dsp && ./bootstrap.sh && ./configure && make && sudo make install && sudo ldconfig
  ```
- **libfobos-sdr-agile** — the Fobos hardware library, built and
  `sudo make install`ed (installs the library, header, pkg-config file, and
  udev rule). See the repo-root README for details.

**CMake (recommended)**

```sh
cd examples/abstract_demod/demod_fsk/c
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This builds three binaries: `demod_fsk`, `test_demod_fsk`, `test_fsk_e2e`.
Optimisation flags: native builds accept `-DCC_ARCH=native` (default) or a CPU
name such as `-DCC_ARCH=cortex-a76`; cross builds use separate
`-DCC_MARCH=armv8.2-a -DCC_MTUNE=cortex-a76`.

**Make (alternative)**

```sh
cd examples/abstract_demod/demod_fsk/c
make            # builds demod_fsk + both test binaries
make CC_ARCH=cortex-a76
```

## Run / Test

### (a) Live off-air demodulation

Requires a Fobos SDR plugged in and an antenna, with an actual M-FSK signal on
the tuned frequency (e.g. a 433 MHz ISM device, a POCSAG pager transmitter, or
a bench signal generator playing an FSK burst).

```sh
# 2-FSK, 9600 baud, tuned to 433.92 MHz, 2 MSPS, 8 samples/symbol → hex dump
./demod_fsk -F 433920000 -s 2000000 -R 9600 -k 8 -M 2 | xxd | head

# Signal sits 100 kHz above centre → shift it to DC with the DDC (-O)
./demod_fsk -F 433820000 -s 2000000 -R 9600 -k 8 -M 2 -O 100000 | xxd

# 4-FSK, 115200 baud, 4 MSPS, 16 samples/symbol, verbose stats to stderr
./demod_fsk -F 868000000 -s 4000000 -R 115200 -k 16 -M 4 -v -o bits.bin
```

With `-v` the tool prints the configured pipeline and a running
`samples / symbols / bits` counter to stderr every 2 s. Press Ctrl-C to stop.

### (b) Built-in self-tests (no hardware required)

Both are built alongside `demod_fsk`.

`test_demod_fsk` — pure-logic unit tests (Gray↔natural round-trip, MSB-first bit
extraction, all four content×format output modes). No liquid-dsp, no hardware.

```sh
./test_demod_fsk
```
PASS looks like: each `TEST n …` block ends with `done.`, followed by the final
line `All tests passed.` (exit code 0). Any failure prints a `FAIL:` line and
the summary `N test(s) FAILED.` (exit code 1).

`test_fsk_e2e` — full synthetic modem loop: generates noisy IQ bursts with
`fskmod`, then demodulates with `fskdem` under three timing strategies (oracle,
no-sync, energy-detector + preamble timing search) and reports symbol-error
rate versus SNR for M = 2 and 4.

```sh
./test_fsk_e2e
```
PASS looks like a table where SER drops toward `0.000` as SNR rises and the
`det/total` column reaches `5/5` (all bursts detected) at usable SNR — e.g. the
`oracle` and `pll` columns should read ~`0.000` at 10–20 dB. This is an
observational benchmark rather than a strict pass/fail assertion.

## Options

| Flag | Long | Default | Description |
|---|---|---|---|
| `-F <hz>` | `--frequency` | — (required) | SDR tuning frequency |
| `-s <hz>` | `--samplerate` | 2 000 000 | SDR capture sample rate |
| `-R <baud>` | `--symrate` | 9600 | FSK symbol rate |
| `-k <n>` | `--sps` | 8 | Samples per symbol (oversampling, ≥ 2) |
| `-M <n>` | `--symbols` | 2 | Number of FSK tones, power of 2 ≥ 2 |
| `-b <0-1>` | `--bandwidth` | 0.50 | fskdem tone bandwidth, normalised to `symrate × sps` |
| `-O <hz>` | `--offset` | 0 | DDC offset from centre; 0 disables the DDC |
| `-c <mode>` | `--coding` | natural | Symbol mapping: `natural` or `gray` (inverse Gray) |
| `-t <mode>` | `--type` | bits | Output content: `bits` or `symbols` |
| `-f <mode>` | `--format` | binary | Output encoding: `binary` or `ascii` |
| `-o <file>` | `--output` | stdout | Output file |
| `-L <n>` | `--lna-gain` | hw default | LNA gain index (0–3) |
| `-V <n>` | `--vga-gain` | hw default | VGA gain index (0–31) |
| `-m <mode>` | `--mode` | sync | Capture mode: `sync` or `async` |
| `-v` | `--verbose` | off | Print config + runtime stats to stderr |
| `-h` | `--help` | — | Print usage and exit |

Output combinations: `bits+binary` → one byte per bit (`0x00`/`0x01`);
`bits+ascii` → `'0'`/`'1'` character stream; `symbols+binary` → one byte per
symbol (`0…M-1`); `symbols+ascii` → decimal symbol per line.

## Files

| File | Description |
|---|---|
| `main.c` | Demodulator: arg parsing, DSP pipeline (DDC → resample → fskdem → bit packing), output |
| `test_demod_fsk.c` | Unit tests — Gray coding, MSB-first bit extraction, output modes (no deps) |
| `test_fsk_e2e.c` | End-to-end synthetic modem test — SER vs SNR with fskmod/fskdem and timing search |
| `CMakeLists.txt` | CMake build for all three binaries; finds liquid-dsp + libfobos_sdr via pkg-config |
| `Makefile` | Plain-make alternative build |
| `dsp.c` / `dsp.h` | Shared sample-format converters and DDC (`shift_unroll_*`) — symlink to `../../shared` |
| `receiver.c` / `receiver.h` | Real-time ring-buffer IQ capture thread — symlink to `../../fobos` |
| `fobos.c` / `ringbufer.c` | Fobos SDR wrapper and lock-free ring buffer — symlinks to `../../fobos` |
</content>
</invoke>
