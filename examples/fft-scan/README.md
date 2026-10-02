# fft-scan — Wideband Panoramic Scanner

Steps the Fobos SDR centre frequency across an arbitrary range and stitches the
per-step FFTs into a single, continuous panoramic spectrum shown live in the
terminal (with optional spectrogram and max-hold PNG export).

## Overview

`fft-scan` sweeps from a start frequency (`-f`) to an end frequency (`-t`) using
the Fobos **hardware scan API** (`fobos_sdr_start_scan`). The hardware itself
manages the frequency stepping: it retunes to each step, discards the IQ
captured while the synthesizer settles, and delivers only settled samples to a
ring buffer, each chunk tagged with the centre frequency it was captured at.
The consumer side reads those chunks, transforms each into a slice of spectrum,
and pastes the slices side by side into one wide panorama that spans the whole
requested range.

Scan geometry is derived from the sample rate: each step covers
`sample_rate × FOBOS_AUTO_BW` (≈80% of the rate) of usable bandwidth, and the
number of steps (hops) is `ceil(span / (bw_per_step − overlap))`, with a minimum
of 2. Only the flat centre bins of each step's FFT are kept for display; with
`-O` overlap the roll-off edges are additionally captured and averaged with the
neighbouring step for seamless boundary blending.

> **⚠ Hardware limit — 256 scan hops.** The Fobos scan API supports at most
> **`FOBOS_MAX_FREQS_CNT` = 256** frequency hops per scan (`fobos_sdr_start_scan`
> rejects more, returning `-8`/`FOBOS_ERR_UNSUPPORTED`). If the requested range
> would need more than 256 hops, `fft-scan` prints an error and exits — it does
> **not** silently reduce coverage. The widest span coverable in one scan is
> therefore about:
>
> ```
> max_span ≈ 256 × (sample_rate × 0.8 − overlap)
> ```
>
> e.g. ≈ 5.1 GHz at 25 MSPS, or ≈ 2.0 GHz at 10 MSPS (no overlap). To scan a
> wider range, **raise the sample rate** (wider hops), **narrow the range**, or
> **reduce `-O` overlap**.

**Per-sweep processing pipeline:**

```
Hardware scan loop (automatic, one settled chunk per step):
  freq[0] → [settle] → IQ chunk[0]  (tagged freq[0])
  freq[1] → [settle] → IQ chunk[1]  (tagged freq[1])
  …

Consumer (one sweep = n_steps chunks):
  for each chunk from the ring buffer:
    step = nearest_step(chunk frequency tag)
    repeat `average` times:
      apply Hamming window → FFTW3 (fftwf) → accumulate power
    convert to dB (fft_log) + DC-centred crop → panorama[step * bins_per_step]
    (blend overlap bins with the previous step, if -O)
  draw_fft(full panorama)         # live terminal render
  sgram_add_row(...)              # optional spectrogram row (-G)
  update max-hold(...)            # optional peak-hold overlay (-x)
```

The first 3 sweeps are discarded as warm-up; the display dB scale is then
auto-computed from the first clean sweep (unless `-m`/`-M` are given) and frozen.

## Build

Build with CMake from `examples/fft-scan/c`:

```sh
cd examples/fft-scan/c
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

The binary is produced at `examples/fft-scan/c/build/fft-scan`.

**Dependencies:**

- `libfftw3-dev` — provides the single-precision FFTW library (`fftw3f`)
- `libpng-dev` — PNG export for spectrogram / max-hold snapshots
- `libfobos-sdr-agile` — the Fobos SDR driver, installed so `pkg-config` finds
  `libfobos_sdr` (the CMake build resolves all three via `pkg-config`)

On Debian/Raspberry Pi OS:

```sh
sudo apt install libfftw3-dev libpng-dev cmake build-essential
```

## Run / Test

Requires a **Fobos SDR connected over USB**. Run the built binary and watch the
terminal: after a few warm-up sweeps a stitched panoramic spectrum is drawn,
refreshing once per sweep, with a status line showing the range, step count,
scan speed (MHz/s), and sweep time. Press `Ctrl-C` to stop; if a PNG was
requested it is written on exit.

```sh
# 1) FM broadcast band, 88–108 MHz, 25 MSPS, 2048-pt FFT, 50 averages.
#    Expect a live panorama with sharp peaks at local FM station frequencies.
./fft-scan -f 88000000 -t 108000000 -r 25000000 -w 2048 -a 50

