/**
 * @file main.c
 * @brief FM broadcast demodulator — Fobos SDR developer example.
 *
 * @section overview Overview
 *
 * This example demonstrates how to receive a real FM broadcast station using the Fobos SDR
 * and produce decoded mono audio using the liquid-dsp library.  It is intended as a starting
 * point for developers building their own SDR and DSP applications.
 *
 * @section pipeline DSP Pipeline
 *
 * The processing chain from antenna to audio file is:
 *
 * @verbatim
 * [Fobos SDR hardware]
 *        │  IQ float32, interleaved, at input_samplerate (e.g. 8 MSPS)
 *        ▼
 * [DDC — shift_unroll_process()]           ← optional, enabled with -O
 *   Translates an off-centre signal to DC by multiplying with a complex
 *   LO tone.  The LO table is pre-computed by shift_unroll_init().
 *        │
 *        ▼
 * [IQ Decimation — firdecim_crcf + msresamp_crcf]
 *   Two-stage down-conversion to output_samplerate (default: 200 kHz).
 *     Stage 1: integer polyphase FIR decimator (fast, handles bulk ratio).
 *     Stage 2: fractional msresamp fine-correction (handles remainder).
 *   Pure integer or pure msresamp paths selected automatically.
 *        │  IQ float32 at output_samplerate (200 kHz for BCFM)
 *        ▼
 * [FM Demodulation — freqdem_demodulate_block()]
 *   Recovers the instantaneous frequency deviation as a float in [−1, +1]
 *   (at full deviation).  kf = FM_KF_HEADROOM × Δf/fs ensures nominal FM
 *   audio sits well below ±1.0 even in the presence of noise.
 *        │  MPX float32 at output_samplerate (mono 0–15 kHz, pilot 19 kHz,
 *        │  stereo 23–53 kHz, RDS 57 kHz — everything up to ~99 kHz)
 *        ▼
 * [Hard clip at ±1.0]
 *   Prevents noise spikes from ringing through the FIR audio LPF.
 *   Perfectly linear below the threshold → no dynamic compression.
 *        │
 *        ▼
 * [Audio LPF — firfilt_rrrf]               ← optional audio stage (-a)
 *   Kaiser-windowed FIR at 15 kHz cutoff (passes mono audio, rejects
 *   the 19 kHz pilot, stereo sub-carrier, RDS, and SCA).
 *        │
 *        ▼
 * [Audio Resample — msresamp_rrrf]
 *   Down-converts from output_samplerate to audio_samplerate (e.g. 48 kHz).
 *        │
 *        ▼
 * [De-emphasis — 1st-order IIR]
 *   y[n] = (1−a)·x[n] + a·y[n−1],  a = exp(−1/(τ·fs))
 *   Corrects the HF pre-emphasis added at the transmitter.
 *   Default τ = 50 µs (Europe); use -D 75 for Americas.
 *        │  Audio float32 at audio_samplerate
 *        ▼
 * [Output gain]                             ← -g flag (default 1.0)
 *        │
 *        ▼
 * [Write f32 or s16]                        ← -e flag (default f32)
 *   Pipe to sox/aplay or save to file for offline analysis.
 * @endverbatim
 *
 * @section quickstart Quick Start
 *
 * Receive FM 102.5 MHz, output raw MPX at 200 kHz float32:
 * @code
 *   ./demod_fm -s 8000000 -F 102500000 -o mpx.f32
 * @endcode
 *
 * Receive FM 96.0 MHz (signal 200 kHz below a 96.2 MHz centre), output decoded mono audio at 48 kHz s16, play with sox:
 * @code
 *   ./demod_fm -s 8000000 -F 96200000 -O -200000 -a 48000 -e s16 | \
 *       sox -t raw -r 48000 -e signed -b 16 -c 1 - -d
 * @endcode
 *
 * @section extending Extending This Example
 *
 * - **Stereo decoding**: extract the pilot tone (19 kHz), double it to 38 kHz, demodulate the DSB-SC sub-carrier (23–53 kHz) to get L−R, combine with L+R (mono) to recover left and right channels.
 * - **RDS decoding**: after the audio LPF, extract the 57 kHz sub-carrier and pass it to a BPSK demodulator and RDS frame decoder.
 * - **Multiple stations**: run multiple instances with different -O offsets, or extend the pipeline to route the MPX into a channeliser.
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
#include <stdbool.h>

#ifndef __USE_POSIX
#define __USE_POSIX 1
#endif
#include <signal.h>

#include <liquid/liquid.h>

#include "dsp.h"
#include "receiver.h"

/* BCFM channel = 200 kHz (±75 kHz deviation + 25 kHz guard each side).
 * IQ sample rate must be ≥ 200 kHz to capture the full FM channel. */
#define DEFAULT_IQ_SAMPLERATE           200000
#define DEFAULT_OUTPUT_BLOCK_SIZE       (16*1024)
#define FOBOS_BUFFER_SIZE               (64*1024)
#define FOBOS_BUFFER_COUNT              32

/* Target duration for one hardware USB buffer fill (ms).
 * Larger = fewer callbacks, less overhead; smaller = lower latency. */
