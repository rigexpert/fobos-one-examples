/**
 * @file main.c
 * @brief Real-time wideband signal detector — Fobos SDR developer example.
 *
 * @section overview Overview
 *
 * Captures IQ data from a Fobos SDR, computes averaged FFT power spectra, and
 * applies an automatic noise-floor estimator to locate occupied channels.  For
 * each detected signal it prints the centre frequency and bandwidth in Hz,
 * calibrated to the RTBW (Real-Time Bandwidth = sample rate) of the capture.
 *
 * Two operating modes are supported:
 *
 * **Single-channel mode** (default): monitors one fixed frequency.
 * @code
 *   ./detector -c 433000000 -r 20000000 -w 2048 -a 20 -t 10 -d
 * @endcode
 *
 * **Scan mode** (-f / -F): uses the Fobos hardware scan API to step across a
 * frequency range and detect signals at each step.
 * @code
 *   ./detector -f 400000000 -F 500000000 -r 20000000 -w 1024 -a 50 -t 10
 * @endcode
 *
 * @section pipeline Processing Pipeline
 *
 * @verbatim
 * [Fobos SDR hardware]
 *        │  IQ float32, interleaved, at samplerate
 *        │  RTBW = samplerate  (full instantaneous analysis bandwidth)
 *        ▼
 * [receiver_read()]  — ring-buffer pull (SCHED_FIFO capture thread)
 *        ▼
 * [fft_apply_precalculated_window()]  — Hamming window
 *        ▼
 * [fft_execute()]  — FFTW3 single-precision forward DFT
 *        ▼
 * [fft_accumulate_power()]  — I²+Q² per bin, summed over `average` iterations
 *        ▼
 * [fft_log()]  — every `average` iterations
 *   10·log10(sum) + baselevel − 10·log10(N), bin-swap → DC-centred dBFS spectrum
 *        │  float[fft_width]  dBFS, DC-centred, calibrated to RTBW
 *        ▼
 * [detector_noise_floor()]
 *   Nth-percentile (default 25th) of the spectrum as an adaptive noise floor.
 *        ▼
 * [detector_find_signals()]
 *   Linear scan: connected runs of bins ≥ noise_floor + threshold_db.
 *   Centroid frequency (power-weighted) and bandwidth (run span) computed per run.
 *        │  detected_signal_t[]
 *        ▼
 * [stdout / -o file]
 *   One line per detected signal per averaging window:
 *     UNIX_MS  CENTER_HZ  BANDWIDTH_HZ  PEAK_DBFS
 * @endverbatim
 *
 * @section output Output Format (RTBW)
 *
 * A header comment is printed once at startup:
 * @code
 *   # RTBW=20000000 CENTER=100000000 FFT=1024 AVG=10 THRESH=10.0dB PCTILE=25
 * @endcode
 *
 * Each detected signal is reported as a space-separated line:
 * @code
 *   UNIX_MS  CENTER_HZ  BW_HZ  PEAK_DBFS
 *   1715640000500 433920135.7 24414.1 -45.2
 * @endcode
 *
 * All frequencies are in Hz.  BW_HZ is calibrated directly in terms of the
 * RTBW: @c hz_per_bin = samplerate / fft_width, so minimum reported BW is
 * one FFT bin wide = @c samplerate/fft_width Hz.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <getopt.h>
#include <assert.h>
#include <limits.h>
#include <sched.h>

#ifndef __USE_POSIX
#define __USE_POSIX 1
#endif
#include <signal.h>

#include "fft.h"
#include "draw.h"
#include "detector.h"
#include "receiver.h"
#include "sgram.h"

/* ------------------------------------------------------------------ constants */

#define DEFAULT_FFT_WIDTH   1024
#define DEFAULT_FREQ        433000000LL
#define DEFAULT_SAMPLERATE  20000000L
#define DEFAULT_BASELEVEL   -75
#define DEFAULT_THRESHOLD   10.0f    /* dB above noise floor */
#define DEFAULT_HYSTERESIS   3.0f    /* fall offset below rise threshold */
#define DEFAULT_PERCENTILE  25       /* noise floor percentile */
#define DEFAULT_LOCAL_FLOOR  0       /* 0 = global floor, >0 = window width */
#define DEFAULT_MIN_BINS     2       /* minimum signal width in FFT bins */
#define DEFAULT_MAX_GAP      0       /* 0 = no gap merging */
#define DEFAULT_AVERAGE     1000
#define DEFAULT_REPORT_MS    5000    /* periodic UPD interval in ms for active signals */
#define DEFAULT_TRACK_WIN    5       /* consecutive missed windows before END event */
#define DEFAULT_CF_TOL_BW    0.5f   /* CF tolerance as fraction of signal BW (50 % = BW/2) */
#define MAX_SIGNALS         256
#define FOBOS_FRAME_SIZE    65536

#define MIN_DB  0.0f
#define MAX_DB  -130.0f

/* Fobos hardware scan constraints */
#define FOBOS_HW_SPS_ALIGN  8192       /* DMA buffer granularity in samples */
#define FOBOS_MIN_HW_SPS    65536      /* minimum samples per scan step */
#define RB_CHUNK_COUNT      32
#define SGRAM_DEFAULT_MEM_MB 256

/* ------------------------------------------------------------------ types */

