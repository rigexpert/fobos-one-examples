# abstract_demod — Baseline digital demodulators (FSK / PSK)

Reference demodulator examples that show how to drive the liquid-dsp primitives for the two most
common digital modulations:

| Example | What it demonstrates |
|---|---|
| [demod_fsk/](demod_fsk/) | Non-coherent M-FSK via liquid `fskdem` (energy per tone bin) |
| [demod_psk/](demod_psk/) | Coherent M-PSK via `symsync_crcf` (timing) + Costas loop (carrier) |

> These are **"abstract" demodulators**: they demonstrate the core DSP on a clean, continuously
> modulated stream. They are **not complete receivers for real over-the-air signals.**

## ⚠ Limitations for real transmitted signals

The examples assume an idealized signal (continuous modulation, known parameters, benign channel).
A real link needs framing and synchronization the examples deliberately leave out:

- **Burst / frame synchronization (FSK and PSK).** Real transmissions are bursts, not a continuous
  stream. You must detect the burst and recover **symbol timing** — typically with a **preamble**
  (a known symbol pattern) that the receiver correlates against to find the burst onset and the
  exact sample phase. Without it the symbol clock drifts and the output is garbage.
- **Carrier-phase ambiguity (PSK).** A Costas loop locks to *a* carrier phase but PSK has an
  **M-fold phase ambiguity** — the recovered constellation can be rotated by any multiple of
  2π/M, so the absolute bit mapping is undefined. Resolve it with **differential encoding (DPSK /
  DQPSK)** — decode the *phase change* between symbols, which is rotation-invariant — or with a
  known pilot/preamble that pins the absolute phase.
- **Frequency/timing offset, AGC, fading.** Real signals arrive with a carrier-frequency offset,
  unknown amplitude, and channel impairments that a production receiver must track.

**Working, OTA-validated implementations** of the above (preamble energy-detection + correlation
timing search for FSK, differential PSK, robust burst handling) live in
[`../aaronia_OTA/`](../aaronia_OTA/) — the over-the-air modem loopback test harness. Treat those as
the "how to make it decode a real transmission" companion to these baseline examples.

## Build & requirements

Each subdirectory builds independently and needs [liquid-dsp](https://github.com/jgaeddert/liquid-dsp)
plus the installed `libfobos-sdr-agile`. See each example's README for exact commands and options.
