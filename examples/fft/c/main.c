/**
 * @file main.c
 * @brief Real-time FFT spectrum analyser — Fobos SDR developer example.
 *
 * @section overview Overview
 *
 * Demonstrates how to capture IQ data from a Fobos SDR and display a live
 * power spectrum in the terminal.  Intended as a starting point for developers
 * building spectrum monitoring, signal identification, or wideband scanning
 * applications on top of the Fobos SDR platform.
 *
 * @section pipeline Processing Pipeline
 *
 * @verbatim
 * [Fobos SDR hardware]
 *        │  IQ float32, interleaved, at input_samplerate (e.g. 20 MSPS)
 *        │  Captured by a dedicated real-time thread (SCHED_FIFO, core 3)
 *        │  and buffered in a lock-free ring buffer — see receiver.h
 *        ▼
 * [receiver_read()]
 *   Pulls one block of IQ samples (= fft_width complex samples, or
 *   fft_width + overlap when overlapped averaging is enabled).
 *        │  complexf[fft_width], normalised to ±1.0
 *        ▼
 * [fft_apply_precalculated_window()]
 *   Multiplies each IQ sample by a pre-computed Hamming window coefficient.
 *   Suppresses spectral leakage from the sharp edges of the finite IQ block.
 *        │
 *        ▼
 * [fft_execute()]
 *   Runs the FFTW3 single-precision forward DFT on the windowed block.
 *   Output is a complex frequency-domain representation of the input.
 *        │  complexf[fft_width] — frequency bins, DC at index 0
 *        ▼
 * [fft_accumulate_power()]
 *   Adds I²+Q² for each bin into a running sum buffer.
 *   Repeated every iteration; written to the display every `average` iterations.
 *   Averaging N FFTs reduces noise variance by √N.
 *        │
 *        ▼
 * [fft_log()]  — every `average` iterations
 *   Converts accumulated power to dBFS: 10·log10(sum) + baselevel − 10·log10(N).
 *   Bin-swap (lower half ↔ upper half) is applied here to place DC at centre.
 *        │  float[fft_width] — dBFS spectrum, DC-centred
 *        ▼
 * [draw_fft()]
 *   Compresses bins to terminal width (peak-preserving), draws spectrum bars
 *   with 8× sub-character resolution using Unicode block characters, and
 *   renders the dB y-axis and frequency x-axis labels.
 *        │
 *        ▼
 * [fwrite() to output file]          ← optional, -o flag
 *   Raw float32 FFT bins written in display order (DC-centred) for
 *   offline analysis in Python / GNU Octave / MATLAB.
 * @endverbatim
 *
 * @section overlap Overlap Mode
 *
 * The @c -s value is the **hop size** (stride) — the number of new samples consumed per FFT iteration.
 * When @c overlap > fft_width, @c (overlap − fft_width) samples are discarded between frames (coarse time stride).
 * When @c 0 < overlap < fft_width, the last @c (fft_width − overlap) samples are retained and @c overlap new samples
 * are appended each iteration (sliding window).  Effective iteration rate = sample_rate / overlap, so very small
 * values cause extremely high iteration rates and will overflow the ring buffer at high sample rates.
 *
 * @section quickstart Quick Start
 *
 * Display a live spectrum around 100 MHz at 20 MSPS, 1024-point FFT:
 * @code
 *   ./fft -w 1024 -c 100000000 -r 20000000 -a 10 -d
 * @endcode
 *
 * Save raw FFT data to a file while also drawing to the terminal:
 * @code
 *   ./fft -w 2048 -c 433000000 -r 8000000 -a 20 -d -o spectrum.f32
 * @endcode
 *
 * @section extending Extending This Example
 *
 * - **Peak hold**: maintain a second @c float[fft_width] buffer and update it
 *   with @c max(peak[i], current[i]) each frame for a spectrum peak-hold display.
 * - **Wideband scan**: step the centre frequency in a loop, capture one averaged
 *   FFT per step, and stitch the results into a panoramic spectrum.
 * - **Signal detection**: threshold the dBFS output to detect carrier presence
 *   and log frequency/time stamps of active signals.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <getopt.h>
#include <fcntl.h>
#include <assert.h>
#include <math.h>
#include <limits.h>
#include <sched.h>

#ifndef __USE_POSIX
#define __USE_POSIX 1
#endif
#include <signal.h>

#include "fft.h"
#include "receiver.h"
#include "draw.h"
#include "sgram.h"
#include "spectrum_png.h"

#define MAX_DB 0.0
#define MIN_DB -130.0

#define DEFAULT_FFT_WIDTH 	1024
#define DEFAULT_FREQ 		100000000LL
#define DEFAULT_SAMPLERATE 	20000000L
#define DEFAULT_BASELEVEL 	-75

#define SGRAM_DEFAULT_MEM_MB  256

#define INPUT_BUF_SIZE 8192

#define FOBOS_FRAME_SIZE 65536

#define MIN_M(x,y) (((x)>(y))?(y):(x))
#define MAX_M(x,y) (((x)<(y))?(y):(x))

typedef struct ctx_t{
	int run;
	int fft_width;
	int overlap;
	int baselavel;
	int avarage;
	int verbose;
	long long cf;
	int rate;
	int lna_gain;
	int vga_gain;
	float display_min_db;
	float display_max_db;
	int   auto_min;        /* 1 = derive min from FFT output each frame */
	int   auto_max;        /* 1 = derive max from FFT output each frame */

	char *out_filename;
	FILE *output;
	fft_plan_s *fft_plan;
	size_t sample_size;
	int draw_fft;

	char  *sgram_file;
	int    n_frames;
	size_t sgram_mem_mb;
	int    max_hold;       /* 1 = enable max-hold display */
	int    max_hold_csv;   /* 1 = save maxhold.csv on exit */
	int    max_hold_png;   /* 1 = save maxhold.png on exit */
} ctx_t;

