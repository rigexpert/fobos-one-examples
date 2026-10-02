# demod_fm — FM Broadcast Demodulator

Receives a real FM broadcast station with the Fobos SDR and outputs decoded mono audio.
It is a worked example of a full SDR→DSP→audio chain built on `libfobos-sdr-agile` and
[liquid-dsp](https://github.com/jgaeddert/liquid-dsp), and is meant as a starting point for
your own receivers.

## Overview

The entire signal path runs in one real-time thread. From antenna to audio:

```
[Fobos SDR]  IQ float32 @ input rate (e.g. 8 MSPS)
     │
     ▼
[DDC — shift_unroll_process()]        ← optional, enabled with -O
     │  translates an off-centre station down to DC
     ▼
[IQ decimation]                        firdecim_crcf (integer) + msresamp_crcf (fractional)
     │  down to the IQ channel rate (default 200 kHz — the full BCFM channel)
     ▼
[FM discriminator — freqdem]           recovers instantaneous frequency deviation as float
     │  MPX signal: mono 0–15 kHz, 19 kHz pilot, stereo 23–53 kHz, RDS 57 kHz
     ▼
[Hard clip at ±1.0]                    stops noise spikes ringing through the audio LPF
     │
     ▼
[Audio LPF — firfilt_rrrf]             ← audio stage, enabled with -a
     │  Kaiser FIR, 15 kHz cutoff — rejects pilot / stereo / RDS
     ▼
[Audio resample — msresamp_rrrf]       IQ channel rate → audio rate (e.g. 48 kHz)
     │
     ▼
[De-emphasis — 1st-order IIR]          a = exp(−1/(τ·fs)), default τ = 50 µs
     │
     ▼
[Output gain] → [f32 or s16 write]     to stdout or a file
```

Notes on the DSP:

- **Two-stage decimation.** For non-integer rate ratios the code uses a fast integer
  polyphase FIR decimator to do the bulk of the work, then a small fractional `msresamp`
  stage to correct the remainder. Exact integer ratios use a single decimator; ratios at or
  below the output rate use pure `msresamp`. Selection is automatic.
- **FM `kf`.** Default `kf = 2.8 × 75 kHz / IQ_rate`, giving 2.8× headroom so nominal audio
  sits near ±0.36 and the hard clip only touches noise. Override with `-k`.
- **De-emphasis.** 50 µs (Europe) by default; use `-D 75` for the Americas, `-D 0` to disable.
- Without `-a`, the tool emits the raw MPX (multiplex) baseband at the IQ channel rate
  instead of decoded audio — useful for offline stereo/RDS experiments.

## Build

The example depends on **liquid-dsp** (built from source) and the installed
**libfobos-sdr-agile** SDR library (pkg-config module `libfobos_sdr`).

### 1. Install liquid-dsp from source

```sh
git clone https://github.com/jgaeddert/liquid-dsp
cd liquid-dsp
./bootstrap.sh
./configure
make
sudo make install
sudo ldconfig
```

### 2. Build demod_fm

The shared sources (`dsp.c`, `fobos.c`, `receiver.c`, `ringbufer.c`) and CMake helper are
already symlinked into `examples/demod_fm/c/`. From that directory, the simplest build is the
plain Makefile:

```sh
cd examples/demod_fm/c
make                     # produces ./demod_fm
```

Or with CMake (uses `FindLiquid.cmake` + pkg-config to locate liquid-dsp and libfobos_sdr):

```sh
cd examples/demod_fm/c
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

On a Raspberry Pi 5 you can tune codegen for the Cortex-A76:

```sh
cmake .. -DCMAKE_BUILD_TYPE=Release -DCC_MARCH=armv8.2-a -DCC_MTUNE=cortex-a76
make -j$(nproc)
```

## Run / Test

Hardware: a Fobos SDR with an antenna, tuned to a strong **local FM broadcast** station.
The FM band is 87.5–108 MHz. Pick a station you can normally hear on a car radio.

### 1. Capture raw MPX to a file (offline analysis)

```sh
./demod_fm -s 8000000 -F 102500000 -o mpx.f32
```

Captures 102.5 MHz and writes float32 MPX at 200 kHz. Add `-d 5` to stop after 5 seconds and
`-v` to print pipeline details to stderr.

### 2. Listen to a station live through the speakers (the real test)

Pipe decoded 48 kHz s16 mono audio into `sox` (play) or `aplay`:

```sh
# sox
./demod_fm -s 8000000 -F 96000000 -a 48000 -e s16 | \
    sox -t raw -r 48000 -e signed -b 16 -c 1 - -d

# or ALSA aplay
./demod_fm -s 8000000 -F 96000000 -a 48000 -e s16 | \
    aplay -f S16_LE -r 48000 -c 1
```

**Success sounds like** clear music or speech with no carrier hiss — a recognisable radio
station. If a talk station, you hear intelligible voice; if music, it is clean and centred.
Heavy static/hiss means the station is weak or slightly off-tune — try a stronger station,
improve the antenna, or nudge the frequency. Add `-g 2` to raise the volume if it is quiet.

### 3. Tune a station offset from the SDR centre (DDC)

Keep the target away from the DC spike by centring the SDR nearby and shifting with `-O`.
`-O` is where the signal sits relative to centre, so `-200000` means "signal is 200 kHz below
centre":

```sh
./demod_fm -s 8000000 -F 96200000 -O -200000 -a 48000 -e s16 -o audio.raw
```

Play the saved file afterwards with `sox -t raw -r 48000 -e signed -b 16 -c 1 audio.raw -d`.

## Options

| Flag | Long | Default | Description |
|---|---|---|---|
| `-s <Hz>` | `--samplerate` | — (required) | SDR hardware IQ sample rate |
| `-F <Hz>` | `--frequency` | — (required) | SDR centre / tuning frequency |
| `-r <Hz>` | `--resamplerate` | 200000 | IQ channel rate after decimation (full BCFM channel) |
| `-a <Hz>` | `--audiorate` | 0 (off) | Audio output rate; enables the LPF + resample + de-emphasis stage |
| `-O <Hz>` | `--offset` | 0 | DDC offset — where the signal sits relative to centre |
| `-b <Hz>` | `--bandwidth` | IQ_rate/2 | FM channel bandwidth hint |
| `-k <factor>` | `--kf` | 2.8·75000/IQ_rate | FM discriminator modulation factor |
| `-e <f32\|s16>` | `--output_format` | f32 | Output sample encoding |
| `-g <factor>` | `--gain` | 1.0 | Output gain after LPF + de-emphasis |
| `-D <µs>` | `--deemphasis` | 50 | De-emphasis time constant (50 EU / 75 Americas / 0 off) |
| `-d <sec>` | `--duration` | 0 (forever) | Recording duration in seconds |
| `-L <idx>` | `--lna-gain` | HW default | LNA gain index (0–3) |
| `-V <idx>` | `--vga-gain` | HW default | VGA gain index (0–31) |
| `-m <sync\|async>` | `--mode` | sync | Capture mode |
| `-o <file>` | `--output` | stdout | Output file path |
| `-v` | `--verbose` | off | Print runtime info to stderr |
| `-h` | `--help` | — | Print usage and exit |

## Files

- `c/main.c` — the demodulator: argument parsing, DSP pipeline setup (`dsp_init`), and the real-time capture/demod/write loop.
- `c/CMakeLists.txt` — CMake build; finds liquid-dsp (`FindLiquid.cmake`) and libfobos_sdr (pkg-config).
- `c/Makefile` — plain Makefile alternative (`-lliquid -lfobos_sdr -lm -lpthread`).
- `c/dsp.c` / `c/dsp.h` — shared DSP helpers (DDC `shift_unroll_*`, format conversion). Symlinked from `examples/shared/`.
- `c/receiver.c` / `c/receiver.h` — Fobos capture wrapper (init/start/read/stop, sync + async). Symlinked from `examples/fobos/`.
- `c/fobos.c`, `c/ringbufer.c` — Fobos device access and the lock-free ring buffer feeding the DSP thread.
- `c/FindLiquid.cmake` — CMake module locating the liquid-dsp headers and library.
