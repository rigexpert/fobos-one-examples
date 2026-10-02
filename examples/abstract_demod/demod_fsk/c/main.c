/**
 * @file main.c
 * @brief M-FSK demodulator — Fobos SDR developer example.
 *
 * @section overview Overview
 *
 * Demonstrates non-coherent M-FSK demodulation (M any power of 2) from
 * a Fobos SDR.  Decoded bits are packed MSB-first into bytes and written to
 * stdout or a file, making the output directly pipeable to protocol decoders or
 * disk capture tools.
 *
 * @section pipeline DSP Pipeline
 *
 * @verbatim
 * [Fobos SDR hardware]
 *        │  IQ float32, interleaved, at samplerate (e.g. 2 MSPS)
 *        │  Dedicated real-time capture thread (SCHED_FIFO, core 3)
 *        ▼
 * [receiver_read()]
 *   Pulls IQ blocks from the lock-free ring buffer.
 *        │
 *        ▼
 * [shift_unroll_process()]           ← optional DDC, -O flag
 *   Shifts the spectrum by -offset Hz, bringing an off-centre FSK signal to DC.
 *   Pre-computed LO table (shift_unroll_init) means the inner loop is pure
 *   multiply-add; NEON-vectorised on ARMv8.
 *        │  IQ float32 at samplerate, signal centred at DC
 *        ▼
 * [msresamp_crcf]
 *   Rational resampler: samplerate  →  symbol_rate × k.
 *   k (samples per symbol) is the oversampling factor fed to fskdem.
 *   The resampler also acts as an anti-alias filter, suppressing out-of-band
 *   energy before symbol detection.
 *        │  IQ float32 at symbol_rate × k
 *        ▼
 * [fskdem_demodulate()]               ← liquid-dsp non-coherent FSK detector
 *   Takes exactly k complex samples and returns a symbol index 0 … M-1.
 *   Internally computes the energy in each of the M tone bins (DFT-based)
 *   and picks the maximum — no carrier phase reference required.
 *        │  unsigned int  symbol ∈ {0 … M-1}
 *        ▼
 * [Symbol → bits]
 *   Each symbol carries log2(M) bits.  Bits are shifted MSB-first into a
 *   32-bit accumulator and flushed as complete bytes (8 bits).
 *   For M=2: 8 symbols → 1 byte.
 *   For M=4: 4 symbols → 1 byte.
 *   For M=8: 8 symbols → 3 bytes.
 *        │  uint8_t byte stream
 *        ▼
 * [fwrite() to output]
 *   Raw byte stream written to stdout or -o file.
 *   Pipe through xxd, od, or a protocol decoder for further analysis.
 * @endverbatim
 *
 * @section quickstart Quick Start
 *
 * Demodulate a 9600-baud 2-FSK signal at 433.92 MHz (SDR at 2 MSPS):
 * @code
 *   ./demod_fsk -F 433920000 -s 2000000 -R 9600 -k 8 -M 2 | xxd | head
 * @endcode
 *
 * The signal is 100 kHz above the tuned centre — use DDC to shift it down:
 * @code
 *   ./demod_fsk -F 433820000 -s 2000000 -R 9600 -k 8 -M 2 -O 100000 | xxd
 * @endcode
 *
 * 4-FSK at 115200 baud, 4 MSPS input, 16 samples per symbol:
 * @code
 *   ./demod_fsk -F 868000000 -s 4000000 -R 115200 -k 16 -M 4 -v | xxd
 * @endcode
 *
 * @section notes Implementation Notes
 *
 * - **Symbol timing**: fskdem receives exactly k samples per call; there is no
 *   closed-loop timing recovery in this example.  If the transmitter and receiver
 *   clocks drift, accumulated timing error will eventually cause symbol boundary
 *   misalignment.  Add a symsync_crcf stage after the resampler for a production
 *   implementation.
 * - **Tone spacing**: fskdem assumes M equally-spaced tones across the bandwidth.
 *   The default bandwidth (0.5 normalised to symbol_rate × k) gives minimum tone
 *   spacing equal to half the symbol rate — the classical MSK condition.  Adjust
 *   with -b for wider-deviation FSK systems.
 * - **Extending**: for POCSAG (1200/2400 baud 2-FSK) or AX.25 (1200 baud AFSK),
 *   pipe the decoded bytes into a frame decoder.  For LoRa-style chirp FSK, a
 *   different demodulation approach is needed.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <getopt.h>
#include <errno.h>
#include <stdbool.h>

#ifndef __USE_POSIX
#define __USE_POSIX 1
#endif
#include <signal.h>

#include <liquid/liquid.h>

#include "dsp.h"
#include "receiver.h"

/* ------------------------------------------------------------------ constants */

