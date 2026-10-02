/**
 * @file test_detector.c
 * @brief Synthetic spectrum tests for the signal detector. No hardware required.
 *
 * Generates deterministic and noisy spectra with FSK-like signals, then runs
 * the detector in several configurations to show the effect of hysteresis,
 * gap merging, and local noise floor estimation.
 *
 * Build standalone (no SDR libraries needed):
 *   gcc -O2 -o test_detector test_detector.c detector.c -lm
 *
 * Or via cmake: make test_detector
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "detector.h"

#define N_BINS      1024
#define SAMPLERATE  20000000.0
#define CENTRE_HZ   433000000.0
#define NOISE_DB    (-90.0f)

static double hz_per_bin = SAMPLERATE / N_BINS;

/* Fill spectrum with a flat value (deterministic, no randomness). */
static void fill_flat(float *spec, int n, float db) {
    for (int i = 0; i < n; i++) spec[i] = db;
}

/* Fill spectrum with white Gaussian noise around noise_db. */
static void fill_noise(float *spec, int n, float noise_db, float std_db,
                       unsigned int seed) {
    srand(seed);
    for (int i = 0; i < n; i++) {
        double u1 = (rand() + 1.0) / (RAND_MAX + 2.0);
        double u2 = (rand() + 1.0) / (RAND_MAX + 2.0);
        double g  = sqrt(-2.0 * log(u1)) * cos(2.0 * M_PI * u2);
        spec[i] = noise_db + (float)(g * std_db);
    }
}

/* Add a Gaussian-spread tone at `bin`.
 * Power falls by 3 dB per `spread` bins from centre. */
static void add_tone(float *spec, int n, int bin, float power_db, float spread) {
    int radius = (int)(spread * 4.0f + 1.0f);
    for (int di = -radius; di <= radius; di++) {
        int b = bin + di;
        if (b < 0 || b >= n) continue;
        float p = power_db - (float)(di * di) / (spread * spread) * 3.0f;
        if (p > spec[b]) spec[b] = p;
    }
}

static void separator(void) {
    printf("  %-56s\n", "--------------------------------------------------------");
}

static void show_spectrum_at(const float *spec, const char *label,
                             int *bins, int n_bins, float threshold) {
    printf("  Spectrum values (threshold=%.1f dBFS):\n", threshold);
    for (int i = 0; i < n_bins; i++) {
        printf("    bin %4d: %+7.2f dBFS  %s\n",
               bins[i], spec[bins[i]],
               spec[bins[i]] >= threshold ? "ACTIVE" : "below");
    }
}

static void print_results(const char *label, int found,
                          const detected_signal_t *sigs) {
    printf("  %-48s -> %d signal(s)\n", label, found);
    if (found == 0) {
        printf("    (none)\n");
        return;
    }
    for (int i = 0; i < found; i++) {
        printf("    [%d] CF=%+7.0f kHz  BW=%5.0f kHz (%.1f bins)  peak=%.1f dBFS\n",
               i,
               (sigs[i].center_hz - CENTRE_HZ) / 1000.0,
               sigs[i].bandwidth_hz / 1000.0,
               sigs[i].bandwidth_hz / hz_per_bin,
               sigs[i].peak_dbfs);
    }
}

static void run(const char *label, const float *spec, int n,
                int local_floor_bins, int percentile,
                float threshold, float hysteresis, int min_bins, int max_gap) {
    float *nf = malloc((size_t)n * sizeof(float));
    if (local_floor_bins > 0) {
        detector_local_noise_floor(spec, n, local_floor_bins, percentile, nf);
    } else {
        float g = detector_noise_floor(spec, n, percentile);
        for (int i = 0; i < n; i++) nf[i] = g;
    }
    detected_signal_t sigs[64];
    int found = detector_find_signals(spec, n, CENTRE_HZ, SAMPLERATE,
                                      nf, threshold, hysteresis,
                                      min_bins, max_gap, sigs, 64);
    print_results(label, found, sigs);
    free(nf);
}

