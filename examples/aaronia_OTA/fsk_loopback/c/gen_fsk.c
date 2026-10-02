/*
 * gen_fsk.c — M-FSK burst IQ generator for loopback tests.
 *
 * Produces N bursts, each structured as:
 *   [NOISE_GAP symbol-periods of AWGN]
 *   [PREAMBLE_SYMS alternating-0/M-1 symbols]
 *   [data_syms FSK symbols: first FIXED_SYMS=8 are 0,1,2,... mod M; rest random]
 *   (trailing gap after last burst)
 *
 * Output:
 *   stdout : raw float32 interleaved IQ (I0 Q0 I1 Q1 …)
 *   stderr : one "INFO …" line, then one "TXSYM b=N n=D sym0 sym1 …" per burst
 *
 * Usage: gen_fsk M K n_bursts data_syms seed [noise_sigma]
 *   M           modulation order: 2, 4, or 8
 *   K           samples per symbol (≥16; M=8 needs K≥128)
 *   n_bursts    number of bursts
 *   data_syms   data symbols per burst
 *   seed        RNG seed (reproduce TX symbols with same seed)
 *   noise_sigma AWGN per-component sigma (default 0.05)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>

#define PREAMBLE_SYMS 8
#define NOISE_GAP     24

static void box_muller(float sigma, float *re, float *im)
{
    float u1 = (rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float u2 = (rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float mag = sigma * sqrtf(-2.0f * logf(u1));
    *re = mag * cosf(2.0f * (float)M_PI * u2);
    *im = mag * sinf(2.0f * (float)M_PI * u2);
}

static void write_f32(float v) { fwrite(&v, sizeof v, 1, stdout); }

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "Usage: %s M K n_bursts data_syms seed [noise_sigma [bw]]\n", argv[0]);
        return 1;
    }
    unsigned int M       = (unsigned int)atoi(argv[1]);
    int          K       = atoi(argv[2]);
    int          n_bursts = atoi(argv[3]);
    int          data_syms = atoi(argv[4]);
    unsigned int seed    = (unsigned int)atoi(argv[5]);
    float        sigma   = argc > 6 ? atof(argv[6]) : 0.05f;
    float        bw      = argc > 7 ? (float)atof(argv[7]) : 0.35f;

    if (M < 2 || (M & (M - 1))) {
        fprintf(stderr, "M must be a power of 2 (2, 4, 8, …)\n"); return 1;
    }
    /* liquid-dsp fskmod requires K in [M, 2048] */
    if (K < (int)M || K > 2048) {
        fprintf(stderr, "K must be in [%u, 2048] for M=%u (K=%d rejected)\n", M, M, K);
        return 1;
    }

    srand(seed);

    fskmod mod = fskmod_create(M, (unsigned int)K, bw);

    /* preamble: alternating 0 / M-1 */
    unsigned int preamble[PREAMBLE_SYMS];
    for (int i = 0; i < PREAMBLE_SYMS; i++)
        preamble[i] = (i % 2 == 0) ? 0u : (M - 1);

    /* TX data symbols for each burst */
    unsigned int **tx = malloc((size_t)n_bursts * sizeof(unsigned int *));
    for (int b = 0; b < n_bursts; b++) {
        tx[b] = malloc((size_t)data_syms * sizeof(unsigned int));
        for (int s = 0; s < data_syms; s++)
            tx[b][s] = (unsigned int)(rand() % (int)M);
    }

    fprintf(stderr, "INFO M=%u K=%d n_bursts=%d data_syms=%d preamble=%d gap=%d\n",
            M, K, n_bursts, data_syms, PREAMBLE_SYMS, NOISE_GAP);

    liquid_float_complex *sym_buf = malloc((size_t)K * sizeof(*sym_buf));
    if (!sym_buf) { fprintf(stderr, "malloc failed\n"); return 1; }

    /* leading noise gap */
    for (int s = 0; s < NOISE_GAP; s++)
        for (int i = 0; i < K; i++) {
            float re, im;
            box_muller(sigma, &re, &im);
            write_f32(re); write_f32(im);
        }

    for (int b = 0; b < n_bursts; b++) {
        /* preamble */
        for (int s = 0; s < PREAMBLE_SYMS; s++) {
            fskmod_modulate(mod, preamble[s], sym_buf);
            for (int i = 0; i < K; i++) {
                float re, im;
                box_muller(sigma, &re, &im);
                write_f32(crealf(sym_buf[i]) + re);
                write_f32(cimagf(sym_buf[i]) + im);
            }
        }
        /* data */
        for (int s = 0; s < data_syms; s++) {
            fskmod_modulate(mod, tx[b][s], sym_buf);
            for (int i = 0; i < K; i++) {
                float re, im;
                box_muller(sigma, &re, &im);
                write_f32(crealf(sym_buf[i]) + re);
                write_f32(cimagf(sym_buf[i]) + im);
            }
        }
        /* trailing noise gap */
        for (int s = 0; s < NOISE_GAP; s++)
            for (int i = 0; i < K; i++) {
                float re, im;
                box_muller(sigma, &re, &im);
                write_f32(re); write_f32(im);
            }

        /* emit TX symbol reference to stderr */
        fprintf(stderr, "TXSYM b=%d n=%d", b, data_syms);
        for (int s = 0; s < data_syms; s++)
            fprintf(stderr, " %u", tx[b][s]);
        fprintf(stderr, "\n");
    }

    fskmod_destroy(mod);
    for (int b = 0; b < n_bursts; b++) free(tx[b]);
    free(tx);
    free(sym_buf);
    return 0;
}