#define HW_BUFFER_TARGET_MS             4

/* Target duration for one DSP processing block at the output sample rate (ms).
 * Controls per-iteration latency: 20 ms is a good balance for streaming audio. */
#define DSP_BLOCK_TARGET_MS             20

/* FM broadcast standard: max carrier deviation is ±75 kHz */
#define FM_MAX_DEVIATION_HZ             75000.0f
/* FM mono audio occupies 0–15 kHz; pilot tone sits at 19 kHz */
#define FM_AUDIO_BANDWIDTH_HZ           15000.0f
#define FM_PILOT_TONE_HZ                19000.0f
/* kf headroom factor: default kf = FM_KF_HEADROOM * Δf/fs
 * At 1× theoretical kf, noise spikes drive the discriminator above ±1.0, ring
 * through the FIR audio LPF and produce audible distortion ("dirty" sound).
 * 2× headroom keeps nominal FM audio at ±0.5 — well inside the tanh limiter's
 * linear region.  Override with -k. */
#define FM_KF_HEADROOM                  2.8f

typedef struct {
	time_t start_time;
	time_t last_report_time;           /* last time verbose stats were printed */
	size_t expected_input_samples;     /* total input samples expected over record duration */
	size_t expected_output_samples;    /* total output samples expected over record duration */
	size_t samples_read_total;         /* cumulative IQ samples received from receiver */
	size_t samples_written_total;      /* cumulative audio samples written to output */
	int    firdecim_factor;            /* integer coarse decimation factor; 0 = no firdecim */
	int    bytes_per_iq_sample;        /* bytes for one IQ pair = sizeof(float) * 2 */
	size_t demod_output_capacity;      /* max demodulated samples per block (buffer allocation size) */
	size_t input_block_samples;        /* IQ samples read from receiver per processing block */
	float  freq_shift_normalized;      /* DDC shift amount = freq_offset_hz / input_samplerate */
	float  ddc_phase;                  /* running DDC oscillator phase across blocks */
	void  *raw_input_buf;              /* raw IQ buffer used before DDC frequency shift */
	float complex *iq_input;           /* complex IQ samples fed into the resampling stage */
	float complex *resampler_output;   /* output of the fine msresamp stage */
	float complex *decimator_output;   /* output of the integer firdecim stage */
	float complex *demod_input;        /* points to whichever buffer feeds the FM demodulator */
	float         *demod_output;       /* FM-demodulated MPX signal as float32 at output_samplerate */
	int16_t       *demod_output_s16;   /* demod_output as s16 — only used when audio stage is disabled */
	msresamp_crcf  fine_resampler;     /* liquid multistage rational resampler (fine correction) */
	firdecim_crcf  decimator;          /* liquid integer decimation FIR filter */
	freqdem        fm_demodulator;      /* liquid FM discriminator; kf set from args.fm_modulation_index */
	/* Audio stage: applied after FM demodulation when -a is specified */
	firfilt_rrrf   audio_lpf;          /* 15 kHz lowpass — removes pilot (19 kHz), stereo, RDS */
	msresamp_rrrf  audio_resampler;    /* resample MPX from output_samplerate to audio_samplerate */
	float         *audio_output;       /* audio samples after LPF and resampling */
	int16_t       *audio_output_s16;   /* audio_output as signed 16-bit */
	size_t         audio_output_capacity; /* allocated length of audio_output in samples */
	float          deemph_coeff;       /* IIR de-emphasis: a = exp(-1/(tau*fs)); 0 = disabled */
	float          deemph_state;       /* IIR de-emphasis previous output sample */
	size_t         fobos_buffer_size;  /* hardware USB transfer buffer size in complex samples */
	bool           run;                /* false when a signal handler requests shutdown */
} ctx_t;

typedef struct {
	int      input_samplerate;         /* SDR hardware capture rate in Hz */
	int      output_samplerate;        /* IQ channel rate after decimation (≥200 kHz for FM broadcast) */
	int      audio_samplerate;         /* final audio output rate (e.g. 48000); 0 = output raw MPX */
	int      signal_bandwidth_hz;      /* FM channel bandwidth in Hz */
	int64_t  center_frequency_hz;      /* SDR tuning frequency in Hz */
	int32_t  freq_offset_hz;           /* DDC shift: Hz offset from center_frequency_hz */
	time_t   record_duration_sec;      /* how many seconds to record; 0 = run forever */
	int      verbose;                  /* 1 = print runtime statistics to stderr */
	char    *output_filename;          /* output file path; NULL = stdout */
	data_format_t output_format;       /* f32 or s16 output encoding */
	size_t   output_block_size;        /* desired output samples per DSP processing block */
	shift_unroll_data_t ddc;           /* precomputed DDC sine/cosine lookup tables */
	float    fm_modulation_index;      /* FM demodulator kf = FM_KF_HEADROOM * Δf/fs */
	float    deemphasis_us;            /* de-emphasis time constant µs: 75 (Americas), 50 (Europe), 0 = off */
	float    output_gain;              /* linear gain applied after audio LPF + de-emphasis */
	int      lna_gain;                 /* LNA gain index; -1 = leave hardware default */
	int      vga_gain;                 /* VGA gain index; -1 = leave hardware default */
	receiver_mode_t receiver_mode;     /* sync or async capture mode */
} args_t;