#define FOBOS_BUFFER_SIZE   (64 * 1024)  /* complex samples per ring-buffer chunk */
#define FOBOS_BUFFER_COUNT  32            /* ring-buffer depth */
#define INPUT_BLOCK_SIZE    8192          /* complex IQ samples per DSP iteration */

#define DEFAULT_SAMPLERATE  2000000       /* SDR capture rate, Hz */
#define DEFAULT_SYMBOL_RATE 9600          /* FSK symbol rate, baud */
#define DEFAULT_SPS         8             /* samples per symbol (oversampling) */
#define DEFAULT_M           2             /* number of FSK symbols */
#define DEFAULT_BANDWIDTH   0.5f          /* fskdem normalised bandwidth */

/* ------------------------------------------------------------------ types */

/** @brief Symbol-to-bits mapping applied after tone detection. */
typedef enum {
    MAP_NATURAL = 0, /**< Tone index used directly as binary value (default). */
    MAP_GRAY    = 1, /**< Inverse Gray code: adjacent tones differ by one bit. */
} map_t;

/** @brief What to write for each decoded symbol. */
typedef enum {
    OUT_BITS    = 0, /**< Individual bits extracted from the symbol (default). */
    OUT_SYMBOLS = 1, /**< Raw symbol index (0..M-1) after mapping. */
} out_content_t;

/** @brief Byte encoding of the output stream. */
typedef enum {
    FMT_BINARY = 0, /**< Raw bytes: 1 byte per symbol or per bit (default). */
    FMT_ASCII  = 1, /**< Text: symbols one per line, bits as '0'/'1' stream. */
} out_format_t;

/**
 * @brief Command-line arguments for the FSK demodulator.
 */
typedef struct {
    uint64_t frequency;        /**< SDR tuning frequency in Hz (required). */
    int      samplerate;       /**< SDR input sample rate in Hz. */
    int      symbol_rate;      /**< FSK symbol rate (baud). */
    int      M;                /**< Number of FSK symbols, power of 2 >= 2. */
    int      k;                /**< Samples per symbol (oversampling factor). */
    float    bandwidth;        /**< fskdem tone bandwidth, normalised to symbol_rate × k.
                                    Typical range: 0.1 – 0.5.  Default: 0.5 (MSK spacing). */
    float    freq_offset_hz;   /**< DDC shift in Hz; 0 = disabled. */
    int      lna_gain;         /**< LNA gain index; -1 = leave hardware default. */
    int      vga_gain;         /**< VGA gain index; -1 = leave hardware default. */
    int      verbose;          /**< Print runtime statistics to stderr every 2 s. */
    char    *output_filename;  /**< Output path; NULL = stdout. */
    receiver_mode_t receiver_mode; /**< Sync or async capture mode. */
    map_t        mapping;   /**< Symbol-to-bits mapping scheme. */
    out_content_t content;  /**< Output content: bits or symbol indices. */
    out_format_t  format;   /**< Output encoding: binary bytes or ASCII text. */
} args_t;

/**
 * @brief Runtime context: DSP objects and running statistics.
 */
typedef struct {
    bool           run;              /**< Set to false by signal handler to stop the loop. */

    fskdem         demodulator;      /**< liquid-dsp M-FSK non-coherent detector. */
    msresamp_crcf  resampler;        /**< Rational resampler: samplerate → symbol_rate × k. */
    float          resamp_rate;      /**< Computed resampling ratio. */

    int            use_ddc;          /**< Non-zero if DDC frequency shift is active. */
    shift_unroll_data_t ddc;         /**< Pre-computed DDC LO table. */
    float          ddc_phase;        /**< Continuous DDC oscillator phase across blocks. */

    int            bits_per_symbol;  /**< log2(M) — bits carried by each FSK symbol. */

    liquid_float_complex *iq_buf;     /**< IQ block read from the ring buffer. */
    liquid_float_complex *resamp_buf; /**< Resampler output buffer. */
    size_t         resamp_buf_size;  /**< Allocated length of resamp_buf in samples. */

    FILE          *output;           /**< Decoded byte stream destination. */
    size_t         samples_total;    /**< Cumulative IQ samples consumed. */
    size_t         symbols_total;    /**< Cumulative FSK symbols decoded. */
    size_t         units_written;    /**< Cumulative output units written (bytes or chars). */
    time_t         start_time;       /**< Wall-clock start (for verbose reports). */
    time_t         last_report;      /**< Wall-clock time of last verbose report. */
} ctx_t;

