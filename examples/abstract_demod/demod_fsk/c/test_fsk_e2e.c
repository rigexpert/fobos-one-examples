/**
 * @file test_fsk_e2e.c
 * @brief End-to-end M-FSK modem test.  No hardware required.
 *
 * Generates a synthetic IQ stream with the structure:
 *
 *   [noise gap] [preamble + data] [noise gap] [preamble + data] ...
 *
 * Then runs three demodulation strategies and compares SER:
 *
 *   oracle   — perfect symbol timing from ground truth
 *   no-sync  — K/2 sample offset, worst-case with no timing recovery
 *   pll      — energy detector finds burst, preamble correlation finds
 *               the best sub-symbol offset within [0, K) (Gardner-like search)
 *
 * The preamble is a known alternating 0 / M-1 sequence.  Timing search
 * scores each of the K candidate offsets by the number of preamble symbols
 * correctly decoded at that offset and picks the best.
 *
 * Build: make test_fsk_e2e
 * Or standalone: gcc -O2 test_fsk_e2e.c -lliquid -lm -o test_fsk_e2e
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>

/* ---------------------------------------------------------------- params */

#define K               16      /* samples per symbol */
#define BW              0.35f   /* normalised bandwidth — valid for M ≤ 4 with K=16 */
/* Note: M=8 requires K≥128 in this liquid-dsp build to produce a unique demod map.
 * Since K is a compile-time constant shared by SAMPS_TOTAL, we test M={2,4} only. */
#define SYMS_PREAMBLE   8       /* alternating 0/M-1 sync preamble per burst */
#define SYMS_DATA       64      /* random data symbols per burst */
#define SYMS_NOISE_GAP  24      /* noise-only symbol periods between bursts */
#define N_BURSTS        5       /* TX/RX bursts per test */

#define SYMS_BURST   (SYMS_PREAMBLE + SYMS_DATA)
#define SYMS_TOTAL   (N_BURSTS * (SYMS_NOISE_GAP + SYMS_BURST) + SYMS_NOISE_GAP)
#define SAMPS_TOTAL  (SYMS_TOTAL * K)

/* ---------------------------------------------------------------- helpers */

