/**
 * @file dsp.c
 * @brief IQ format conversion and DDC frequency shift implementation.
 *
 * Implements the two DSP helpers declared in dsp.h:
 *
 * **convert_s16_f** — straightforward element-wise cast and normalisation.
 *
 * **shift_unroll_init / shift_unroll_process** — Digital Down Converter (DDC)
 * using a pre-computed LO (local oscillator) look-up table.
 *
 * The LUT stores one processing block's worth of sin/cos values at the desired
 * shift frequency.  At runtime, shift_unroll_process() combines the LUT values
 * with a starting phase (carried over from the previous block) via the angle
 * addition identity:
 * @verbatim
 *   cos(α+β) = cos(α)cos(β) − sin(α)sin(β)
 *   sin(α+β) = sin(α)cos(β) + cos(α)sin(β)
 * @endverbatim
 * This keeps the LO phase continuous across block boundaries without a
 * per-sample sin/cos call.
 *
 * On ARMv8 targets the f32 path is auto-selected at compile time via
 * __ARM_NEON and processes four complex samples per NEON iteration using
 * vld2q_f32 / vmulq_f32 / vaddq_f32 / vst2q_f32.  A portable scalar
 * fallback handles all other targets and the tail samples on ARM.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <getopt.h>
#include <fcntl.h>
#include <errno.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

#include "liquid/liquid.h"
#include "dsp.h"

#ifndef M_PI
#define M_PI 3.1415926535897932384626433832795
#endif 

#define iof(complexf_input_p,i) (*(((float*)complexf_input_p)+2*(i)))
#define qof(complexf_input_p,i) (*(((float*)complexf_input_p)+2*(i)+1))

void convert_s16_f(short* input, float* output, int input_size){
    for(int i=0;i<input_size;i++)
        output[i]=(float)input[i]/SHRT_MAX; //@convert_s16_f
}

shift_unroll_data_t shift_unroll_init(float rate, int size){
	shift_unroll_data_t output;
	output.phase_increment=2*rate*M_PI;
	output.size = size;
	output.dsin = (float*)aligned_alloc(16, sizeof(float) * size);
	output.dcos = (float*)aligned_alloc(16, sizeof(float) * size);
	float myphase = 0;
	for (int i=0;i<size;i++) {
		myphase += output.phase_increment;
		while (myphase>M_PI)
			myphase-=2*M_PI;
		while (myphase<-M_PI)
			myphase+=2*M_PI;
		output.dsin[i]=sin(myphase);
		output.dcos[i]=cos(myphase);
	}
	return output;
}

float shift_unroll_process(void *input, data_format_t input_format, complexf* output, int input_size, shift_unroll_data_t* d, float starting_phase){
	float cos_start = cos(starting_phase);
	float sin_start = sin(starting_phase);
	float cos_val, sin_val;

	if(input_format == data_format_s16){
		for (int i=0;i<input_size; i++) {
			cos_val = cos_start * d->dcos[i] - sin_start * d->dsin[i];
			sin_val = sin_start * d->dcos[i] + cos_start * d->dsin[i];
			float in_i = (((float)*(uint16_t *)input + (2*i)))/SHRT_MAX;
			float in_q = (((float)*(uint16_t *)input + (2*i+1)))/SHRT_MAX;
			iof(output,i) = cos_val*in_i - sin_val*in_q;
			qof(output,i) = sin_val*in_i + cos_val*in_q;
		}
	} else if(input_format == data_format_f32){
		const float *in = (const float *)input;
		float       *out = (float *)output;

#ifdef __ARM_NEON
		/* Process 4 complex samples per iteration using NEON.
		 *
		 * Each sample: out = in * LO,  where LO[i] = exp(j*(phase + i*inc))
		 *              = (cs + j*ss) * (dcos[i] + j*dsin[i])
		 *
		 * lo_re = cs*dcos - ss*dsin
		 * lo_im = ss*dcos + cs*dsin
		 * out_re = lo_re*in_re - lo_im*in_im
		 * out_im = lo_im*in_re + lo_re*in_im
		 */
		const float32x4_t cs_v = vdupq_n_f32(cos_start);
		const float32x4_t ss_v = vdupq_n_f32(sin_start);

		int i = 0;
		for (; i <= input_size - 4; i += 4) {
			/* deinterleave: in_re=[re0..re3], in_im=[im0..im3] */
			float32x4x2_t iq = vld2q_f32(in + 2*i);
			float32x4_t in_re = iq.val[0];
			float32x4_t in_im = iq.val[1];

			float32x4_t lc = vld1q_f32(d->dcos + i);
			float32x4_t ls = vld1q_f32(d->dsin + i);

			float32x4_t lo_re = vsubq_f32(vmulq_f32(cs_v, lc), vmulq_f32(ss_v, ls));
			float32x4_t lo_im = vaddq_f32(vmulq_f32(ss_v, lc), vmulq_f32(cs_v, ls));

			float32x4_t out_re = vsubq_f32(vmulq_f32(lo_re, in_re), vmulq_f32(lo_im, in_im));
			float32x4_t out_im = vaddq_f32(vmulq_f32(lo_im, in_re), vmulq_f32(lo_re, in_im));

			float32x4x2_t res;
			res.val[0] = out_re;
			res.val[1] = out_im;
			vst2q_f32(out + 2*i, res);
		}
		/* scalar tail for remaining samples */
		for (; i < input_size; i++) {
			cos_val = cos_start * d->dcos[i] - sin_start * d->dsin[i];
			sin_val = sin_start * d->dcos[i] + cos_start * d->dsin[i];
			out[2*i]   = cos_val * in[2*i]   - sin_val * in[2*i+1];
			out[2*i+1] = sin_val * in[2*i]   + cos_val * in[2*i+1];
		}
#else
		for (int i = 0; i < input_size; i++) {
			cos_val = cos_start * d->dcos[i] - sin_start * d->dsin[i];
			sin_val = sin_start * d->dcos[i] + cos_start * d->dsin[i];
			iof(output,i) = cos_val*iof(input,i) - sin_val*qof(input,i);
			qof(output,i) = sin_val*iof(input,i) + cos_val*qof(input,i);
		}
#endif
	}
	starting_phase+=input_size*d->phase_increment;
	while (starting_phase>M_PI)
		starting_phase-=2*M_PI;
	while (starting_phase<-M_PI)
		starting_phase+=2*M_PI;

	return starting_phase;
}

