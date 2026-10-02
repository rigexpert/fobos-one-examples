/**
 * @file fft.h
 * @brief FFT helper library: windowing, power accumulation, and log conversion.
 *
 * Wraps the FFTW3 single-precision library with a small set of helpers that
 * cover the typical SDR spectrum-analyser pipeline:
 *
 * 1. Allocate a plan with fft_create_plan() — done once at startup.
 * 2. Pre-compute the Hamming window with fft_precalculate_window() — done once.
 * 3. Per FFT iteration:
 *    - fft_apply_precalculated_window() — multiply IQ samples by the window.
 *    - fft_execute() — run the FFTW plan.
 *    - fft_accumulate_power() — add |Z|² of each bin into a running average buffer.
 * 4. Every N iterations:
 *    - fft_log() — convert the averaged power to dBFS and bin-swap (DC to centre).
 *
 * The window suppresses spectral leakage from the hard edges of each finite
 * IQ block.  Averaging N FFTs before taking the log reduces noise variance by
 * √N, revealing weak signals buried in the noise floor.
 *
 * If the library is compiled with @c -DFFTW3_THREADS, the FFTW plan is created
 * with multi-thread support using all CPU cores visible to the process.
 */

#ifndef FFT_H
#define FFT_H

#include <stdint.h>
#include <fftw3.h>

/**
 * @brief Access the in-phase (real) component of sample @p i in an interleaved
 *        complex float array pointed to by @p complexf_input_p.
 */
#define iof(complexf_input_p, i) (*(((float *)(complexf_input_p)) + 2*(i)))

/**
 * @brief Access the quadrature (imaginary) component of sample @p i in an
 *        interleaved complex float array pointed to by @p complexf_input_p.
 */
#define qof(complexf_input_p, i) (*(((float *)(complexf_input_p)) + 2*(i) + 1))

/**
 * @brief Interleaved complex float32 sample (I/Q pair).
 *
 * Memory layout is identical to @c fftwf_complex, so buffers of @c complexf
 * can be cast directly to @c fftwf_complex * without copying.
 */
typedef struct complexf_s  { float   i; float   q; } complexf;

/** @brief Interleaved complex signed-16-bit sample. */
typedef struct complexs16_s { int16_t i; int16_t q; } complexs16;

/** @brief Interleaved complex unsigned-8-bit sample (RTL-SDR native format). */
typedef struct complexs8_u  { uint8_t i; uint8_t q; } complexu8;

/**
 * @brief FFTW execution plan and its associated buffer pointers.
 *
 * Created once by fft_create_plan() and reused for every FFT call.
 * FFTW plans are not thread-safe; use one plan per thread.
 */
typedef struct fft_plan_s {
    int   size;    /**< FFT length in complex samples (must be a power of two). */
    void *input;   /**< Pointer to the windowed input buffer (@c complexf array). */
    void *output;  /**< Pointer to the frequency-domain output buffer (@c complexf array). */
    fftwf_plan plan; /**< Underlying FFTW single-precision plan. */
} fft_plan_s;

/**
 * @brief Sample encoding of an IQ stream.
 */
typedef enum data_format_enum {
    data_format_f32 = 0, /**< Normalised float32 I/Q (±1.0). */
    data_format_s16,     /**< Signed 16-bit I/Q. */
    data_format_u8       /**< Unsigned 8-bit I/Q (RTL-SDR style, centred at 127). */
} data_format_enum;

/** @brief Compact alias for @c data_format_enum. */
typedef uint8_t data_format_t;

/**
 * @brief Convert accumulated linear power to dBFS and add an offset.
 *
 * Computes @c output[i] = 10·log10(input[i]) + add_db for each bin.
 * Typically called after fft_accumulate_power() has summed @p N FFTs;
 * pass @c add_db = baselevel - 10·log10(N) to normalise the result.
 *
 * @param[in]  input   Accumulated linear power buffer (@p size floats).
 * @param[out] output  Resulting dBFS values (@p size floats; may alias @p input).
 * @param[in]  size    Number of FFT bins to process (= FFT length / 2 per half).
 * @param[in]  add_db  dB offset added to every bin (e.g. base level − averaging correction).
 */
