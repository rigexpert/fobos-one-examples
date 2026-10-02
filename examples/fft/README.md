# `fft` — Real-time Spectrum Analyser

Captures IQ data from a Fobos SDR and displays a live power spectrum in the
terminal, with optional raw-bin, spectrogram-PNG, and max-hold output. A
compact starting point for building spectrum-monitoring, signal-identification,
or wideband-scanning tools on top of the Fobos SDR platform.

## Overview

A dedicated real-time thread (`SCHED_FIFO`, pinned to CPU core 3) streams IQ
samples from the hardware into a lock-free ring buffer. The main loop pulls one
block per iteration, windows it, transforms it, and accumulates power over
`-a` averaged FFTs before converting to dBFS and rendering. Averaging N FFTs
reduces noise variance by √N.

The spectrum is drawn with ANSI escape sequences and Unicode eighth-block
characters (`▁▂▃▄▅▆▇█`) for 8× sub-character vertical resolution. Drawing is
capped at 30 fps and the first 3 frames are discarded to avoid start-up
artefacts. Display min/max dB are auto-derived from the first good frame unless
pinned with `-m` / `-M`.

Processing pipeline:

```
[Fobos SDR]  IQ float32 @ sample_rate
     │  real-time capture thread → lock-free ring buffer (receiver.c)
     ▼
receiver_read()                     one block = fft_width complex samples
     │                              (sliding window when -s < fft_width)
     ▼
fft_apply_precalculated_window()    Hamming window (suppresses leakage)
     ▼
fft_execute()                       FFTW3 single-precision forward DFT
     ▼
fft_accumulate_power()              sum I²+Q² per bin over `average` FFTs
     ▼
fft_log()                           → dBFS + bin-swap (DC to centre)
     ▼
draw_fft()  (terminal, -d)   /   fwrite() raw f32 bins (-o)
            sgram_add_row()  (spectrogram PNG, -G)
```

## Build

Run from `examples/fft/c`:

```sh
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make
```

Dependencies:

- `libfftw3-dev` (single-precision `fftw3f`) and `libpng-dev`:
  `sudo apt install libfftw3-dev libpng-dev`
