/*
 * demod_fsk_file.c — M-FSK demodulator for raw IQ files.
 *
 * Reads all raw float32 interleaved IQ samples from stdin, then:
 *   1. Energy detector (per symbol period) to find burst onsets.
 *   2. Preamble correlation timing search across K sub-symbol offsets.
 *   3. Data symbol demodulation with the recovered timing.
 *
 * Output (stdout): one "RXSYM b=N n=D sym0 sym1 …" line per detected burst.
 *
 * Usage: demod_fsk_file M K preamble_syms data_syms [e_threshold_db [bw [Fs f0_hz f1_hz ...]]]
 *   M             modulation order (2, 4, 8)
 *   K             samples per symbol
 *   preamble_syms symbols in alternating-0/M-1 preamble
 *   data_syms     data symbols per burst to output
 *   e_threshold_db energy threshold above noise floor in dB (default 6.0)
 *   bw            fractional bandwidth — must be < 0.5 for liquid-dsp (default 0.35)
 *   Fs f0 f1 …    when bw >= 0.5: sample rate (Hz) and explicit tone frequencies (Hz)
 *                  uses internal DFT demodulation instead of fskdem
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef float _Complex fcmplx;

/* ---- DFT-based demodulation (for bw >= 0.5 or explicit tone frequencies) ---- */

/* Goertzel: DFT power at a single normalised frequency f (cycles/sample). */
static float goertzel_power(const fcmplx *x, int K, float f_norm)
{
    float cr = 0.0f, ci = 0.0f;
    for (int n = 0; n < K; n++) {
        float angle = -2.0f * (float)M_PI * f_norm * (float)n;
        float xr = crealf(x[n]), xi = cimagf(x[n]);
        cr += xr * cosf(angle) - xi * sinf(angle);
        ci += xr * sinf(angle) + xi * cosf(angle);
    }
    return cr * cr + ci * ci;
}

static unsigned int demod_dft(const fcmplx *block, int K, int M,
                               const float *tone_norm_freqs)
{
    float best = -1.0f;
    unsigned int best_sym = 0;
    for (int m = 0; m < M; m++) {
        float p = goertzel_power(block, K, tone_norm_freqs[m]);
        if (p > best) { best = p; best_sym = (unsigned int)m; }
    }
    return best_sym;
}

static float tone_energy_dft(const fcmplx *block, int K, int M,
                              const float *tone_norm_freqs)
{
    float e = 0.0f;
    for (int m = 0; m < M; m++)
        e += goertzel_power(block, K, tone_norm_freqs[m]);
    return e;
}

static int timing_search_dft(const fcmplx *samps, int K, int M,
                              const float *tone_norm_freqs,
                              const unsigned int *preamble, int preamble_syms)
{
    int best_off = 0, best_score = -1;
    for (int delta = 0; delta < K; delta++) {
        int score = 0;
        for (int s = 0; s < preamble_syms; s++) {
            unsigned int sym = demod_dft(samps + delta + s * K, K, M, tone_norm_freqs);
            if (sym == preamble[s]) score++;
        }
        if (score > best_score) { best_score = score; best_off = delta; }
    }
    return best_off;
}

/* ---- liquid-dsp based demodulation (for bw < 0.5) ---- */
#include <liquid/liquid.h>

static float block_power(const liquid_float_complex *s, int n)
{
    float e = 0.0f;
    for (int i = 0; i < n; i++)
        e += crealf(s[i]) * crealf(s[i]) + cimagf(s[i]) * cimagf(s[i]);
    return e / (float)n;
}

static int timing_search_fskdem(fskdem dem,
                                const liquid_float_complex *samps,
                                const unsigned int *preamble, int preamble_syms,
                                int K)
{
    int best_off = 0, best_score = -1;
    for (int delta = 0; delta < K; delta++) {
        int score = 0;
        for (int s = 0; s < preamble_syms; s++) {
            unsigned int sym = fskdem_demodulate(dem,
                (liquid_float_complex *)(samps + delta + s * K));
            if (sym == preamble[s]) score++;
        }
        if (score > best_score) { best_score = score; best_off = delta; }
    }
    return best_off;
}

/* ---- common ---- */