# 2) 2.4 GHz ISM band with a spectrogram PNG saved after 100 sweeps.
#    Produces 2g4.png (waterfall, newest row at top) plus the live display.
./fft-scan -f 2400000000 -t 2500000000 -r 20000000 -w 2048 -a 20 -G 2g4.png -n 100

# 3) 400–500 MHz max-hold capture, 4096-pt FFT, export peak-hold PNG.
#    Runs 10 sweeps, then writes maxhold.png (and shows the yellow max-hold ticks live).
./fft-scan -f 400000000 -t 500000000 -r 20000000 -w 4096 -a 100 -xg -n 10
```

**What success looks like:** the terminal shows a full-width spectrum across the
requested range (not just a single tuned slice), the `SCAN … | speed:… MHz/s`
status line updates each sweep, and any requested `-G`/`-xg` PNG is written on
exit ("saving spectrogram to …" / "done."). Signals appear at their true
absolute frequencies because each step is routed to its panorama slot by its
hardware frequency tag.

## Options

| Flag | Default | Description |
|---|---|---|
| `-f <Hz>` | 88 000 000 | Scan start (from) frequency |
| `-t <Hz>` | 108 000 000 | Scan end (to) frequency |
| `-r <Hz>` | 25 000 000 | Sample rate (≈ usable bandwidth per step) |
| `-w <N>` | 2048 | FFT width, must be a power of 2 |
| `-a <N>` | 50 | FFTs averaged per step (> 0) |
| `-b <dB>` | -75 | Noise-floor / base reference level (auto-corrected by `−10·log10(average)`) |
| `-s <N>` | = `-w` | FFT hop size in samples (< `-w` gives windowed overlap) |
| `-O <Hz>` | 0 | Step overlap in Hz; keeps step boundaries inside each filter's flat passband (must be `< RTBW`). More overlap → more steps → slower sweep |
| `-L <0-3>` | driver default | LNA gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB) |
| `-V <0-31>` | driver default | VGA gain (0..+62 dB, 2 dB step) |
| `-m <dB>` | auto | Display minimum dB (disables auto-min) |
| `-M <dB>` | auto | Display maximum dB (disables auto-max) |
| `-G <file>` | — | Write spectrogram PNG to `<file>` on exit (newest row at top) |
| `-n <N>` | unlimited | Exit after N productive sweeps and save the spectrogram (> 0) |
| `-S <MB>` | 256 | Spectrogram pixel-buffer memory limit in MB (> 0) |
| `-x[c][g]` | off | Enable max-hold display (yellow tick per bin; press `c` to clear). Append `c` to also save `maxhold.csv`, `g` to also save `maxhold.png` on exit |
| `-v` | off | Verbose output |
| `-h` | — | Print help and exit |

## Files

Source lives in `c/`; the DSP/driver/render modules are symlinked in from the
shared example libraries.

| File | Role |
|---|---|
| `c/main.c` | Scanner entry point: arg parsing, scan-geometry computation, main sweep loop, panorama stitching, overlap blending, display/PNG output |
| `c/CMakeLists.txt` | CMake build (resolves `fftw3f`, `libfobos_sdr`, `libpng` via pkg-config; `-Ofast` release flags) |
| `c/fft.c` / `c/fft.h` | FFTW3 (single-precision) plan management, Hamming window, power accumulation, dB conversion, FFTW wisdom load/save |
| `c/receiver.c` / `c/receiver.h` | Wrapper over the Fobos hardware scan API + ring buffer (`receiver_start_scan`, `receiver_read_scan_chunk`, gain/rate control) |
| `c/fobos.c` / `c/fobos.h` | Fobos SDR device access layer used by the receiver |
| `c/ringbufer.c` / `c/ringbuffer.h` | Lock-free ring buffer holding settled, frequency-tagged IQ chunks |
| `c/draw.c` / `c/draw.h` | Terminal spectrum renderer, max-hold overlay, keypress handling |
| `c/sgram.c` / `c/sgram.h` | Spectrogram (waterfall) row accumulator and PNG writer (`-G`) |
| `c/spectrum_png.c` / `c/spectrum_png.h` | Single-spectrum PNG/CSV export for max-hold snapshots (`-xg`, `-xc`) |