static args_t args;
static ctx_t  ctx;

static void signal_handler(int signum) {
	static int count = 0;
	fprintf(stderr, "Received signal %d\n", signum);
	//in case second time signal - force exit
	if (count)
		exit(-1);
	count++;
	ctx.run = false;
}

void print_help(void) {
	fprintf(stderr, "demod_fm takes floating point IQ data from an SDR receiver and produces FM-demodulated audio\n");
	fprintf(stderr, "usage: demod_fm args\n");
	fprintf(stderr, "  -s, --samplerate   <hz>       SDR input sample rate (e.g. 35000000)\n");
	fprintf(stderr, "  -r, --resamplerate <hz>       IQ channel rate after decimation (default: %d — full BCFM channel width)\n", DEFAULT_IQ_SAMPLERATE);
	fprintf(stderr, "  -a, --audiorate    <hz>       audio output rate (e.g. 48000); enables LPF + resamp\n");
	fprintf(stderr, "  -F, --frequency    <hz>       SDR center frequency (e.g. 102500000)\n");
	fprintf(stderr, "  -O, --offset       <hz>       DDC frequency offset from center (default: 0)\n");
	fprintf(stderr, "  -b, --bandwidth    <hz>       FM channel bandwidth hint (default: output_samplerate/2)\n");
	fprintf(stderr, "  -k, --kf           <factor>   FM kf (default: %.0f * %.0f / output_samplerate; higher = more headroom for noise)\n", FM_KF_HEADROOM, FM_MAX_DEVIATION_HZ);
	fprintf(stderr, "  -o, --output       <file>     output file (default: stdout)\n");
	fprintf(stderr, "  -e, --output_format <f32|s16> output sample format (default: f32)\n");
	fprintf(stderr, "  -d, --duration     <seconds>  recording duration; 0 = run forever\n");
	fprintf(stderr, "  -L, --lna-gain     <value>    LNA gain 0..3 (0,1: 0 dB, 2: +16, 3: +33)\n");
	fprintf(stderr, "  -V, --vga-gain     <value>    VGA gain 0..31 (0..+62 dB, 2 dB step)\n");
	fprintf(stderr, "  -m, --mode         <sync|async> capture mode (default: sync)\n");
	fprintf(stderr, "  -D, --deemphasis   <us>         de-emphasis time constant µs: 50 (default/Europe), 75 (Americas), 0 = off\n");
	fprintf(stderr, "  -g, --gain         <factor>   output gain after LPF + de-emphasis (default: 1.0)\n");
	fprintf(stderr, "  -v, --verbose                 print runtime information\n");
	fprintf(stderr, "  -h, --help                    print this message\n\n");
	fprintf(stderr, "Example — FM 102.5 MHz, raw MPX at 200 kHz (full BCFM channel):\n");
	fprintf(stderr, "  demod_fm -s 8000000 -F 102500000 -o mpx.f32\n\n");
	fprintf(stderr, "Example — mono audio at 48 kHz s16:\n");
	fprintf(stderr, "  demod_fm -s 8000000 -a 48000 -F 102500000 -e s16 -o audio.raw\n");
}

static void dump_info(void) {
	fprintf(stderr, "Center frequency:       %ld Hz\n", args.center_frequency_hz);
	fprintf(stderr, "Frequency offset (DDC): %d Hz  (normalized shift: %+f, signal is at center%+d Hz)\n", args.freq_offset_hz, ctx.freq_shift_normalized, args.freq_offset_hz);
	fprintf(stderr, "Input sample rate:      %d Hz\n",  args.input_samplerate);
	fprintf(stderr, "IQ channel rate:        %d Hz\n",  args.output_samplerate);
	fprintf(stderr, "FM modulation index:    %.6f  (= %.0f * %.0f Hz / %d Hz)\n", args.fm_modulation_index, FM_KF_HEADROOM, FM_MAX_DEVIATION_HZ, args.output_samplerate);
	if (args.audio_samplerate > 0) {
		fprintf(stderr, "Audio output rate:      %d Hz\n",  args.audio_samplerate);
		fprintf(stderr, "Audio LPF cutoff:       %.0f Hz  (pilot at %.0f Hz is rejected)\n", FM_AUDIO_BANDWIDTH_HZ, FM_PILOT_TONE_HZ);
		if (args.deemphasis_us > 0.0f)
			fprintf(stderr, "De-emphasis:            %.0f µs (a=%.4f)\n", args.deemphasis_us, ctx.deemph_coeff);
		else
			fprintf(stderr, "De-emphasis:            disabled\n");
		fprintf(stderr, "Output gain:            %.2f\n", args.output_gain);
	} else {
		fprintf(stderr, "Audio stage:            disabled — raw MPX output at IQ channel rate\n");
	}
	fprintf(stderr, "Output block size:      %ld samples\n", args.output_block_size);
	fprintf(stderr, "Fobos hardware buffer:  %ld samples\n", ctx.fobos_buffer_size);
	fprintf(stderr, "Input block samples:    %ld\n",    ctx.input_block_samples);
	fprintf(stderr, "Demod output capacity:  %ld samples\n", ctx.demod_output_capacity);
	fprintf(stderr, "Capture mode:           %s\n", (args.receiver_mode == receiver_mode_sync) ? "SYNC" : "ASYNC");
}

