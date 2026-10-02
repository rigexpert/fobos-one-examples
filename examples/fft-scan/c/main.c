/**
 * @file main.c
 * @brief Wideband FFT spectrum scanner — Fobos SDR developer example.
 *
 * Steps the Fobos SDR centre frequency from -f (from) to -t (to) using the
 * hardware-internal scanning API (fobos_sdr_start_scan).  The hardware manages
 * frequency stepping and filters buffers captured while re-tuning; only
 * settled IQ data reaches the ring buffer, tagged with its centre frequency.
 *
 * @section pipeline Processing Pipeline (per sweep)
 *
 * @verbatim
 * Hardware scan loop (automatic):
 *   freq[0] → [settle] → IQ chunk[0]  (tagged with freq[0])
 *   freq[1] → [settle] → IQ chunk[1]  (tagged with freq[1])
 *   ...
 *
 * Consumer (one iteration):
 *   for each of n_steps chunks from ring buffer:
 *     look up step from chunk frequency tag
 *     for average iterations:
 *       fft_apply_window → fft_execute → fft_accumulate_power
 *     fft_log + DC-centred crop → panorama[step * bins_per_step]
 *   draw_fft(panorama)
 * @endverbatim
 *
 * @section quickstart Quick Start
 *
 * Scan the FM broadcast band (88–108 MHz) at 25 MSPS with a 2048-point FFT:
 * @code
 *   ./fft-scan -f 88000000 -t 108000000 -r 25000000 -w 2048 -a 50
 * @endcode
 *
 * @section notes Notes
 *
 * - Effective bandwidth per step = sample_rate × FOBOS_AUTO_BW (see receiver.h).
 * - hw_sps (hardware samples per step) = round-up-to-8192( max(65536, average*fft_width) ).
 * - Each hw_sps chunk provides exactly average FFT windows.
 * - DC-centred crop takes the center bins_per_step bins from each averaged FFT,
 *   mapping frequency exactly (no manual retune timing or skip logic needed).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <assert.h>
#include <signal.h>

#include "fft.h"
#include "receiver.h"
#include "draw.h"
#include "sgram.h"
#include "spectrum_png.h"

#define MAX_DB              0.0f
#define MIN_DB           -130.0f

#define DEFAULT_FFT_WIDTH   2048
#define DEFAULT_FREQ_FROM   88000000LL
#define DEFAULT_FREQ_TO    108000000LL
#define DEFAULT_SAMPLERATE 25000000L
#define DEFAULT_BASELEVEL  -75
#define DEFAULT_AVERAGE     50

/* Fobos hardware constraints (FOBOS_AUTO_BW comes from receiver.h) */
#define FOBOS_HW_SPS_ALIGN  8192        /* hardware DMA buffer size granularity (samples) */
#define FOBOS_MIN_HW_SPS    65536       /* minimum hardware samples per scan step */

/* Receiver ring-buffer depth (chunks) */
#define RB_CHUNK_COUNT      32

/* Number of sweeps to discard at startup before computing auto min/max scale */
#define SCAN_WARMUP_SWEEPS  3

/* Default memory budget for the spectrogram pixel buffer */
#define SGRAM_DEFAULT_MEM_MB  256

typedef struct ctx_t {
    int       run;
    int       fft_width;
    int       average;
    int       baselevel;
    int       verbose;
    long long freq_from;
    long long freq_to;
    int       rate;
    int       lna_gain;
    int       vga_gain;
    double    overlap_hz;      /* step overlap in Hz (0 = no overlap) */
    int       fft_overlap;     /* FFT window hop size in samples (0 = fft_width, no overlap) */
    float     display_min_db;
    float     display_max_db;
    int       auto_min;
    int       auto_max;
    int       warm_frames;     /* sweeps discarded before auto min/max is computed */
    /* spectrogram */
    char     *sgram_file;      /* -G: output PNG filename; NULL = no PNG output */
    int       n_frames;        /* -n: stop after this many productive sweeps (0 = unlimited) */
    size_t    sgram_mem_mb;    /* -S: pixel buffer memory limit in MB */
    int       max_hold;        /* -x: enable per-bin max-hold display */
    int       max_hold_csv;    /* -xc: save maxhold.csv on exit */
    int       max_hold_png;    /* -xg: save maxhold.png on exit */
} ctx_t;

static ctx_t ctx;

static float    *max_hold_buf = NULL;  /* per-bin panorama peak; NULL when disabled */

