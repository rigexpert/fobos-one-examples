/**
 * @file dsp.h
 * @brief Lightweight DSP helpers: IQ format conversion and DDC frequency shifting.
 *
 * Provides two building blocks used in the FM demodulator pipeline:
 *
 * **Format conversion** — converts interleaved signed-16-bit IQ samples produced
 * by many SDR drivers into the normalised float32 format expected by liquid-dsp.
 *
 * **Digital Down Converter (DDC)** — shifts the centre of the IQ spectrum by a
 * fractional normalised frequency, bringing an off-centre signal of interest to
 * DC before decimation and demodulation.  The implementation pre-computes one
 * full block of the local-oscillator (LO) sine/cosine values at initialisation
 * time; per-block processing then uses only multiply–add operations.  On ARMv8
 * targets the inner loop is vectorised with NEON to handle four complex samples
 * per cycle.
 *
 * Typical usage:
 * @code
 *   // At startup — build LUT for one processing block
 *   float norm_shift = -freq_offset_hz / (float)sample_rate;  // negate: DDC shifts UP
 *   shift_unroll_data_t ddc = shift_unroll_init(norm_shift, block_size);
 *   float phase = 0.0f;
 *
 *   // In the main processing loop
 *   phase = shift_unroll_process(raw_iq, data_format_f32,
 *                                shifted_iq, block_size, &ddc, phase);
 * @endcode
 */

#ifndef DEMOD_H
#define DEMOD_H

#ifndef SHRT_MAX
#define SHRT_MAX 32767
#endif

/**
 * @brief Sample encoding of an IQ stream.
 *
 * Passed to shift_unroll_process() so it can handle both raw S16 capture
 * buffers and the normalised float32 buffers used by liquid-dsp.
 */
typedef enum {
    data_format_f32 = 0, /**< Interleaved float32 I/Q, normalised to ±1.0 */
    data_format_s16      /**< Interleaved signed-16-bit I/Q */
} data_format_t;

/**
 * @brief Interleaved complex float32 sample (one I/Q pair).
 *
 * Memory layout matches the @c float_complex convention used by liquid-dsp,
 * so buffers of @c complexf can be cast directly to @c liquid_float_complex *.
 */
typedef struct complexf {
    float i; /**< In-phase (real) component, normalised to ±1.0 */
    float q; /**< Quadrature (imaginary) component, normalised to ±1.0 */
} complexf;

/**
 * @brief Precomputed local-oscillator (LO) tables for the DDC.
 *
 * Created once by shift_unroll_init() for a fixed block size and frequency
 * shift.  The arrays @c dsin and @c dcos hold one full period of the LO
 * waveform sampled at the given @c phase_increment; shift_unroll_process()
 * multiplies these against incoming IQ samples to translate the spectrum.
 *
 * The arrays are 16-byte aligned so NEON/SSE vector loads can operate
 * without alignment penalties.
 */
typedef struct shift_unroll_data_t {
    float *dsin;           /**< Pre-computed LO sine values, length @c size */
    float *dcos;           /**< Pre-computed LO cosine values, length @c size */
    float  phase_increment;/**< Phase step per sample = 2π × normalised_shift */
    int    size;           /**< Block size the LUT was built for (samples) */
} shift_unroll_data_t;

/**
 * @brief Convert an interleaved S16 IQ buffer to normalised float32.
 *
 * Each 16-bit sample is divided by @c SHRT_MAX (32767) to produce a value in
 * the range [−1.0, +1.0].  The output buffer may be the same as the input
 * only if @c sizeof(short) == @c sizeof(float) (it is not on any standard
 * platform), so use separate buffers.
 *
 * @param[in]  input       Interleaved S16 I/Q samples.
 * @param[out] output      Corresponding float32 samples (same count).
 * @param[in]  input_size  Total number of scalar values (2 × IQ pair count).
 */
void convert_s16_f(short *input, float *output, int input_size);

/**
 * @brief Build DDC look-up tables for a given block size and frequency shift.
 *
 * Pre-computes one block of the LO waveform (sine and cosine) at the
 * specified normalised shift frequency.  Call this once at startup for the
 * largest block size that will be used; the returned struct is passed to every
 * call of shift_unroll_process().
 *
 * The caller is responsible for freeing @c result.dsin and @c result.dcos
 * when the DDC is no longer needed.
 *
 * @param[in] rate  Normalised frequency shift in cycles per sample.
 *                  Positive = shift spectrum right (up); negative = left (down).
 *                  Typical value: @c -freq_offset_hz / sample_rate.
 * @param[in] size  Processing block size in complex samples.
 * @return          Populated @c shift_unroll_data_t ready for shift_unroll_process().
 */
shift_unroll_data_t shift_unroll_init(float rate, int size);

/**
 * @brief Apply the DDC frequency shift to one block of IQ samples.
 *
 * Multiplies each complex input sample by the corresponding pre-computed LO
 * value, implementing the frequency translation:
 * @verbatim
 *   out[n] = in[n] × exp(j × (starting_phase + n × phase_increment))
 * @endverbatim
 *
 * On ARMv8 targets the loop is unrolled and vectorised with NEON (four complex
 * samples per iteration).  The scalar fallback is used on other architectures.
 *
 * Phase is tracked across calls so that the LO is continuous across block
 * boundaries; pass the returned phase value back as @p starting_phase on the
 * next call.
 *
 * @param[in]     input          Raw IQ input buffer (S16 or float32 interleaved).
 * @param[in]     input_format   Encoding of @p input; see @c data_format_t.
 * @param[out]    output         Frequency-shifted complex float32 output.
 * @param[in]     input_size     Number of complex samples to process.
 * @param[in]     d              Pre-computed LUT from shift_unroll_init().
 * @param[in]     starting_phase LO phase at the first sample of this block (radians).
 * @return                       Updated LO phase for the start of the next block.
 */
float shift_unroll_process(void *input, data_format_t input_format,
                           complexf *output, int input_size,
                           shift_unroll_data_t *d, float starting_phase);

#endif
