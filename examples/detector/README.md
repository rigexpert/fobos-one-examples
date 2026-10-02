# detector — Signal Detector

Real-time wideband occupied-channel detector for the [RigExpert Fobos SDR](https://rigexpert.com/fobos/).
It streams IQ from the receiver, computes averaged FFT power spectra, estimates an automatic noise
floor, and reports every occupied channel it finds with its centre frequency, bandwidth, and peak power.

## Overview

The detector converts each averaged FFT into a DC-centred dBFS spectrum and locates signals with a
fully automatic, per-capture pipeline:

- **Automatic noise floor** — the noise level is taken as the *N*th percentile (default 25th) of the
  spectrum, so no manual reference level is needed. It can be a single global value or a per-bin
  sliding-window estimate (`-l`) that follows receiver filter rolloff and 1/f slope.
- **Occupied-channel detection** — bins above `noise_floor + threshold` (default 10 dB) start a run.
  A hysteresis fall threshold (`threshold − hysteresis`) bridges shallow intra-signal dips, adjacent
  runs within `-g` bins are merged, and runs narrower than `-n` bins are discarded. Each surviving run
  yields a power-weighted centroid centre frequency, an edge-to-edge bandwidth, and a peak dBFS value.
- **Two modes:**
  - *Fixed RTBW* (default) — monitors one fixed centre frequency (`-c`) over the full real-time
    bandwidth (RTBW = the sample rate).
  - *Scan* — enabled when both `-f` and `-F` are given; uses the Fobos hardware scan API to step across
    a frequency range and detect signals at each step, stitching the steps into one panorama.
- **Signal tracking** — detections are matched frame-to-frame by centre-frequency proximity. Each
  tracked signal produces lifecycle events: `START` (first seen, stderr-only), `UPD` (periodic update
  every `-P` ms while still present), and `END` (dropped after `-T` consecutive missed windows).

### Output format

A two-line header is printed once at startup, followed by one line per `UPD`/`END` event
(`START` is emitted to stderr only, and only with `-v`):

```
# RTBW=20000000 CENTER=433000000 FFT=2048 AVG=20 THRESH=10.0dB HYST=3.0dB PCTILE=25 LOCAL=0 MINBINS=2 MAXGAP=0 REPORT_MS=5000 TRACK_WIN=5 CF_TOL_HZ=29296 CF_TOL_BW=0.50
# EVENT START_MS END_MS CF_HZ BW_HZ PEAK_DBFS DUR_MS UPDATES
UPD 1715640000500 1715640005500 433920135.7 24414.1 -45.2 5000 42
END 1715640000500 1715640012300 433920210.4 24414.1 -44.8 11800 98
```

Fields: event tag, START timestamp (UNIX ms), event timestamp (UNIX ms), centre frequency (Hz),
bandwidth (Hz), peak power (dBFS), duration since START (ms), and number of matched update windows.
All frequencies are in Hz; the minimum reported bandwidth is one FFT bin = `rate / fft_width` Hz.

## Build

From `examples/detector/c`:

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The build produces two binaries: `detector` (the SDR tool) and `test_detector` (a standalone unit
test of the detection core that needs no hardware).

**Dependencies:**

- `libfftw3-dev` (single-precision `fftw3f`) and `libpng-dev`:
  ```sh
  sudo apt install libfftw3-dev libpng-dev
  ```
- The installed **libfobos-sdr-agile** library (resolved via `pkg-config` as `libfobos_sdr`). Build and
  install it first — from the repo's `libfobos-sdr-agile/` submodule: `mkdir build && cd build && cmake .. && make && sudo make install && sudo ldconfig`.

`CMAKE_C_FLAGS_RELEASE` uses `-Ofast -march=<CC_MARCH> -mtune=<CC_MTUNE>` (both default to `native`);
override `-DCC_ARCH=`, `-DCC_MARCH=`, `-DCC_MTUNE=` for cross-compilation to another CPU.

## Run / Test

Requires a connected **Fobos SDR** (except for `test_detector`, which runs without hardware). Run the
binary from the `build` directory. Press Ctrl-C to stop.

```sh
# 1. Fixed RTBW: monitor 433 MHz, 20 MSPS, 2048-pt FFT, 20-FFT averaging,
#    10 dB threshold, with a live terminal spectrum
./detector -c 433000000 -r 20000000 -w 2048 -a 20 -t 10 -d

# 2. Scan 400-500 MHz with the hardware sweep, 1024-pt FFT, 50-FFT averaging
./detector -f 400000000 -F 500000000 -r 20000000 -w 1024 -a 50 -t 10

# 3. Scan and save a spectrogram PNG, stopping after 20 sweeps
./detector -f 400000000 -F 500000000 -r 20000000 -w 1024 -a 50 -t 10 -G scan.png -N 20
```

**What success looks like:** after the `initializing fobos...` line and the two `#` header lines, the
tool prints event lines as signals come and go — e.g. a transmitter on 433.92 MHz appearing yields a
`START` (stderr with `-v`), periodic `UPD` lines while it is on, and an `END` line when it stops:

```
# RTBW=20000000 CENTER=433000000 FFT=2048 AVG=20 THRESH=10.0dB HYST=3.0dB PCTILE=25 ...
# EVENT START_MS END_MS CF_HZ BW_HZ PEAK_DBFS DUR_MS UPDATES
UPD 1715640000500 1715640005500 433919980.2 24414.1 -46.1 5000 40
END 1715640000500 1715640009800 433920102.7 24414.1 -45.7 9300 74
```

With `-d`, a live spectrum is drawn in the terminal alongside the event output. A quiet band simply
produces no event lines beyond the header.

To exercise the detection algorithm without an SDR:

```sh
./test_detector
```

## Options

| Flag | Long | Default | Description |
|---|---|---|---|
| `-w <n>` | `--fft-width` | 1024 | FFT width (power of 2) |
| `-a <n>` | `--average` | 1000 | FFTs averaged per detection window |
| `-c <hz>` | `--freq` | 433000000 | Centre frequency (fixed-RTBW mode) |
| `-r <hz>` | `--rate` | 20000000 | Sample rate = RTBW |
| `-b <dBFS>` | `--baselevel` | -75 | FFT base (reference) level |
| `-t <dB>` | `--threshold` | 10.0 | Detection threshold above noise floor (>0) |
| `-H <dB>` | `--hysteresis` | 3.0 | Fall-threshold offset below rise (0 = off) |
| `-p <n>` | `--percentile` | 25 | Noise-floor percentile (1–99) |
| `-l <bins>` | `--local-floor` | 0 | Local noise-floor window size (0 = global) |
| `-n <n>` | `--min-bins` | 2 | Minimum signal width in FFT bins (≥1) |
| `-g <bins>` | `--max-gap` | 0 | Max gap to bridge when merging runs (0 = off) |
| `-P <ms>` | `--report-ms` | 5000 | Periodic UPD interval for active signals (0 = START+END only) |
| `-T <n>` | `--track-win` | 5 | Consecutive missed windows before END (≥1) |
| `-C <hz>` | `--cf-tol` | 3 bins | Absolute floor for CF match tolerance (default `3*rate/fft_width`) |
| `-B <frac>` | `--cf-tol-bw` | 0.50 | CF match tolerance as fraction of signal BW |
| `-d` | `--draw` | off | Draw live spectrum in terminal |
| `-o <file>` | `--output` | stdout | Write detections to file |
| `-L <n>` | `--lna-gain` | — | LNA gain index (0–3: 0,1 = 0 dB, 2 = +16 dB, 3 = +33 dB) |
| `-V <n>` | `--vga-gain` | — | VGA gain index (0–31: 0..+62 dB, 2 dB step) |
| `-m <dBFS>` | `--min-db` | auto | Display minimum dB |
| `-M <dBFS>` | `--max-db` | auto | Display maximum dB |
| `-v` | `--verbose` | off | Verbose info (incl. START events) to stderr |
| `-f <hz>` | `--freq-from` | — | Scan start frequency (enables scan mode) |
| `-F <hz>` | `--freq-to` | — | Scan end frequency (enables scan mode) |
| `-O <hz>` | `--overlap` | 0 | Step overlap in Hz (scan mode) |
| `-G <file>` | `--sgram` | — | Save spectrogram PNG on exit (scan mode) |
| `-N <n>` | `--n-frames` | 0 | Stop after N sweeps (0 = unlimited, scan mode) |
| `-S <mb>` | `--sgram-mb` | 256 | Spectrogram memory limit in MB (scan mode) |
| `-h` | `--help` | — | Print usage and exit |

## Files

| File | Description |
|---|---|
| `main.c` | CLI parsing, SDR setup, capture/FFT loop, fixed-RTBW + scan modes, signal tracking and event emission |
| `detector.c` | Detection core: percentile noise floor (global + local), hysteresis run detection, gap merge, centroid/BW/peak measurement |
| `detector.h` | Public detector API and the `detected_signal_t` result type |
| `test_detector.c` | Standalone unit test of the detection core (no hardware needed) |
| `CMakeLists.txt` | Build for the `detector` and `test_detector` targets; links `fftw3f`, `libfobos_sdr`, `libpng` |
| `fft.c` / `draw.c` | Shared FFTW3/window/dBFS helpers and terminal spectrum renderer (symlinked from `../../shared/`) |
| `fobos.c` / `receiver.c` / `ringbufer.c` / `sgram.c` | SDR wrapper, ring-buffer capture thread, and spectrogram PNG writer (symlinked from `../../fobos/`) |
