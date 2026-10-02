/**
 * @file sgram.h
 * @brief Rolling spectrogram: accumulates FFT sweeps and writes an annotated PNG.
 *
 * Each call to sgram_add_row() stores one panorama sweep as a coloured pixel
 * row using a thermal colourmap (black=low → blue → cyan → green → yellow →
 * red=high).  The buffer is circular: once full, oldest rows are overwritten.
 *
 * sgram_write_png() composes a PNG with:
 *   - A header band listing scan parameters (freq range, rate, FFT width, bin bw, …)
 *   - A left margin with elapsed-time labels (Y axis, seconds from start)
 *   - The spectrogram data (newest row at top, oldest at bottom)
 *   - A bottom margin with frequency tick marks and labels (X axis)
 *
 * Call sgram_set_params() after sgram_create() and before the first
 * sgram_add_row() to supply the scan parameters used for annotation.
 */

#ifndef SGRAM_H
#define SGRAM_H

#include <stddef.h>

/** Scan parameters used to annotate the PNG axes and header. */
typedef struct {
    double freq_from_hz;  /**< Start frequency (Hz). */
    double freq_to_hz;    /**< Stop frequency (Hz). */
    int    rate;          /**< Sample rate (Hz). */
    int    fft_width;     /**< FFT size (bins). */
    int    average;       /**< FFTs averaged per sweep. */
    int    n_steps;       /**< Number of scan steps. */
    double overlap_hz;    /**< Step overlap (Hz); 0 if disabled. */
} sgram_params_t;

typedef struct sgram_t sgram_t;

/**
 * @brief Allocate a spectrogram buffer.
 *
 * @param width           Panorama width in bins (pixels per row).
 * @param mem_limit_bytes Maximum memory for the pixel buffer.
 *                        Row capacity = mem_limit_bytes / (width × 3).
 * @return Handle, or NULL on allocation failure.
 */
sgram_t *sgram_create(int width, size_t mem_limit_bytes);

/**
 * @brief Store scan parameters for PNG annotation.
 *
 * Optional but strongly recommended.  Call once after sgram_create().
 * Without this, sgram_write_png() writes plain pixel data with no axes.
 */
void sgram_set_params(sgram_t *sg, const sgram_params_t *p);

/**
 * @brief Append one panorama sweep as a pixel row.
 *
 * Records a wall-clock timestamp so the Y-axis can show elapsed time.
 * Maps each bin's dB value from [min_db, max_db] to the thermal colourmap.
 *
 * @param sg       Spectrogram handle.
 * @param panorama dB values, one per bin (length = width from sgram_create).
 * @param min_db   dB value that maps to the cold end of the colourmap.
 * @param max_db   dB value that maps to the hot end.
 */
void sgram_add_row(sgram_t *sg, const float *panorama,
                   float min_db, float max_db);

/**
 * @brief Write all accumulated rows to an annotated PNG file.
 *
 * Newest sweep is at the top of the image; oldest is at the bottom.
 * If sgram_set_params() was called, the output includes a parameter header,
 * Y-axis time labels (seconds from first row), and X-axis frequency labels.
 *
 * @param sg       Spectrogram handle.
 * @param filename Output file path.
 * @return 0 on success, -1 on failure.
 */
int sgram_write_png(const sgram_t *sg, const char *filename);

/** @brief Number of rows currently stored (capped at capacity). */
int sgram_row_count(const sgram_t *sg);

/** @brief Free all resources. */
void sgram_destroy(sgram_t *sg);

#endif /* SGRAM_H */