typedef struct {
    int      run;
    int      fft_width;
    int      baselevel;
    int      average;
    int      verbose;
    long     cf;
    long     rate;
    int      lna_gain;
    int      vga_gain;
    float    threshold_db;     /**< Detection threshold above noise floor in dB. */
    float    hysteresis_db;   /**< Fall threshold offset (0 = no hysteresis). */
    int      percentile;      /**< Percentile for noise-floor estimation (0-99). */
    int      local_floor_bins;/**< Sliding-window size for local floor (0 = global). */
    int      min_bins;        /**< Minimum signal width in FFT bins. */
    int      max_gap_bins;    /**< Max gap to bridge when merging runs (0 = off). */
    int      report_interval_ms; /**< Periodic UPD interval in ms (0 = START+END only). */
    int      track_timeout;   /**< Consecutive missed windows before END event. */
    double   cf_tolerance_hz; /**< CF matching minimum tolerance (absolute floor). */
    float    cf_tol_bw_frac;  /**< CF tolerance as fraction of signal BW. */
    float    display_min_db;
    float    display_max_db;
    char    *out_filename;
    FILE    *output;
    FILE    *det_output;     /**< Detections go here (stdout or -o file). */
    fft_plan_s *fft_plan;
    int      draw_fft;
    /* scan mode */
    long long freq_from;     /**< Scan start frequency (Hz); 0 = single-channel mode. */
    long long freq_to;       /**< Scan end frequency (Hz). */
    double    overlap_hz;    /**< Step overlap in Hz (0 = no overlap). */
    char     *sgram_file;    /**< Output PNG filename; NULL = no PNG. */
    int       n_frames;      /**< Stop after this many sweeps (0 = unlimited). */
    int       sgram_mem_mb;  /**< Spectrogram buffer memory limit in MB. */
} ctx_t;

static ctx_t ctx;

/* Working buffers — global to avoid large stack allocs */
static complexf *f32_input  = NULL;
static complexf *fft_output = NULL;
static complexf *windowed   = NULL;
static float    *windowt    = NULL;
static float    *avrg_output = NULL;
static float    *spectrum   = NULL;    /* DC-centred dBFS, used for detection */
static float    *noise_arr  = NULL;    /* per-bin noise floor, length fft_width */

/* Scan-mode panorama buffers */
static float    *panorama       = NULL;
static float    *chunk_buf      = NULL;
static float    *step_buf       = NULL;
static float    *prev_right_ov  = NULL;

static detected_signal_t signals[MAX_SIGNALS];

/* ------------------------------------------------------------------ tracking */

typedef struct {
    double center_hz;      /* last observed CF */
    double bandwidth_hz;   /* last observed BW */
    float  peak_dbfs;      /* maximum peak since START */
    long   start_ms;       /* timestamp of START event */
    long   last_seen_ms;   /* timestamp of last matched detection */
    long   last_print_ms;  /* timestamp of last UPD/START print */
    int    miss_count;     /* consecutive spectrum updates without a match */
    int    update_count;   /* spectrum updates matched to this signal */
} tracked_signal_t;

static tracked_signal_t tracked[MAX_SIGNALS];
static int n_tracked = 0;

/* Auto-ranging state for display (NAN = not yet observed). */
static float auto_min_db = NAN;
static float auto_max_db = NAN;

/* ------------------------------------------------------------------ helpers */

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long)ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void signal_handler(int signum) {
    static int count = 0;
    fprintf(stderr, "received signal %d\n", signum);
    if (count)
        exit(-1);
    count++;
    ctx.run = 0;
}

static int log2n(int x) {
    int result = -1;
    for (int i = 0; i < 31; i++) {
        if ((x >> i) & 1) {
            if (result == -1)
                result = i;
            else
                return -1;
        }
    }
    return result;
}

/* ------------------------------------------------------------------ output */

static void print_header(void) {
    fprintf(ctx.det_output,
            "# RTBW=%ld CENTER=%ld FFT=%d AVG=%d"
            " THRESH=%.1fdB HYST=%.1fdB PCTILE=%d"
            " LOCAL=%d MINBINS=%d MAXGAP=%d"
            " REPORT_MS=%d TRACK_WIN=%d CF_TOL_HZ=%.0f CF_TOL_BW=%.2f\n",
            ctx.rate, ctx.cf, ctx.fft_width, ctx.average,
            ctx.threshold_db, ctx.hysteresis_db, ctx.percentile,
            ctx.local_floor_bins, ctx.min_bins, ctx.max_gap_bins,
            ctx.report_interval_ms, ctx.track_timeout,
            ctx.cf_tolerance_hz, ctx.cf_tol_bw_frac);
    fprintf(ctx.det_output,
            "# EVENT START_MS END_MS CF_HZ BW_HZ PEAK_DBFS DUR_MS UPDATES\n");
    fflush(ctx.det_output);
}

/* Emit one line per event.  START is printed to stderr only (verbose).
 * UPD and END both carry start_ms so the full emission window is visible
 * in a single output line. */
