/**
 * @file test_psk_e2e.c
 * @brief End-to-end M-PSK modem test. No hardware required.
 *
 * Generates a synthetic symbol stream, applies a continuous carrier frequency
 * offset and AWGN, then compares three demodulation strategies:
 *
 *   oracle   — perfect frequency knowledge; rotate each sample by the exact
 *               accumulated phase before demodulating.  Sets the noise floor.
 *
 *   no-sync  — demodulate with no carrier correction.  The frequency offset
 *               causes the carrier phase to accumulate continuously, crossing
 *               symbol decision boundaries repeatedly → SER → (1 - 1/M) at
 *               high SNR (the phase is effectively uniformly distributed).
 *
 *   pll      — nco_crcf Costas loop tracks the carrier frequency offset.
 *               After N_TRAIN convergence symbols the loop has locked; errors
 *               are counted over the following N_DATA symbols.  At high SNR
 *               the PLL SER approaches the oracle floor.
 *
 * Frequency offset model:
 *   rx[i] = tx_iq[i] · exp(j · 2π · FREQ_OFFSET · i) + AWGN
 *
 * FREQ_OFFSET = 0.01 cycles/symbol (≈ 100 ppm at 100 kHz symbol rate).
 * Over N_DATA=800 symbols this produces 8 full carrier rotations — the
 * no-sync receiver loses all phase reference even at arbitrarily high SNR.
 * The PLL (starting from zero frequency estimate) converges within N_TRAIN
 * symbols and then tracks the offset with near-zero residual error.
 *
 * Note on PSK phase ambiguity:
 *   M-PSK has an M-fold carrier phase ambiguity.  A pure static offset test
 *   would require differential encoding to resolve which of the M equivalent
 *   lock points the Costas loop chose.  A frequency offset avoids this: the
 *   nearest stable PLL lock is at FREQ_OFFSET (the true value), and all other
 *   stable points are separated by ±1/M cycles/symbol ≫ FREQ_OFFSET.
 *
 * Build: make test_psk_e2e
 * Or standalone: gcc -O2 test_psk_e2e.c -lliquid -lm -o test_psk_e2e
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <liquid/liquid.h>

/* ---------------------------------------------------------------- params */

#define N_TRAIN      600       /* symbols for PLL convergence, not counted in SER */
#define N_DATA       800       /* evaluated symbols after PLL convergence */
#define N_TOTAL      (N_TRAIN + N_DATA)
#define K_SEEDS      8         /* trials averaged per (M, SNR) point */

/* Frequency offset in cycles per symbol. 0.01 = 1% of symbol rate.
 * Over N_DATA=800 data symbols the carrier rotates 8 full turns → no-sync
 * SER → (M-1)/M at high SNR (uniformly distributed phase decisions). */
#define FREQ_OFFSET  0.01f

/* Costas loop bandwidth (normalised to symbol rate).
 * Wide enough for fast acquisition but the M-fold phase ambiguity may cause
 * false lock with random training data; averaging over K_SEEDS trials gives
 * stable statistics.  Real systems resolve ambiguity with differential
 * encoding (DPSK) or a known pilot/preamble sequence. */
#define PLL_BW       0.10f

/* ---------------------------------------------------------------- types */

typedef struct {
    int sym_errors;
    int sym_total;
} result_t;

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

static modulation_scheme m_to_psk(unsigned int M) {
    switch (M) {
        case 2:  return LIQUID_MODEM_PSK2;
        case 4:  return LIQUID_MODEM_PSK4;
        case 8:  return LIQUID_MODEM_PSK8;
        case 16: return LIQUID_MODEM_PSK16;
        default:
            fprintf(stderr, "unsupported M=%u\n", M);
            exit(1);
    }
}

static liquid_float_complex rotate_sample(liquid_float_complex x, float theta) {
    float c = cosf(theta), s = sinf(theta);
    float xr = ((float *)&x)[0], xi = ((float *)&x)[1];
    liquid_float_complex y;
    ((float *)&y)[0] = xr * c - xi * s;
    ((float *)&y)[1] = xr * s + xi * c;
    return y;
}

/* ----------------------------------------------------------- test runner */