static int read_all_f32(float **out)
{
    size_t cap  = 1 << 20;
    size_t used = 0;
    float *buf  = malloc(cap * sizeof(float));
    if (!buf) return -1;
    while (1) {
        if (used == cap) {
            cap *= 2;
            float *nb = realloc(buf, cap * sizeof(float));
            if (!nb) { free(buf); return -1; }
            buf = nb;
        }
        size_t got = fread(buf + used, sizeof(float), cap - used, stdin);
        used += got;
        if (got == 0) break;
    }
    *out = buf;
    return (int)(used / 2) * 2;
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s M K preamble_syms data_syms [e_threshold_db [bw [Fs f0_hz f1_hz ...]]]\n",
            argv[0]);
        return 1;
    }
    unsigned int M          = (unsigned int)atoi(argv[1]);
    int          K          = atoi(argv[2]);
    int          preamble_n = atoi(argv[3]);
    int          data_n     = atoi(argv[4]);
    float        thresh_db  = argc > 5 ? atof(argv[5]) : 6.0f;
    float        bw         = argc > 6 ? (float)atof(argv[6]) : 0.35f;

    /* Choose demodulation mode */
    int use_dft = (bw >= 0.5f);
    float *tone_freqs = NULL;  /* normalised, used only when use_dft=1 */

    if (use_dft) {
        if (argc < (int)(8 + M)) {
            fprintf(stderr, "For bw>=0.5 must provide Fs and %u tone frequencies (Hz)\n", M);
            fprintf(stderr, "Usage: %s M K pre data thresh bw Fs f0 f1 ...\n", argv[0]);
            return 1;
        }
        float Fs = (float)atof(argv[7]);
        tone_freqs = malloc((size_t)M * sizeof(float));
        if (!tone_freqs) return 1;
        fprintf(stderr, "DFT mode: Fs=%.0fHz tones:", Fs);
        for (unsigned int m = 0; m < M; m++) {
            tone_freqs[m] = (float)atof(argv[8 + m]) / Fs;
            fprintf(stderr, " %.1fkHz", tone_freqs[m] * Fs / 1000.0f);
        }
        fprintf(stderr, "\n");
    }

    float *raw = NULL;
    int    nf  = read_all_f32(&raw);
    if (nf < 0 || !raw) { fprintf(stderr, "read failed\n"); return 1; }
    int n_samps = nf / 2;

    if (n_samps == 0) {
        fprintf(stderr, "no samples\n"); free(raw); free(tone_freqs); return 1;
    }

    liquid_float_complex *stream = malloc((size_t)n_samps * sizeof(*stream));
    if (!stream) { free(raw); free(tone_freqs); return 1; }
    for (int i = 0; i < n_samps; i++) {
        ((float *)&stream[i])[0] = raw[2 * i];
        ((float *)&stream[i])[1] = raw[2 * i + 1];
    }
    free(raw);

    int n_syms = n_samps / K;
    if (n_syms == 0) {
        fprintf(stderr, "too few samples for K=%d\n", K);
        free(stream); free(tone_freqs); return 1;
    }

    /* Per-symbol energy */
    size_t sym_bytes = (size_t)n_syms * sizeof(float);
    float *e_sym = malloc(sym_bytes);
    if (!e_sym) { free(stream); free(tone_freqs); return 1; }

    if (use_dft) {
        for (int sym = 0; sym < n_syms; sym++)
            e_sym[sym] = tone_energy_dft((fcmplx *)(stream + sym * K),
                                          K, (int)M, tone_freqs);
    } else {
        for (int sym = 0; sym < n_syms; sym++)
            e_sym[sym] = block_power(stream + sym * K, K);
    }

    /* Estimate noise floor as median of lowest quartile */
    float *sorted = malloc(sym_bytes);
    if (!sorted) { free(e_sym); free(stream); free(tone_freqs); return 1; }
    for (int i = 0; i < n_syms; i++) sorted[i] = e_sym[i];

    int q = n_syms / 4;
    int cmp_float(const void *a, const void *b) {
        float fa = *(const float *)a, fb = *(const float *)b;
        return (fa > fb) - (fa < fb);
    }
    qsort(sorted, (size_t)n_syms, sizeof(float), cmp_float);
    float noise_floor = (q > 0) ? sorted[q / 2] : sorted[0];
    free(sorted);

    float threshold = noise_floor * powf(10.0f, thresh_db / 10.0f);

    unsigned int *preamble = malloc((size_t)preamble_n * sizeof(unsigned int));
    for (int i = 0; i < preamble_n; i++)
        preamble[i] = (i % 2 == 0) ? 0u : (M - 1);

    fskdem dem = NULL;
    if (!use_dft)
        dem = fskdem_create(M, (unsigned int)K, bw);

    unsigned int *rx_data = malloc((size_t)data_n * sizeof(unsigned int));

    int in_burst = 0, burst_num = 0;
    for (int sym = 0; sym < n_syms; sym++) {
        if (!in_burst && e_sym[sym] > threshold) {
            in_burst = 1;
            int samp = sym * K;
            int need = (preamble_n + data_n) * K + K;
            if (samp + need > n_samps) break;

            int delta;
            if (use_dft) {
                delta = timing_search_dft((fcmplx *)(stream + samp),
                                          K, (int)M, tone_freqs, preamble, preamble_n);
            } else {
                delta = timing_search_fskdem(dem, stream + samp,
                                             preamble, preamble_n, K);
            }

            int base = samp + delta + preamble_n * K;
            for (int s = 0; s < data_n; s++) {
                int pos = base + s * K;
                if (pos + K > n_samps) { rx_data[s] = 0xffffffff; continue; }
                if (use_dft)
                    rx_data[s] = demod_dft((fcmplx *)(stream + pos),
                                            K, (int)M, tone_freqs);
                else
                    rx_data[s] = fskdem_demodulate(dem,
                                    (liquid_float_complex *)(stream + pos));
            }

            printf("RXSYM b=%d n=%d", burst_num, data_n);
            for (int s = 0; s < data_n; s++) printf(" %u", rx_data[s]);
            printf("\n");
            fflush(stdout);
            burst_num++;

        } else if (in_burst && e_sym[sym] <= threshold) {
            in_burst = 0;
        }
    }

    if (dem) fskdem_destroy(dem);
    free(preamble);
    free(rx_data);
    free(e_sym);
    free(stream);
    free(tone_freqs);
    return 0;
}