static void emit_event(const char *tag, long ts, const tracked_signal_t *t) {
    long dur = ts - t->start_ms;
    if (t->start_ms == ts) {
        /* START — only log to stderr when verbose; no stdout line yet */
        if (ctx.verbose)
            fprintf(stderr, "[START] %.0f Hz  BW=%.0f Hz  peak=%.1f dBFS\n",
                    t->center_hz, t->bandwidth_hz, t->peak_dbfs);
        return;
    }
    fprintf(ctx.det_output, "%s %ld %ld %.1f %.1f %.1f %ld %d\n",
            tag, t->start_ms, ts,
            t->center_hz, t->bandwidth_hz, t->peak_dbfs,
            dur, t->update_count);
    fflush(ctx.det_output);
    if (ctx.verbose) {
        fprintf(stderr, "[%s] CF=%.0f Hz  BW=%.0f Hz  peak=%.1f dBFS"
                "  dur=%ld ms  updates=%d\n",
                tag, t->center_hz, t->bandwidth_hz, t->peak_dbfs,
                dur, t->update_count);
    }
}

/* Called every `average` FFT iterations with the DC-centred dBFS spectrum. */
static void process_spectrum(float *spec, double center_hz) {
    /* Auto-range: scan spectrum for min/max, then apply slow EMA so the
     * display adapts without jumping on transient spikes.
     * Only used for the axis that the user left unset (isnan). */
    if (isnan(ctx.display_min_db) || isnan(ctx.display_max_db)) {
        float smin = spec[0], smax = spec[0];
        for (int i = 1; i < ctx.fft_width; i++) {
            if (spec[i] < smin)
                smin = spec[i];
            if (spec[i] > smax)
                smax = spec[i];
        }
        if (isnan(auto_min_db)) {
            auto_min_db = smin - 5.0f;
            auto_max_db = smax + 3.0f;
        } else {
            auto_min_db += 0.05f * (smin - 5.0f - auto_min_db);
            auto_max_db += 0.05f * (smax + 3.0f - auto_max_db);
        }
    }

    if (ctx.draw_fft) {
        float dmin = isnan(ctx.display_min_db) ? auto_min_db : ctx.display_min_db;
        float dmax = isnan(ctx.display_max_db) ? auto_max_db : ctx.display_max_db;
        draw_fft(spec, ctx.fft_width, dmin, dmax);
    }

    /* Build noise floor array (global or local). */
    if (ctx.local_floor_bins > 0) {
        detector_local_noise_floor(spec, ctx.fft_width,
                                   ctx.local_floor_bins, ctx.percentile,
                                   noise_arr);
    } else {
        float g = detector_noise_floor(spec, ctx.fft_width, ctx.percentile);
        for (int i = 0; i < ctx.fft_width; i++)
            noise_arr[i] = g;
    }

    int n = detector_find_signals(spec, ctx.fft_width,
                                  center_hz, (double)ctx.rate,
                                  noise_arr,
                                  ctx.threshold_db, ctx.hysteresis_db,
                                  ctx.min_bins, ctx.max_gap_bins,
                                  signals, MAX_SIGNALS);

    long ts = now_ms();

    /* --- match detected signals to tracked entries --- */
    int matched_tracked[MAX_SIGNALS];
    int matched_det[MAX_SIGNALS];
    memset(matched_tracked, 0, sizeof(int) * n_tracked);
    memset(matched_det,     0, sizeof(int) * n);

    for (int j = 0; j < n_tracked; j++) {
        double best_dist = 1e18;
        int    best_i    = -1;
        for (int i = 0; i < n; i++) {
            if (matched_det[i])
                continue;
            double pair_bw = tracked[j].bandwidth_hz > signals[i].bandwidth_hz
                             ? tracked[j].bandwidth_hz : signals[i].bandwidth_hz;
            double tol = ctx.cf_tolerance_hz;
            double bw_tol = ctx.cf_tol_bw_frac * pair_bw;
            if (bw_tol > tol)
                tol = bw_tol;

            double dist = fabs(signals[i].center_hz - tracked[j].center_hz);
            if (dist <= tol && dist < best_dist) {
                best_dist = dist;
                best_i    = i;
            }
        }
        if (best_i >= 0) {
            matched_tracked[j] = 1;
            matched_det[best_i] = 1;
            tracked[j].center_hz    = signals[best_i].center_hz;
            tracked[j].bandwidth_hz = signals[best_i].bandwidth_hz;
            if (signals[best_i].peak_dbfs > tracked[j].peak_dbfs)
                tracked[j].peak_dbfs = signals[best_i].peak_dbfs;
            tracked[j].last_seen_ms = ts;
            tracked[j].miss_count   = 0;
            tracked[j].update_count++;
        }
    }

    /* --- periodic UPD events for still-active signals --- */
    if (ctx.report_interval_ms > 0) {
        for (int j = 0; j < n_tracked; j++) {
            if (matched_tracked[j] &&
                (ts - tracked[j].last_print_ms) >= ctx.report_interval_ms) {
                emit_event("UPD", ts, &tracked[j]);
                tracked[j].last_print_ms = ts;
            }
        }
    }

    /* --- age out unmatched tracked signals --- */
    int new_n = 0;
    for (int j = 0; j < n_tracked; j++) {
        if (!matched_tracked[j])
            tracked[j].miss_count++;
        if (matched_tracked[j] || tracked[j].miss_count < ctx.track_timeout) {
            tracked[new_n++] = tracked[j];
        } else {
            emit_event("END", ts, &tracked[j]);
        }
    }
    n_tracked = new_n;

    /* --- create new tracked entries for unmatched detections --- */
    for (int i = 0; i < n; i++) {
        if (matched_det[i] || n_tracked >= MAX_SIGNALS)
            continue;
        tracked_signal_t *t = &tracked[n_tracked++];
        t->center_hz    = signals[i].center_hz;
        t->bandwidth_hz = signals[i].bandwidth_hz;
        t->peak_dbfs    = signals[i].peak_dbfs;
        t->start_ms     = ts;
        t->last_seen_ms = ts;
        t->last_print_ms = ts;
        t->miss_count   = 0;
        t->update_count = 1;
        emit_event("START", ts, t);
    }

    if (ctx.verbose && n_tracked > 0) {
        float gnoise = detector_noise_floor(spec, ctx.fft_width, ctx.percentile);
        fprintf(stderr, "[%ld] noise=%.1f dBFS  %d active\n",
                ts, gnoise, n_tracked);
    }
}