static void run_test(unsigned int M, float snr_db, unsigned int seed,
                     result_t *r_oracle,
                     result_t *r_nosync,
                     result_t *r_pll) {
    srand(seed);

    modulation_scheme scheme = m_to_psk(M);

    /* SNR → noise sigma:  PSK symbols have |x|²=1, complex AWGN power=2σ²
     * SNR_linear = 1/(2σ²)  →  σ = sqrt(0.5 · 10^(-SNR/10)) */
    float sigma = sqrtf(0.5f * powf(10.0f, -snr_db / 10.0f));

    /* generate random TX symbols */
    unsigned int tx[N_TOTAL];
    for (int i = 0; i < N_TOTAL; i++)
        tx[i] = (unsigned int)(rand() % M);

    /* modulate → apply continuous frequency offset → add AWGN */
    liquid_float_complex rx[N_TOTAL];
    {
        modemcf mod = modemcf_create(scheme);
        for (int i = 0; i < N_TOTAL; i++) {
            liquid_float_complex s;
            modemcf_modulate(mod, tx[i], &s);
            /* accumulated carrier phase: 2π · FREQ_OFFSET · sample_index */
            float phi = 2.0f * (float)M_PI * FREQ_OFFSET * (float)i;
            liquid_float_complex rotated = rotate_sample(s, phi);
            liquid_float_complex noise = gen_noise(sigma);
            ((float *)&rx[i])[0] = ((float *)&rotated)[0] + ((float *)&noise)[0];
            ((float *)&rx[i])[1] = ((float *)&rotated)[1] + ((float *)&noise)[1];
        }
        modemcf_destroy(mod);
    }

    memset(r_oracle, 0, sizeof(*r_oracle));
    memset(r_nosync, 0, sizeof(*r_nosync));
    memset(r_pll,    0, sizeof(*r_pll));
    r_oracle->sym_total = r_nosync->sym_total = r_pll->sym_total = N_DATA;

    /* ---- oracle: exact phase correction per sample -------------------- */
    {
        modemcf dem = modemcf_create(scheme);
        for (int i = N_TRAIN; i < N_TOTAL; i++) {
            float phi = -2.0f * (float)M_PI * FREQ_OFFSET * (float)i;
            liquid_float_complex corrected = rotate_sample(rx[i], phi);
            unsigned int s;
            modemcf_demodulate(dem, corrected, &s);
            if (s != tx[i]) r_oracle->sym_errors++;
        }
        modemcf_destroy(dem);
    }

    /* ---- no-sync: demodulate directly, no carrier correction ---------- */
    {
        modemcf dem = modemcf_create(scheme);
        for (int i = N_TRAIN; i < N_TOTAL; i++) {
            unsigned int s;
            modemcf_demodulate(dem, rx[i], &s);
            if (s != tx[i]) r_nosync->sym_errors++;
        }
        modemcf_destroy(dem);
    }

    /* ---- pll: Costas loop tracks frequency offset --------------------- */
    {
        modemcf  dem = modemcf_create(scheme);
        nco_crcf nco = nco_crcf_create(LIQUID_NCO);
        nco_crcf_pll_set_bandwidth(nco, PLL_BW);
        /* NCO starts at zero frequency — must acquire FREQ_OFFSET during N_TRAIN */

        for (int i = 0; i < N_TOTAL; i++) {
            liquid_float_complex sym;
            nco_crcf_mix_down(nco, rx[i], &sym);

            unsigned int s;
            modemcf_demodulate(dem, sym, &s);

            float phase_err = modemcf_get_demodulator_phase_error(dem);
            nco_crcf_pll_step(nco, phase_err);
            nco_crcf_step(nco);

            if (i >= N_TRAIN && s != tx[i])
                r_pll->sym_errors++;
        }

        nco_crcf_destroy(nco);
        modemcf_destroy(dem);
    }
}

/* ------------------------------------------------------------------ main */

int main(void) {
    printf("End-to-end M-PSK test\n");
    printf("N_TRAIN=%d  N_DATA=%d  K_SEEDS=%d  FREQ_OFFSET=%.4f cycles/sym  PLL_BW=%.3f\n\n",
           N_TRAIN, N_DATA, K_SEEDS, FREQ_OFFSET, PLL_BW);

    /* Show theoretical no-sync asymptote: phase rotates many times,
     * SER → (M-1)/M as phase becomes uniformly distributed */
    printf("Theoretical no-sync SER at very high SNR:\n");
    printf("  BPSK(2): %.2f   QPSK(4): %.2f   8-PSK(8): %.2f\n\n",
           1.0 - 1.0/2, 1.0 - 1.0/4, 1.0 - 1.0/8);

    printf("%-5s  %7s  | %-16s  %-16s  %-16s\n",
           "M", "SNR(dB)", "oracle SER", "no-sync SER", "pll SER");
    printf("-----  -------  | --------------  --------------  --------------\n");

    unsigned int m_vals[]  = {2, 4, 8};
    float        snr_vals[] = {-3.0f, 0.0f, 3.0f, 5.0f, 8.0f, 10.0f, 15.0f, 20.0f};

    for (int mi = 0; mi < 3; mi++) {
        unsigned int M = m_vals[mi];
        for (int si = 0; si < 8; si++) {
            result_t ro_acc = {0, 0}, rn_acc = {0, 0}, rp_acc = {0, 0};

            for (int k = 0; k < K_SEEDS; k++) {
                result_t ro, rn, rp;
                unsigned int seed = 42u + (unsigned)(mi * 1000 + si * 100 + k);
                run_test(M, snr_vals[si], seed, &ro, &rn, &rp);
                ro_acc.sym_errors += ro.sym_errors;
                ro_acc.sym_total  += ro.sym_total;
                rn_acc.sym_errors += rn.sym_errors;
                rn_acc.sym_total  += rn.sym_total;
                rp_acc.sym_errors += rp.sym_errors;
                rp_acc.sym_total  += rp.sym_total;
            }

            float ser_o = (float)ro_acc.sym_errors / ro_acc.sym_total;
            float ser_n = (float)rn_acc.sym_errors / rn_acc.sym_total;
            float ser_p = (float)rp_acc.sym_errors / rp_acc.sym_total;

            printf("%-5u  %+7.1f  | %5d/%-5d %5.3f  %5d/%-5d %5.3f  %5d/%-5d %5.3f\n",
                   M, snr_vals[si],
                   ro_acc.sym_errors, ro_acc.sym_total, ser_o,
                   rn_acc.sym_errors, rn_acc.sym_total, ser_n,
                   rp_acc.sym_errors, rp_acc.sym_total, ser_p);
        }
        printf("\n");
    }

    return 0;
}