void fft_log(float *input, float *output, int size, float add_db);

/**
 * @brief Accumulate squared magnitude (power) of one FFT output into a buffer.
 *
 * Adds @c I²+Q² of each bin to the running sum in @p output.  Call this once
 * per FFT iteration; after @p N iterations call fft_log() to produce dBFS.
 * The caller must zero @p output before starting a new averaging window.
 *
 * @param[in]     input   Complex FFT output from fft_execute() (@p size samples).
 * @param[in,out] output  Accumulation buffer (@p size floats, caller-zeroed).
 * @param[in]     size    Number of bins (= FFT length).
 */
void fft_accumulate_power(complexf *input, float *output, int size);

/**
 * @brief Execute the FFT on the data currently in @p plan->input.
 *
 * A thin wrapper around @c fftwf_execute().  The input buffer must have been
 * filled (and windowed) before this call; the result appears in @p plan->output.
 *
 * @param[in] plan  Plan created by fft_create_plan().
 */
void fft_execute(fft_plan_s *plan);

/**
 * @brief Allocate and configure an FFTW single-precision forward plan.
 *
 * Uses @c FFTW_MEASURE to find the fastest algorithm for this size on the
 * current hardware (may take a few seconds the first time for large sizes).
 * If compiled with @c -DFFTW3_THREADS, initialises multi-threaded FFTW using
 * all CPU cores visible to the process.
 *
 * @param[in] size    FFT length in complex samples (must be a power of two).
 * @param[in] input   FFTW-allocated input buffer (@p size @c complexf samples).
 * @param[in] output  FFTW-allocated output buffer (@p size @c complexf samples).
 * @return Pointer to a heap-allocated @c fft_plan_s, or NULL on failure.
 *         Free with @c fftwf_destroy_plan(plan->plan); free(plan).
 */
fft_plan_s *fft_create_plan(int size, complexf *input, complexf *output);

/**
 * @brief Load FFTW wisdom from @p path into the global wisdom system.
 *
 * Call before fft_create_plan() so FFTW can reuse a previously measured plan
 * instead of re-benchmarking.  Silent if the file does not exist yet.
 */
void fft_load_wisdom(const char *path);

/**
 * @brief Export the current FFTW wisdom to @p path.
 *
 * Call after fft_create_plan() to persist the measured plan for future runs.
 * Prints a warning to stderr on failure.
 */
void fft_save_wisdom(const char *path);

/**
 * @brief Apply a pre-computed Hamming window to a block of complex IQ samples.
 *
 * Multiplies each complex sample by the corresponding scalar window coefficient:
 * @verbatim
 *   output[i] = input[i] × windowt[i]
 * @endverbatim
 * The window reduces spectral leakage at the cost of slightly widening the
 * main lobe.  Hamming offers fast roll-off with moderate stopband attenuation,
 * which is a good trade-off for SDR spectrum analysis.
 *
 * @param[in]  input    Raw IQ samples to window (@p size @c complexf).
 * @param[out] output   Windowed output (@p size @c complexf; must not alias @p input).
 * @param[in]  size     Number of complex samples (= FFT length).
 * @param[in]  windowt  Pre-computed window coefficients from fft_precalculate_window().
 */
void fft_apply_precalculated_window(complexf *input, complexf *output, int size, float *windowt);

/**
 * @brief Compute and store Hamming window coefficients for a given FFT length.
 *
 * Fills @p windowt with @p size Hamming window values in the range (0, 1].
 * Call this once at startup; pass the result to every fft_apply_precalculated_window()
 * call to avoid recomputing the coefficients on each iteration.
 *
 * @param[out] windowt  Caller-allocated float array of at least @p size elements.
 * @param[in]  size     FFT length (number of window coefficients to generate).
 * @return @p windowt (for optional chaining).
 */
float *fft_precalculate_window(float *windowt, int size);

#endif