/* ------------------------------------------------------------------ scan helpers */

/* Round up n to nearest multiple of FOBOS_HW_SPS_ALIGN, minimum FOBOS_MIN_HW_SPS. */
static int hw_samples_det(int n) {
    if (n < FOBOS_MIN_HW_SPS)
        n = FOBOS_MIN_HW_SPS;
    return ((n + FOBOS_HW_SPS_ALIGN - 1) / FOBOS_HW_SPS_ALIGN) * FOBOS_HW_SPS_ALIGN;
}

/* Return the index of the frequency in freqs[] nearest to target. */
static int nearest_step_det(double target, double *freqs, int count) {
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

/* ------------------------------------------------------------------ args */

static void print_help(void) {
    printf("detector — real-time wideband signal detector using Fobos SDR\n");
    printf("usage: detector [options]\n\n");
    printf("Single-channel mode:\n");
    printf("  -w, --fft-width  <n>      FFT width, power of 2 (default %d)\n", DEFAULT_FFT_WIDTH);
    printf("  -a, --average    <n>      FFTs to average per detection (default %d)\n", DEFAULT_AVERAGE);
    printf("  -c, --freq       <hz>     centre frequency (default %lld)\n", (long long)DEFAULT_FREQ);
    printf("  -r, --rate       <hz>     sample rate = RTBW (default %ld)\n", DEFAULT_SAMPLERATE);
    printf("  -b, --baselevel  <dBFS>   FFT base level (default %d)\n", DEFAULT_BASELEVEL);
    printf("  -t, --threshold  <dB>     detection threshold above noise floor (default %.1f)\n", DEFAULT_THRESHOLD);
    printf("  -H, --hysteresis <dB>     fall threshold offset for hysteresis (default %.1f, 0=off)\n", DEFAULT_HYSTERESIS);
    printf("  -p, --percentile <n>      percentile for noise-floor estimate (default %d)\n", DEFAULT_PERCENTILE);
    printf("  -l, --local-floor <bins>  local noise floor window size (default %d, 0=global)\n", DEFAULT_LOCAL_FLOOR);
    printf("  -n, --min-bins   <n>      minimum signal width in FFT bins (default %d)\n", DEFAULT_MIN_BINS);
    printf("  -g, --max-gap    <bins>   max gap to merge between signals (default %d, 0=off)\n", DEFAULT_MAX_GAP);
    printf("  -P, --report-ms  <ms>     periodic UPD interval for continuous signals\n");
    printf("                            (default %d ms)\n", DEFAULT_REPORT_MS);
    printf("  -T, --track-win  <n>      consecutive missed windows before END event\n");
    printf("                            (default %d)\n", DEFAULT_TRACK_WIN);
    printf("  -C, --cf-tol     <hz>     minimum CF matching tolerance (absolute floor)\n");
    printf("                            (default 3 FFT bins = 3*rate/fft_width Hz)\n");
    printf("  -B, --cf-tol-bw  <frac>   CF tolerance as fraction of signal BW\n");
    printf("                            (default %.2f — expands tolerance for wide signals)\n",
           DEFAULT_CF_TOL_BW);
    printf("  -d, --draw                draw spectrum in terminal\n");
    printf("  -o, --output     <file>   write detections to file (default: stdout)\n");
    printf("  -L, --lna-gain   <n>      LNA gain index 0..3 (0,1: 0 dB, 2: +16, 3: +33)\n");
    printf("  -V, --vga-gain   <n>      VGA gain index 0..31 (0..+62 dB, 2 dB step)\n");
    printf("  -m, --min-db     <dBFS>   display minimum dB\n");
    printf("  -M, --max-db     <dBFS>   display maximum dB\n");
    printf("  -v, --verbose             print verbose info to stderr\n");
    printf("  -h, --help                this message\n\n");
    printf("Scan mode (hardware frequency sweep, activates when -f and -F are given):\n");
    printf("  -f, --freq-from  <hz>     scan start frequency\n");
    printf("  -F, --freq-to    <hz>     scan end frequency\n");
    printf("  -O, --overlap    <hz>     step overlap in Hz (default 0)\n");
    printf("  -G, --sgram      <file>   save spectrogram PNG to file on exit\n");
    printf("  -N, --n-frames   <n>      stop and save after N sweeps (default 0 = unlimited)\n");
    printf("  -S, --sgram-mb   <mb>     spectrogram memory limit in MB (default %d)\n", SGRAM_DEFAULT_MEM_MB);
    printf("\n");
    printf("Output (RTBW format):\n");
    printf("  # RTBW=<samplerate> CENTER=<cf> FFT=<width> ...\n");
    printf("  EVENT START_MS END_MS CF_HZ BW_HZ PEAK_DBFS DUR_MS UPDATES\n\n");
    printf("Example — scan 20 MHz around 433 MHz:\n");
    printf("  ./detector -c 433000000 -r 20000000 -w 2048 -a 20 -t 10 -d\n");
    printf("Example — hardware scan 400-500 MHz, save spectrogram:\n");
    printf("  ./detector -f 400000000 -F 500000000 -r 20000000 -w 1024 -a 50 -t 10 -G out.png\n");
}

static void parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"fft-width",   required_argument, 0, 'w'},
        {"average",     required_argument, 0, 'a'},
        {"freq",        required_argument, 0, 'c'},
        {"rate",        required_argument, 0, 'r'},
        {"baselevel",   required_argument, 0, 'b'},
        {"threshold",   required_argument, 0, 't'},
        {"hysteresis",  required_argument, 0, 'H'},
        {"percentile",  required_argument, 0, 'p'},
        {"local-floor", required_argument, 0, 'l'},
        {"min-bins",    required_argument, 0, 'n'},
        {"max-gap",     required_argument, 0, 'g'},
        {"report-ms",   required_argument, 0, 'P'},
        {"track-win",   required_argument, 0, 'T'},
        {"cf-tol",      required_argument, 0, 'C'},
        {"cf-tol-bw",   required_argument, 0, 'B'},
        {"draw",        no_argument,       0, 'd'},
        {"output",      required_argument, 0, 'o'},
        {"lna-gain",    required_argument, 0, 'L'},
        {"vga-gain",    required_argument, 0, 'V'},
        {"min-db",      required_argument, 0, 'm'},
        {"max-db",      required_argument, 0, 'M'},
        {"verbose",     no_argument,       0, 'v'},
        {"freq-from",   required_argument, 0, 'f'},
        {"freq-to",     required_argument, 0, 'F'},
        {"overlap",     required_argument, 0, 'O'},
        {"sgram",       required_argument, 0, 'G'},
        {"n-frames",    required_argument, 0, 'N'},
        {"sgram-mb",    required_argument, 0, 'S'},
        {"help",        no_argument,       0, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt, long_index = 0;
    while ((opt = getopt_long(argc, argv,
                              "w:a:c:r:b:t:H:p:l:n:g:P:T:C:B:do:L:V:m:M:vf:F:O:G:N:S:h",
                              long_options, &long_index)) != -1) {
        switch (opt) {
            case 'w':
                ctx.fft_width = atoi(optarg);
                if (log2n(ctx.fft_width) == -1) {
                    fprintf(stderr, "fft-width must be a power of 2\n");
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
            case 'c':
                ctx.cf = atol(optarg);
                break;
            case 'r':
                ctx.rate = atol(optarg);
                if (ctx.rate <= 0) {
                    fprintf(stderr, "rate must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'b':
                ctx.baselevel = atoi(optarg);
                break;
            case 't':
                ctx.threshold_db = atof(optarg);
                if (ctx.threshold_db <= 0.0f) {
                    fprintf(stderr, "threshold must be > 0 dB\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'H':
                ctx.hysteresis_db = atof(optarg);
                if (ctx.hysteresis_db < 0.0f) {
                    fprintf(stderr, "hysteresis must be >= 0 dB\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'p':
                ctx.percentile = atoi(optarg);
                if (ctx.percentile < 1 || ctx.percentile > 99) {
                    fprintf(stderr, "percentile must be in [1, 99]\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'l':
                ctx.local_floor_bins = atoi(optarg);
                if (ctx.local_floor_bins < 0) {
                    fprintf(stderr, "local-floor must be >= 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'n':
                ctx.min_bins = atoi(optarg);
                if (ctx.min_bins < 1) {
                    fprintf(stderr, "min-bins must be >= 1\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'g':
                ctx.max_gap_bins = atoi(optarg);
                if (ctx.max_gap_bins < 0) {
                    fprintf(stderr, "max-gap must be >= 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'P':
                ctx.report_interval_ms = atoi(optarg);
                if (ctx.report_interval_ms < 0) {
                    fprintf(stderr, "report-ms must be >= 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'T':
                ctx.track_timeout = atoi(optarg);
                if (ctx.track_timeout < 1) {
                    fprintf(stderr, "track-win must be >= 1\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'C':
                ctx.cf_tolerance_hz = atof(optarg);
                if (ctx.cf_tolerance_hz <= 0.0) {
                    fprintf(stderr, "cf-tol must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'B':
                ctx.cf_tol_bw_frac = atof(optarg);
                if (ctx.cf_tol_bw_frac < 0.0f) {
                    fprintf(stderr, "cf-tol-bw must be >= 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'd':
                ctx.draw_fft = 1;
                break;
            case 'o':
                ctx.out_filename = optarg;
                break;
            case 'L':
                ctx.lna_gain = atoi(optarg);
                break;
            case 'V':
                ctx.vga_gain = atoi(optarg);
                break;
            case 'm':
                ctx.display_min_db = atof(optarg);
                break;
            case 'M':
                ctx.display_max_db = atof(optarg);
                break;
            case 'v':
                ctx.verbose = 1;
                break;
            case 'f':
                ctx.freq_from = atoll(optarg);
                break;
            case 'F':
                ctx.freq_to = atoll(optarg);
                break;
            case 'O':
                ctx.overlap_hz = atof(optarg);
                break;
            case 'G':
                ctx.sgram_file = optarg;
                break;
            case 'N':
                ctx.n_frames = atoi(optarg);
                if (ctx.n_frames <= 0) {
                    fprintf(stderr, "-N must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'S':
                ctx.sgram_mem_mb = atoi(optarg);
                if (ctx.sgram_mem_mb <= 0) {
                    fprintf(stderr, "-S must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'h':
                print_help();
                exit(EXIT_SUCCESS);
            default:
                exit(EXIT_FAILURE);
        }
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[]) {
    /* defaults */
    memset(&ctx, 0, sizeof(ctx));
    ctx.fft_width     = DEFAULT_FFT_WIDTH;
    ctx.cf            = DEFAULT_FREQ;
    ctx.rate          = DEFAULT_SAMPLERATE;
    ctx.baselevel     = DEFAULT_BASELEVEL;
    ctx.average       = DEFAULT_AVERAGE;
    ctx.threshold_db     = DEFAULT_THRESHOLD;
    ctx.hysteresis_db    = DEFAULT_HYSTERESIS;
    ctx.percentile       = DEFAULT_PERCENTILE;
    ctx.local_floor_bins = DEFAULT_LOCAL_FLOOR;
    ctx.min_bins         = DEFAULT_MIN_BINS;
    ctx.max_gap_bins     = DEFAULT_MAX_GAP;
    ctx.report_interval_ms = DEFAULT_REPORT_MS;
    ctx.track_timeout    = DEFAULT_TRACK_WIN;
    ctx.cf_tolerance_hz  = 0.0;
    ctx.cf_tol_bw_frac   = DEFAULT_CF_TOL_BW;
    ctx.lna_gain      = -1;
    ctx.vga_gain      = -1;
    ctx.display_min_db = ctx.display_max_db = nanf("");
    ctx.run           = 1;
    ctx.det_output    = stdout;
    ctx.sgram_mem_mb  = SGRAM_DEFAULT_MEM_MB;

    parse_args(argc, argv);

    /* Default CF tolerance: 3 FFT bins.  Applied only if user didn't set -C. */
    if (ctx.cf_tolerance_hz <= 0.0)
        ctx.cf_tolerance_hz = 3.0 * (double)ctx.rate / (double)ctx.fft_width;

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* open detection output file if requested */
    if (ctx.out_filename) {
        ctx.det_output = fopen(ctx.out_filename, "w");
        if (!ctx.det_output) {
            perror("cannot open output file");
            return EXIT_FAILURE;
        }
    }

    /* allocate common FFT buffers */
    f32_input   = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    fft_output  = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    windowed    = (complexf *)fftwf_alloc_complex(ctx.fft_width);
    windowt     = (float *)malloc(sizeof(float) * ctx.fft_width);
    avrg_output = (float *)malloc(sizeof(float) * ctx.fft_width);
    spectrum    = (float *)calloc(ctx.fft_width, sizeof(float));
    noise_arr   = (float *)malloc(sizeof(float) * ctx.fft_width);
    assert(f32_input && fft_output && windowed && windowt && avrg_output && spectrum && noise_arr);

    fft_precalculate_window(windowt, ctx.fft_width);

    /* baselevel correction for averaging: subtract 10·log10(N) from dBFS */
    ctx.baselevel -= (int)(10.0 * log10((double)ctx.average));

    int scan_mode = (ctx.freq_from > 0 && ctx.freq_to > 0 && ctx.freq_to > ctx.freq_from);

    if (scan_mode) {
        /* ============================================================ scan mode */

        double bw_per_step = ctx.rate * FOBOS_AUTO_BW;
        long long span     = ctx.freq_to - ctx.freq_from;

        if (ctx.overlap_hz < 0.0 || ctx.overlap_hz >= bw_per_step) {
            fprintf(stderr, "error: -O overlap must be in [0, %.0f) Hz (< one step BW)\n",
                    bw_per_step);
            return EXIT_FAILURE;
        }

        double step_spacing = bw_per_step - ctx.overlap_hz;
        int n_steps = (int)ceil((double)span / step_spacing);
        if (n_steps < 2) {
            n_steps = 2;
            fprintf(stderr, "note: span fits in one step; hardware requires >= 2, using 2\n");
        }
        if (n_steps > FOBOS_MAX_FREQS_CNT) {
            fprintf(stderr, "error: %d steps required but hardware limit is %d.\n"
                            "       Increase rate, reduce span, or reduce overlap.\n",
                    n_steps, FOBOS_MAX_FREQS_CNT);
            return EXIT_FAILURE;
        }

        double actual_spacing = (double)span / n_steps;
        int bins_per_step     = (int)round((double)ctx.fft_width * actual_spacing / ctx.rate);
        int hov               = (int)round((double)ctx.fft_width * ctx.overlap_hz
                                           / (2.0 * ctx.rate));
        int hw_sps            = hw_samples_det(ctx.average * ctx.fft_width);
        int panorama_bins     = n_steps * bins_per_step;

        double *step_freqs = malloc((size_t)n_steps * sizeof(double));
        assert(step_freqs);
        for (int s = 0; s < n_steps; s++)
            step_freqs[s] = ctx.freq_from + actual_spacing * (s + 0.5);

        printf("scan mode: %.3f - %.3f MHz  |  %d steps  |  %.3f MHz/step  |  %d bins/step\n",
               ctx.freq_from / 1e6, ctx.freq_to / 1e6,
               n_steps, actual_spacing / 1e6, bins_per_step);
        printf("  hw_sps  : %d samples/step (%d FFTs/chunk)\n",
               hw_sps, hw_sps / ctx.fft_width);

        /* --- spectrogram init --- */
        sgram_t *sgram = NULL;
        if (ctx.sgram_file) {
            size_t mem_bytes = (size_t)ctx.sgram_mem_mb * 1024UL * 1024UL;
            sgram = sgram_create(panorama_bins, mem_bytes);
            if (!sgram) {
                fprintf(stderr, "failed to allocate spectrogram buffer\n");
                return EXIT_FAILURE;
            }
            sgram_params_t sp = {
                .freq_from_hz = (double)ctx.freq_from,
                .freq_to_hz   = (double)ctx.freq_to,
                .rate         = (int)ctx.rate,
                .fft_width    = ctx.fft_width,
                .average      = ctx.average,
                .n_steps      = n_steps,
                .overlap_hz   = ctx.overlap_hz,
            };
            sgram_set_params(sgram, &sp);
            int cap_rows = (int)(mem_bytes / ((size_t)panorama_bins * 3));
            printf("  sgram   : %s  (%d rows capacity, %d MB)\n",
                   ctx.sgram_file, cap_rows, ctx.sgram_mem_mb);
        }

        /* --- scan buffers --- */
        panorama      = (float *)calloc((size_t)panorama_bins, sizeof(float));
        chunk_buf     = (float *)malloc((size_t)hw_sps * 2 * sizeof(float));
        step_buf      = (float *)malloc((size_t)(bins_per_step + 2 * hov) * sizeof(float));
        prev_right_ov = hov > 0 ? (float *)malloc((size_t)hov * sizeof(float)) : NULL;
        assert(panorama && chunk_buf && step_buf);

        /* --- receiver init --- */
        printf("initializing fobos (scan mode)...\n");
        if (receiver_init((size_t)hw_sps, RB_CHUNK_COUNT) != 0) {
            fprintf(stderr, "failed to init receiver\n");
            return EXIT_FAILURE;
        }
        if (ctx.lna_gain >= 0)
            receiver_set_lna((uint8_t)ctx.lna_gain);
        if (ctx.vga_gain >= 0)
            receiver_set_vga((uint8_t)ctx.vga_gain);
        if (receiver_set_samplerate((uint32_t)ctx.rate) != 0) {
            fprintf(stderr, "failed to set sample rate\n");
            return EXIT_FAILURE;
        }

        /* --- FFT plan with wisdom --- */
        fft_load_wisdom("fftw_wisdom");
        ctx.fft_plan = fft_create_plan(ctx.fft_width, windowed, fft_output);
        fft_save_wisdom("fftw_wisdom");

        /* --- draw init --- */
        if (ctx.draw_fft) {
            uint64_t display_cf   = (uint64_t)((ctx.freq_from + ctx.freq_to) / 2);
            uint64_t display_rate = (uint64_t)span;
            draw_init(display_cf, display_rate,
                      ctx.baselevel, 0, ctx.average,
                      (uint32_t)panorama_bins, MIN_DB, MAX_DB);
            draw_hide_cursor();
        }

        fflush(stdout);
        setvbuf(stdout, NULL, _IOFBF, 1 << 18);

        /* Print detection header with first step's centre freq as placeholder */
        ctx.cf   = (long)step_freqs[0];
        print_header();

        if (receiver_start_scan(step_freqs, n_steps, hw_sps) != 0) {
            fprintf(stderr, "failed to start hardware scan\n");
            return EXIT_FAILURE;
        }

        /* --- main scan loop --- */
        int sweep_count = 0;
        while (ctx.run) {
            for (int i = 0; i < n_steps && ctx.run; i++) {
                double freq_hz = 0.0;
                int samples = receiver_read_scan_chunk(chunk_buf, (size_t)hw_sps, &freq_hz);
                if (samples <= 0)
                    break;

                int step = nearest_step_det(freq_hz, step_freqs, n_steps);

                /* Accumulate ctx.average FFTs from the settled chunk. */
                memset(avrg_output, 0, sizeof(float) * ctx.fft_width);
                for (int iter = 0; iter < ctx.average; iter++) {
                    complexf *src = ((complexf *)chunk_buf) + iter * ctx.fft_width;
                    fft_apply_precalculated_window(src, windowed, ctx.fft_width, windowt);
                    fft_execute(ctx.fft_plan);
                    fft_accumulate_power(fft_output, avrg_output, ctx.fft_width);
                }

                /* Build DC-centred spectrum for detection (bin-swap). */
                fft_log(avrg_output,                       spectrum + ctx.fft_width / 2,
                        ctx.fft_width / 2, ctx.baselevel);
                fft_log(avrg_output + ctx.fft_width / 2,   spectrum,
                        ctx.fft_width / 2, ctx.baselevel);

                process_spectrum(spectrum, freq_hz);

                /* Extract center bins for panorama / sgram. */
                if (sgram || ctx.draw_fft) {
                    int total_extract = bins_per_step + 2 * hov;
                    int htotal        = total_extract / 2;
                    /* avrg_output is still valid: fft_log wrote to spectrum, not avrg_output */
                    fft_log(avrg_output + ctx.fft_width - htotal, step_buf,
                            htotal, ctx.baselevel);
                    fft_log(avrg_output, step_buf + htotal,
                            total_extract - htotal, ctx.baselevel);

                    float *dst = panorama + step * bins_per_step;
                    memcpy(dst, step_buf + hov, (size_t)bins_per_step * sizeof(float));

                    if (step > 0 && hov > 0) {
                        for (int j = 0; j < hov; j++)
                            dst[j] = (dst[j] + prev_right_ov[j]) * 0.5f;
                    }
                    if (hov > 0)
                        memcpy(prev_right_ov, step_buf + hov + bins_per_step,
                               (size_t)hov * sizeof(float));
                }
            }

            sweep_count++;

            if (ctx.draw_fft) {
                float dmin = isnan(ctx.display_min_db) ? auto_min_db : ctx.display_min_db;
                float dmax = isnan(ctx.display_max_db) ? auto_max_db : ctx.display_max_db;
                draw_fft(panorama, (size_t)panorama_bins, dmin, dmax);
            }

            if (sgram) {
                float dmin = isnan(ctx.display_min_db) ? auto_min_db : ctx.display_min_db;
                float dmax = isnan(ctx.display_max_db) ? auto_max_db : ctx.display_max_db;
                sgram_add_row(sgram, panorama, dmin, dmax);
            }

            if (ctx.n_frames > 0 && sweep_count >= ctx.n_frames)
                ctx.run = 0;
        }

        receiver_stop_scan();
        receiver_close();

        if (ctx.draw_fft)
            draw_show_cursor();

        free(panorama);
        free(chunk_buf);
        free(step_buf);
        free(prev_right_ov);
        free(step_freqs);

        if (sgram) {
            printf("\nsaving spectrogram to %s (%d sweeps)...\n",
                   ctx.sgram_file, sgram_row_count(sgram));
            fflush(stdout);
            if (sgram_write_png(sgram, ctx.sgram_file) == 0)
                printf("done.\n");
            sgram_destroy(sgram);
        }

    } else {
        /* ============================================ normal single-channel mode */

        printf("initializing fobos...\n");
        if (receiver_init(FOBOS_FRAME_SIZE, 32) != 0) {
            fprintf(stderr, "failed to open fobos device\n");
            return EXIT_FAILURE;
        }

        if (ctx.lna_gain >= 0)
            receiver_set_lna((uint8_t)ctx.lna_gain);
        if (ctx.vga_gain >= 0)
            receiver_set_vga((uint8_t)ctx.vga_gain);

        if (receiver_set_freq((uint64_t)ctx.cf) != 0) {
            fprintf(stderr, "failed to set frequency\n");
            return EXIT_FAILURE;
        }
        if (receiver_set_samplerate((uint32_t)ctx.rate) != 0) {
            fprintf(stderr, "failed to set sample rate\n");
            return EXIT_FAILURE;
        }

        printf("RTBW = %ld Hz  |  centre = %ld Hz  |  bin = %.1f Hz\n",
               ctx.rate, ctx.cf, (double)ctx.rate / ctx.fft_width);

        fft_load_wisdom("fftw_wisdom");
        ctx.fft_plan = fft_create_plan(ctx.fft_width, windowed, fft_output);
        fft_save_wisdom("fftw_wisdom");

        if (ctx.draw_fft)
            draw_init((uint64_t)ctx.cf, (uint64_t)ctx.rate, ctx.baselevel,
                      0, ctx.average, (uint32_t)ctx.fft_width, MIN_DB, MAX_DB);

        fflush(stdout);
        setvbuf(stdout, NULL, _IOFBF, 1 << 18);

        if (ctx.draw_fft)
            draw_hide_cursor();

        print_header();

        receiver_start(receiver_mode_sync);

        int iteration = 0;
        while (ctx.run) {
            if (receiver_read(f32_input, ctx.fft_width) != ctx.fft_width)
                break;

            fft_apply_precalculated_window(f32_input, windowed, ctx.fft_width, windowt);
            fft_execute(ctx.fft_plan);
            fft_accumulate_power(fft_output, avrg_output, ctx.fft_width);

            if (!(++iteration % ctx.average)) {
                fft_log(avrg_output,                     spectrum + ctx.fft_width / 2,
                        ctx.fft_width / 2, ctx.baselevel);
                fft_log(avrg_output + ctx.fft_width / 2, spectrum,
                        ctx.fft_width / 2, ctx.baselevel);
                memset(avrg_output, 0, sizeof(float) * ctx.fft_width);

                process_spectrum(spectrum, (double)ctx.cf);
            }
        }

        receiver_stop();
        receiver_close();

        if (ctx.draw_fft)
            draw_show_cursor();
    }

    /* common cleanup */
    fftwf_free(f32_input);
    fftwf_free(fft_output);
    fftwf_free(windowed);
    free(windowt);
    free(avrg_output);
    free(spectrum);
    free(noise_arr);

    if (ctx.det_output != stdout)
        fclose(ctx.det_output);

    return 0;
}