static complexf *f32_input   = NULL;
static complexf *fft_output  = NULL;
static complexf *windowed    = NULL;
static float    *windowt     = NULL;
static float    *avrg_output    = NULL;
static float    *panorama       = NULL;
static float    *chunk_buf      = NULL;
static float    *step_buf       = NULL;   /* per-step dB buffer: main + overlap bins */
static float    *prev_right_ov  = NULL;   /* right-overlap dB bins saved from previous step */

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void signal_handler(int signum) {
    static int count = 0;
    fprintf(stderr, "signal %d\n", signum);
    if (count++)
        exit(-1);
    ctx.run = 0;
}

static void print_help(void) {
    printf("usage: fft-scan [options]\n");
    printf("  -f <Hz>   from frequency (default %lld)\n", DEFAULT_FREQ_FROM);
    printf("  -t <Hz>   to frequency   (default %lld)\n", DEFAULT_FREQ_TO);
    printf("  -r <Hz>   sample rate    (default %ld)\n",  DEFAULT_SAMPLERATE);
    printf("  -w <N>    FFT width, power of 2 (default %d)\n", DEFAULT_FFT_WIDTH);
    printf("  -a <N>    FFTs averaged per step (default %d)\n", DEFAULT_AVERAGE);
    printf("  -b <dB>   base level dB  (default %d)\n",  DEFAULT_BASELEVEL);
    printf("  -L <N>    LNA gain 0..3   (0,1: 0 dB, 2: +16 dB, 3: +33 dB)\n");
    printf("  -V <N>    VGA gain 0..31  (0..+62 dB, 2 dB step)\n");
    printf("  -s <N>    FFT hop size in samples (default = fft_width, no window overlap)\n");
    printf("  -O <Hz>   step overlap in Hz (default 0); adjacent steps share this bandwidth,\n");
    printf("            keeping step boundaries inside each filter's flat passband.\n");
    printf("            Trade-off: more steps → slower sweep.\n");
    printf("  -m <dB>   display min dB\n");
    printf("  -M <dB>   display max dB\n");
    printf("  -G <file> write spectrogram PNG to <file> on exit (newest row at top)\n");
    printf("  -n <N>    exit after N productive sweeps and save spectrogram\n");
    printf("  -S <MB>   spectrogram memory limit in MB (default %d)\n", SGRAM_DEFAULT_MEM_MB);
    printf("  -x[c][g]  enable max-hold display (yellow ▄ tick per bin); press 'c' to clear\n");
    printf("            add 'c' to also save maxhold.csv on exit\n");
    printf("            add 'g' to also save maxhold.png on exit (capped at %d px wide)\n",
           SPECTRUM_PNG_MAX_W);
    printf("  -v        verbose\n");
    printf("  -h        help\n");
}

static int is_power_of_two(int x) {
    return x > 0 && (x & (x - 1)) == 0;
}

