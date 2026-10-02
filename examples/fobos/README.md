# fobos — SDR capture layer (reference modules)

The **single source** for the C modules that handle talking to the FobosOne and moving IQ from
the USB device to the DSP thread. **Not a standalone program.** Each example **symlinks** the
modules it needs from here into its own `c/` directory (and lists them in its `CMakeLists.txt`),
so each module lives in exactly one place and edits propagate to every example — there is nothing
to build here.

## Modules

### `fobos.c` / `fobos.h` — hardware abstraction
Device enumeration, configuration (frequency, sample rate, LNA/VGA gain, bandwidth), and IQ
acquisition over `libfobos-sdr-agile`. Exposes two capture modes:
- **synchronous** — blocking reads, simple to drive from one thread;
- **asynchronous / scan** — callback-driven streaming and the hardware `fobos_sdr_start_scan`
  panoramic sweep (used by `fft-scan` and `detector`'s scan mode).

### `receiver.c` / `receiver.h` — real-time capture thread
Wraps `fobos.h` behind a simple **pull** interface. A dedicated capture thread runs at
`SCHED_FIFO` priority pinned to a fixed CPU core and pushes IQ into the ring buffer; the DSP
thread pulls settled chunks. Provides **back-pressure** (the producer overwrites the oldest chunk
if the consumer falls behind) and, in scan mode, tags each chunk with its centre frequency.
> The pinned core is set by `RECEIVER_CPU_CORE` in `receiver.c` — lower it on boards with fewer
> cores.

### `ringbufer.c` / `ringbuffer.h` — lock-free ring buffer
A **single-producer / single-consumer, lock-free** multi-chunk ring buffer sized so the SDR
capture thread and the DSP thread can run on separate cores without mutexes. This is the hand-off
point between `receiver.c` (producer) and the example's processing loop (consumer).

### `sgram.c` / `sgram.h` — rolling spectrogram → PNG
Accumulates FFT sweeps into a rolling image and writes an annotated PNG (thermal colourmap:
black → blue → cyan → green → yellow → red) with frequency/time axes. Backs the `-G` spectrogram
option in the FFT examples. Requires `libpng`.

## Which examples symlink these modules

Each example symlinks in the modules it needs:

| Module | fft | fft-scan | detector | demod_fm | demod_fsk | demod_psk |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| `fobos` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `receiver` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `ringbuffer` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `sgram` | ✓ | ✓ | ✓ | | | |

Links: [fft](../fft/), [fft-scan](../fft-scan/), [detector](../detector/),
[demod_fm](../demod_fm/), [demod_fsk](../abstract_demod/demod_fsk/),
[demod_psk](../abstract_demod/demod_psk/).

## See also

[`../shared/`](../shared/) — the DSP/display reference modules (`fft`, `draw`, `dsp`,
`spectrum_png`).