int main(void) {
    float spec[N_BINS];
    float nf_global;

    printf("Detector synthetic test\n");
    printf("Bins=%d  RTBW=%.0f Hz  bin=%.0f Hz  centre=%.0f MHz\n\n",
           N_BINS, SAMPLERATE, hz_per_bin, CENTRE_HZ / 1e6);

    /* ------------------------------------------------------------------ */
    printf("TEST 1: single narrowband signal  (SNR=25 dB, flat background)\n");
    separator();
    fill_flat(spec, N_BINS, NOISE_DB);
    add_tone(spec, N_BINS, 512, NOISE_DB + 25.0f, 1.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    run("global/no-hyst/no-merge",  spec, N_BINS, 0,  25, 10.0f, 0.0f, 2, 0);
    run("local64/hyst3/gap2",       spec, N_BINS, 64, 25, 10.0f, 3.0f, 2, 2);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /*
     * 2-FSK: tones 8 bins apart (spread=1 → active range ±2 bins).
     * Active bins: [298-302] and [306-310].  Gap at bins 303-305 (3 bins).
     * Gap power = max(tone_at_dist3=-92, flat_background=-90) = -90 dBFS.
     * Threshold = -80 dBFS  =>  gap is NOT active  =>  detector splits.
     */
    printf("TEST 2: 2-FSK  (tones at bins 300 and 308, spacing=8, SNR=25 dB)\n");
    separator();
    fill_flat(spec, N_BINS, NOISE_DB);
    add_tone(spec, N_BINS, 300, NOISE_DB + 25.0f, 1.0f);
    add_tone(spec, N_BINS, 308, NOISE_DB + 25.0f, 1.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    {
        int show[] = {300, 302, 303, 305, 308};
        show_spectrum_at(spec, "key", show, 5, nf_global + 10.0f);
    }
    run("global/no-hyst/no-merge  (OLD — splits FSK)",  spec, N_BINS, 0,  25, 10.0f, 0.0f, 2, 0);
    run("global/hyst12/no-merge   (hyst bridges gap)",  spec, N_BINS, 0,  25, 10.0f, 12.0f, 2, 0);
    run("global/no-hyst/gap3      (gap merge)",         spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 3);
    run("local64/hyst3/gap3",                           spec, N_BINS, 64, 25, 10.0f, 3.0f,  2, 3);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /*
     * 4-FSK: tones at bins 480, 488, 496, 504 (spacing=8).
     * 3 gaps of 3 bins each, gap power=-90 dBFS < -80 threshold => splits.
     */
    printf("TEST 3: 4-FSK  (tones at 480/488/496/504, spacing=8, SNR=25 dB)\n");
    separator();
    fill_flat(spec, N_BINS, NOISE_DB);
    add_tone(spec, N_BINS, 480, NOISE_DB + 25.0f, 1.0f);
    add_tone(spec, N_BINS, 488, NOISE_DB + 25.0f, 1.0f);
    add_tone(spec, N_BINS, 496, NOISE_DB + 25.0f, 1.0f);
    add_tone(spec, N_BINS, 504, NOISE_DB + 25.0f, 1.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    run("global/no-hyst/no-merge  (OLD — 4 splits)",  spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 0);
    run("global/hyst12/no-merge   (hyst)",             spec, N_BINS, 0,  25, 10.0f, 12.0f, 2, 0);
    run("global/no-hyst/gap3      (gap merge => 1)",   spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 3);
    run("local64/hyst3/gap3",                          spec, N_BINS, 64, 25, 10.0f, 3.0f,  2, 3);
    printf("\n");

    /* ------------------------------------------------------------------ */
    printf("TEST 4: 8-FSK  (8 tones, spacing=8, SNR=25 dB)\n");
    separator();
    fill_flat(spec, N_BINS, NOISE_DB);
    for (int i = 0; i < 8; i++)
        add_tone(spec, N_BINS, 440 + i * 8, NOISE_DB + 25.0f, 1.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    run("global/no-hyst/no-merge  (OLD — 8 splits)",  spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 0);
    run("global/hyst12/no-merge",                     spec, N_BINS, 0,  25, 10.0f, 12.0f, 2, 0);
    run("global/no-hyst/gap3      (gap merge => 1)",  spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 3);
    run("local64/hyst3/gap3",                         spec, N_BINS, 64, 25, 10.0f, 3.0f,  2, 3);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /*
     * Non-flat noise floor: 0.04 dB/bin slope.
     *   bin   0: noise = -90.0 dBFS
     *   bin 512: noise = -69.6 dBFS
     *   bin 900: noise = -54.0 dBFS
     *
     * Global 25th-pct floor ≈ -79.7 dBFS  =>  threshold ≈ -69.7 dBFS.
     * At bin 900 noise is -54 which is >> -69.7 => many false positives.
     *
     * Local floor (window=128) follows the slope:
     * local floor near bin 900 ≈ -54 dBFS  =>  threshold ≈ -44 dBFS.
     * Noise std 1.5 dB => 3σ peak ≈ -54+4.5 = -49.5 << -44 => no false pos.
     *
     * A real signal at bin 900 is placed at local_noise+15 = -39 dBFS.
     */
    printf("TEST 5: tilted noise floor  (+0.04 dB/bin slope, one real signal)\n");
    separator();
    fill_noise(spec, N_BINS, NOISE_DB, 1.5f, 42);
    for (int i = 0; i < N_BINS; i++)
        spec[i] += 0.04f * i;
    add_tone(spec, N_BINS, 900, NOISE_DB + 0.04f * 900.0f + 15.0f, 2.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    printf("  Noise at bin 900 ≈ %.1f dBFS  signal at %.1f dBFS\n",
           NOISE_DB + 0.04f * 900.0f, NOISE_DB + 0.04f * 900.0f + 15.0f);
    run("global/no-hyst   (false positives across band)", spec, N_BINS,   0, 25, 10.0f, 0.0f, 2, 0);
    run("local128/hyst3   (adaptive => only real signal)", spec, N_BINS, 128, 25, 10.0f, 3.0f, 2, 2);
    printf("\n");

    /* ------------------------------------------------------------------ */
    /*
     * Two separate narrowband signals 20 bins apart.
     * A large max_gap would incorrectly merge them.
     */
    printf("TEST 6: two adjacent signals (A@bin 400, B@bin 420, gap=16 bins)\n");
    separator();
    fill_flat(spec, N_BINS, NOISE_DB);
    add_tone(spec, N_BINS, 400, NOISE_DB + 25.0f, 1.0f);
    add_tone(spec, N_BINS, 420, NOISE_DB + 20.0f, 2.0f);
    nf_global = detector_noise_floor(spec, N_BINS, 25);
    printf("  Global noise floor: %.1f dBFS  threshold: %.1f dBFS\n",
           nf_global, nf_global + 10.0f);
    run("global/no-hyst/no-merge  (correct: 2)",  spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 0);
    run("global/no-hyst/gap20     (over-merge!)",  spec, N_BINS, 0,  25, 10.0f, 0.0f,  2, 20);
    run("local64/hyst3/gap3       (correct: 2)",   spec, N_BINS, 64, 25, 10.0f, 3.0f,  2, 3);
    printf("\n");

    return 0;
}
