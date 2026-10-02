# FobosOne SDR — Applications, Examples & Tools

Software for the [RigExpert Fobos / FobosOne SDR](https://rigexpert.com/fobos/) receiver, built on
top of the [libfobos-sdr-agile](libfobos-sdr-agile/) library. This repository contains a
browser-based spectrum-analyzer application, a set of C developer examples (each a complete,
production-quality SDR pipeline — USB IQ streaming → real-time display, detection, demodulation),
and standalone tools.

## Repository layout

| Path | What it is |
|---|---|
| **[web_spectrum/](web_spectrum/)** | Real-time web spectrum analyzer + waterfall with absolute-power (dBm) calibration; native **C++** and **Python** backends. See [web_spectrum/README.md](web_spectrum/README.md). |
| **[examples/](examples/)** | C developer examples (see the table below); each has its own README. |
| **[tools/](tools/)** | Standalone utilities — [tools/cal_sweep](tools/cal_sweep/) (receive-calibration sweep). |
| [libfobos-sdr-agile/](libfobos-sdr-agile/) | The hardware library (git submodule). |

---

## Hardware

| Revision | Support |
|---|---|
| hw.rev 2.x | requires special firmware |
| hw.rev 3.x | requires special firmware |
| hw.rev 4.x and newer | native (no firmware change needed) |

Frequency coverage depends on hardware revision; typical range is **1 MHz – 6 GHz**.

---

## Examples at a Glance

Each example has its own README with build + test instructions; the detailed reference sections
further down remain here for convenience.

| Example | Binary | What it does |
|---|---|---|
| [fft](examples/fft/) | `fft` | Live FFT spectrum in terminal |
| [fft-scan](examples/fft-scan/) | `fft-scan` | Wideband panoramic FFT scan |
| [detector](examples/detector/) | `detector` | Automatic signal detection |
| [demod_fm](examples/demod_fm/) | `demod_fm` | FM broadcast demodulation |
| [abstract_demod](examples/abstract_demod/) | `demod_fsk`, `demod_psk` | Baseline M-FSK / M-PSK demodulators (clean-signal reference — see the group README for real-signal limits) |
| [aaronia_OTA](examples/aaronia_OTA/) | — | FSK/PSK over-the-air modem loopback test harness (Aaronia TX + Fobos RX) |
| [shared](examples/shared/), [fobos](examples/fobos/) | — | Shared DSP / capture modules used by the above |

---

## Prerequisites

### Common

```sh
sudo apt install build-essential cmake pkg-config libusb-1.0-0-dev
```

### libfobos-sdr-agile (hardware library — git submodule)

```sh
git submodule update --init --recursive
cd libfobos-sdr-agile
mkdir build && cd build
cmake ..
make
sudo make install   # installs library, header, pkg-config, and udev rule
sudo ldconfig
sudo udevadm control --reload-rules && sudo udevadm trigger
```

> The CMake install script places `fobos-sdr.rules` into `/etc/udev/rules.d/` automatically —
> no manual copy needed.  The `udevadm` reload makes the rule take effect without a reboot.

### FFT examples (`fft`, `fft-scan`, `detector`)

```sh
sudo apt install libfftw3-dev libpng-dev
```

### Demodulation examples (`demod_fm`, `demod_fsk`, `demod_psk`)

These require [liquid-dsp](https://github.com/jgaeddert/liquid-dsp):

```sh
git clone https://github.com/jgaeddert/liquid-dsp
cd liquid-dsp && ./bootstrap.sh && ./configure && make && sudo make install && sudo ldconfig
```

---

## Building (native)

Each example has its own `CMakeLists.txt`.  The general pattern is:

```sh
cd examples/<name>/c
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

The optional `CC_ARCH` variable is passed to `-march` / `-mtune` in Release mode:

```sh
cmake .. -DCMAKE_BUILD_TYPE=Release -DCC_ARCH=native        # optimise for the current CPU (default)
cmake .. -DCMAKE_BUILD_TYPE=Release -DCC_ARCH=cortex-a76    # optimise for RPi 5 (when building on RPi 5)
```

---

## Cross-Compilation (x86-64 host → RPi 5 / aarch64)

Cross-compilation lets you build binaries for the Raspberry Pi on a faster x86-64 workstation.
The process has three one-time preparation steps — toolchain, rootfs, CMake toolchain file — and
then a single cmake invocation for every project.

### Step 1 — Install the cross-compiler

```sh
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu binutils-aarch64-linux-gnu
```

### Step 2 — Prepare the target rootfs

The rootfs provides the target libraries and headers that CMake and pkg-config resolve against.
The easiest source is an actual Raspberry Pi with all dependencies already installed.

**On the RPi 5** — install every library the examples need:

```sh
sudo apt install libusb-1.0-0-dev libfftw3-dev libpng-dev
# if building demod examples:
# build and install liquid-dsp from source (see Prerequisites above)
```

**On the x86-64 host** — mirror the RPi filesystem:

```sh
export ROOTFS=/opt/rpi5-rootfs
mkdir -p $ROOTFS

# sync the library and header trees (adjust pi@rpi5.local to your device)
rsync -avz --rsync-path="sudo rsync" \
    pi@rpi5.local:/{lib,usr/lib,usr/include,usr/share/pkgconfig} \
    $ROOTFS/usr/

# fix absolute symlinks so they resolve inside the rootfs
# (the find command rewrites /usr/lib/... links to relative paths)
find $ROOTFS -type l | while read link; do
    target=$(readlink "$link")
    if [[ "$target" == /* ]]; then
        ln -snf "$ROOTFS$target" "$link"
    fi
done
```

**Alternative A — Docker (no physical device needed)**

Requires QEMU binfmt support so the package manager can run arm64 binaries on the host:

```sh
# One-time QEMU setup
sudo apt install qemu-user-static binfmt-support
docker run --rm --privileged multiarch/qemu-user-static --reset -p yes

# Build a minimal arm64 sysroot image
cat > Dockerfile.rpi5-sysroot << 'EOF'
FROM arm64v8/debian:bookworm
RUN apt-get update && apt-get install -y \
        libusb-1.0-0-dev \
        libfftw3-dev     \
        libpng-dev       \
    && rm -rf /var/lib/apt/lists/*
EOF

docker build --platform linux/arm64 \
    -f Dockerfile.rpi5-sysroot \
    -t rpi5-sysroot .

# Extract the filesystem tree into the rootfs directory
export ROOTFS=/opt/rpi5-rootfs
mkdir -p $ROOTFS
CID=$(docker create --platform linux/arm64 rpi5-sysroot)
docker export $CID | tar -C $ROOTFS -xf -
docker rm $CID
```

Add the `liquid-dsp` build step to the Dockerfile before the `apt-get clean` line if you need
the demodulation examples.

**Alternative B — Buildroot (full toolchain + sysroot from source, no device, no Docker)**

Buildroot compiles both the cross-toolchain *and* every target library from source, giving a
hermetic, version-pinned environment.  First-run compilation takes 1–2 hours; incremental
rebuilds are fast thanks to stamp files — interrupt and resume with the same `make` command.

> **Tested with:** Buildroot 2025.02.x on Ubuntu 22.04 host.  
> `raspberrypi5_defconfig` first appeared in 2025.02; earlier releases only ship
> `raspberrypi4_64_defconfig`.

**Known host issue — m4 1.4.19 on glibc ≥ 2.35 (Ubuntu 22.04+)**

Buildroot 2025.02 uses m4 1.4.19 as a host build tool.  It fails to compile on Ubuntu 22.04 /
glibc 2.35 due to an incomplete `stack_t` definition.  Apply a one-line patch before starting:

```sh
# Clone Buildroot to /opt and set up an out-of-tree build directory
sudo git clone https://git.buildroot.net/buildroot --depth 1 --branch 2025.02 /opt/buildroot
sudo chown -R $USER /opt/buildroot    # so make can write stamp files
mkdir -p /opt/buildroot-rpi5          # out-of-tree build output

# Patch m4 for glibc 2.35 (adds missing sys/signal.h include)
cat > /opt/buildroot/package/m4/0001-fix-c-stack-ucontext-glibc235.patch << 'PATCH'
--- a/lib/c-stack.c
+++ b/lib/c-stack.c
@@ -39,6 +39,9 @@
 #include <errno.h>
 #include <inttypes.h>
 #include <signal.h>
+#if defined __linux__
+# include <sys/signal.h>
+#endif
 #include <stddef.h>
PATCH

# Register the patch's SHA-256 so Buildroot accepts it
echo "05e65d3fcf1ac40ff57c73362d16431c52f3ead70f3604692c8ed326bcb6b454  0001-fix-c-stack-ucontext-glibc235.patch" \
    >> /opt/buildroot/package/m4/m4.hash
```

**Configure and build:**

```sh
# Apply the RPi 5 base config to the out-of-tree directory
make -C /opt/buildroot O=/opt/buildroot-rpi5 raspberrypi5_defconfig

# Enable the libraries the examples need (append to the generated .config)
cat >> /opt/buildroot-rpi5/.config << 'EOF'
BR2_PACKAGE_LIBUSB=y
BR2_PACKAGE_FFTW=y
BR2_PACKAGE_FFTW_SINGLE=y
BR2_PACKAGE_LIBPNG=y
EOF
make -C /opt/buildroot O=/opt/buildroot-rpi5 olddefconfig

# Build — 1-2 h on first run.  Safe to interrupt; resume with the same command.
make -C /opt/buildroot O=/opt/buildroot-rpi5 -j$(nproc)
```

> Buildroot only enables **single-precision FFTW** (`fftw3f`).  These examples use only
> `fftwf_*` functions so the double-precision library is not required.

After the build set these once per shell session:

```sh
export BR_SYSROOT=/opt/buildroot-rpi5/staging
export BR_CC=/opt/buildroot-rpi5/host/bin/aarch64-buildroot-linux-gnu-gcc
```

**Buildroot-specific CMake toolchain file**

Buildroot's toolchain paths differ from the system cross-compiler, so it needs its own file.
Save this as `~/rpi5-buildroot-toolchain.cmake`:

```cmake
# ~/rpi5-buildroot-toolchain.cmake
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ROOTFS /opt/buildroot-rpi5/staging)

set(CMAKE_C_COMPILER   /opt/buildroot-rpi5/host/bin/aarch64-buildroot-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER /opt/buildroot-rpi5/host/bin/aarch64-buildroot-linux-gnu-g++)

set(CMAKE_SYSROOT        ${ROOTFS})
set(CMAKE_FIND_ROOT_PATH ${ROOTFS})

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

> Buildroot uses a **flat** `usr/lib/pkgconfig/` layout — there is no
> `aarch64-linux-gnu` multi-arch sub-directory as in Debian/Ubuntu sysroots.

**liquid-dsp for Buildroot**

liquid-dsp is not a Buildroot package; cross-compile it manually against the sysroot.
The Buildroot sysroot contains `fftw3f`, which liquid-dsp will find and link correctly:

```sh
git clone https://github.com/jgaeddert/liquid-dsp
cd liquid-dsp && ./bootstrap.sh

CC=$BR_CC \
CFLAGS="-march=armv8.2-a -mtune=cortex-a76 -O2 --sysroot=$BR_SYSROOT" \
LDFLAGS="--sysroot=$BR_SYSROOT" \
PKG_CONFIG_LIBDIR=$BR_SYSROOT/usr/lib/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=$BR_SYSROOT \
./configure --host=aarch64-buildroot-linux-gnu --prefix=/usr

make -j$(nproc)
make install DESTDIR=$BR_SYSROOT
```

Continue at **Step 4** below, substituting:
- toolchain file → `~/rpi5-buildroot-toolchain.cmake`
- `$ROOTFS` → `$BR_SYSROOT`
- `PKG_CONFIG_LIBDIR` → `$BR_SYSROOT/usr/lib/pkgconfig` (no `aarch64-linux-gnu` component)

### Step 3 — Create a CMake toolchain file

Save this once as `~/rpi5-toolchain.cmake` (or anywhere convenient):

```cmake
# ~/rpi5-toolchain.cmake
set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(ROOTFS /opt/rpi5-rootfs)     # must match the path used in Step 2

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

set(CMAKE_SYSROOT       ${ROOTFS})
set(CMAKE_FIND_ROOT_PATH ${ROOTFS})

# search programs on the host, libraries/headers only in the sysroot
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
```

### Step 4 — Build libfobos-sdr-agile for the target

Use `-DCMAKE_INSTALL_PREFIX=/usr` (not `$ROOTFS/usr`) combined with `DESTDIR` so that the
generated `.pc` file records `prefix=/usr`.  Without this, `PKG_CONFIG_SYSROOT_DIR` would
prepend the rootfs path a second time and the examples would fail to locate the headers.

```sh
cd libfobos-sdr-agile
mkdir -p build-rpi5 && cd build-rpi5

PKG_CONFIG_LIBDIR=$ROOTFS/usr/lib/aarch64-linux-gnu/pkgconfig:$ROOTFS/usr/lib/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=$ROOTFS \
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=~/rpi5-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr

make -j$(nproc)
sudo make install DESTDIR=$ROOTFS   # installs under $ROOTFS/usr with prefix=/usr in the .pc
```

> The udev rule is written to `$ROOTFS/etc/udev/rules.d/` — harmless during cross-build.
> Run `sudo make install` (without DESTDIR) on the RPi itself to activate it there.

### Step 5 — Build an example

GCC 11's aarch64 cross-compiler accepts ISA names for `-march` (`armv8.2-a`) and CPU names
for `-mtune` (`cortex-a76`), but not the same value for both.  The CMakeLists files expose
separate `CC_MARCH` and `CC_MTUNE` variables for this case; `CC_ARCH` still controls both for
native builds.

```sh
cd examples/fft/c         # or fft-scan, detector, demod_fm, …
mkdir -p build-rpi5 && cd build-rpi5

PKG_CONFIG_LIBDIR=$ROOTFS/usr/lib/aarch64-linux-gnu/pkgconfig:$ROOTFS/usr/lib/pkgconfig \
PKG_CONFIG_SYSROOT_DIR=$ROOTFS \
cmake .. \
    -DCMAKE_TOOLCHAIN_FILE=~/rpi5-toolchain.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCC_MARCH=armv8.2-a \
    -DCC_MTUNE=cortex-a76

make -j$(nproc)
```

`armv8.2-a` is the ISA implemented by the Cortex-A76 cores in the RPi 5's BCM2712 SoC.
`-mtune=cortex-a76` lets GCC schedule instructions for the specific pipeline.

### Step 6 — Deploy to the RPi

```sh
scp fft pi@rpi5.local:~/
# or for a full example directory:
rsync -avz build-rpi5/fft-scan pi@rpi5.local:~/
```

---

## Examples

### `fft` — Real-time Spectrum Analyser

**Source:** [examples/fft/c/](examples/fft/c/)

Captures IQ data from the Fobos SDR and displays a live power spectrum in the terminal using ANSI
escape sequences and Unicode eighth-block characters (▁▂▃▄▅▆▇█) for 8× sub-character vertical
resolution.  Averaging N FFTs reduces noise variance by √N.

**Processing pipeline:**

```
[Fobos SDR] → receiver_read() → Hamming window → FFTW3 DFT
           → accumulate I²+Q² → dBFS conversion + bin-swap
           → draw_fft() (terminal) / fwrite() (file)
```

**Quick start:**

```sh
# Live spectrum at 100 MHz, 20 MSPS, 1024-point FFT
./fft -c 100000000 -r 20000000 -w 1024 -a 10 -d

# Save raw FFT bins while drawing
./fft -c 433000000 -r 8000000 -w 2048 -a 20 -d -o spectrum.f32

# Save spectrogram PNG after 200 frames
./fft -c 100000000 -r 20000000 -w 2048 -a 50 -d -G sgram.png -N 200
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `-c <Hz>` | 100 000 000 | Centre frequency |
| `-r <Hz>` | 20 000 000 | Sample rate |
| `-w <N>` | 1024 | FFT width (power of 2) |
| `-a <N>` | 1 | FFTs averaged per display frame |
| `-b <dB>` | -75 | Noise floor reference level |
| `-s <N>` | = `-w` | Hop size in samples (< `-w` enables sliding-window overlap) |
| `-L <0-3>` | — | LNA gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB) |
| `-V <0-31>` | — | VGA gain (0..+62 dB, 2 dB step) |
| `-d` | off | Draw spectrum in terminal |
| `-m <dB>` | auto | Display minimum dB |
| `-M <dB>` | auto | Display maximum dB |
| `-o <file>` | stdout | Write raw float32 FFT bins to file |
| `-n <N>` | unlimited | Exit after N averaged frames |
| `-G <file>` | — | Save spectrogram PNG on exit |
| `-N <N>` | — | Exit after N frames and save spectrogram |
| `-S <MB>` | 256 | Spectrogram buffer memory limit |
| `-x[c][g]` | off | Max-hold display; `c` = save CSV, `g` = save PNG |
| `-v` | off | Verbose output |

---

### `fft-scan` — Wideband Panoramic Scanner

**Source:** [examples/fft-scan/c/](examples/fft-scan/c/)

Steps the Fobos SDR centre frequency from `-f` to `-t` using the hardware scan API
(`fobos_sdr_start_scan`).  The hardware manages frequency stepping and filters out IQ data captured
during retuning; only settled samples reach the ring buffer, each tagged with its centre frequency.
Per-step FFTs are stitched into a single panoramic spectrum and displayed or saved.

**Processing pipeline (per sweep):**

```
Hardware scan: freq[0]→IQ[0], freq[1]→IQ[1], …

Consumer (per step):
  ring buffer pull → Hamming window → FFTW3 → accumulate → dBFS
  → DC-centred crop → panorama[step * bins_per_step]

draw_fft(full panorama)
```

**Quick start:**

```sh
# Scan FM broadcast band (88–108 MHz)
./fft-scan -f 88000000 -t 108000000 -r 25000000 -w 2048 -a 50

# ISM 2.4 GHz band with spectrogram PNG
./fft-scan -f 2400000000 -t 2500000000 -r 20000000 -w 2048 -a 20 -G 2g4.png -n 100

# Max-hold snapshot of 400–500 MHz, export PNG
./fft-scan -f 400000000 -t 500000000 -r 20000000 -w 4096 -a 100 -xg -n 10
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `-f <Hz>` | 88 000 000 | Scan start frequency |
| `-t <Hz>` | 108 000 000 | Scan end frequency |
| `-r <Hz>` | 25 000 000 | Sample rate (= bandwidth per step) |
| `-w <N>` | 2048 | FFT width (power of 2) |
| `-a <N>` | 50 | FFTs averaged per step |
| `-b <dB>` | -75 | Noise floor reference level |
| `-s <N>` | = `-w` | FFT hop size in samples |
| `-O <Hz>` | 0 | Step overlap in Hz (keeps step boundaries inside filter passband) |
| `-L <0-3>` | — | LNA gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB) |
| `-V <0-31>` | — | VGA gain (0..+62 dB, 2 dB step) |
| `-m <dB>` | auto | Display minimum dB |
| `-M <dB>` | auto | Display maximum dB |
| `-n <N>` | unlimited | Exit after N productive sweeps |
| `-G <file>` | — | Save spectrogram PNG on exit (newest row at top) |
| `-S <MB>` | 256 | Spectrogram buffer memory limit |
| `-x[c][g]` | off | Max-hold display; `c` = save `maxhold.csv`, `g` = save `maxhold.png` |
| `-v` | off | Verbose output |

---

### `detector` — Signal Detector

**Source:** [examples/detector/c/](examples/detector/c/)

Applies an automatic noise-floor estimator (Nth-percentile of the dBFS spectrum) to locate occupied
channels in real time.  Works in two modes:

- **Single-channel** (default): monitors one fixed frequency.
- **Scan mode** (`-f`/`-F`): uses the hardware scan API to step across a range.

For each detected signal it prints centre frequency, bandwidth, and peak power.  Tracks signals
across frames: `START`, `UPD`, and `END` events are reported per active signal.

**Output format:**

```
# RTBW=20000000 CENTER=433000000 FFT=1024 AVG=10 THRESH=10.0dB PCTILE=25
1715640000500 433920135.7 24414.1 -45.2
# UNIX_MS   CENTER_HZ   BW_HZ   PEAK_DBFS
```

**Quick start:**

```sh
# Single-channel: monitor 433 MHz, 20 MSPS, 10 dB threshold
./detector -c 433000000 -r 20000000 -w 2048 -a 20 -t 10 -d

# Scan 400–500 MHz
./detector -f 400000000 -F 500000000 -r 20000000 -w 1024 -a 50 -t 10
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `-c <Hz>` | 433 000 000 | Centre frequency (single-channel mode) |
| `-r <Hz>` | 20 000 000 | Sample rate |
| `-w <N>` | 1024 | FFT width (power of 2) |
| `-a <N>` | 1000 | FFTs averaged per detection window |
| `-t <dB>` | 10.0 | Detection threshold above noise floor |
| `-H <dB>` | 3.0 | Hysteresis (signal falls when level drops below `threshold - hysteresis`) |
| `-p <0-99>` | 25 | Noise floor percentile |
| `-B <N>` | 2 | Minimum signal width in FFT bins |
| `-g <N>` | 0 | Max gap in bins to bridge when merging adjacent runs |
| `-f <Hz>` | — | Scan start frequency (enables scan mode) |
| `-F <Hz>` | — | Scan end frequency |
| `-O <Hz>` | 0 | Step overlap in Hz |
| `-L <0-3>` | — | LNA gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB) |
| `-V <0-31>` | — | VGA gain (0..+62 dB, 2 dB step) |
| `-d` | off | Draw live spectrum in terminal |
| `-o <file>` | stdout | Write detections to file |
| `-G <file>` | — | Save spectrogram PNG on exit |
| `-n <N>` | unlimited | Exit after N sweeps |
| `-v` | off | Verbose output |

---

### `demod_fm` — FM Broadcast Demodulator

**Source:** [examples/demod_fm/c/](examples/demod_fm/c/)
**Requires:** liquid-dsp

Receives a real FM broadcast station and produces decoded mono audio.  The full DSP chain runs in a
single real-time thread: DDC (optional), integer + fractional decimation to 200 kHz, FM
discriminator, hard clip, audio LPF, audio resampler, and 50 µs de-emphasis.  Output is raw
`float32` or `int16` audio suitable for `sox`/`aplay`.

**Processing pipeline:**

```
[Fobos SDR] → DDC (-O) → IQ decimation → FM discriminator
           → hard clip → audio LPF → audio resample
           → de-emphasis → output gain → f32/s16 output
```

**Quick start:**

```sh
# Receive 102.5 MHz, raw MPX float32 at 200 kHz
./demod_fm -s 8000000 -F 102500000 -o mpx.f32

# Play decoded mono audio through speakers (requires sox)
./demod_fm -s 8000000 -F 96000000 -a 48000 -e s16 | \
    sox -t raw -r 48000 -e signed -b 16 -c 1 - -d

# Tune to a station 200 kHz below the SDR centre
./demod_fm -s 8000000 -F 96200000 -O -200000 -a 48000 -e s16 -o audio.raw
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `-s <Hz>` | — | IQ sample rate from hardware |
| `-F <Hz>` | — | SDR centre frequency |
| `-O <Hz>` | 0 | DDC offset (shift station to DC) |
| `-a <Hz>` | 200 000 | Audio output sample rate |
| `-e f32\|s16` | f32 | Output encoding |
| `-g <gain>` | 1.0 | Output gain multiplier |
| `-D <µs>` | 50 | De-emphasis time constant (50 µs EU / 75 µs Americas) |
| `-k <kf>` | auto | FM discriminator modulation factor |
| `-o <file>` | stdout | Output file |

---

### `demod_fsk` — M-FSK Demodulator

**Source:** [examples/abstract_demod/demod_fsk/c/](examples/abstract_demod/demod_fsk/c/)
**Requires:** liquid-dsp

Non-coherent M-FSK demodulation (M = any power of 2).  Decoded bits are packed MSB-first into bytes
and written to stdout or a file, making the output directly pipeable to protocol decoders.  The
inner loop is NEON-vectorised on ARMv8 (Raspberry Pi 4/5).

**Pipeline:** DDC (optional) → rational resampler to `symbol_rate × k` → liquid-dsp `fskdem`
energy detector → symbol-to-bits packing → byte output.

**Quick start:**

```sh
# Demodulate 2-FSK at 9600 baud, 433 MHz, 2 MSPS
./demod_fsk -c 433000000 -r 2000000 -b 9600 -M 2 -o bits.bin

# Pipe to xxd for inspection
./demod_fsk -c 433000000 -r 2000000 -b 9600 -M 2 | xxd | head
```

---

### `demod_psk` — M-PSK Demodulator

**Source:** [examples/abstract_demod/demod_psk/c/](examples/abstract_demod/demod_psk/c/)
**Requires:** liquid-dsp

Coherent M-PSK demodulation (M = 2 to 64, power of 2).  Includes symbol timing recovery (Gardner
TED + polyphase RRC filter bank) and carrier phase recovery (Costas PLL).  Output is a raw bit
stream packed MSB-first into bytes.

**Pipeline:** DDC (optional) → rational resampler → symbol timing recovery (`symsync_crcf`) →
Costas loop carrier recovery (`nco_crcf` + `modemcf`) → symbol-to-bits packing → byte output.

**Quick start:**

```sh
# Demodulate BPSK at 9600 baud, 433 MHz
./demod_psk -c 433000000 -r 2000000 -b 9600 -M 2 -o bits.bin

# QPSK at 19200 baud
./demod_psk -c 433000000 -r 2000000 -b 19200 -M 4 -o bits.bin
```

---

## Shared Modules

The reusable C modules live as **reference copies** under
[examples/fobos/](examples/fobos/) (SDR capture layer — see its
[README](examples/fobos/README.md)) and [examples/shared/](examples/shared/) (DSP/display — see
its [README](examples/shared/README.md)). Each example **symlinks** the modules it needs from
these directories into its own `c/` (and lists them in its `CMakeLists.txt`), so each module has
a single source and edits propagate to every example while each still builds standalone.

| Module | Description |
|---|---|
| [`fobos/fobos.c`](examples/fobos/) | FobosOne hardware abstraction: enumerate, configure (freq/rate/gain/BW), sync + async/scan IQ acquisition |
| [`fobos/receiver.c`](examples/fobos/) | Real-time capture thread (SCHED_FIFO, pinned core) feeding a ring buffer; pull interface + back-pressure; per-chunk frequency tags in scan mode |
| [`fobos/ringbufer.c`](examples/fobos/) | Lock-free single-producer/single-consumer multi-chunk ring buffer |
| [`fobos/sgram.c`](examples/fobos/) | Rolling spectrogram + libpng writer (thermal colourmap) |
| [`shared/fft.c`](examples/shared/) | FFTW3 plan management, Hamming window, power accumulation, dBFS conversion |
| [`shared/draw.c`](examples/shared/) | Terminal spectrum display: ANSI escapes + Unicode block chars (▁–█) |
| [`shared/dsp.c`](examples/shared/) | IQ format conversion (S16 → float32) **and a Digital Down Converter** (`shift_unroll`) — a NEON-vectorised frequency-shifting mixer that brings an off-centre signal to baseband |
| [`shared/spectrum_png.c`](examples/shared/) | 1-D spectrum snapshot to PNG or CSV (peak-compresses bins > 4096 px) |

**Which example symlinks which module:**

| Module | [fft](examples/fft/) | [fft-scan](examples/fft-scan/) | [detector](examples/detector/) | [demod_fm](examples/demod_fm/) | [demod_fsk](examples/abstract_demod/demod_fsk/) | [demod_psk](examples/abstract_demod/demod_psk/) |
|---|:--:|:--:|:--:|:--:|:--:|:--:|
| `fobos`, `receiver`, `ringbuffer` | ✓ | ✓ | ✓ | ✓ | ✓ | ✓ |
| `sgram` | ✓ | ✓ | ✓ | | | |
| `fft`, `draw`, `spectrum_png` | ✓ | ✓ | ✓ | | | |
| `dsp` | | | | ✓ | ✓ | ✓ |

> The `detector` example additionally has its own `detector.c` (detection core); the `demod_*`
> examples add their liquid-dsp demodulator in `main.c`. The over-the-air harness in
> [examples/aaronia_OTA/](examples/aaronia_OTA/) keeps its own separate `c/` tools.

---

## Spectrogram PNG Output

Several examples can save a spectrogram (time-frequency heat map) PNG via `-G <file>`.
Each sweep adds one row; the newest row is at the top.  Memory is bounded by `-S <MB>` (default 256 MB).

Example screenshots from `fft-scan`:

| Scan | Image |
|---|---|
| FM band 88–108 MHz | ![FM](examples/fft-scan/spectr.png) |
| 2.4 GHz ISM band | ![2.4G](examples/fft-scan/2400.png) |

---

## Platform Notes

- Tested on **Linux** (Ubuntu 18.04/22.04, Raspbian).
- Real-time capture thread uses `SCHED_FIFO` and is pinned to CPU core 3.  On single-core or
  dual-core boards adjust `RECEIVER_CPU_CORE` in `receiver.c`.
- FFTW3 wisdom files are written to the current directory on first run (`fftw_wisdom.dat`) to
  speed up subsequent launches.

---

## License

See [libfobos-sdr-agile/LICENSE](libfobos-sdr-agile/LICENSE) for the library license.
Example code is provided as-is for developer use.
