/*
 * measure.c — Fobos SDR peak-power meter for calibration sweeps.
 *
 * Tunes to FREQ_HZ, then for every (lna, vga) combination captures IQ,
 * computes the FFT peak bin power in dBFS and prints a CSV line:
 *
 *   freq_hz,lna,vga,peak_dbfs
 *
 * Usage:  measure FREQ_HZ [RATE_HZ [N_FFT]]
 * Defaults: RATE_HZ=2500000  N_FFT=65536
 * Output: 128 lines (LNA 0..3 × VGA 0..31)
 */
#include "fobos.h"
#include <fftw3.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <inttypes.h>

#define LNA_MAX   FOBOS_LNA_GAIN_MAX
#define VGA_MAX   FOBOS_VGA_GAIN_MAX
#define DEF_RATE  2500000
#define DEF_N     65536
#define WARMUP    3     /* extra reads discarded after each gain change */

/* Hann-windowed FFT, returns peak bin power in dBFS (0 dBFS = full scale). */
static float fft_peak_dbfs(const float *iq, int n)
{
    fftwf_complex *in  = fftwf_alloc_complex(n);
    fftwf_complex *out = fftwf_alloc_complex(n);
    fftwf_plan     p   = fftwf_plan_dft_1d(n, in, out, FFTW_FORWARD, FFTW_ESTIMATE);

    for (int i = 0; i < n; i++) {
        float w = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (n - 1)));
        in[i][0] = iq[2*i]   * w;
        in[i][1] = iq[2*i+1] * w;
    }
    fftwf_execute(p);

    /* Skip DC guard (±1%) to avoid DC offset spike */
    int guard = n / 100;
    float peak = 0.0f;
    for (int i = 0; i < n; i++) {
        int d = (i <= n/2) ? i : n - i;
        if (d < guard) continue;
        float m2 = out[i][0]*out[i][0] + out[i][1]*out[i][1];
        if (m2 > peak) peak = m2;
    }

    fftwf_destroy_plan(p);
    fftwf_free(in);
    fftwf_free(out);

    /* Normalise: Hann window coherent power gain = (N/2)^2 * 2 */
    float norm = (float)n * 0.5f;
    norm = norm * norm * 2.0f;
    return 10.0f * log10f(peak / norm + 1e-30f);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s FREQ_HZ [RATE_HZ [N_FFT]]\n", argv[0]);
        return 1;
    }
    uint64_t freq = (uint64_t)atof(argv[1]);
    int      rate = argc > 2 ? (int)atof(argv[2]) : DEF_RATE;
    int      n    = argc > 3 ? atoi(argv[3])       : DEF_N;

    fobos_device_t *dev = fobos_open_by_idx(0);
    if (!dev) {
        fprintf(stderr, "fobos_open_by_idx failed\n");
        return 1;
    }

    fobos_set_frequency(dev, freq);
    fobos_set_samplerate(dev, rate);

    float *buf = malloc((size_t)n * 2 * sizeof(float));
    if (!buf) { fobos_close(dev); return 1; }

    for (int lna = 0; lna <= LNA_MAX; lna++) {
        fobos_set_lna_gain(dev, lna);
        for (int vga = 0; vga <= VGA_MAX; vga++) {
            fobos_set_vga_gain(dev, vga);

            /* Warmup: discard buffers to let gain change settle */
            fobos_start_sync(dev, n);
            for (int w = 0; w < WARMUP; w++)
                fobos_read_samples_sync(dev, buf, n);
            /* Measurement */
            fobos_read_samples_sync(dev, buf, n);
            fobos_stop_sync(dev);

            float pdb = fft_peak_dbfs(buf, n);
            printf("%" PRIu64 ",%d,%d,%.3f\n", freq, lna, vga, pdb);
            fflush(stdout);
        }
    }

    free(buf);
    fobos_close(dev);
    return 0;
}