static ctx_t ctx;

//temporary buffer for skipping samples, it is not used for actual FFT calculation
complexf *skip_buff = NULL;

//input IQ buffer for FFT calculation, in case of input format is not f32, it is used for format conversion output, otherwise it is used directly for FFT calculation
complexf* f32_input = NULL;

//output buffer for FFT calculation
complexf* fft_output = NULL;

//windowed input buffer for FFT calculation, it is used for applying window function to input IQ samples before FFT calculation
complexf* windowed = NULL;

//precalculated window function values, it is used for applying window function to input IQ samples before FFT calculation 
//and it is precalculated to avoid redundant calculation for each FFT iteration, it is calculated based on Hamming window function
float *windowt = NULL;

//average output buffer for accumulating power of FFT output, it is used for calculating average power of FFT output 
//over multiple iterations, and it is used for calculating log power of FFT output after average calculation
float* avrg_output = NULL;

//output buffer to swap FFT output bins to put DC in the middle of output, it is used for storing log power of FFT output after calculation, and it is
//used for writing to output file
float *output = NULL;

/* max-hold buffer: per-bin peak in dBFS; -INFINITY = not yet observed */
static float *max_hold_buf = NULL;

static void signal_handler(int signum);
static void print_help();
static void dump_ctx(void);
static void write_fft_results(ctx_t *ctx, float* avrg_output, float* output);
static int log2n(int x);
inline static void convert_s16_f(short* input, float* output, int input_size);
inline static void convert_u8_f(unsigned char* input, float* output, int input_size);
static void parse_args(int argc, char*argv[]);

static long now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
}

static void signal_handler(int signum) {
    static int count = 0;
    fprintf(stderr, "Received signal %d\n", signum);
    if (count)
        exit(-1);
    count++;
    ctx.run = 0;
}