static args_t args;
static ctx_t  ctx;

/* ------------------------------------------------------------------ helpers */

static void signal_handler(int signum) {
    static int count = 0;
    fprintf(stderr, "received signal %d\n", signum);
    if (count)
        exit(-1);
    count++;
    ctx.run = false;
}

/* Inverse Gray code: recovers natural binary from Gray-coded tone index.
 * If the transmitter maps bit pattern n to tone gray(n), the receiver
 * applies this to get n back from the received tone index. */
static unsigned int gray_to_natural(unsigned int g) {
    unsigned int n = g;
    while (g >>= 1)
        n ^= g;
    return n;
}

static void print_help(void) {
    fprintf(stderr, "demod_fsk — M-FSK demodulator using Fobos SDR + liquid-dsp\n");
    fprintf(stderr, "usage: demod_fsk [options]\n\n");
    fprintf(stderr, "  -F, --frequency   <hz>       SDR tuning frequency (required)\n");
    fprintf(stderr, "  -s, --samplerate  <hz>       SDR capture rate (default: %d)\n", DEFAULT_SAMPLERATE);
    fprintf(stderr, "  -R, --symrate     <baud>     FSK symbol rate (default: %d)\n", DEFAULT_SYMBOL_RATE);
    fprintf(stderr, "  -k, --sps         <n>        samples per symbol (default: %d)\n", DEFAULT_SPS);
    fprintf(stderr, "  -M, --symbols     <n>        number of FSK symbols, power of 2 >= 2 (default: %d)\n", DEFAULT_M);
    fprintf(stderr, "  -b, --bandwidth   <0.0-1.0>  fskdem tone bandwidth normalised to\n");
    fprintf(stderr, "                               symbol_rate × sps (default: %.2f)\n", DEFAULT_BANDWIDTH);
    fprintf(stderr, "  -O, --offset      <hz>       DDC frequency offset from centre (default: 0)\n");
    fprintf(stderr, "  -c, --coding      <natural|gray>     symbol mapping (default: natural)\n");
    fprintf(stderr, "  -t, --type        <bits|symbols>     output content (default: bits)\n");
    fprintf(stderr, "  -f, --format      <binary|ascii>     output encoding (default: binary)\n");
    fprintf(stderr, "                                         bits+binary:   1 byte/bit (0x00/0x01)\n");
    fprintf(stderr, "                                         bits+ascii:    '0'/'1' stream\n");
    fprintf(stderr, "                                         symbols+binary: 1 byte/symbol (0..M-1)\n");
    fprintf(stderr, "                                         symbols+ascii:  decimal, one per line\n");
    fprintf(stderr, "  -o, --output      <file>             output file (default: stdout)\n");
    fprintf(stderr, "  -L, --lna-gain    <value>    LNA gain index 0..3 (0,1: 0 dB, 2: +16, 3: +33)\n");
    fprintf(stderr, "  -V, --vga-gain    <value>    VGA gain index 0..31 (0..+62 dB, 2 dB step)\n");
    fprintf(stderr, "  -m, --mode        <sync|async> capture mode (default: sync)\n");
    fprintf(stderr, "  -v, --verbose                print runtime statistics to stderr\n");
    fprintf(stderr, "  -h, --help                   print this message\n\n");
    fprintf(stderr, "Example — 9600 baud 2-FSK at 433.92 MHz, 2 MSPS:\n");
    fprintf(stderr, "  demod_fsk -F 433920000 -s 2000000 -R 9600 -k 8 -M 2 | xxd\n\n");
    fprintf(stderr, "Example — 4800 baud 4-FSK, signal 50 kHz above centre:\n");
    fprintf(stderr, "  demod_fsk -F 868000000 -s 2000000 -R 4800 -k 8 -M 4 -O 50000 | xxd\n");
}

