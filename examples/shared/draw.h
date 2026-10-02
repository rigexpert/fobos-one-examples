/**
 * @file draw.h
 * @brief Terminal spectrum display: header bar, spectrum waterfall, and frequency axis.
 *
 * Renders a live FFT power spectrum directly in the terminal using ANSI escape
 * sequences and Unicode eighth-block characters (▁▂▃▄▅▆▇█) for 8× sub-character
 * vertical resolution.
 *
 * Layout (top to bottom):
 * @verbatim
 * ┌─────────────────────────────────────────────────────────────────────────────┐
 * │   CF: 100000000 Hz | SR: 20000000 S/s | Base: -75.0 dB | Ovlp: 0% | …    │  ← header (row 1)
 * │ -10dB|████████████████████████████▇▆▅▃▂▁                                   │
 * │      |                        ██████████▇▆▅▄▃▂▁                            │
 * │ -50dB|          ▂▃▄███████████████████████████████▇▆▄▃▂▁                   │  ← spectrum bars
 * │      |     ▂▄▆█████████████████████████████████████████▇▅▃▁                │
 * │-100dB|  ▃▅███████████████████████████████████████████████████▆▄▃▁          │
 * │        --------+----------+----------+----------+----------+----------+----- │  ← tick row
 * │                90.0M     92.0M     94.0M     96.0M     98.0M    100.0M     │  ← label row
 * └─────────────────────────────────────────────────────────────────────────────┘
 * @endverbatim
 *
 * Call draw_init() once at startup, then draw_fft() each time a new averaged
 * FFT result is ready.  The display adapts automatically to the current terminal
 * size on every call to draw_fft().
 */

#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

/**
 * @brief Initialise the display context and print the static header line.
 *
 * Stores the parameters used to build the header and frequency axis labels.
 * Must be called before draw_fft().  Call draw_hide_cursor() after this to
 * suppress cursor flicker during live updates.
 *
 * @param[in] cf         Centre frequency in Hz (used for frequency-axis labels).
 * @param[in] rate       Sample rate in Hz (defines the displayed bandwidth).
 * @param[in] baselavel  Reference noise floor in dB (shown in the header).
 * @param[in] overlap    Overlap between consecutive FFT windows in samples
 *                       (shown as a percentage of @p fft_width in the header).
 * @param[in] avarage    Number of FFTs averaged per display update (shown in header).
 * @param[in] fft_width  FFT length in bins (shown in header).
 * @param[in] min_db     Minimum dB level mapped to the bottom of the display.
 * @param[in] max_db     Maximum dB level mapped to the top of the display.
 */
void draw_init(uint64_t cf, uint64_t rate, float baselavel, int overlap,
               int avarage, uint32_t fft_width, float min_db, float max_db);

/**
 * @brief Render one frame of the spectrum display.
 *
 * Compresses @p fft_bins FFT magnitude values into the current terminal width,
 * draws the spectrum bars with sub-character vertical resolution, and renders
 * the frequency axis (tick marks + labels) below.
 *
 * Reads the terminal size on each call so the display adapts when the window
 * is resized.  The left margin is reserved for dB axis labels; the remaining
 * width is used for spectrum bars.
 *
 * @param[in] mag       FFT magnitude in dBFS, @p fft_bins values, DC-centred
 *                      (lower half of FFT followed by upper half, i.e. as
 *                      produced by fft_log() with the bin-swap already applied).
 * @param[in] fft_bins  Total number of magnitude values in @p mag (= FFT length).
 * @param[in] min_db    Lower bound of the dB scale (maps to bottom of display).
 * @param[in] max_db    Upper bound of the dB scale (maps to top of display).
 */
void draw_fft(float *mag, size_t fft_bins, float min_db, float max_db);

/**
 * @brief Erase the entire terminal and move the cursor to the top-left corner.
 *
 * Use once at startup before draw_hide_cursor() to give the spectrum a clean
 * background.  Avoid calling during live updates as it causes visible flicker.
 */
void draw_clear_screen(void);

/**
 * @brief Hide the terminal cursor to eliminate flicker during live updates.
 *
 * Call once after draw_init().  Always pair with draw_show_cursor() on exit
 * so the terminal is left in a usable state.
 */
void draw_hide_cursor(void);

/**
 * @brief Restore the terminal cursor.
 *
 * Call before the program exits (or from a signal handler) to undo the effect
 * of draw_hide_cursor().
 */
void draw_show_cursor(void);

/**
 * @brief Register a per-bin max-hold buffer for display.
 *
 * The buffer is owned by the caller and must remain valid until the next call
 * to draw_set_maxhold() or until the program exits.  Pass @p maxhold = NULL to
 * disable max-hold rendering.  While active a yellow ▄ tick is drawn one row
 * above the live spectrum bar for each column whose max-hold level exceeds the
 * current live value.  The header line shows "| MAX-HOLD (c=clear)".
 *
 * @param[in] maxhold  dBFS values, @p n_bins wide (same order as @c mag in
 *                     draw_fft()).  May be NULL to disable.
 * @param[in] n_bins   Length of @p maxhold.  Ignored when @p maxhold is NULL.
 */
void draw_set_maxhold(const float *maxhold, int n_bins);

/**
 * @brief Put stdin into raw non-blocking mode for keypress polling.
 *
 * After this call, draw_check_keypress() returns individual key codes without
 * waiting for Enter and without echoing.  Call draw_disable_keypress() before
 * exit to restore the original terminal settings.
 */
void draw_enable_keypress(void);

/**
 * @brief Restore stdin to the original (cooked) terminal mode.
 *
 * Safe to call even if draw_enable_keypress() was never called.
 */
void draw_disable_keypress(void);

/**
 * @brief Poll for a single keypress (non-blocking).
 *
 * Returns the character code of the key pressed, or -1 if no key is available.
 * Requires draw_enable_keypress() to have been called first.
 *
 * @return ASCII/Unicode code point, or -1 if no key pressed.
 */
int draw_check_keypress(void);

#endif
