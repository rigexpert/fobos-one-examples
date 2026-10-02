/**
 * @file dsp.h
 * @brief DSP primitives: a reusable FFTW plan, a window, DC-notch interpolation, and a
 *        coherent single-bin carrier power meter.
 */
#pragma once

#include <mutex>
#include <vector>
#include <fftw3.h>

/**
 * @brief A cached single-precision forward FFTW plan with its input/output buffers.
 *
 * Call @ref ensure to (re)allocate for a given transform size; the plan and buffers are
 * reused across calls of the same size. FFTW plan creation/destruction is serialized
 * internally, but @ref in / @ref out are not — use one FFT instance per thread.
 */
struct FFT {
    int n = 0;                     ///< Current transform size (0 until ensure()).
    fftwf_plan plan = nullptr;     ///< The FFTW plan (nullptr until ensure()).
    fftwf_complex* in = nullptr;   ///< Input buffer of @ref n complex samples.
    fftwf_complex* out = nullptr;  ///< Output buffer of @ref n complex bins.

    /**
     * @brief Ensure the plan and buffers are allocated for a given size.
     * @param size Transform length. A no-op if already sized to @p size.
     */
    void ensure(int size);

    /// @brief Destroy the plan and free the buffers, resetting to the empty state.
    void clear();

    /// @brief Destructor; calls @ref clear.
    ~FFT();

private:
    /// @return The shared mutex serializing FFTW plan create/destroy (not thread-safe).
    static std::mutex& plan_mutex();
};

/**
 * @brief Build a Hamming window.
 * @param n Window length.
 * @return The @p n window coefficients.
 */
std::vector<float> hamming(int n);

/**
 * @brief Interpolate across the DC/LO bins of a spectrum, in place.
 *
 * Replaces the bins within @p width_hz of the center (fftshifted, DC at n/2) with a
 * linear ramp between the neighbours so the LO spike doesn't dominate the trace.
 * @param psd      Power spectrum in dB (fftshifted); modified in place.
 * @param n        Number of bins in @p psd.
 * @param rate     Sample rate in Hz (bin spacing = rate / n).
 * @param width_hz Width of the notch in Hz; a no-op if <= 0.
 */
void notch_dc(std::vector<float>& psd, int n, double rate, double width_hz);

/**
 * @brief Measure the strongest carrier's absolute power coherently.
 *
 * Finds the dominant tone (outside the DC/LO region), refines its frequency to sub-bin
 * accuracy, then does a coherent DFT at that exact frequency. The result is FFT-size
 * independent and free of scalloping loss.
 * @param scratch     A reusable FFT instance (resized to @p n internally).
 * @param iq          Interleaved I/Q samples (2*n floats).
 * @param n           Number of complex samples.
 * @param rate        Sample rate in Hz.
 * @param center_freq RX center frequency in Hz (added to the baseband offset).
 * @param[out] dbfs   Carrier power in dBFS, or NaN if no carrier was found.
 * @param[out] freq   Absolute carrier frequency in Hz, or NaN if none.
 */
void carrier_meter(FFT& scratch, const float* iq, int n, double rate, double center_freq,
                   double* dbfs, double* freq);