static void dump_info(void) {
    int fsk_rate = args.symbol_rate * args.k;
    fprintf(stderr, "Center frequency:   %lu Hz\n", args.frequency);
    fprintf(stderr, "SDR sample rate:    %d Hz\n", args.samplerate);
    fprintf(stderr, "Symbol rate:        %d baud\n", args.symbol_rate);
    fprintf(stderr, "Symbols (M):        %d  (%d bits/symbol)\n", args.M, ctx.bits_per_symbol);
    fprintf(stderr, "Samples/symbol (k): %d\n", args.k);
    fprintf(stderr, "fskdem input rate:  %d Hz  (= symbol_rate × k)\n", fsk_rate);
    fprintf(stderr, "Resample ratio:     %.6f  (%d → %d Hz)\n", ctx.resamp_rate, args.samplerate, fsk_rate);
    fprintf(stderr, "fskdem bandwidth:   %.3f  (normalised to fskdem input rate)\n", args.bandwidth);
    if (args.freq_offset_hz != 0.0f)
        fprintf(stderr, "DDC offset:         %+.0f Hz\n", args.freq_offset_hz);
    else
        fprintf(stderr, "DDC offset:         disabled\n");
    fprintf(stderr, "Capture mode:       %s\n", args.receiver_mode == receiver_mode_sync ? "sync" : "async");
    fprintf(stderr, "Symbol mapping:     %s\n", args.mapping == MAP_GRAY ? "gray" : "natural");
    fprintf(stderr, "Output content:     %s\n", args.content == OUT_SYMBOLS ? "symbols" : "bits");
    fprintf(stderr, "Output format:      %s\n", args.format  == FMT_ASCII   ? "ascii"   : "binary");
    fprintf(stderr, "Input block:        %d IQ samples\n", INPUT_BLOCK_SIZE);
    fprintf(stderr, "Resamp buf:         %zu IQ samples\n", ctx.resamp_buf_size);
}