static void parse_args(int argc, char *argv[]) {
    int opt;
    while ((opt = getopt(argc, argv, "f:t:r:w:a:b:s:O:L:V:m:M:G:n:S:x::vh")) != -1) {
        switch (opt) {
            case 'f': ctx.freq_from      = atoll(optarg); break;
            case 't': ctx.freq_to        = atoll(optarg); break;
            case 'r': ctx.rate           = atol(optarg);  break;
            case 'w':
                ctx.fft_width = atoi(optarg);
                if (!is_power_of_two(ctx.fft_width)) {
                    fprintf(stderr, "fft_width must be a power of 2\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'a':
                ctx.average = atoi(optarg);
                if (ctx.average <= 0) {
                    fprintf(stderr, "average must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'b': ctx.baselevel      = atoi(optarg);    break;
            case 's':
                ctx.fft_overlap = atoi(optarg);
                if (ctx.fft_overlap <= 0) {
                    fprintf(stderr, "-s hop size must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'O': ctx.overlap_hz     = atof(optarg);    break;
            case 'L': ctx.lna_gain       = atoi(optarg);    break;
            case 'V': ctx.vga_gain       = atoi(optarg);    break;
            case 'm': ctx.display_min_db = atof(optarg); ctx.auto_min = 0; break;
            case 'M': ctx.display_max_db = atof(optarg); ctx.auto_max = 0; break;
            case 'G': ctx.sgram_file     = optarg;           break;
            case 'n':
                ctx.n_frames = atoi(optarg);
                if (ctx.n_frames <= 0) {
                    fprintf(stderr, "-n must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'S':
                ctx.sgram_mem_mb = (size_t)atoi(optarg);
                if (ctx.sgram_mem_mb == 0) {
                    fprintf(stderr, "-S must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'x':
                ctx.max_hold = 1;
                if (optarg) {
                    for (const char *p = optarg; *p; p++) {
                        if (*p == 'c')
                            ctx.max_hold_csv = 1;
                        if (*p == 'g')
                            ctx.max_hold_png = 1;
                    }
                }
                break;
            case 'v': ctx.verbose        = 1;               break;
            case 'h': print_help(); exit(EXIT_SUCCESS);
            default:  exit(EXIT_FAILURE);
        }
    }
}

/* Round up n to the nearest multiple of FOBOS_HW_SPS_ALIGN, minimum FOBOS_MIN_HW_SPS. */
static int hw_samples(int n) {
    if (n < FOBOS_MIN_HW_SPS)
        n = FOBOS_MIN_HW_SPS;
    return ((n + FOBOS_HW_SPS_ALIGN - 1) / FOBOS_HW_SPS_ALIGN) * FOBOS_HW_SPS_ALIGN;
}

/* Return the index of the frequency in freqs[] nearest to target. */
static int nearest_step(double target, double *freqs, int count) {
    int best = 0;
    double best_dist = fabs(target - freqs[0]);
    for (int i = 1; i < count; i++) {
        double d = fabs(target - freqs[i]);
        if (d < best_dist) {
            best_dist = d;
            best = i;
        }
    }
    return best;
}

int main(int argc, char *argv[]) {
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    memset(&ctx, 0, sizeof(ctx_t));
    ctx.run           = 1;
    ctx.fft_width     = DEFAULT_FFT_WIDTH;
    ctx.freq_from     = DEFAULT_FREQ_FROM;
    ctx.freq_to       = DEFAULT_FREQ_TO;
    ctx.rate          = DEFAULT_SAMPLERATE;
    ctx.average       = DEFAULT_AVERAGE;
    ctx.baselevel     = DEFAULT_BASELEVEL;
    ctx.lna_gain      = -1;
    ctx.vga_gain      = -1;
    ctx.display_min_db = 0.0f;
    ctx.display_max_db = 0.0f;
    ctx.auto_min       = 1;
    ctx.auto_max       = 1;
    ctx.sgram_file     = NULL;
    ctx.n_frames       = 0;
    ctx.sgram_mem_mb   = SGRAM_DEFAULT_MEM_MB;

    parse_args(argc, argv);

    if (ctx.freq_from >= ctx.freq_to) {
        fprintf(stderr, "error: -f must be less than -t\n");
        exit(EXIT_FAILURE);
    }

    /* ---- scan geometry -------------------------------------------------- */
    double bw_per_step = ctx.rate * FOBOS_AUTO_BW;
    long long span     = ctx.freq_to - ctx.freq_from;

    if (ctx.overlap_hz < 0.0 || ctx.overlap_hz >= bw_per_step) {
        fprintf(stderr, "error: -O overlap must be in [0, %.0f) Hz (< RTBW)\n", bw_per_step);
        exit(EXIT_FAILURE);
    }

    /* With overlap, steps are spaced closer together so each step's display
     * region uses only the flat centre of the hardware filter; the roll-off
     * edges are captured but used only for boundary blending with neighbours. */
    double step_spacing = bw_per_step - ctx.overlap_hz;
    int n_steps         = (int)ceil((double)span / step_spacing);
    if (n_steps < 2) {
        n_steps = 2;
        fprintf(stderr, "note: span fits in one step; hardware requires >= 2, using 2\n");
    }
    if (n_steps > FOBOS_MAX_FREQS_CNT) {
        fprintf(stderr, "error: %d steps required but hardware limit is %d.\n"
                        "       Increase sample rate, reduce span, or reduce overlap.\n",
                n_steps, FOBOS_MAX_FREQS_CNT);
        exit(EXIT_FAILURE);
    }
    double actual_spacing = (double)span / n_steps;   /* adjusted to cover span exactly */

    /* bins_per_step: panorama bins contributed by each step (non-overlapping display). */
    int bins_per_step = (int)round((double)ctx.fft_width * actual_spacing / ctx.rate);

    /* hov: half-overlap in FFT bins — extra bins extracted on each side of the
     * crop for boundary blending.  0 when overlap_hz == 0 (no blending). */
    int hov = (int)round((double)ctx.fft_width * ctx.overlap_hz / (2.0 * ctx.rate));

    /* Hardware samples per step: enough for ctx.average FFT windows, rounded
     * up to the nearest multiple of 8192 required by the Fobos scanner. */
    int hop    = (ctx.fft_overlap > 0) ? ctx.fft_overlap : ctx.fft_width;
    int hw_sps = hw_samples((ctx.average - 1) * hop + ctx.fft_width);

    double *step_freqs = malloc(n_steps * sizeof(double));
    assert(step_freqs);
    for (int s = 0; s < n_steps; s++)
        step_freqs[s] = ctx.freq_from + actual_spacing * (s + 0.5);

    /* ---- average baselevel correction ----------------------------------- */
    ctx.baselevel -= (int)(10.0 * log10((double)ctx.average));

    /* ---- startup info ---------------------------------------------------- */
    printf("fft-scan (hardware scan mode)\n");
    printf("  range     : %.3f - %.3f MHz\n", ctx.freq_from / 1e6, ctx.freq_to / 1e6);
    printf("  span      : %.3f MHz\n", span / 1e6);
    printf("  steps     : %d  (%.3f MHz spacing, %.3f MHz display/step)\n",
           n_steps, actual_spacing / 1e6, actual_spacing / 1e6);
    printf("  bw/step   : %.3f MHz  (80%% of %.0f MSPS)\n", bw_per_step / 1e6, ctx.rate / 1e6);
    if (ctx.overlap_hz > 0.0)
        printf("  overlap   : %.3f MHz  (%d bins/side blended at boundaries)\n",
               ctx.overlap_hz / 1e6, hov);
    printf("  bins/step : %d (%.0f%% of FFT, %.1f kHz/bin)\n",
           bins_per_step, 100.0 * bins_per_step / ctx.fft_width,
           ctx.rate / 1e3 / ctx.fft_width);
    printf("  total bins: %d\n", n_steps * bins_per_step);
    printf("  hop       : %d samples%s\n",
           hop, hop == ctx.fft_width ? " (no window overlap)" : " (windowed overlap)");
    printf("  hw_sps    : %d samples/step (%d FFTs/chunk)\n",
           hw_sps, ctx.average);

    /* ---- spectrogram init ------------------------------------------------- */
    int panorama_bins = n_steps * bins_per_step;
    sgram_t *sgram = NULL;
    if (ctx.sgram_file) {
        size_t mem_bytes = ctx.sgram_mem_mb * 1024UL * 1024UL;
        sgram = sgram_create(panorama_bins, mem_bytes);
        if (!sgram) {
            fprintf(stderr, "failed to allocate spectrogram buffer\n");
            exit(EXIT_FAILURE);
        }
        int cap_rows = (int)(mem_bytes / ((size_t)panorama_bins * 3));
        printf("  sgram     : %s  (%d rows capacity, %zu MB)\n",
               ctx.sgram_file, cap_rows, ctx.sgram_mem_mb);
        if (ctx.n_frames > 0)
            printf("  n_frames  : %d\n", ctx.n_frames);

        sgram_params_t sp = {
            .freq_from_hz = (double)ctx.freq_from,
            .freq_to_hz   = (double)ctx.freq_to,
            .rate         = ctx.rate,
            .fft_width    = ctx.fft_width,
            .average      = ctx.average,
            .n_steps      = n_steps,
            .overlap_hz   = ctx.overlap_hz,
        };
        sgram_set_params(sgram, &sp);
    }

    /* ---- receiver init --------------------------------------------------- */
    printf("initializing fobos...\n");
    if (receiver_init(hw_sps, RB_CHUNK_COUNT) != 0) {
        fprintf(stderr, "failed to init receiver\n");
        exit(EXIT_FAILURE);
    }
    if (ctx.lna_gain >= 0) {
        printf("set LNA gain to %d\n", ctx.lna_gain);
        if (receiver_set_lna(ctx.lna_gain) != 0)
            fprintf(stderr, "failed to set LNA gain\n");
    }
    if (ctx.vga_gain >= 0) {
        printf("set VGA gain to %d\n", ctx.vga_gain);
        if (receiver_set_vga(ctx.vga_gain) != 0)
            fprintf(stderr, "failed to set VGA gain\n");
    }
    printf("set sample rate to %d\n", ctx.rate);
    if (receiver_set_samplerate(ctx.rate) != 0) {
        fprintf(stderr, "failed to set sample rate\n");
        exit(EXIT_FAILURE);
    }

    /* ---- buffer allocation ----------------------------------------------- */
    f32_input   = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    fft_output  = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    windowed    = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    windowt     = malloc(sizeof(float) * ctx.fft_width);
    avrg_output = malloc(sizeof(float) * ctx.fft_width);
    panorama      = calloc(panorama_bins, sizeof(float));
    chunk_buf     = malloc((size_t)hw_sps * 2 * sizeof(float));
    step_buf      = malloc((bins_per_step + 2 * hov) * sizeof(float));
    prev_right_ov = hov > 0 ? malloc(hov * sizeof(float)) : NULL;
    if (ctx.max_hold) {
        max_hold_buf = malloc((size_t)panorama_bins * sizeof(float));
        assert(max_hold_buf);
        for (int i = 0; i < panorama_bins; i++)
            max_hold_buf[i] = -INFINITY;
    }
    assert(f32_input && fft_output && windowed &&
           windowt && avrg_output && panorama && chunk_buf && step_buf);

    fft_precalculate_window(windowt, ctx.fft_width);

    printf("create FFT plan...\n");
    fft_load_wisdom("fftw_wisdom");
    fft_plan_s *fft_plan = fft_create_plan(ctx.fft_width, windowed, fft_output);
    fft_save_wisdom("fftw_wisdom");

    /* ---- display init ---------------------------------------------------- */
    uint64_t display_cf   = (uint64_t)((ctx.freq_from + ctx.freq_to) / 2);
    uint64_t display_rate = (uint64_t)span;

    draw_init(display_cf, display_rate,
              ctx.baselevel, 0, ctx.average, n_steps * bins_per_step,
              MIN_DB, MAX_DB);
    draw_hide_cursor();
    if (ctx.max_hold)
        draw_enable_keypress();

    fflush(stdout);
    setvbuf(stdout, NULL, _IOFBF, 1 << 18);

    printf("starting hardware scan...\n");
    if (receiver_start_scan(step_freqs, n_steps, hw_sps) != 0) {
        fprintf(stderr, "failed to start hardware scan\n");
        exit(EXIT_FAILURE);
    }

    /* ---- main scan loop -------------------------------------------------- */
    int sweep_count = 0;
    while (ctx.run) {
        long t_start = now_ns();

        /* Collect one complete sweep: one settled chunk per step.
         * The hardware cycles steps 0→1→…→(n-1)→0→…; each chunk in the ring
         * buffer is tagged with its centre frequency so we can route it to the
         * correct panorama slot regardless of which step we start reading from. */
        for (int i = 0; i < n_steps && ctx.run; i++) {
            double freq_hz = 0.0;
            int samples = receiver_read_scan_chunk(chunk_buf, (size_t)hw_sps, &freq_hz);
            if (samples <= 0)
                break;

            int step = nearest_step(freq_hz, step_freqs, n_steps);

            /* Accumulate ctx.average FFT windows from the chunk.
             * Windows are taken sequentially from the start of chunk_buf. */
            memset(avrg_output, 0, sizeof(float) * ctx.fft_width);
            for (int iter = 0; iter < ctx.average; iter++) {
                complexf *src = ((complexf *)chunk_buf) + iter * hop;
                fft_apply_precalculated_window(src, windowed, ctx.fft_width, windowt);
                fft_execute(fft_plan);
                fft_accumulate_power((complexf *)fft_output, avrg_output, ctx.fft_width);
            }

            /* DC-centred crop → step_buf, then blend into panorama.
             *
             * step_buf layout (bins_per_step + 2*hov total):
             *   [0 .. hov-1]               left overlap  (lower freqs, shared with prev step)
             *   [hov .. hov+bps-1]         main display  (→ panorama slot)
             *   [hov+bps .. 2*hov+bps-1]   right overlap (higher freqs, shared with next step)
             *
             * htotal = half of total extract; fft_log writes neg freqs first then pos freqs
             * so step_buf[0] = most negative freq extracted, step_buf[htotal] ≈ DC. */
            int total_extract = bins_per_step + 2 * hov;
            int htotal        = total_extract / 2;
            fft_log(avrg_output + ctx.fft_width - htotal, step_buf,
                    htotal,                ctx.baselevel);
            fft_log(avrg_output,                           step_buf + htotal,
                    total_extract - htotal, ctx.baselevel);

            /* Write main display region to panorama. */
            float *dst = panorama + step * bins_per_step;
            memcpy(dst, step_buf + hov, bins_per_step * sizeof(float));

            /* Blend left boundary: average the leftmost hov bins of the main
             * region with the right-overlap saved from the previous step.
             * Both cover the same frequencies seen through adjacent filters. */
            if (step > 0 && hov > 0) {
                for (int j = 0; j < hov; j++)
                    dst[j] = (dst[j] + prev_right_ov[j]) * 0.5f;
            }

            /* Save right overlap for the next step's left-boundary blend. */
            if (hov > 0)
                memcpy(prev_right_ov, step_buf + hov + bins_per_step,
                       hov * sizeof(float));
        }

        double sweep_s    = (now_ns() - t_start) * 1e-9;
        double scan_mhz_s = (double)span / 1e6 / sweep_s;

        /* Discard first 3 sweeps — they may contain init artefacts. */
        if (ctx.warm_frames < SCAN_WARMUP_SWEEPS) {
            ctx.warm_frames++;
            continue;
        }

        /* Auto min/max: computed from data on the first clean sweep, then frozen. */
        if (ctx.auto_min || ctx.auto_max) {
            float pmin =  1e30f, pmax = -1e30f;
            for (int i = 0; i < panorama_bins; i++) {
                float v = panorama[i];
                if (v < pmin)
                    pmin = v;
                if (v > pmax)
                    pmax = v;
            }
            if (ctx.auto_min) {
                ctx.display_min_db = pmin;
                ctx.auto_min = 0;
            }
            if (ctx.auto_max) {
                ctx.display_max_db = pmax;
                ctx.auto_max = 0;
            }
            if (ctx.display_max_db - ctx.display_min_db < 10.0f)
                ctx.display_max_db = ctx.display_min_db + 10.0f;
        }

        sweep_count++;

        /* Update per-bin max-hold from the fresh panorama. */
        if (max_hold_buf) {
            for (int i = 0; i < panorama_bins; i++)
                if (panorama[i] > max_hold_buf[i])
                    max_hold_buf[i] = panorama[i];
            draw_set_maxhold(max_hold_buf, panorama_bins);
        }

        draw_fft(panorama, (size_t)panorama_bins, ctx.display_min_db, ctx.display_max_db);

        /* Overwrite row 1 with scan-specific status (draw_fft puts CF/SR header there). */
        printf("\033[1;1H\033[2K");
        printf("SCAN %.3f-%.3f MHz | steps:%d | speed:%.1f MHz/s | sweep:%.2fs"
               " | FFT:%d avg:%d | sweeps:%d%s",
               ctx.freq_from / 1e6, ctx.freq_to / 1e6,
               n_steps, scan_mhz_s, sweep_s,
               ctx.fft_width, ctx.average, sweep_count,
               max_hold_buf ? " | MAX-HOLD (c=clear)" : "");

        /* Poll keypress: 'c'/'C' resets max hold. */
        if (max_hold_buf) {
            int key = draw_check_keypress();
            if (key == 'c' || key == 'C') {
                for (int i = 0; i < panorama_bins; i++)
                    max_hold_buf[i] = -INFINITY;
            }
        }

        if (sgram) {
            sgram_add_row(sgram, panorama, ctx.display_min_db, ctx.display_max_db);
            if (ctx.n_frames > 0 && sgram_row_count(sgram) >= ctx.n_frames)
                ctx.run = 0;
        }
    }

    receiver_stop_scan();
    receiver_close();

    fftwf_free(f32_input);
    fftwf_free(fft_output);
    fftwf_free(windowed);
    free(windowt);
    free(avrg_output);
    free(panorama);
    free(chunk_buf);
    free(step_buf);
    free(prev_right_ov);
    free(step_freqs);
    free(max_hold_buf);

    draw_disable_keypress();
    draw_show_cursor();

    if (sgram) {
        printf("\nsaving spectrogram to %s (%d sweeps)...\n",
               ctx.sgram_file, sgram_row_count(sgram));
        fflush(stdout);
        if (sgram_write_png(sgram, ctx.sgram_file) == 0)
            printf("done.\n");
        sgram_destroy(sgram);
    }

    /* Save max-hold snapshot if requested. */
    if (max_hold_buf) {
        if (ctx.max_hold_csv)
            spectrum_csv_write("maxhold.csv", max_hold_buf, panorama_bins,
                               (double)ctx.freq_from, (double)ctx.freq_to);
        if (ctx.max_hold_png)
            spectrum_png_write("maxhold.png", max_hold_buf, panorama_bins,
                               ctx.display_min_db, ctx.display_max_db,
                               (double)ctx.freq_from, (double)ctx.freq_to, 0);
    }

    return 0;
}
