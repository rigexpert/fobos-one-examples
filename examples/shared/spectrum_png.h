/**
 * @file spectrum_png.h
 * @brief Save a 1-D FFT spectrum snapshot as a PNG image or CSV file.
 *
 * Both functions accept a raw dBFS array (same layout as draw_fft() — DC-centred,
 * lower half then upper half of the FFT) together with the frequency range it
 * spans.
 *
 * **PNG width limiting**: rendering a 200,000-bin panorama at one pixel per bin
 * would require hundreds of megabytes of RAM and many image viewers would fail
 * to open the result.  spectrum_png_write() therefore accepts a @p max_px_width
 * cap; when the actual bin count exceeds it the bins are peak-compressed into
 * @p max_px_width columns using the same algorithm as draw_fft().  Pass 0 to
 * use the built-in default (SPECTRUM_PNG_MAX_W = 4096 pixels).
 *
 * **PNG image format**:
 * @verbatim
 *   width  : min(n_bins, max_px_width)
 *   height : SPECTRUM_PNG_IMG_H (384 px)
 *   colour : blue → cyan → yellow gradient bar (↑ = high power); top row = white
 *   bg     : near-black dark blue (#0a0a18)
 * @endverbatim
 *
 * **CSV format**: two columns, one row per (possibly compressed) bin:
 * @verbatim
 *   freq_hz,dbfs
 *   88000000.0,-82.5
 *   88009765.6,-81.1
 *   ...
 * @endverbatim
 */

#ifndef SPECTRUM_PNG_H
#define SPECTRUM_PNG_H

/** Default maximum PNG output width in pixels. */
#define SPECTRUM_PNG_MAX_W    4096
/** PNG image height in pixels. */
#define SPECTRUM_PNG_IMG_H     384

/**
 * @brief Write a spectrum snapshot as a bar-chart PNG.
 *
 * @param filename      Output file path (e.g. "maxhold.png").
 * @param spec          dBFS values, @p n_bins wide.  -INFINITY bins are treated
 *                      as @p min_db (bar at zero height).
 * @param n_bins        Number of elements in @p spec.
 * @param min_db        dBFS value that maps to bar height 0 (bottom).
 * @param max_db        dBFS value that maps to bar height 1 (top).
 * @param freq_from_hz  Frequency of @p spec[0] in Hz.
 * @param freq_to_hz    Frequency of @p spec[n_bins-1] in Hz.
 * @param max_px_width  Maximum output image width; 0 = use SPECTRUM_PNG_MAX_W.
 *                      When n_bins > max_px_width the bins are peak-compressed.
 * @return 0 on success, -1 on error (prints reason to stderr).
 */
int spectrum_png_write(const char *filename,
                       const float *spec, int n_bins,
                       float min_db, float max_db,
                       double freq_from_hz, double freq_to_hz,
                       int max_px_width);

/**
 * @brief Write a spectrum snapshot as a two-column CSV.
 *
 * Columns: @c freq_hz (centre frequency of each bin, Hz) and @c dbfs.
 * -INFINITY entries are written as the literal string "-inf".
 *
 * @param filename      Output file path (e.g. "maxhold.csv").
 * @param spec          dBFS values, @p n_bins wide.
 * @param n_bins        Number of elements in @p spec.
 * @param freq_from_hz  Frequency of @p spec[0] in Hz.
 * @param freq_to_hz    Frequency of @p spec[n_bins-1] in Hz.
 * @return 0 on success, -1 on error.
 */
int spectrum_csv_write(const char *filename,
                       const float *spec, int n_bins,
                       double freq_from_hz, double freq_to_hz);

#endif /* SPECTRUM_PNG_H */