static void print_help() {
	printf("usage: fft args\n");
	printf("\t--fft_width \t-w <fft_width>\t\t: fft width, should be power of 2\n");
	printf("\t--overlap \t-s <hop>\t: hop size (new samples per FFT); smaller = more overlap, higher CPU\n");
	printf("\t--baselevel \t-b <baselevel_dB>\t: fft base level\n");
	printf("\t--average \t-a <rate>\t: average rate\n");
	printf("\t--central-freq \t-c <freq>\t: central freq\n\n");
	printf("\t--sample-rate \t-r <rate>\t: sample rate\n\n");
	printf("\t--frames \t-n <count>\t: exit after <count> averaged frames\n\n");
	printf("\t--output \t-o <filename>\t: output file name, stdout by default\n\n");
	printf("\t--help \t\t-h \t\t\t: prints this usage information\n");
	printf("\t--verbose \t\t-v \t\t\t: print runtime information\n");
	printf("\t--draw-fft \t\t-d \t\t\t: draw FFT in terminal\n");
	printf("\t--lna-gain \t\t-L \t\t\t: set LNA gain 0..3 (0,1: 0 dB, 2: +16 dB, 3: +33 dB)\n");
	printf("\t--vga-gain \t\t-V \t\t\t: set VGA gain 0..31 (0..+62 dB, 2 dB step)\n");
	printf("\t--min-db \t\t-m <dB>\t\t\t: minimum display level in dB (default: baselevel-30)\n");
	printf("\t--max-db \t\t-M <dB>\t\t\t: maximum display level in dB (default: baselevel+50)\n");
	printf("\t--sgram \t\t-G <file>\t\t: save spectrogram PNG to <file> on exit\n");
	printf("\t--sgram-frames \t\t-N <count>\t\t: stop after <count> averaged frames and save PNG\n");
	printf("\t--sgram-mem \t\t-S <MB>\t\t\t: spectrogram memory limit in MB (default %d)\n", SGRAM_DEFAULT_MEM_MB);
	printf("\t--max-hold \t\t-x[c][g]\t\t: enable max-hold display (yellow ▄ tick per bin); press 'c' to clear\n");
	printf("\t\t\t\t\t\t\t  add 'c' to also save maxhold.csv on exit\n");
	printf("\t\t\t\t\t\t\t  add 'g' to also save maxhold.png on exit\n");
	printf("\t\t\t\t\t\t\t  e.g. -xcg = live + CSV + PNG\n");
}

static void dump_ctx(void){	
	printf("fft width %d\n", ctx.fft_width);
	printf("overlap %d\n", ctx.overlap);
	printf("base level %d\n", ctx.baselavel);
	printf("avrg %d\n", ctx.avarage);
	printf("central freq %lld\n", ctx.cf);
	printf("sample rate %d\n", ctx.rate);
	printf("sample size %lu\n", ctx.sample_size);
	printf("output file %s\n", ctx.out_filename ? ctx.out_filename : "stdout");
	printf("verbose %d\n", ctx.verbose);
	printf("draw fft %d\n", ctx.draw_fft);
	printf("lna gain %d\n", ctx.lna_gain);
	printf("vga gain %d\n", ctx.vga_gain);
}

static void write_fft_results(ctx_t *ctx, float* avrg_output, float* output) {
    static int frame_count = 0;
    fft_log(avrg_output, output + ctx->fft_width/2, ctx->fft_width/2, ctx->baselavel);
    fft_log(avrg_output + ctx->fft_width/2, output, ctx->fft_width/2, ctx->baselavel);
    memset(avrg_output, 0, sizeof(float) * ctx->fft_width);

    frame_count++;

    /* Update max-hold buffer (every averaged frame, not just on draw). */
    if (max_hold_buf) {
        for (int i = 0; i < ctx->fft_width; i++)
            if (output[i] > max_hold_buf[i])
                max_hold_buf[i] = output[i];
        draw_set_maxhold(max_hold_buf, ctx->fft_width);
    }

    if (ctx->draw_fft) {
        static long t_last_draw = 0;
        static int warm = 0;
        long t_now = now_ns();
        if (t_now - t_last_draw >= 33333333L) {   /* cap at 30 fps */
            if (warm < 3) {
                warm++;   /* discard first 3 frames — they may contain init artefacts */
            } else {
                if (ctx->auto_min || ctx->auto_max) {
                    float pmin =  1e30f, pmax = -1e30f;
                    for (int i = 0; i < ctx->fft_width; i++) {
                        float v = output[i];
                        if (v < pmin)
                            pmin = v;
                        if (v > pmax)
                            pmax = v;
                    }
                    if (ctx->auto_min) {
                        ctx->display_min_db = pmin;
                        ctx->auto_min = 0;
                    }
                    if (ctx->auto_max) {
                        ctx->display_max_db = pmax;
                        ctx->auto_max = 0;
                    }
                    if (ctx->display_max_db - ctx->display_min_db < 10.0f)
                        ctx->display_max_db = ctx->display_min_db + 10.0f;
                }
                draw_fft(output, ctx->fft_width, ctx->display_min_db, ctx->display_max_db);

                /* Poll for keypress: 'c' / 'C' clears max hold. */
                int key = draw_check_keypress();
                if ((key == 'c' || key == 'C') && max_hold_buf) {
                    for (int i = 0; i < ctx->fft_width; i++)
                        max_hold_buf[i] = -INFINITY;
                }
            }
            t_last_draw = t_now;
        }
    }

    fwrite(output, sizeof(float), ctx->fft_width, ctx->output);
}

