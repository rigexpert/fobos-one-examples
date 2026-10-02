/**
 * @file fft.c
 * @brief FFT helper implementation: Hamming windowing, FFTW execution, power accumulation, and log conversion.
 *
 * **Windowing** — fft_precalculate_window() computes one block of Hamming coefficients using the formula from DSP Guide ch. 16.
 * Hamming is chosen over Blackman for its faster roll-off, which is preferable for live spectrum display where temporal resolution
 * matters more than ultimate stopband attenuation.  fft_apply_precalculated_window() multiplies each complex sample by its
 * coefficient; both I and Q are scaled identically so the phase is preserved.
 *
 * **FFT** — fft_create_plan() wraps @c fftwf_plan_dft_1d (single-precision, forward DFT).  FFTW_MEASURE is used so the library
 * benchmarks candidate algorithms at plan creation time and selects the fastest for the current hardware.
 * If compiled with @c -DFFTW3_THREADS, all CPU cores are used.
 *
 * **Power accumulation** — fft_accumulate_power() adds |Z|² = I²+Q² for each bin into a running sum.
 * Summing power (not amplitude) before the log gives a statistically consistent noise floor estimate:
 * the noise variance decreases as 1/N where N is the number of accumulated FFTs.
 *
 * **Log conversion** — fft_log() computes 10·log10(sum) + add_db.  The caller supplies
 * add_db = baselevel − 10·log10(N) to compensate for N accumulations and shift the result to the desired
 * display reference level.  The DC-centring bin swap (lower half ↔ upper half) is done by the caller in
 * main.c, not here, keeping this module agnostic of display concerns.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <errno.h>
#include <assert.h>

#include <fftw3.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "fft.h"

#define FFTW3_THEADS_NUM nprocs()

#ifdef FFTW3_THREADS
static int nprocs(){
  cpu_set_t cs;
  CPU_ZERO(&cs);
  sched_getaffinity(0, sizeof(cs), &cs);
  return CPU_COUNT(&cs);
}
#endif

inline static float firdes_wkernel_hamming(float rate){
    //Explanation at Chapter 16 of dspguide.com, page 2
    //Hamming window has worse stopband attentuation and passband ripple than Blackman, but it has faster rolloff.
    rate=0.5+rate/2;
    return 0.54-0.46*cos(2*M_PI*rate);
}

float *fft_precalculate_window(float *windowt, int size){    
    assert(windowt);

    for(int i=0;i<size;i++) //@precalculate_window
    {
        float rate=(float)i/(size-1);
        windowt[i] = firdes_wkernel_hamming(2.0*rate+1.0);
    }
    return windowt;
}


void fft_apply_precalculated_window(complexf* input, complexf* output, int size, float *windowt) {
    const float *in  = (const float *)input;
    float       *out = (float *)output;
    int i = 0;
#ifdef __ARM_NEON
    for (; i <= size - 4; i += 4) {
        float32x4x2_t iq = vld2q_f32(in + 2 * i);
        float32x4_t w = vld1q_f32(windowt + i);
        iq.val[0] = vmulq_f32(iq.val[0], w);
        iq.val[1] = vmulq_f32(iq.val[1], w);
        vst2q_f32(out + 2 * i, iq);
    }
#endif
    for (; i < size; i++) {
        out[2*i]   = in[2*i]   * windowt[i];
        out[2*i+1] = in[2*i+1] * windowt[i];
    }
}

void fft_load_wisdom(const char *path) {
    fftwf_import_wisdom_from_filename(path); /* silent if file absent */
}

void fft_save_wisdom(const char *path) {
    if (!fftwf_export_wisdom_to_filename(path))
        fprintf(stderr, "fft: failed to save wisdom to %s\n", path);
}

fft_plan_s* fft_create_plan(int size, complexf* input, complexf* output){
#ifdef FFTW3_THREADS
	fftwf_init_threads();
	fftwf_plan_with_nthreads(FFTW3_THEADS_NUM);
#endif

	fft_plan_s* plan=(fft_plan_s*)malloc(sizeof(fft_plan_s));
	plan->plan = fftwf_plan_dft_1d(size, (fftwf_complex*)input, (fftwf_complex*)output, FFTW_FORWARD, FFTW_MEASURE);
	plan->size=size;
	plan->input=(void*)input;
	plan->output=(void*)output;
	return plan;
}

void fft_execute(fft_plan_s* plan){
	fftwf_execute(plan->plan);
}

void fft_accumulate_power(complexf* input, float* output, int size) {
    const float *in = (const float *)input;
    int i = 0;
#ifdef __ARM_NEON
    for (; i <= size - 4; i += 4) {
        float32x4x2_t iq  = vld2q_f32(in + 2 * i);
        float32x4_t   pow = vaddq_f32(vmulq_f32(iq.val[0], iq.val[0]),
                                      vmulq_f32(iq.val[1], iq.val[1]));
        vst1q_f32(output + i, vaddq_f32(vld1q_f32(output + i), pow));
    }
#endif
    for (; i < size; i++)
        output[i] += in[2*i]*in[2*i] + in[2*i+1]*in[2*i+1];
}

void fft_log(float* input, float* output, int size, float add_db) {
    for(int i=0;i<size;i++) 
		output[i]=log10(input[i]) * 10 + add_db; 
}

