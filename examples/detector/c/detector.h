/**
 * @file detector.h
 * @brief Wideband signal detector operating on an FFT power spectrum.
 *
 * Finds occupied channels in a dBFS spectrum and reports their centre
 * frequency, bandwidth, and peak power.  The detection model:
 *
 * 1. **Noise floor** — either a single global Nth-percentile value
 *    (detector_noise_floor) or a per-bin local estimate that tracks
 *    receiver filter rolloff and 1/f noise (detector_local_noise_floor).
 *
 * 2. **Hysteresis threshold** — bins above noise_floor[i]+threshold_db
 *    start a signal run (rise threshold).  Once inside a run the bin
 *    must stay above noise_floor[i]+threshold_db-hysteresis_db to
 *    remain active (fall threshold).  This bridges shallow intra-signal
 *    power dips without raising the false-detection rate.
 *
 * 3. **Gap merging** — after the hysteresis scan, adjacent runs whose
 *    gap is <= max_gap_bins are joined into one signal.  Use this for
 *    FSK signals whose inter-tone dips are too deep for hysteresis to
 *    span: detect each tone individually, then merge the results.
 *
 * 4. **Minimum width filter** — merged runs shorter than min_width_bins
 *    are discarded (noise spikes, carrier leakage).
 *
 * 5. **Centre and bandwidth** — the centre frequency is the
 *    power-weighted centroid (linear domain, not dB) of the final run,
 *    computed from the original spectrum so sub-bin accuracy is preserved.
 */

#ifndef DETECTOR_H
#define DETECTOR_H

#include <stdint.h>

/** @brief One detected signal. */
typedef struct {
    double center_hz;    /**< Power-weighted centroid frequency in Hz. */
    double start_hz;     /**< Lower threshold-crossing edge in Hz. */
    double stop_hz;      /**< Upper threshold-crossing edge in Hz. */
    double bandwidth_hz; /**< = stop_hz - start_hz. */
    float  peak_dbfs;    /**< Peak bin power inside the signal in dBFS. */
} detected_signal_t;

/**
 * @brief Estimate a single global noise floor from a percentile of the spectrum.
 *
 * @param spectrum   Power spectrum in dBFS, length @p n.
 * @param n          Number of bins.
 * @param percentile Percentile to extract (0-99). Typical: 25.
 * @return Estimated noise floor in dBFS.
 */
float detector_noise_floor(const float *spectrum, int n, int percentile);

/**
 * @brief Compute a per-bin local noise floor using a sliding-window percentile.
 *
 * For each bin @p i the @p percentile of the surrounding @p window_bins values
 * is written to @p out[i].  This tracks receiver filter rolloff and low-frequency
 * noise slope, avoiding false positives near spectrum edges that a single global
 * value causes.
 *
 * @param spectrum    Power spectrum in dBFS, length @p n.
 * @param n           Number of bins.
 * @param window_bins Sliding-window width (32-128 typical).
 *                    Should be >= 2-3x the widest expected signal in bins.
 * @param percentile  Percentile to extract (0-99). Typical: 25.
 * @param out         Output array, length @p n.  Must be pre-allocated.
 */
void detector_local_noise_floor(const float *spectrum, int n,
                                int window_bins, int percentile,
                                float *out);

/**
 * @brief Find signals in a DC-centred power spectrum.
 *
 * Three-step process:
 *  1. Hysteresis scan  — collect runs using per-bin rise/fall thresholds.
 *  2. Gap merge        — join adjacent runs with gap <= max_gap_bins.
 *  3. Filter & measure — drop runs < min_width_bins; compute centroid/BW/peak.
 *
 * @param spectrum       Power spectrum in dBFS, DC-centred, length @p n.
 * @param n              Number of bins (= FFT width).
 * @param cf_hz          Tuner centre frequency in Hz.
 * @param samplerate     SDR sample rate in Hz (= RTBW).
 * @param noise_floor    Per-bin noise floor array, length @p n.
 *                       Fill with the same scalar for a global floor.
 * @param threshold_db   Rise threshold above noise_floor[i] in dB.
 * @param hysteresis_db  Fall offset: fall_thresh = threshold_db - hysteresis_db.
 *                       Pass 0 to disable.
 * @param min_width_bins Minimum signal width in bins after merging.
 * @param max_gap_bins   Maximum inter-run gap to bridge. Pass 0 to disable.
 * @param out            Output array for detected signals.
 * @param max_out        Capacity of @p out.
 * @return Number of signals written to @p out (<= max_out).
 */
int detector_find_signals(const float *spectrum, int n,
                          double cf_hz, double samplerate,
                          const float *noise_floor,
                          float threshold_db,
                          float hysteresis_db,
                          int min_width_bins,
                          int max_gap_bins,
                          detected_signal_t *out, int max_out);

#endif