- the installed **libfobos-sdr-agile** library (provides the `libfobos_sdr`
  pkg-config module, header, and udev rule — build and `sudo make install` it
  from the repo's `libfobos-sdr-agile/` submodule first).

CMake locates all three via `pkg-config` (`fftw3f`, `libfobos_sdr`, `libpng`).
The binary is written to the `build` directory as `fft`.

Optional CPU tuning (Release only): `-DCC_ARCH=native` (default), or separate
`-DCC_MARCH=…` / `-DCC_MTUNE=…` for cross-builds.

## Run / Test

Hardware required: a **Fobos SDR connected over USB** (the udev rule from
`sudo make install` grants non-root access). Run the built binary from the
`build` directory; press `Ctrl-C` to stop.

```sh
# Live spectrum around 100 MHz, 20 MSPS, 1024-point FFT, 10-FFT averaging
./fft -c 100000000 -r 20000000 -w 1024 -a 10 -d
```

Success looks like a continuously updating bar spectrum filling the terminal,
with a dB y-axis on the left and a frequency x-axis (centred on `-c`) along the
bottom. The program first prints its config (`effective iteration rate…`,
`set frequency…`, `set sample rate…`, `start processing samples…`) and then
switches to the live display.

```sh
# Save raw float32 FFT bins (DC-centred) to a file while drawing
./fft -c 433000000 -r 8000000 -w 2048 -a 20 -d -o spectrum.f32
```

Success: a live display plus a growing `spectrum.f32` — one `fft_width`-long
row of `float32` dBFS values per averaged frame, ready to load in
NumPy/Octave/MATLAB.

```sh
# Run 200 averaged frames, then exit and save a spectrogram PNG
./fft -c 100000000 -r 20000000 -w 2048 -a 50 -d -G sgram.png -N 200
```

Success: the program stops on its own after 200 frames, prints
`saving spectrogram to sgram.png …`, and leaves a time-frequency heat-map PNG
on disk.

## Options

| Flag (short / long) | Default | Description |
|---|---|---|
| `-w`, `--fft_width <N>` | 1024 | FFT width; must be a power of 2 |
| `-c`, `--central-freq <Hz>` | 100 000 000 | Centre frequency |
| `-r`, `--sample-rate <Hz>` | 20 000 000 | Sample rate (= bandwidth) |
| `-a`, `--average <N>` | 1000 | FFTs accumulated per displayed/output frame |
| `-b`, `--baselevel <dB>` | -75 | dBFS reference level (average-compensated internally) |
| `-s`, `--overlap <hop>` | 0 (= `fft_width`) | Hop size in samples; `< fft_width` = sliding-window overlap (higher CPU), `> fft_width` = stride/skip |
| `-L`, `--lna-gain <0-3>` | hardware default | LNA gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB) |
| `-V`, `--vga-gain <0-31>` | hardware default | VGA gain (0..+62 dB, 2 dB step) |
| `-d`, `--draw-fft` | off | Draw the spectrum in the terminal |
| `-m`, `--min-db <dB>` | auto | Display minimum dB (auto-derived if unset) |
| `-M`, `--max-db <dB>` | auto | Display maximum dB (auto-derived if unset) |
| `-o`, `--output <file>` | stdout | Write raw float32 FFT bins to file (binary) |
| `-n`, `--frames <N>` | unlimited | Exit after N averaged frames |
| `-G`, `--sgram <file>` | — | Save spectrogram PNG to `<file>` on exit |
| `-N`, `--sgram-frames <N>` | — | Exit after N averaged frames (pairs with `-G`) |
| `-S`, `--sgram-mem <MB>` | 256 | Spectrogram buffer memory limit |
| `-x`, `--max-hold[c][g]` | off | Max-hold overlay; `c` = also save `maxhold.csv`, `g` = also save `maxhold.png` on exit (e.g. `-xcg`); press `c` while running to clear |
| `-v`, `--verbose` | off | Print config and periodic per-phase timing stats |
| `-h`, `--help` | — | Print usage and exit |

Notes: `-n` and `-N` set the same frame limit. A very small `-s` raises the
iteration rate; the program warns above 100 000 iter/s (ring-buffer overflow
risk). Interrupt handling flushes any partial averaged frame before exit.

## Files

Most files under `c/` are symlinks into the shared example libraries; only
`main.c` and the build files are local to this example.

| File | Description |
|---|---|
| `main.c` | This example: arg parsing, buffer setup, the capture→FFT→accumulate→draw loop, and file/PNG/max-hold output |
| `CMakeLists.txt` | CMake build (finds `fftw3f`, `libfobos_sdr`, `libpng` via pkg-config; installs `fft` to `bin`) |
| `Makefile` | Plain-Makefile alternative to the CMake build |
| `fft.c` / `fft.h` | FFTW3 plan/wisdom management, Hamming window, power accumulation, dBFS conversion (`../../shared/`) |
| `draw.c` / `draw.h` | Terminal spectrum renderer: ANSI + Unicode block chars, axes, keypress polling, max-hold overlay (`../../shared/`) |
| `spectrum_png.c` / `spectrum_png.h` | 1-D spectrum snapshot to PNG or CSV (`../../shared/`) |
| `fobos.c` / `fobos.h` | Fobos SDR wrapper: open, configure, start/stop (`../../fobos/`) |
| `receiver.c` / `receiver.h` | Real-time IQ capture thread (`SCHED_FIFO`, core 3) feeding the ring buffer (`../../fobos/`) |
| `ringbufer.c` / `ringbuffer.h` | Lock-free single-producer/single-consumer ring buffer (`../../fobos/`) |
| `sgram.c` / `sgram.h` | Spectrogram pixel buffer + libpng writer (`../../fobos/`) |