static void box_muller(float sigma, float *re, float *im) {
    float u1 = (rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float u2 = (rand() + 1.0f) / ((float)RAND_MAX + 2.0f);
    float mag = sigma * sqrtf(-2.0f * logf(u1));
    *re = mag * cosf(2.0f * (float)M_PI * u2);
    *im = mag * sinf(2.0f * (float)M_PI * u2);
}

static liquid_float_complex gen_noise(float sigma) {
    float re, im;
    box_muller(sigma, &re, &im);
    liquid_float_complex c;
    ((float *)&c)[0] = re;
    ((float *)&c)[1] = im;
    return c;
}

static float block_power(const liquid_float_complex *s, int n) {
    float e = 0.0f;
    for (int i = 0; i < n; i++) {
        float re = ((const float *)&s[i])[0];
        float im = ((const float *)&s[i])[1];
        e += re * re + im * im;
    }
    return e / (float)n;
}

/* ------------------------------------------------------ timing recovery */

/*
 * Preamble-correlation timing search.
 *
 * Tries all K sub-symbol offsets starting at `samples`.  For each offset
 * decodes SYMS_PREAMBLE symbols and counts matches against the known
 * preamble.  Returns the offset with the highest score.
 *
 * This is equivalent to a coarse PLL acquisition: the receiver does not
 * know which sample within the symbol period is sample 0, so it tries all
 * K possibilities and picks the one the demodulator "agrees with" most.
 */
static int timing_search(fskdem dem,
                         const liquid_float_complex *samples,
                         const unsigned int *preamble,
                         int max_offset) {
    int best_offset = 0;
    int best_score  = -1;

    for (int delta = 0; delta < max_offset; delta++) {
        int score = 0;
        for (int s = 0; s < SYMS_PREAMBLE; s++) {
            unsigned int sym = fskdem_demodulate(dem,
                (liquid_float_complex *)(samples + delta + s * K));
            if (sym == preamble[s])
                score++;
        }
        if (score > best_score) {
            best_score  = score;
            best_offset = delta;
        }
    }
    return best_offset;
}

/* ----------------------------------------------------------- test runner */

typedef struct {
    int sym_errors;
    int sym_total;
    int timing_offset;  /* sub-symbol offset found by preamble search */
    int det_bursts;     /* bursts found by energy detector */
} result_t;

/* Demodulate one burst from `start_samp` with given timing offset δ.
 * Skips the preamble and fills rx_data[SYMS_DATA]. */
static void demod_burst(fskdem dem,
                        const liquid_float_complex *stream,
                        int start_samp, int delta, int stream_len,
                        unsigned int *rx_data) {
    int base = start_samp + delta + SYMS_PREAMBLE * K;
    for (int s = 0; s < SYMS_DATA; s++) {
        int pos = base + s * K;
        if (pos + K > stream_len) {
            rx_data[s] = 0xffffffff;  /* out-of-bounds sentinel */
            continue;
        }
        rx_data[s] = fskdem_demodulate(dem,
            (liquid_float_complex *)(stream + pos));
    }
}

static void run_test(unsigned int M, float snr_db, unsigned int seed,
                     result_t *r_oracle,
                     result_t *r_nosync,
                     result_t *r_pll) {
    srand(seed);

    /* SNR → noise sigma.
     * fskmod output has unit power per sample (|x|² = 1 on average).
     * For complex AWGN: noise_power = 2σ².  SNR = 1 / (2σ²).
     * So σ = sqrt(0.5 · 10^(-SNR_dB/10)). */
    float sigma = sqrtf(0.5f * powf(10.0f, -snr_db / 10.0f));

    /* energy threshold = midpoint between noise floor and signal floor */
    float e_noise  = 2.0f * sigma * sigma;
    float e_signal = 1.0f + e_noise;
    float threshold = (e_noise + e_signal) * 0.5f;

    /* preamble: alternating 0 and M-1 */
    unsigned int preamble[SYMS_PREAMBLE];
    for (int i = 0; i < SYMS_PREAMBLE; i++)
        preamble[i] = (i % 2 == 0) ? 0 : (M - 1);

    /* random TX data for each burst */
    unsigned int tx[N_BURSTS][SYMS_DATA];
    for (int b = 0; b < N_BURSTS; b++)
        for (int s = 0; s < SYMS_DATA; s++)
            tx[b][s] = (unsigned int)(rand() % M);

    /* ---- generate full IQ stream ---------------------------------------- */
    liquid_float_complex *stream = malloc(SAMPS_TOTAL * sizeof(*stream));

    fskmod mod = fskmod_create(M, K, BW);

    int pos = 0;
    int burst_start[N_BURSTS];  /* ground-truth burst start samples */

    /* leading noise gap */
    for (int s = 0; s < SYMS_NOISE_GAP; s++)
        for (int i = 0; i < K; i++)
            stream[pos++] = gen_noise(sigma);

    for (int b = 0; b < N_BURSTS; b++) {
        burst_start[b] = pos;

        /* preamble */
        liquid_float_complex sym_buf[K];
        for (int s = 0; s < SYMS_PREAMBLE; s++) {
            fskmod_modulate(mod, preamble[s], sym_buf);
            for (int i = 0; i < K; i++) {
                liquid_float_complex n = gen_noise(sigma);
                ((float *)&stream[pos])[0] = ((float *)&sym_buf[i])[0] + ((float *)&n)[0];
                ((float *)&stream[pos])[1] = ((float *)&sym_buf[i])[1] + ((float *)&n)[1];
                pos++;
            }
        }

        /* data */
        for (int s = 0; s < SYMS_DATA; s++) {
            fskmod_modulate(mod, tx[b][s], sym_buf);
            for (int i = 0; i < K; i++) {
                liquid_float_complex n = gen_noise(sigma);
                ((float *)&stream[pos])[0] = ((float *)&sym_buf[i])[0] + ((float *)&n)[0];
                ((float *)&stream[pos])[1] = ((float *)&sym_buf[i])[1] + ((float *)&n)[1];
                pos++;
            }
        }

        /* trailing noise gap */
        for (int s = 0; s < SYMS_NOISE_GAP; s++)
            for (int i = 0; i < K; i++)
                stream[pos++] = gen_noise(sigma);
    }

    fskmod_destroy(mod);

    /* ---- demodulate ------------------------------------------------------ */
    fskdem dem = fskdem_create(M, K, BW);
    unsigned int rx[SYMS_DATA];

    memset(r_oracle, 0, sizeof(*r_oracle));
    memset(r_nosync, 0, sizeof(*r_nosync));
    memset(r_pll,    0, sizeof(*r_pll));

    r_oracle->sym_total = r_nosync->sym_total = r_pll->sym_total =
        N_BURSTS * SYMS_DATA;

    /* Strategy 1: oracle (perfect timing) */
    for (int b = 0; b < N_BURSTS; b++) {
        demod_burst(dem, stream, burst_start[b], 0, SAMPS_TOTAL, rx);
        for (int s = 0; s < SYMS_DATA; s++)
            if (rx[s] != tx[b][s]) r_oracle->sym_errors++;
    }

    /* Strategy 2: no sync — K/2 offset applied to each burst */
    for (int b = 0; b < N_BURSTS; b++) {
        demod_burst(dem, stream, burst_start[b], K / 2, SAMPS_TOTAL, rx);
        for (int s = 0; s < SYMS_DATA; s++)
            if (rx[s] != tx[b][s]) r_nosync->sym_errors++;
    }

    /* Strategy 3: energy detector + preamble timing search (PLL) */
    {
        /* per-symbol-period energy */
        float *e_sym = malloc(SYMS_TOTAL * sizeof(float));
        for (int sym = 0; sym < SYMS_TOTAL; sym++)
            e_sym[sym] = block_power(stream + sym * K, K);

        int in_burst = 0, det_start_sym = -1;
        int det_burst = 0;

        for (int sym = 0; sym < SYMS_TOTAL && det_burst < N_BURSTS; sym++) {
            if (!in_burst && e_sym[sym] > threshold) {
                in_burst = 1;
                det_start_sym = sym;

                /* ---- timing search ---- */
                int samp = det_start_sym * K;
                /* need SYMS_PREAMBLE symbols + K-1 extra samples for search */
                if (samp + (SYMS_PREAMBLE * K + K) > SAMPS_TOTAL)
                    break;

                int delta = timing_search(dem, stream + samp,
                                          preamble, K);

                /* demodulate data with recovered timing */
                demod_burst(dem, stream, samp, delta, SAMPS_TOTAL, rx);

                /* match this detection to the closest TX burst */
                int data_samp = samp + delta + SYMS_PREAMBLE * K;
                int best_b = 0;
                int best_dist = abs(data_samp -
                    (burst_start[0] + SYMS_PREAMBLE * K));
                for (int b = 1; b < N_BURSTS; b++) {
                    int d = abs(data_samp -
                        (burst_start[b] + SYMS_PREAMBLE * K));
                    if (d < best_dist) { best_dist = d; best_b = b; }
                }

                for (int s = 0; s < SYMS_DATA; s++)
                    if (rx[s] != tx[best_b][s]) r_pll->sym_errors++;

                r_pll->timing_offset += delta;
                det_burst++;

            } else if (in_burst && e_sym[sym] <= threshold) {
                in_burst = 0;
            }
        }

        r_pll->det_bursts = det_burst;
        free(e_sym);
    }

    fskdem_destroy(dem);
    free(stream);
}

/* ------------------------------------------------------------------ main */

int main(void) {
    printf("End-to-end M-FSK test\n");
    printf("K=%d sps  BW=%.2f  %d bursts × (%d pre + %d data syms)  gap=%d syms\n\n",
           K, BW, N_BURSTS, SYMS_PREAMBLE, SYMS_DATA, SYMS_NOISE_GAP);

    printf("%-3s  %7s  | %-14s  %-14s  %-14s  %-9s\n",
           "M", "SNR(dB)", "oracle SER", "no-sync SER", "pll SER", "det/total");
    printf("---  -------  | ------------  ------------  ------------  ---------\n");

    int    m_vals[]  = {2, 4};
    float  snr_vals[] = {-5.0f, 0.0f, 5.0f, 10.0f, 15.0f, 20.0f};

    for (int mi = 0; mi < 2; mi++) {
        unsigned int M = (unsigned int)m_vals[mi];
        for (int si = 0; si < 6; si++) {
            result_t ro, rn, rp;
            run_test(M, snr_vals[si], 42 + (unsigned)(mi * 100 + si), &ro, &rn, &rp);

            float ser_o = (float)ro.sym_errors / ro.sym_total;
            float ser_n = (float)rn.sym_errors / rn.sym_total;
            float ser_p = (float)rp.sym_errors / rp.sym_total;

            printf("%-3u  %+7.1f  | %5d/%-5d %4.3f  %5d/%-5d %4.3f  %5d/%-5d %4.3f  %d/%d\n",
                   M, snr_vals[si],
                   ro.sym_errors, ro.sym_total, ser_o,
                   rn.sym_errors, rn.sym_total, ser_n,
                   rp.sym_errors, rp.sym_total, ser_p,
                   rp.det_bursts, N_BURSTS);
        }
        printf("\n");
    }

    return 0;
}
