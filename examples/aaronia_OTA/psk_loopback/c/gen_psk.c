/*
 * gen_psk.c — M-PSK burst IQ generator for loopback tests.
 *
 * Produces N bursts, each structured as:
 *   [NOISE_GAP symbol-periods of AWGN]
 *   [PREAMBLE_SYMS BPSK pilot symbols (all 0) for carrier acquisition]
 *   [data_syms M-PSK data symbols]
 *   (trailing gap after last burst)
 *
 * Each PSK symbol occupies K samples (rectangular pulse, upsampled).
 *
 * Output:
 *   stdout : raw float32 interleaved IQ (I0 Q0 I1 Q1 …)
 *   stderr : one "INFO …" line, then one "TXSYM b=N n=D sym0 sym1 …" per burst
 *
 * Usage: gen_psk M K n_bursts data_syms seed [noise_sigma]
 *   M           PSK order: 2=BPSK, 4=QPSK, 8=8PSK
 *   K           samples per symbol (≥4)
 *   n_bursts    number of bursts
 *   data_syms   data symbols per burst
 *   seed        RNG seed
 *   noise_sigma AWGN per-component sigma (default 0.05)
 */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <liquid/liquid.h>

#define PREAMBLE_SYMS 16
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

static void emit_symbol(liquid_float_complex s, int K, float sigma)
{
    for (int i = 0; i < K; i++) {
        float nr, ni;
        box_muller(sigma, &nr, &ni);
        write_f32(crealf(s) + nr);
        write_f32(cimagf(s) + ni);
    }
}

int main(int argc, char **argv)
{
    if (argc < 6) {
        fprintf(stderr, "Usage: %s M K n_bursts data_syms seed [noise_sigma]\n", argv[0]);
        return 1;
    }
    unsigned int M        = (unsigned int)atoi(argv[1]);
    int          K        = atoi(argv[2]);
    int          n_bursts = atoi(argv[3]);
    int          data_syms = atoi(argv[4]);
    unsigned int seed     = (unsigned int)atoi(argv[5]);
    float        sigma    = argc > 6 ? atof(argv[6]) : 0.05f;

    if (M < 2 || (M & (M - 1))) {
        fprintf(stderr, "M must be a power of 2\n"); return 1;
    }

    modulation_scheme ms;
    switch (M) {
        case 2:  ms = LIQUID_MODEM_BPSK;  break;
        case 4:  ms = LIQUID_MODEM_QPSK;  break;
        case 8:  ms = LIQUID_MODEM_PSK8;  break;
        case 16: ms = LIQUID_MODEM_PSK16; break;
        default: fprintf(stderr, "Unsupported M\n"); return 1;
    }

    srand(seed);
    modem mod = modem_create(ms);

    /* TX data */
    unsigned int **tx = malloc((size_t)n_bursts * sizeof(unsigned int *));
    for (int b = 0; b < n_bursts; b++) {
        tx[b] = malloc((size_t)data_syms * sizeof(unsigned int));
        for (int s = 0; s < data_syms; s++)
            tx[b][s] = (unsigned int)(rand() % (int)M);
    }

    fprintf(stderr, "INFO M=%u K=%d n_bursts=%d data_syms=%d preamble=%d gap=%d\n",
            M, K, n_bursts, data_syms, PREAMBLE_SYMS, NOISE_GAP);

    liquid_float_complex sample;

    /* leading noise gap */
    for (int s = 0; s < NOISE_GAP; s++) {
        float re, im; box_muller(sigma, &re, &im);
        for (int i = 0; i < K; i++) {
            float nr, ni; box_muller(sigma, &nr, &ni);
            write_f32(re + nr); write_f32(im + ni);
        }
    }

    for (int b = 0; b < n_bursts; b++) {
        /* BPSK preamble: all symbol 0 — known phase reference */
        for (int s = 0; s < PREAMBLE_SYMS; s++) {
            modem_modulate(mod, 0, &sample);
            emit_symbol(sample, K, sigma);
        }
        /* data */
        for (int s = 0; s < data_syms; s++) {
            modem_modulate(mod, tx[b][s], &sample);
            emit_symbol(sample, K, sigma);
        }
        /* trailing noise gap */
        for (int s = 0; s < NOISE_GAP; s++) {
            float re, im; box_muller(sigma, &re, &im);
            for (int i = 0; i < K; i++) {
                float nr, ni; box_muller(sigma, &nr, &ni);
                write_f32(re + nr); write_f32(im + ni);
            }
        }

        fprintf(stderr, "TXSYM b=%d n=%d", b, data_syms);
        for (int s = 0; s < data_syms; s++)
            fprintf(stderr, " %u", tx[b][s]);
        fprintf(stderr, "\n");
    }

    modem_destroy(mod);
    for (int b = 0; b < n_bursts; b++) free(tx[b]);
    free(tx);
    return 0;
}
