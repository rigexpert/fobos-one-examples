/*
 * demod_psk_file.c — M-PSK demodulator for raw IQ files.
 *
 * Reads all raw float32 interleaved IQ samples from stdin, then for each burst:
 *   1. Energy detector to find burst onset (per symbol period).
 *      Requires 2 consecutive above-threshold symbol periods to avoid false trips.
 *   2. Preamble (all-0) to estimate carrier phase via angle averaging.
 *   3. Data symbols demodulated with phase correction applied.
 *
 * Output (stdout): one "RXSYM b=N n=D sym0 sym1 …" line per detected burst.
 *
 * Usage: demod_psk_file M K preamble_syms data_syms [e_threshold_db]
 *   e_threshold_db: energy threshold above noise floor in dB (default 15.0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>

static int read_all_f32(float **out)
{
    size_t cap = 1 << 20, used = 0;
    float *buf = malloc(cap * sizeof(float));
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

static float block_power(const float *iq, int n_samps)
{
    float e = 0.0f;
    for (int i = 0; i < n_samps * 2; i += 2)
        e += iq[i] * iq[i] + iq[i + 1] * iq[i + 1];
    return e / (float)n_samps;
}

/*
 * Rectangular integrate-and-dump one symbol.
 * sample_base: first sample index of this symbol.
 */
static liquid_float_complex integrate_dump(const float *iq, int sample_base, int K)
{
    float re = 0.0f, im = 0.0f;
    for (int i = 0; i < K; i++) {
        re += iq[(sample_base + i) * 2];
        im += iq[(sample_base + i) * 2 + 1];
    }
    return (liquid_float_complex)((re / K) + _Complex_I * (im / K));
}

int main(int argc, char **argv)
{
    if (argc < 5) {
        fprintf(stderr,
            "Usage: %s M K preamble_syms data_syms [e_threshold_db]\n", argv[0]);
        return 1;
    }
    unsigned int M          = (unsigned int)atoi(argv[1]);
    int          K          = atoi(argv[2]);
    int          preamble_n = atoi(argv[3]);
    int          data_n     = atoi(argv[4]);
    float        thresh_db  = argc > 5 ? atof(argv[5]) : 15.0f;

    modulation_scheme ms;
    switch (M) {
        case 2:  ms = LIQUID_MODEM_BPSK;  break;
        case 4:  ms = LIQUID_MODEM_QPSK;  break;
        case 8:  ms = LIQUID_MODEM_PSK8;  break;
        case 16: ms = LIQUID_MODEM_PSK16; break;
        default: fprintf(stderr, "Unsupported M\n"); return 1;
    }

    /* Expected constellation point for symbol 0 (reference for phase correction) */
    modem ref_mod = modem_create(ms);
    liquid_float_complex ref_sym0;
    modem_modulate(ref_mod, 0u, &ref_sym0);
    modem_destroy(ref_mod);
    float ref_angle0 = atan2f(cimagf(ref_sym0), crealf(ref_sym0));

    float *raw = NULL;
    int    nf  = read_all_f32(&raw);
    if (nf < 0) { fprintf(stderr, "read failed\n"); return 1; }
    int n_samps = nf / 2;
    int n_syms  = n_samps / K;

    /* Per-symbol energy */
    float *e_sym = malloc((size_t)n_syms * sizeof(float));
    for (int sym = 0; sym < n_syms; sym++)
        e_sym[sym] = block_power(raw + sym * K * 2, K);

    /* Noise floor: median of lowest quartile */
    float *sorted = malloc((size_t)n_syms * sizeof(float));
    memcpy(sorted, e_sym, (size_t)n_syms * sizeof(float));
    int q = n_syms / 4;
    for (int i = 1; i < n_syms; i++) {
        float v = sorted[i]; int j = i - 1;
        while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
        sorted[j + 1] = v;
    }
    float noise_floor = sorted[q / 2];
    free(sorted);
    float threshold = noise_floor * powf(10.0f, thresh_db / 10.0f);

    modem dem = modem_create(ms);
    unsigned int *rx_data = malloc((size_t)data_n * sizeof(unsigned int));

    int in_burst    = 0;
    int burst_num   = 0;
    int consec_high = 0;

    for (int sym = 0; sym < n_syms; sym++) {
        if (!in_burst) {
            if (e_sym[sym] > threshold) {
                consec_high++;
                if (consec_high < 2) continue;   /* require 2 consecutive to start */
                /* Burst onset: back up to first high-energy symbol */
                int burst_start_sym = sym - consec_high + 1;
                int burst_samp = burst_start_sym * K;
                consec_high = 0;
                in_burst = 1;

                int need_samps = (preamble_n + data_n) * K;
                if (burst_samp + need_samps > n_samps) break;

                /* Phase estimate from preamble */
                float sum_re = 0.0f, sum_im = 0.0f;
                for (int s = 0; s < preamble_n; s++) {
                    liquid_float_complex p = integrate_dump(raw, burst_samp + s * K, K);
                    sum_re += crealf(p);
                    sum_im += cimagf(p);
                }
                float recv_angle = atan2f(sum_im, sum_re);
                float pc = ref_angle0 - recv_angle;
                liquid_float_complex corr = (liquid_float_complex)(cosf(pc) + _Complex_I * sinf(pc));

                /* Demodulate data */
                int data_samp = burst_samp + preamble_n * K;
                for (int s = 0; s < data_n; s++) {
                    liquid_float_complex samp = integrate_dump(raw, data_samp + s * K, K);
                    samp *= corr;
                    modem_demodulate(dem, samp, &rx_data[s]);
                }

                printf("RXSYM b=%d n=%d", burst_num, data_n);
                for (int s = 0; s < data_n; s++) printf(" %u", rx_data[s]);
                printf("\n");
                burst_num++;
            } else {
                consec_high = 0;
            }
        } else if (e_sym[sym] <= threshold) {
            in_burst = 0;
        }
    }

    modem_destroy(dem);
    free(rx_data);
    free(e_sym);
    free(raw);
    return 0;
}