static void parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"frequency",  required_argument, 0, 'F'},
        {"samplerate", required_argument, 0, 's'},
        {"symrate",    required_argument, 0, 'R'},
        {"sps",        required_argument, 0, 'k'},
        {"symbols",    required_argument, 0, 'M'},
        {"bandwidth",  required_argument, 0, 'b'},
        {"offset",     required_argument, 0, 'O'},
        {"coding",     required_argument, 0, 'c'},
        {"type",       required_argument, 0, 't'},
        {"format",     required_argument, 0, 'f'},
        {"output",     required_argument, 0, 'o'},
        {"lna-gain",   required_argument, 0, 'L'},
        {"vga-gain",   required_argument, 0, 'V'},
        {"mode",       required_argument, 0, 'm'},
        {"verbose",    no_argument,       0, 'v'},
        {"help",       no_argument,       0, 'h'},
        {NULL, 0, NULL, 0}
    };

    int opt, long_index = 0;
    while ((opt = getopt_long(argc, argv, "F:s:R:k:M:b:O:c:t:f:o:L:V:m:vh", long_options, &long_index)) != -1) {
        switch (opt) {
            case 'F':
                args.frequency = (uint64_t)atoll(optarg);
                break;
            case 's':
                args.samplerate = atoi(optarg);
                if (args.samplerate <= 0) {
                    fprintf(stderr, "samplerate must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'R':
                args.symbol_rate = atoi(optarg);
                if (args.symbol_rate <= 0) {
                    fprintf(stderr, "symbol rate must be > 0\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'k':
                args.k = atoi(optarg);
                if (args.k < 2) {
                    fprintf(stderr, "sps (samples per symbol) must be >= 2\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'M':
                args.M = atoi(optarg);
                if (args.M < 2 || (args.M & (args.M - 1)) != 0) {
                    fprintf(stderr, "symbols (-M) must be a power of 2 >= 2\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'b':
                args.bandwidth = atof(optarg);
                if (args.bandwidth <= 0.0f || args.bandwidth >= 1.0f) {
                    fprintf(stderr, "bandwidth must be in (0, 1)\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'O':
                args.freq_offset_hz = atof(optarg);
                break;
            case 'c':
                if (strcmp(optarg, "gray") == 0)
                    args.mapping = MAP_GRAY;
                else if (strcmp(optarg, "natural") == 0)
                    args.mapping = MAP_NATURAL;
                else {
                    fprintf(stderr, "unknown coding '%s': use natural or gray\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 't':
                if (strcmp(optarg, "bits") == 0)
                    args.content = OUT_BITS;
                else if (strcmp(optarg, "symbols") == 0)
                    args.content = OUT_SYMBOLS;
                else {
                    fprintf(stderr, "unknown type '%s': use bits or symbols\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'f':
                if (strcmp(optarg, "binary") == 0)
                    args.format = FMT_BINARY;
                else if (strcmp(optarg, "ascii") == 0)
                    args.format = FMT_ASCII;
                else {
                    fprintf(stderr, "unknown format '%s': use binary or ascii\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'o':
                args.output_filename = optarg;
                break;
            case 'L':
                args.lna_gain = atoi(optarg);
                break;
            case 'V':
                args.vga_gain = atoi(optarg);
                break;
            case 'm':
                if (strcmp(optarg, "async") == 0)
                    args.receiver_mode = receiver_mode_async;
                else if (strcmp(optarg, "sync") == 0)
                    args.receiver_mode = receiver_mode_sync;
                else {
                    fprintf(stderr, "unknown mode '%s': use sync or async\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'v':
                args.verbose = 1;
                break;
            case 'h':
                print_help();
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "unknown option; try -h\n");
                exit(EXIT_FAILURE);
        }
    }

    if (args.frequency == 0) {
        fprintf(stderr, "center frequency is required (-F)\n");
        print_help();
        exit(EXIT_FAILURE);
    }
}

/* ------------------------------------------------------------------ main */

int main(int argc, char *argv[]) {
    /* defaults */
    args.samplerate    = DEFAULT_SAMPLERATE;
    args.symbol_rate   = DEFAULT_SYMBOL_RATE;
    args.M             = DEFAULT_M;
    args.k             = DEFAULT_SPS;
    args.bandwidth     = DEFAULT_BANDWIDTH;
    args.freq_offset_hz = 0.0f;
    args.lna_gain      = -1;
    args.vga_gain      = -1;
    args.receiver_mode = receiver_mode_sync;
    args.output_filename = NULL;
    args.mapping = MAP_NATURAL;
    args.content = OUT_BITS;
    args.format  = FMT_BINARY;

    parse_args(argc, argv);

    /* signal handling */
    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* open output */
    if (args.output_filename) {
        ctx.output = fopen(args.output_filename, "wb");
        if (!ctx.output) {
            fprintf(stderr, "cannot open '%s': %s\n", args.output_filename, strerror(errno));
            return EXIT_FAILURE;
        }
    } else {
        ctx.output = stdout;
    }

    ctx.bits_per_symbol = __builtin_ctz((unsigned)args.M); /* log2(M) */

    /* resampling ratio: SDR rate → symbol_rate × k */
    ctx.resamp_rate = (float)(args.symbol_rate * args.k) / (float)args.samplerate;

    /* warn about extreme iteration rates in sliding-window mode */
    fprintf(stderr, "effective iteration rate: %.0f iter/s  (block=%d, ratio=%.5f)\n",
            (double)args.samplerate / INPUT_BLOCK_SIZE, INPUT_BLOCK_SIZE, ctx.resamp_rate);

    /* resampler — 60 dB stopband attenuation */
    ctx.resampler = msresamp_crcf_create(ctx.resamp_rate, 60.0f);

    /* FSK demodulator */
    ctx.demodulator = fskdem_create((unsigned int)args.M, (unsigned int)args.k, args.bandwidth);

    /* optional DDC */
    ctx.use_ddc = (args.freq_offset_hz != 0.0f);
    if (ctx.use_ddc) {
        /* negate: shift_unroll shifts UP by rate, so pass -offset to shift signal DOWN to DC */
        float norm_shift = -args.freq_offset_hz / (float)args.samplerate;
        ctx.ddc = shift_unroll_init(norm_shift, INPUT_BLOCK_SIZE);
        ctx.ddc_phase = 0.0f;
    }

    /* allocate IQ input buffer */
    ctx.iq_buf = (liquid_float_complex *)malloc(sizeof(liquid_float_complex) * INPUT_BLOCK_SIZE);
    if (!ctx.iq_buf) {
        fprintf(stderr, "malloc iq_buf failed\n");
        return EXIT_FAILURE;
    }

    /* allocate resampler output buffer
     * For downsampling (resamp_rate < 1): output < input.
     * For upsampling (resamp_rate > 1): output > input.  The ×2 + k margin is for safety. */
    ctx.resamp_buf_size = (size_t)(INPUT_BLOCK_SIZE * fmaxf(ctx.resamp_rate, 1.0f) * 2.0f) + (size_t)args.k + 64;
    ctx.resamp_buf = (liquid_float_complex *)malloc(sizeof(liquid_float_complex) * ctx.resamp_buf_size);
    if (!ctx.resamp_buf) {
        fprintf(stderr, "malloc resamp_buf failed\n");
        return EXIT_FAILURE;
    }

    /* open SDR */
    fprintf(stderr, "initializing fobos...\n");
    if (receiver_init(FOBOS_BUFFER_SIZE, FOBOS_BUFFER_COUNT) != 0) {
        fprintf(stderr, "failed to open fobos device\n");
        return EXIT_FAILURE;
    }

    if (args.lna_gain >= 0)
        receiver_set_lna((uint8_t)args.lna_gain);
    if (args.vga_gain >= 0)
        receiver_set_vga((uint8_t)args.vga_gain);

    receiver_set_freq(args.frequency);
    receiver_set_samplerate((uint32_t)args.samplerate);

    if (args.verbose)
        dump_info();

    ctx.run = true;
    ctx.start_time = ctx.last_report = time(NULL);

    receiver_start(args.receiver_mode);

    while (ctx.run) {
        /* ── 1. read IQ block ────────────────────────────────────────────── */
        if (receiver_read(ctx.iq_buf, INPUT_BLOCK_SIZE) != INPUT_BLOCK_SIZE)
            break;
        ctx.samples_total += INPUT_BLOCK_SIZE;

        /* ── 2. DDC (optional) ──────────────────────────────────────────── */
        if (ctx.use_ddc) {
            ctx.ddc_phase = shift_unroll_process(
                ctx.iq_buf, data_format_f32,
                (complexf *)ctx.iq_buf,   /* in-place */
                INPUT_BLOCK_SIZE,
                &ctx.ddc, ctx.ddc_phase);
        }

        /* ── 3. resample to symbol_rate × k ─────────────────────────────── */
        unsigned int n_out = 0;
        msresamp_crcf_execute(ctx.resampler, ctx.iq_buf, INPUT_BLOCK_SIZE, ctx.resamp_buf, &n_out);

        /* ── 4. demodulate: k samples → 1 symbol ─────────────────────────── */
        unsigned int i = 0;
        while (i + (unsigned int)args.k <= n_out) {
            unsigned int sym = fskdem_demodulate(ctx.demodulator, ctx.resamp_buf + i);
            i += (unsigned int)args.k;
            ctx.symbols_total++;

            /* ── 5. symbol mapping ──────────────────────────────────────── */
            unsigned int val = (args.mapping == MAP_GRAY)
                               ? gray_to_natural(sym) : sym;

            /* ── 6. output ───────────────────────────────────────────────── */
            if (args.content == OUT_SYMBOLS) {
                if (args.format == FMT_ASCII) {
                    ctx.units_written += (size_t)fprintf(ctx.output, "%u\n", val);
                } else {
                    uint8_t b = (uint8_t)val;
                    fwrite(&b, 1, 1, ctx.output);
                    ctx.units_written++;
                }
            } else { /* OUT_BITS */
                for (int b = ctx.bits_per_symbol - 1; b >= 0; b--) {
                    unsigned int bit = (val >> b) & 1u;
                    if (args.format == FMT_ASCII) {
                        char c = '0' + bit;
                        fwrite(&c, 1, 1, ctx.output);
                    } else {
                        uint8_t byt = (uint8_t)bit;
                        fwrite(&byt, 1, 1, ctx.output);
                    }
                    ctx.units_written++;
                }
            }
        }

        /* ── 6. verbose statistics ────────────────────────────────────────── */
        if (args.verbose) {
            time_t now = time(NULL);
            if (now - ctx.last_report >= 2) {
                long elapsed = (long)(now - ctx.start_time);
                fprintf(stderr, "[+%lds] samples: %zu  symbols: %zu  %s out: %zu\n",
                        elapsed, ctx.samples_total, ctx.symbols_total,
                        args.content == OUT_SYMBOLS ? "symbols" : "bits",
                        ctx.units_written);
                ctx.last_report = now;
            }
        }
    }

    /* ── shutdown ────────────────────────────────────────────────────────── */
    receiver_stop();
    receiver_close();

    fskdem_destroy(ctx.demodulator);
    msresamp_crcf_destroy(ctx.resampler);
    if (ctx.use_ddc) {
        free(ctx.ddc.dsin);
        free(ctx.ddc.dcos);
    }
    free(ctx.iq_buf);
    free(ctx.resamp_buf);

    if (ctx.output != stdout)
        fclose(ctx.output);

    if (args.verbose) {
        long elapsed = (long)(time(NULL) - ctx.start_time);
        fprintf(stderr, "done: %zu samples, %zu symbols, %zu %s out in %lds\n",
                ctx.samples_total, ctx.symbols_total, ctx.units_written,
                args.content == OUT_SYMBOLS ? "symbols" : "bits", elapsed);
    }

    return 0;
}
