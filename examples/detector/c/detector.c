/**
 * @file detector.c
 * @brief Wideband signal detector implementation.
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "detector.h"

static int cmp_float_asc(const void *a, const void *b) {
    float fa = *(const float *)a;
    float fb = *(const float *)b;
    return (fa > fb) - (fa < fb);
}

float detector_noise_floor(const float *spectrum, int n, int percentile) {
    if (n <= 0)
        return -100.0f;

    float *tmp = malloc((size_t)n * sizeof(float));
    if (!tmp)
        return -100.0f;

    memcpy(tmp, spectrum, (size_t)n * sizeof(float));
    qsort(tmp, (size_t)n, sizeof(float), cmp_float_asc);

    int idx = (int)((long)n * percentile / 100);
    if (idx < 0)
        idx = 0;
    if (idx >= n)
        idx = n - 1;
    float result = tmp[idx];

    free(tmp);
    return result;
}

void detector_local_noise_floor(const float *spectrum, int n,
                                int window_bins, int percentile,
                                float *out) {
    if (n <= 0 || !out)
        return;
    if (window_bins < 1)
        window_bins = 1;
    if (window_bins > n)
        window_bins = n;

    float *buf = malloc((size_t)window_bins * sizeof(float));
    if (!buf) {
        float g = detector_noise_floor(spectrum, n, percentile);
        for (int i = 0; i < n; i++)
            out[i] = g;
        return;
    }

    for (int i = 0; i < n; i++) {
        int lo = i - window_bins / 2;
        int hi = lo + window_bins - 1;
        if (lo < 0)
            lo = 0;
        if (hi >= n)
            hi = n - 1;
        int w = hi - lo + 1;

        memcpy(buf, spectrum + lo, (size_t)w * sizeof(float));
        qsort(buf, (size_t)w, sizeof(float), cmp_float_asc);

        int idx = (int)((long)w * percentile / 100);
        if (idx < 0)
            idx = 0;
        if (idx >= w)
            idx = w - 1;
        out[i] = buf[idx];
    }

    free(buf);
}

int detector_find_signals(const float *spectrum, int n,
                          double cf_hz, double samplerate,
                          const float *noise_floor,
                          float threshold_db,
                          float hysteresis_db,
                          int min_width_bins,
                          int max_gap_bins,
                          detected_signal_t *out, int max_out) {
    if (!spectrum || n <= 0 || !out || max_out <= 0 || !noise_floor)
        return 0;

    if (hysteresis_db < 0.0f)
        hysteresis_db = 0.0f;
    if (max_gap_bins  < 0)
        max_gap_bins  = 0;

    double hz_per_bin = samplerate / (double)n;
    double f_left     = cf_hz - samplerate / 2.0;

    /* Step 1: collect raw runs using hysteresis thresholds.
     *
     * Rise: noise_floor[i] + threshold_db        (enter run)
     * Fall: noise_floor[i] + threshold_db - hysteresis_db  (leave run)
     *
     * Maximum possible non-overlapping runs = n/2. */
    int max_runs = n / 2 + 1;
    typedef struct { int start, end; } run_t;
    run_t *runs = malloc((size_t)max_runs * sizeof(run_t));
    if (!runs)
        return 0;

    int n_runs    = 0;
    int run_start = -1;

    for (int i = 0; i <= n; i++) {
        int active;
        if (i < n) {
            float rise = noise_floor[i] + threshold_db;
            float fall = rise - hysteresis_db;
            active = (run_start < 0) ? (spectrum[i] >= rise)
                                     : (spectrum[i] >= fall);
        } else {
            active = 0;
        }

        if (active && run_start < 0) {
            run_start = i;
        } else if (!active && run_start >= 0) {
            if (n_runs < max_runs) {
                runs[n_runs].start = run_start;
                runs[n_runs].end   = i - 1;
                n_runs++;
            }
            run_start = -1;
        }
    }

    /* Step 2: merge adjacent runs whose gap <= max_gap_bins. */
    if (max_gap_bins > 0 && n_runs > 1) {
        int j = 0;
        for (int i = 1; i < n_runs; i++) {
            if (runs[i].start - runs[j].end - 1 <= max_gap_bins)
                runs[j].end = runs[i].end;
            else
                runs[++j] = runs[i];
        }
        n_runs = j + 1;
    }

    /* Step 3: compute output for runs >= min_width_bins.
     *
     * Centroid and peak come from the original spectrum over the merged
     * run, so accuracy is not degraded by the merge step. */
    int n_found = 0;
    for (int r = 0; r < n_runs && n_found < max_out; r++) {
        int width = runs[r].end - runs[r].start + 1;
        if (width < min_width_bins)
            continue;

        double power_sum = 0.0, weighted_sum = 0.0;
        float  peak = spectrum[runs[r].start];

        for (int j = runs[r].start; j <= runs[r].end; j++) {
            double p = pow(10.0, spectrum[j] / 10.0);
            double f = f_left + (j + 0.5) * hz_per_bin;
            power_sum    += p;
            weighted_sum += p * f;
            if (spectrum[j] > peak)
                peak = spectrum[j];
        }

        out[n_found].center_hz    = (power_sum > 0.0)
                                    ? weighted_sum / power_sum
                                    : f_left + (runs[r].start + width / 2.0) * hz_per_bin;
        out[n_found].start_hz     = f_left + runs[r].start * hz_per_bin;
        out[n_found].stop_hz      = f_left + (runs[r].end + 1) * hz_per_bin;
        out[n_found].bandwidth_hz = out[n_found].stop_hz - out[n_found].start_hz;
        out[n_found].peak_dbfs    = peak;
        n_found++;
    }

    free(runs);
    return n_found;
}