static int log2n(int x){
    int result=-1;
    for(int i=0;i<31;i++)
    {
        if((x>>i)&1) //@@log2n
        {
            if (result == -1)
                result = i;
            else
                return -1;
        }
    }
    return result;
}

inline static void convert_s16_f(short* input, float* output, int input_size){
    for(int i=0;i<input_size;i++)
        output[i]=(float)input[i]/SHRT_MAX; //@convert_s16_f
}

inline static void convert_u8_f(unsigned char* input, float* output, int input_size){
    for(int i=0;i<input_size;i++)
        output[i]=((float)input[i])/(UCHAR_MAX/2.0)-1.0; //@convert_u8_f
}

static void parse_args(int argc, char*argv[]){
	int opt = 0;
	int long_index = 0;

	static struct option long_options[] = {
		{"fft_width",	required_argument,	0,	'w' },
		{"overlap",		required_argument,	0,	's' },
		{"baselevel",	required_argument,	0,	'b' },
		{"average",		required_argument,	0,	'a' },
		{"central-freq",required_argument,	0,	'c' },
		{"sample-rate",	required_argument,	0,	'r' },
		{"frames",		required_argument,	0,	'n' },
		{"output",		required_argument,	0,	'o' },
		{"verbose",		no_argument,		0,	'v' },
		{"help",		no_argument,		0,	'h' },
		{"draw-fft",	no_argument,		0,	'd' },
		{"lna-gain",	required_argument,	0,	'L' },
		{"vga-gain",	required_argument,	0,	'V' },
		{"min-db",		required_argument,	0,	'm' },
		{"max-db",		required_argument,	0,	'M' },
		{"sgram",		required_argument,	0,	'G' },
		{"sgram-frames",required_argument,	0,	'N' },
		{"sgram-mem",	required_argument,	0,	'S' },
		{"max-hold",	optional_argument,	0,	'x' },
		{NULL,			0,			NULL,	 0  }
	};

	while ((opt = getopt_long(argc, argv, "w:s:b:a:o:c:r:n:vhdL:V:m:M:G:N:S:x::", long_options, &long_index)) != -1) {
		switch (opt) {
			case 'w' :
				ctx.fft_width = atoi(optarg);
				if(log2n(ctx.fft_width) == -1){ 
					fprintf(stderr, "fft_size should be power of 2\n");				
					exit(EXIT_FAILURE);
				}
				break;
			case 's' :
				ctx.overlap = atoi(optarg);
				if (ctx.overlap <= 0) {
					fprintf(stderr, "overlap must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'b' :
				ctx.baselavel = atoi(optarg);
				break;
			case 'a' :
				ctx.avarage = atoi(optarg);
				if (ctx.avarage <= 0) {
					fprintf(stderr, "avarage must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'c':
				ctx.cf = atoll(optarg);
				break;
			case 'r':
				ctx.rate = atol(optarg);
				break;
			case 'n':
				ctx.n_frames = atoi(optarg);
				if (ctx.n_frames <= 0) {
					fprintf(stderr, "-n must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'o' :
				ctx.out_filename = optarg;
				break;			
			case 'h' :
				print_help();
				exit(EXIT_SUCCESS);
				break;
			case 'v' :
				ctx.verbose = 1;
				break;
			case 'd' :
				ctx.draw_fft = 1;
				break;
			case 'L' :
				ctx.lna_gain = atoi(optarg);
				break;
			case 'V' :
				ctx.vga_gain = atoi(optarg);
				break;
			case 'm' :
				ctx.display_min_db = atof(optarg);
				ctx.auto_min = 0;
				break;
			case 'M' :
				ctx.display_max_db = atof(optarg);
				ctx.auto_max = 0;
				break;
			case 'G' :
				ctx.sgram_file = optarg;
				break;
			case 'N' :
				ctx.n_frames = atoi(optarg);
				if (ctx.n_frames <= 0) {
					fprintf(stderr, "sgram-frames must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'S' :
				ctx.sgram_mem_mb = (size_t)atoi(optarg);
				if (ctx.sgram_mem_mb == 0) {
					fprintf(stderr, "sgram-mem must be > 0\n");
					exit(EXIT_FAILURE);
				}
				break;
			case 'x' :
				ctx.max_hold = 1;
				if (optarg) {
					for (const char *p = optarg; *p; p++) {
						if (*p == 'c')
							ctx.max_hold_csv = 1;
						if (*p == 'g')
							ctx.max_hold_png = 1;
					}
				}
				break;
			default:
				exit(EXIT_FAILURE);
		}
	}
}

int main(int argc, char*argv[]){
	struct sigaction sa;

    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
	
	memset((void*)&ctx, 0, sizeof(ctx_t));

	ctx.lna_gain = -1;   /* -1 = leave hardware default */
	ctx.vga_gain = -1;
	ctx.run = 1;
	ctx.output = stdout;
	ctx.sample_size = sizeof(float) * 2;
	ctx.fft_width = DEFAULT_FFT_WIDTH;
	ctx.cf = DEFAULT_FREQ;
	ctx.rate = DEFAULT_SAMPLERATE;
	ctx.overlap = 0;
	ctx.avarage = 1000;
	ctx.baselavel = DEFAULT_BASELEVEL;
	ctx.display_min_db = 0.0f;
	ctx.display_max_db = 0.0f;
	ctx.auto_min = 1;
	ctx.auto_max = 1;
	ctx.sgram_mem_mb = SGRAM_DEFAULT_MEM_MB;

	parse_args(argc, argv);

	if(!ctx.fft_width){
		fprintf(stderr, "fft width is required\n");
		print_help();
		exit(EXIT_FAILURE);
	}

#ifdef _WIN32
	else{
		_setmode( _fileno( ctx.input ), _O_BINARY );
	}
#endif

	if (ctx.out_filename) {
		ctx.output= fopen(ctx.out_filename, "wb");
	}
#ifdef _WIN32
	else{
		_setmode( _fileno( ctx.output ), _O_BINARY );
	}
#endif

	if (ctx.verbose)
		dump_ctx();

	/* Validate overlap / compute effective iteration rate.
	 *
	 * The -s value is the HOP SIZE (stride): new samples consumed per FFT
	 * iteration.  Effective rate = sample_rate / hop_size.
	 *
	 *   overlap == 0           → hop = fft_width  (no overlap, lowest CPU)
	 *   0 < overlap < fft_width → hop = overlap    (sliding window, high CPU when overlap is small)
	 *   overlap > fft_width    → hop = overlap     (skip/stride, coarser time, lowest CPU)
	 *
	 * A very small hop size causes the consumer to fall behind the producer
	 * (ring buffer overflow) because the iteration rate becomes too high. */
	if (ctx.rate > 0) {
		int hop = (ctx.overlap > 0 && ctx.overlap < ctx.fft_width) ? ctx.overlap : ctx.fft_width;
		long iter_per_sec = (long)ctx.rate / hop;
		printf("effective iteration rate: %ld iter/s (hop=%d samples)\n", iter_per_sec, hop);
		if (iter_per_sec > 100000L)
			fprintf(stderr, "WARNING: iteration rate %ld iter/s is very high — "
			        "increase -s (hop size) to avoid ring buffer overflow\n", iter_per_sec);
	}

	printf("initializing fobos...\n");
	if (receiver_init(FOBOS_FRAME_SIZE, 32) != 0) {
		fprintf(stderr, "failed to init receiver\n");
		exit(EXIT_FAILURE);
	}

	if (ctx.lna_gain >= 0) {
		printf("set LNA gain to %d\n", ctx.lna_gain);
		if (receiver_set_lna(ctx.lna_gain) != 0)
			fprintf(stderr, "failed to set LNA gain\n");
	}

	if (ctx.vga_gain >= 0) {
		printf("set VGA gain to %d\n", ctx.vga_gain);
		if (receiver_set_vga(ctx.vga_gain) != 0)
			fprintf(stderr, "failed to set VGA gain\n");
	}

	printf("set frequency to %lld\n", ctx.cf);
	if (receiver_set_freq((uint64_t)ctx.cf) != 0) {
		fprintf(stderr, "failed to set frequency\n");
		exit(EXIT_FAILURE);
	}

	printf("set sample rate to %d\n", ctx.rate);
	if (receiver_set_samplerate((uint32_t)ctx.rate) != 0) {
		fprintf(stderr, "failed to set sample rate\n");
		exit(EXIT_FAILURE);
	}

	printf("allocate buffers...\n");

    //temporary buffer for skipping samples, it is not used for actual FFT calculation
	skip_buff = (complexf*)malloc(sizeof(complexf) * INPUT_BUF_SIZE);
    assert(skip_buff);

    f32_input = (complexf*)fftwf_alloc_complex(ctx.fft_width);
	assert(f32_input);

	fft_output = (complexf*)fftwf_alloc_complex(ctx.fft_width);
	assert(fft_output);

	windowed = (complexf*)fftwf_alloc_complex(ctx.fft_width);
	assert(windowed);

    //precalculated window function values, it is used for applying window function to input IQ samples before FFT calculation 
    //and it is precalculated to avoid redundant calculation for each FFT iteration, it is calculated based on Hamming window function
	windowt = (float*) malloc(sizeof(float) * ctx.fft_width);
	assert(windowt);

    //average output buffer for accumulating power of FFT output, it is used for calculating average power of FFT output 
    //over multiple iterations, and it is used for calculating log power of FFT output after average calculation
	avrg_output=(float*)malloc(sizeof(float)*ctx.fft_width);
	assert(avrg_output);

	output = calloc(ctx.fft_width, sizeof(float));
	assert(output);

	if (ctx.max_hold) {
		max_hold_buf = malloc(sizeof(float) * ctx.fft_width);
		assert(max_hold_buf);
		for (int i = 0; i < ctx.fft_width; i++)
			max_hold_buf[i] = -INFINITY;
	}

	printf("precalculate window function values...\n");
    //precalculate window function values for applying window function to input IQ samples before FFT calculation, it is calculated based on Hamming window function 
    //and it is precalculated to avoid redundant calculation for each FFT iteration
	fft_precalculate_window(windowt, ctx.fft_width);

	ctx.baselavel -= 10.0*log10(ctx.avarage);
	
	printf("base level after avarage compensation: %d\n", ctx.baselavel);

	printf("create FFT plan...\n");
    //create FFT plan for FFT calculation, it is created based on FFT width and input/output buffers, and it is created once to avoid redundant creation for each FFT iteration
	fft_load_wisdom("fftw_wisdom");
	ctx.fft_plan = fft_create_plan(ctx.fft_width, windowed, fft_output);
	fft_save_wisdom("fftw_wisdom");

	int iteration = 0;

	printf("start processing samples...\n");
	draw_init((uint64_t)ctx.cf, (uint64_t)ctx.rate, ctx.baselavel, ctx.overlap, ctx.avarage, ctx.fft_width, MIN_DB, MAX_DB);
	draw_hide_cursor();
	if (ctx.max_hold)
		draw_enable_keypress();

	sgram_t *sgram = NULL;
	if (ctx.sgram_file) {
		size_t mem_bytes = ctx.sgram_mem_mb * 1024UL * 1024UL;
		sgram = sgram_create(ctx.fft_width, mem_bytes);
		if (!sgram) {
			fprintf(stderr, "failed to create spectrogram buffer\n");
			exit(EXIT_FAILURE);
		}
		sgram_params_t sp = {
			.freq_from_hz = (double)ctx.cf - ctx.rate / 2.0,
			.freq_to_hz   = (double)ctx.cf + ctx.rate / 2.0,
			.rate         = ctx.rate,
			.fft_width    = ctx.fft_width,
			.average      = ctx.avarage,
			.n_steps      = 1,
			.overlap_hz   = 0.0,
		};
		sgram_set_params(sgram, &sp);
	}
	int sgram_warmup = 0;

	/* Buffer stdout so every draw frame is written atomically in one syscall */
	fflush(stdout);
	setvbuf(stdout, NULL, _IOFBF, 1 << 18);

	receiver_start(receiver_mode_sync);

	/* per-phase timing accumulators (verbose mode) */
	long perf_read = 0, perf_fft = 0;
	long perf_n = 0, perf_t_report = now_ns();

	do {
		long t0 = 0, t1;
		int read_samples = 0;

		if (ctx.verbose)
			t0 = now_ns();

		if (ctx.overlap > ctx.fft_width) {
			if ((read_samples = receiver_read(f32_input, ctx.fft_width)) != ctx.fft_width) {
				fprintf(stderr, "Failed to read samples, read %d, expected %d\n", read_samples, ctx.fft_width);
				break;
			}
			if ((read_samples = receiver_read(skip_buff, ctx.overlap - ctx.fft_width)) != ctx.overlap - ctx.fft_width) {
				fprintf(stderr, "Failed to read samples, read %d, expected %d\n", read_samples, ctx.overlap - ctx.fft_width);
				break;
			}
		} else if (ctx.overlap > 0) {
			memmove(f32_input, f32_input + ctx.overlap, (ctx.fft_width - ctx.overlap) * sizeof(complexf));
			if ((read_samples = receiver_read(f32_input + ctx.fft_width - ctx.overlap, ctx.overlap)) != ctx.overlap) {
				fprintf(stderr, "Failed to read samples, read %d, expected %d\n", read_samples, ctx.overlap);
				break;
			}
		} else {
			if ((read_samples = receiver_read(f32_input, ctx.fft_width)) != ctx.fft_width) {
				fprintf(stderr, "Failed to read samples, read %d, expected %d\n", read_samples, ctx.fft_width);
				break;
			}
		}

		if (ctx.verbose) {
			t1 = now_ns();
			perf_read += t1 - t0;
			t0 = t1;
		}

		fft_apply_precalculated_window(f32_input, windowed, ctx.fft_width, windowt);
		fft_execute(ctx.fft_plan);
		fft_accumulate_power((complexf*)fft_output, avrg_output, ctx.fft_width);

		if (ctx.verbose) {
			t1 = now_ns();
			perf_fft += t1 - t0;
			perf_n++;
		}

		if (!(++iteration % ctx.avarage)) {
			write_fft_results(&ctx, avrg_output, output);
			int avg_frames = iteration / ctx.avarage;
			if (ctx.n_frames > 0 && avg_frames >= ctx.n_frames)
				ctx.run = 0;
			if (sgram) {
				if (sgram_warmup < 3) {
					sgram_warmup++;
				} else {
					sgram_add_row(sgram, output, ctx.display_min_db, ctx.display_max_db);
				}
			}
		}

		if (ctx.verbose && perf_n > 0) {
			long now = now_ns();
			if (now - perf_t_report >= 2000000000L) {
				fprintf(stderr, "\n[perf] %ld iter/s | read avg %ld μs | fft avg %ld μs\n",
				        perf_n * 1000000000L / (now - perf_t_report),
				        perf_read / perf_n / 1000,
				        perf_fft  / perf_n / 1000);
				perf_read = perf_fft = perf_n = 0;
				perf_t_report = now;
			}
		}

	} while (ctx.run);

	receiver_stop();

	if (sgram) {
		printf("\nsaving spectrogram to %s (%d frames)...\n", ctx.sgram_file, sgram_row_count(sgram));
		sgram_write_png(sgram, ctx.sgram_file);
		sgram_destroy(sgram);
	}

	/* Save max-hold snapshot if requested. */
	if (max_hold_buf) {
		double freq_from = (double)ctx.cf - ctx.rate / 2.0;
		double freq_to   = (double)ctx.cf + ctx.rate / 2.0;
		if (ctx.max_hold_csv)
			spectrum_csv_write("maxhold.csv", max_hold_buf, ctx.fft_width,
			                   freq_from, freq_to);
		if (ctx.max_hold_png)
			spectrum_png_write("maxhold.png", max_hold_buf, ctx.fft_width,
			                   ctx.display_min_db, ctx.display_max_db,
			                   freq_from, freq_to, 0);
	}

	//on last iteration, if the number of iterations is not multiple of average rate, we need to calculate log 
	//power of FFT output for remaining iterations and write to output file
	if((iteration % ctx.avarage)){
		for(int i = 0; i < (ctx.avarage - iteration % ctx.avarage); i++ ){
			fft_accumulate_power((complexf*)fft_output, avrg_output, ctx.fft_width);
		}
		write_fft_results(&ctx, avrg_output, output);
	}

	fftwf_free(f32_input);
	fftwf_free(fft_output);
	fftwf_free(windowed);
	free(windowt);
	free(avrg_output);
	free(skip_buff);
	free(output);
	free(max_hold_buf);

	if(ctx.output != stdin)
		fclose(ctx.output);

	draw_disable_keypress();
	draw_show_cursor();

	return 0;
}