void parse_args(int argc, char *argv[]) {
	int opt        = 0;
	int long_index = 0;

	struct option long_options[] = {
		{"samplerate",   required_argument, 0, 's'},
		{"resamplerate", required_argument, 0, 'r'},
		{"audiorate",    required_argument, 0, 'a'},
		{"bandwidth",    required_argument, 0, 'b'},
		{"output_format",optional_argument, 0, 'e'},
		{"output",       optional_argument, 0, 'o'},
		{"verbose",      optional_argument, 0, 'v'},
		{"help",         optional_argument, 0, 'h'},
		{"length",       required_argument, 0, 'l'},
		{"frequency",    required_argument, 0, 'F'},
		{"offset",       required_argument, 0, 'O'},
		{"kf",           required_argument, 0, 'k'},
		{"lna-gain",     optional_argument, 0, 'L'},
		{"vga-gain",     optional_argument, 0, 'V'},
		{"duration",     optional_argument, 0, 'd'},
		{"mode",         optional_argument, 0, 'm'},
		{"deemphasis",   optional_argument, 0, 'D'},
		{"gain",         required_argument, 0, 'g'},
		{NULL, 0, NULL, 0}
	};

	while ((opt = getopt_long(argc, argv, "s:r:a:b:l:vhF:O:k:o:L:V:d:m:D:g:",
	                          long_options, &long_index)) != -1) {
		switch (opt) {
			case 's':
				args.input_samplerate = atoi(optarg);
				if (args.input_samplerate <= 0) {
					fprintf(stderr, "samplerate must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'r':
				args.output_samplerate = atoi(optarg);
				if (args.output_samplerate <= 0) {
					fprintf(stderr, "resamplerate must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'a':
				args.audio_samplerate = atoi(optarg);
				if (args.audio_samplerate <= 0) {
					fprintf(stderr, "audiorate must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'b':
				args.signal_bandwidth_hz = atoi(optarg);
				if (args.signal_bandwidth_hz <= 0) {
					fprintf(stderr, "bandwidth must be > 0 Hz\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'o':
				args.output_filename = optarg;
				break;
			case 'e':
				if (strcmp(optarg, "f32") == 0) {
					args.output_format = data_format_f32;
				} else if (strcmp(optarg, "s16") == 0) {
					args.output_format = data_format_s16;
				} else {
					fprintf(stderr, "unknown data format %s\n", optarg);
					exit(EXIT_FAILURE);
				}
				break;
			case 'l':
				args.output_block_size = atoi(optarg);
				if (args.output_block_size <= 0) {
					fprintf(stderr, "items to process must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'h':
				print_help();
				exit(EXIT_SUCCESS);
				break;
			case 'v':
				args.verbose = 1;
				break;
			case 'F':
				args.center_frequency_hz = atoll(optarg);
				break;
			case 'O':
				args.freq_offset_hz = atol(optarg);
				break;
			case 'k':
				args.fm_modulation_index = atof(optarg);
				break;
			case 'L':
				args.lna_gain = atoi(optarg);
				break;
			case 'V':
				args.vga_gain = atoi(optarg);
				break;
			case 'd':
				args.record_duration_sec = atoi(optarg);
				break;
			case 'm':
				if (strcmp(optarg, "sync") == 0)
					args.receiver_mode = receiver_mode_sync;
				else if (strcmp(optarg, "async") == 0)
					args.receiver_mode = receiver_mode_async;
				else {
					fprintf(stderr, "Unknown mode %s\n", optarg);
					exit(EXIT_FAILURE);
				}
				break;
			case 'D':
				args.deemphasis_us = atof(optarg);
				break;
			case 'g':
				args.output_gain = atof(optarg);
				break;
			default:
				exit(EXIT_FAILURE);
		}
	}
}

static int fobossdr_init(void) {
	fprintf(stderr, "initializing fobos...\n");
	int ret;
	do {
		if ((ret = receiver_init(ctx.fobos_buffer_size, FOBOS_BUFFER_COUNT)) != 0)
			break;

		if (args.lna_gain >= 0) {
			fprintf(stderr, "set LNA gain to %d\n", args.lna_gain);
			if ((ret = receiver_set_lna(args.lna_gain)) != 0)
				fprintf(stderr, "failed to set LNA gain\n");
		}

		if (args.vga_gain >= 0) {
			fprintf(stderr, "set VGA gain to %d\n", args.vga_gain);
			if ((ret = receiver_set_vga(args.vga_gain)) != 0)
				fprintf(stderr, "failed to set VGA gain\n");
		}

		fprintf(stderr, "set frequency to %ld\n", args.center_frequency_hz);
		if ((ret = receiver_set_freq(args.center_frequency_hz)) != 0)
			fprintf(stderr, "failed to set frequency\n");

		fprintf(stderr, "set sample rate to %d\n", args.input_samplerate);
		if ((ret = receiver_set_samplerate(args.input_samplerate)) != 0)
			fprintf(stderr, "failed to set sample rate\n");

		ret = 0;
	} while (0);

	return ret;
}

static void dsp_init(void) {
	ctx.firdecim_factor     = 0;
	ctx.demod_output_capacity = args.output_block_size;
	ctx.input_block_samples   = args.output_block_size;

	if (args.input_samplerate % args.output_samplerate != 0) {
		/* Non-integer ratio.  For large coarse_factor use a two-stage path:
		 *   stage 1 — integer firdecim at the full input rate (fast, proven to 60 MSPS)
		 *   stage 2 — small msresamp fine-correction at the reduced rate (cheap)
		 * Running the full multistage halfband resampler at high input rate is the
		 * bottleneck at ≥ 35 MSPS; doing the bulk of the work as integer decimation
		 * is ~3× faster. */
		int coarse_factor = args.input_samplerate / args.output_samplerate; /* rounded down */
		if (coarse_factor >= 2) {
			/* Stage 1 brings input rate down to just above target:
			 *   intermediate_rate = input_samplerate / coarse_factor  (slightly above output_samplerate) */
			float intermediate_rate = (float)args.input_samplerate / coarse_factor;
			float fine_correction   = (float)args.output_samplerate / intermediate_rate; /* ≤ 1.0 */

			ctx.firdecim_factor       = coarse_factor;
			ctx.input_block_samples   = args.output_block_size * coarse_factor;
			ctx.demod_output_capacity = (size_t)(args.output_block_size * fine_correction) + 64;

			ctx.iq_input         = malloc(ctx.input_block_samples  * sizeof(float complex));
			ctx.decimator_output = malloc((args.output_block_size + 64) * sizeof(float complex));
			ctx.resampler_output = malloc(ctx.demod_output_capacity * sizeof(float complex));

			ctx.decimator    = firdecim_crcf_create_kaiser(coarse_factor, 8, 60.0f);
			firdecim_crcf_set_scale(ctx.decimator, 1.0f / coarse_factor);
			ctx.fine_resampler = msresamp_crcf_create(fine_correction, 60.0f);
			ctx.demod_input  = ctx.resampler_output;

			if (args.verbose)
				fprintf(stderr, "Two-stage: firdecim(%d) -> %.0f Hz, fine msresamp(%.6f) -> %d Hz\n", coarse_factor, intermediate_rate, fine_correction, args.output_samplerate);
		} else {
			/* coarse_factor < 2: sample rate barely above target or upsampling — pure msresamp */
			int reduced_input_rate  = args.input_samplerate;
			int reduced_output_rate = args.output_samplerate;
			{   /* reduce ratio to lowest terms via Euclidean GCD */
				int remainder, greater = reduced_input_rate, lesser = reduced_output_rate;
				while (lesser) {
					remainder = lesser;
					lesser    = greater % lesser;
					greater   = remainder;
				}
				reduced_input_rate  /= greater;
				reduced_output_rate /= greater;
			}
			if (args.verbose)
				fprintf(stderr, "Will use rational resampler, interpolation %d, decimation %d\n", reduced_output_rate, reduced_input_rate);
			ctx.input_block_samples   = args.output_block_size;
			ctx.demod_output_capacity = (size_t)((double)args.output_block_size * reduced_output_rate / reduced_input_rate) + 64;
			ctx.iq_input         = malloc(ctx.input_block_samples  * sizeof(float complex));
			ctx.resampler_output = malloc(ctx.demod_output_capacity * sizeof(float complex));
			ctx.fine_resampler   = msresamp_crcf_create((float)reduced_output_rate / reduced_input_rate, 60.0f);
			ctx.demod_input  = ctx.resampler_output;
		}
	} else if ((ctx.firdecim_factor = args.input_samplerate / args.output_samplerate) > 1) {
		/* Exact integer decimation ratio — single polyphase FIR decimator */
		ctx.input_block_samples   = args.output_block_size * ctx.firdecim_factor;
		ctx.demod_output_capacity = args.output_block_size;

		ctx.iq_input         = malloc(ctx.input_block_samples  * sizeof(float complex));
		ctx.decimator_output = malloc(ctx.demod_output_capacity * sizeof(float complex));

		unsigned int filter_delay_taps              = 8;
		float        filter_stopband_attenuation_db = 60.0f;
		if (args.verbose)
			fprintf(stderr, "Will use integer decimation, factor %d\n", ctx.firdecim_factor);
		ctx.decimator = firdecim_crcf_create_kaiser(ctx.firdecim_factor, filter_delay_taps, filter_stopband_attenuation_db);
		firdecim_crcf_set_scale(ctx.decimator, 1.0f / (float)ctx.firdecim_factor);
		ctx.demod_input = ctx.decimator_output;
	} else {
		/* No resampling needed */
		if (args.verbose)
			fprintf(stderr, "No resampler needed\n");
		ctx.iq_input    = malloc(ctx.input_block_samples * sizeof(float complex));
		ctx.demod_input = ctx.iq_input;
	}

	/* MPX buffer: holds FM-demodulated signal at output_samplerate before audio stage */
	ctx.demod_output = malloc((ctx.demod_output_capacity + 64) * sizeof(float));
	/* s16 conversion buffer only needed when there is no audio stage */
	if (args.audio_samplerate == 0 && args.output_format == data_format_s16)
		ctx.demod_output_s16 = malloc((ctx.demod_output_capacity + 64) * sizeof(int16_t));

	if (ctx.freq_shift_normalized)
		ctx.raw_input_buf = malloc(ctx.input_block_samples * ctx.bytes_per_iq_sample);

	/* kf = FM_MAX_DEVIATION_HZ / output_samplerate:
	 * demodulator output reaches ±1.0 at full FM deviation (±75 kHz). */
	ctx.fm_demodulator = freqdem_create(args.fm_modulation_index);

	if (ctx.freq_shift_normalized)
		args.ddc = shift_unroll_init(ctx.freq_shift_normalized, ctx.input_block_samples);

	/* ---- Optional audio stage (enabled with -a) ---- */
	if (args.audio_samplerate > 0) {
		/* LPF at 15 kHz: passes mono audio, rejects 19 kHz pilot, stereo (23–53 kHz), RDS (57 kHz).
		 * Transition band runs from 15 kHz to 19 kHz (where the pilot sits).
		 * Filter length from Kaiser formula: n = (As - 7.95) / (14.36 * transition_bw_normalized). */
		float lpf_cutoff_norm     = FM_AUDIO_BANDWIDTH_HZ / (float)args.output_samplerate;
		float lpf_transition_norm = (FM_PILOT_TONE_HZ - FM_AUDIO_BANDWIDTH_HZ) / (float)args.output_samplerate;
		unsigned int lpf_len = (unsigned int)((60.0f - 7.95f) / (14.36f * lpf_transition_norm)) + 4;
		if (lpf_len % 2 == 0)
			lpf_len++; /* odd length for symmetric (linear-phase) FIR */
		ctx.audio_lpf = firfilt_rrrf_create_kaiser(lpf_len, lpf_cutoff_norm, 60.0f, 0.0f);

		/* Resample from output_samplerate to audio_samplerate.
		 * msresamp_rrrf handles both integer ratios (e.g. 192k/48k = 4) and
		 * non-integer ones (e.g. 192k/44.1k), and tolerates variable block sizes
		 * from the fine IQ resampler. */
		float audio_resamp_rate   = (float)args.audio_samplerate / (float)args.output_samplerate;
		ctx.audio_output_capacity = (size_t)(ctx.demod_output_capacity * audio_resamp_rate) + 64;
		ctx.audio_resampler       = msresamp_rrrf_create(audio_resamp_rate, 60.0f);

		ctx.audio_output = malloc(ctx.audio_output_capacity * sizeof(float));
		if (args.output_format == data_format_s16)
			ctx.audio_output_s16 = malloc(ctx.audio_output_capacity * sizeof(int16_t));

		if (args.deemphasis_us > 0.0f) {
			ctx.deemph_coeff = expf(-1.0f / (args.deemphasis_us * 1e-6f * args.audio_samplerate));
			ctx.deemph_state = 0.0f;
		}

		if (args.verbose)
			fprintf(stderr, "Audio stage: LPF @ %.0f Hz (%u taps), resamp %.0f -> %d Hz, de-emphasis %.0f us\n", FM_AUDIO_BANDWIDTH_HZ, lpf_len, (float)args.output_samplerate, args.audio_samplerate, args.deemphasis_us);
	}
}

static void tune_processing_size(void) {
	/* Grow hw buffer until one fill covers at least HW_BUFFER_TARGET_MS ms of input */
	size_t hw_target_samples = (size_t)args.input_samplerate * HW_BUFFER_TARGET_MS / 1000;
	while (ctx.fobos_buffer_size < hw_target_samples)
		ctx.fobos_buffer_size <<= 1;

	/* Grow DSP block until one iteration covers at least DSP_BLOCK_TARGET_MS ms of output */
	size_t dsp_target_samples = (size_t)args.output_samplerate * DSP_BLOCK_TARGET_MS / 1000;
	while (args.output_block_size < dsp_target_samples)
		args.output_block_size <<= 1;
}

int main(int argc, char *argv[]) {
	int ret;
	FILE *output_file = stdout;
	struct sigaction signal_action;

	signal_action.sa_handler = signal_handler;
	sigemptyset(&signal_action.sa_mask);
	signal_action.sa_flags = 0;
	sigaction(SIGTERM, &signal_action, NULL);
	sigaction(SIGINT,  &signal_action, NULL);

	memset((void *)&ctx, 0, sizeof(ctx_t));
	ctx.run                   = true;
	ctx.bytes_per_iq_sample   = sizeof(float) * 2;
	ctx.freq_shift_normalized = 0.0f;
	ctx.ddc_phase             = 0.0f;
	ctx.fobos_buffer_size     = FOBOS_BUFFER_SIZE;

	memset((void *)&args, 0, sizeof(args_t));
	args.output_filename   = NULL;
	args.output_format     = data_format_f32;
	args.output_block_size = DEFAULT_OUTPUT_BLOCK_SIZE;
	args.receiver_mode     = receiver_mode_sync;
	args.deemphasis_us     = 50.0f;  /* FM broadcast standard: 50 µs Europe, 75 µs Americas */
	args.output_gain       = 1.0f;
	args.lna_gain          = -1;   /* -1 = leave hardware default untouched */
	args.vga_gain          = -1;

	parse_args(argc, argv);

	if (args.output_samplerate == 0)
		args.output_samplerate = DEFAULT_IQ_SAMPLERATE;

	tune_processing_size();

	if (args.output_filename) {
		output_file = fopen(args.output_filename, "wb");
		if (!output_file) {
			fprintf(stderr, "failed to open output file %s\n", args.output_filename);
			exit(EXIT_FAILURE);
		}
	}

	if (args.input_samplerate == 0 || args.center_frequency_hz == 0) {
		fprintf(stderr, "-s [--samplerate] or -F [--frequency] not specified!\n");
		print_help();
		exit(EXIT_FAILURE);
	}

	if (!args.signal_bandwidth_hz)
		args.signal_bandwidth_hz = args.output_samplerate / 2;

	if (!args.fm_modulation_index)
		args.fm_modulation_index = FM_KF_HEADROOM * FM_MAX_DEVIATION_HZ / (float)args.output_samplerate;

	/* Negate: -O specifies where the target signal sits relative to centre (e.g. -200000 means
	 * "signal is 200 kHz below centre").  The DDC must shift the spectrum UP by that amount,
	 * i.e. freq_shift_normalized = -freq_offset_hz / fs. */
	if (args.freq_offset_hz)
		ctx.freq_shift_normalized = -(float)args.freq_offset_hz / (float)args.input_samplerate;

#ifdef _WIN32
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	dsp_init();

	if (args.verbose)
		dump_info();

	if ((ret = fobossdr_init()) != 0) {
		fprintf(stderr, "Error initing FobosSDR %d\n", ret);
		exit(-1);
	}

	ctx.start_time       = time(NULL);
	ctx.last_report_time = ctx.start_time;

	ctx.expected_input_samples  = args.input_samplerate  * args.record_duration_sec;
	ctx.expected_output_samples = (args.audio_samplerate > 0 ? args.audio_samplerate : args.output_samplerate) * args.record_duration_sec;

	ctx.samples_read_total    = 0;
	ctx.samples_written_total = 0;

	fprintf(stderr, "start processing samples...\n");

	if ((ret = receiver_start(args.receiver_mode)))
		fprintf(stderr, "Error starting Fobos %d\n", ret);

	do {
		int samples_read;

		if (ctx.freq_shift_normalized) {
			/* Read raw IQ then apply DDC frequency shift into iq_input */
			if ((samples_read = receiver_read((float *)ctx.raw_input_buf, ctx.input_block_samples)) != (int)ctx.input_block_samples) {
				fprintf(stderr, "Failed to read samples: got %d, expected %ld\n", samples_read, ctx.input_block_samples);
				break;
			}
			ctx.ddc_phase = shift_unroll_process(ctx.raw_input_buf, data_format_f32, (complexf *)ctx.iq_input, ctx.input_block_samples, &args.ddc, ctx.ddc_phase);
		} else {
			/* No frequency shift — read directly into the IQ buffer */
			if ((samples_read = receiver_read((float *)ctx.iq_input, ctx.input_block_samples)) != (int)ctx.input_block_samples) {
				fprintf(stderr, "Failed to read samples: got %d, expected %ld\n", samples_read, ctx.input_block_samples);
				break;
			}
		}

		if (samples_read < 0) {
			fprintf(stderr, "Error reading data: %d\n", errno);
			exit(EXIT_FAILURE);
		}

		ctx.samples_read_total += samples_read;

		/* Resample from input rate to output rate */
		size_t demod_sample_count = ctx.demod_output_capacity;
		if (ctx.firdecim_factor > 1) {
			if (ctx.fine_resampler) {
				/* Two-stage: integer firdecim brings rate close, fine msresamp corrects remainder */
				firdecim_crcf_execute_block(ctx.decimator, ctx.iq_input, args.output_block_size, ctx.decimator_output);
				unsigned int resampled_count;
				msresamp_crcf_execute(ctx.fine_resampler, ctx.decimator_output, args.output_block_size, ctx.resampler_output, &resampled_count);
				demod_sample_count = resampled_count;
			} else {
				/* Pure integer decimation */
				firdecim_crcf_execute_block(ctx.decimator, ctx.iq_input, ctx.demod_output_capacity, ctx.decimator_output);
			}
		} else if (ctx.fine_resampler) {
			/* Pure msresamp (input rate barely above output rate or upsampling) */
			unsigned int resampled_count;
			msresamp_crcf_execute(ctx.fine_resampler, ctx.iq_input, ctx.input_block_samples, ctx.resampler_output, &resampled_count);
			demod_sample_count = resampled_count;
		}

		/* FM demodulate — produces MPX signal at output_samplerate */
		freqdem_demodulate_block(ctx.fm_demodulator, ctx.demod_input, demod_sample_count, ctx.demod_output);

		/* Hard clip to ±1.0 before the audio LPF.
		 * With kf = FM_KF_HEADROOM × Δf/fs, nominal FM audio sits at ≤±0.36,
		 * so the clip never touches the FM signal — only noise spikes that exceed
		 * 2.8× the FM deviation.  A normalised LPF (DC gain = 1) guarantees the
		 * filter output is also ≤ ±1.0, preventing ringing artefacts.
		 * Unlike tanh, a hard clip is perfectly linear below the threshold and
		 * introduces no dynamic compression on the audio content. */
		for (size_t i = 0; i < demod_sample_count; i++) {
			if      (ctx.demod_output[i] >  1.0f) 
				ctx.demod_output[i] =  1.0f;
			else if (ctx.demod_output[i] < -1.0f) 
				ctx.demod_output[i] = -1.0f;
		}

		/* Optional audio stage: LPF + resample to audio_samplerate */
		float  *output_samples;
		size_t  output_sample_count;
		if (args.audio_samplerate > 0) {
			/* In-place LPF: safe because firfilt processes forward, each output
			 * overwrites the input sample already consumed into the delay line. */
			firfilt_rrrf_execute_block(ctx.audio_lpf, ctx.demod_output, demod_sample_count, ctx.demod_output);
			unsigned int audio_count;
			msresamp_rrrf_execute(ctx.audio_resampler, ctx.demod_output, demod_sample_count, ctx.audio_output, &audio_count);

			/* De-emphasis: 1st-order IIR y[n] = (1-a)*x[n] + a*y[n-1]
			 * Corrects FM pre-emphasis boost applied at the transmitter. */
			if (ctx.deemph_coeff > 0.0f) {
				float a = ctx.deemph_coeff;
				float b = 1.0f - a;
				for (unsigned int i = 0; i < audio_count; i++) {
					ctx.deemph_state = b * ctx.audio_output[i] + a * ctx.deemph_state;
					ctx.audio_output[i] = ctx.deemph_state;
				}
			}

			if (args.output_gain != 1.0f) {
				for (unsigned int i = 0; i < audio_count; i++)
					ctx.audio_output[i] *= args.output_gain;
			}

			output_samples      = ctx.audio_output;
			output_sample_count = audio_count;
		} else {
			output_samples      = ctx.demod_output;
			output_sample_count = demod_sample_count;
		}

		/* Write output in requested format */
		if (args.output_format == data_format_f32) {
			fwrite(output_samples, sizeof(float), output_sample_count, output_file);
		} else {
			int16_t *s16_buf = args.audio_samplerate > 0 ? ctx.audio_output_s16 : ctx.demod_output_s16;
			for (size_t i = 0; i < output_sample_count; i++) {
				float v = output_samples[i];
				if (v >  1.0f)
					v =  1.0f;
				if (v < -1.0f)
					v = -1.0f;
				s16_buf[i] = (int16_t)(v * (float)SHRT_MAX);
			}
			fwrite(s16_buf, sizeof(int16_t), output_sample_count, output_file);
		}

		ctx.samples_written_total += output_sample_count;

		if (args.record_duration_sec) {
			time_t current_time = time(NULL);
			if (ctx.samples_read_total >= ctx.expected_input_samples) {
				fprintf(stderr, "Processing aim reached, exiting...\n");
				break;
			} else if (args.verbose && (current_time != ctx.last_report_time)) {
				fprintf(stderr, "Elapsed time: %ld seconds\n", current_time - ctx.start_time);
				ctx.last_report_time = current_time;
			}
		}
	} while (ctx.run);

	fprintf(stderr, "Expected to read %lu,  read %lu\n",  ctx.expected_input_samples,  ctx.samples_read_total);
	fprintf(stderr, "Expected to write %lu, wrote %lu\n", ctx.expected_output_samples, ctx.samples_written_total);

	fflush(stderr);

	receiver_stop();

	freqdem_destroy(ctx.fm_demodulator);

	if (ctx.audio_lpf)
		firfilt_rrrf_destroy(ctx.audio_lpf);
	if (ctx.audio_resampler)
		msresamp_rrrf_destroy(ctx.audio_resampler);

	if (ctx.decimator)
		firdecim_crcf_destroy(ctx.decimator);
	if (ctx.fine_resampler)
		msresamp_crcf_destroy(ctx.fine_resampler);

	if (ctx.raw_input_buf)
		free(ctx.raw_input_buf);

	free(ctx.iq_input);
	if (ctx.resampler_output)
		free(ctx.resampler_output);
	if (ctx.decimator_output)
		free(ctx.decimator_output);
	free(ctx.demod_output);
	if (ctx.demod_output_s16)
		free(ctx.demod_output_s16);
	if (ctx.audio_output)
		free(ctx.audio_output);
	if (ctx.audio_output_s16)
		free(ctx.audio_output_s16);

	return 0;
}
