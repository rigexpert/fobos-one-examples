# shared — DSP / display modules (single source)

The **single source** for the DSP and display C modules used by the examples. **Not a standalone
program.** Each example **symlinks** the modules it needs from here into its own `c/` directory
(and lists them in its `CMakeLists.txt`), so each module lives in exactly one place and edits
propagate to every example — there is nothing to build here.

## Modules

### `fft.c` / `fft.h` — spectral engine
FFTW3 (single-precision) plan management with wisdom caching, a **Hamming window**, accumulation
of `|FFT|²` power across the N averaged transforms, conversion to **dBFS**, and a half-swap so DC
lands in the centre of the display array. This is the core that turns raw IQ into the power
spectrum the FFT examples draw.

### `draw.c` / `draw.h` — terminal spectrum display
Real-time renderer that maps the dBFS spectrum to column heights and draws it with **ANSI escape
sequences + Unicode eighth-block glyphs (▁▂▃▄▅▆▇█)** for 8× sub-character vertical resolution.
Handles the frequency/level axis labels and auto- or fixed-scale (`-m`/`-M`) rendering.

### `dsp.c` / `dsp.h` — IQ conversion + Digital Down Converter (DDC)
Two things, not just conversion:
1. **Format conversion** — `convert_s16_f()` turns interleaved signed-16-bit IQ (as produced by
   many SDR drivers) into normalised ±1.0 float32 for liquid-dsp.
2. **DDC / frequency shift** — `shift_unroll_init()` + `shift_unroll_process()` mix the IQ by a
   normalised frequency to bring an **off-centre signal down to baseband (DC)**. The local
   oscillator is a precomputed ("unrolled") sin/cos table for a fixed block size; each block is
   multiplied by that LO while the **phase is carried continuously across block boundaries**, so
   there are no phase discontinuities. The inner loop is **NEON-vectorised** on ARMv8. This backs
   the `-O` tuning-offset in the `demod_*` examples.

### `spectrum_png.c` / `spectrum_png.h` — spectrum snapshot → PNG / CSV
Writes one 1-D spectrum as a PNG image (or a CSV of the bins). When the spectrum is wider than the
image (> 4096 px) it **peak-compresses** groups of bins so no narrow peak is dropped. Requires
`libpng`.

### `FindLiquid.cmake` — CMake finder
Locates the [liquid-dsp](https://github.com/jgaeddert/liquid-dsp) library + headers for the
`demod_*` examples.

## Which examples symlink these modules

| Module | fft | fft-scan | detector | demod_fm | demod_fsk | demod_psk |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| `fft` | ✓ | ✓ | ✓ | | | |
| `draw` | ✓ | ✓ | ✓ | | | |
| `spectrum_png` | ✓ | ✓ | ✓ | | | |
| `dsp` | | | | ✓ | ✓ | ✓ |
| `FindLiquid.cmake` | | | | ✓ | ✓ | ✓ |

Links: [fft](../fft/), [fft-scan](../fft-scan/), [detector](../detector/),
[demod_fm](../demod_fm/), [demod_fsk](../abstract_demod/demod_fsk/),
[demod_psk](../abstract_demod/demod_psk/).

## See also

[`../fobos/`](../fobos/) — the SDR capture layer (`fobos`, `receiver`, `ringbuffer`, `sgram`).
