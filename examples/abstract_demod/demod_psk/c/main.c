/**
 * @file main.c
 * @brief M-PSK demodulator — Fobos SDR developer example.
 *
 * @section overview Overview
 *
 * Demonstrates coherent M-PSK demodulation (M any power of 2 from 2 to 64)
 * from a Fobos SDR.  Decoded bits are written to stdout or a file, making the
 * output directly pipeable to protocol decoders or disk capture tools.
 *
 * @section pipeline DSP Pipeline
 *
 * @verbatim
 * [Fobos SDR hardware]
 *        │  IQ float32, interleaved, at samplerate (e.g. 2 MSPS)
 *        ▼
 * [receiver_read()]
 *   Pulls IQ blocks from the lock-free ring buffer.
 *        │
 *        ▼
 * [shift_unroll_process()]           ← optional DDC, -O flag
 *   Shifts the spectrum by -offset Hz, bringing an off-centre PSK signal to DC.
 *        │  IQ float32 at samplerate, signal centred at DC
 *        ▼
 * [msresamp_crcf]
 *   Rational resampler: samplerate → symbol_rate × k.
 *   Also acts as anti-alias / matched-filter pre-filter.
 *        │  IQ float32 at symbol_rate × k
 *        ▼
 * [symsync_crcf]                     ← Gardner timing error detector + polyphase RRC bank
 *   Recovers symbol timing from the oversampled stream.  Uses a root-raised-cosine
 *   polyphase filter bank (npfb=32 banks) to interpolate the optimal sample instant.
 *   Output rate ≈ symbol_rate (one complex sample per symbol).
 *        │  IQ float32 at symbol_rate
 *        ▼
 * [nco_crcf + modemcf]               ← Costas loop carrier phase recovery
 *   For each timing-corrected symbol sample:
 *     1. nco_crcf_mix_down() — rotate by NCO estimate of carrier phase
 *     2. modemcf_demodulate() — hard-decision PSK demodulator
 *     3. modemcf_get_demodulator_phase_error() — angle between received sample
 *        and ideal constellation point for the decoded symbol
 *     4. nco_crcf_pll_step() + nco_crcf_step() — update Costas PLL
 *        │  unsigned int  symbol ∈ {0 … M-1}
 *        ▼
 * [Symbol → bits]
 *   Each symbol carries log2(M) bits, extracted MSB-first.
 *        │  uint8_t byte stream
 *        ▼
 * [fwrite() to output]
 * @endverbatim
 *
 * @section quickstart Quick Start
 *
 * Demodulate a 9600-baud QPSK signal at 433.92 MHz (SDR at 2 MSPS):
 * @code
 *   ./demod_psk -F 433920000 -s 2000000 -R 9600 -k 8 -M 4 | xxd | head
 * @endcode
 *
 * BPSK signal 100 kHz above the tuned centre — use DDC to shift it down:
 * @code
 *   ./demod_psk -F 433820000 -s 2000000 -R 9600 -k 8 -M 2 -O 100000 | xxd
 * @endcode
 *
 * 8-PSK at 115200 baud, 4 MSPS input, 16 samples per symbol:
 * @code
 *   ./demod_psk -F 868000000 -s 4000000 -R 115200 -k 16 -M 8 -v | xxd
 * @endcode
 *
 * @section notes Implementation Notes
 *
 * - **Phase ambiguity**: PSK has an M-fold carrier phase ambiguity.  The Costas
 *   loop may lock to any of the M equivalent phase solutions.  Real-world
 *   systems resolve this with differential encoding (DPSK) or a pilot sequence.
 * - **PLL bandwidth**: the -A option sets the Costas loop bandwidth (normalised
 *   to the symbol rate).  Wider bandwidth = faster acquisition but worse SNR.
 *   Typical range: 0.01 – 0.10.  Default 0.05 gives ~50 symbol convergence time.
 * - **RRC rolloff**: the -r option sets the root-raised-cosine rolloff factor β.
 *   It must match the transmitter's RRC filter.  Common values: 0.25, 0.35, 0.5.
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

#define FOBOS_BUFFER_SIZE   (64 * 1024)
#define FOBOS_BUFFER_COUNT  32
#define INPUT_BLOCK_SIZE    8192

#define DEFAULT_SAMPLERATE  2000000
#define DEFAULT_SYMBOL_RATE 9600
#define DEFAULT_SPS         8        /* samples per symbol (oversampling into symsync) */
#define DEFAULT_M           4        /* QPSK */
#define DEFAULT_ROLLOFF     0.25f    /* RRC excess bandwidth */
#define DEFAULT_PLL_BW      0.05f   /* Costas loop bandwidth, normalised to symbol rate */
#define SYMSYNC_FILT_DELAY  3        /* RRC filter semi-length in symbols */
#define SYMSYNC_NPFB        32       /* polyphase filter banks */

/* ------------------------------------------------------------------ types */

typedef enum {
    MAP_NATURAL = 0,
    MAP_GRAY    = 1,
} map_t;

typedef enum {
    OUT_BITS    = 0,
    OUT_SYMBOLS = 1,
} out_content_t;

typedef enum {
    FMT_BINARY = 0,
    FMT_ASCII  = 1,
} out_format_t;

typedef struct {
    uint64_t frequency;
    int      samplerate;
    int      symbol_rate;
    int      M;              /**< PSK constellation size: 2,4,8,16,32,64 */
    int      k;              /**< Samples per symbol fed to symsync */
    float    rolloff;        /**< RRC rolloff factor β */
    float    pll_bw;         /**< Costas loop bandwidth, normalised to symbol rate */
    float    freq_offset_hz;
    int      lna_gain;
    int      vga_gain;
    int      verbose;
    char    *output_filename;
    receiver_mode_t receiver_mode;
    map_t        mapping;
    out_content_t content;
    out_format_t  format;
} args_t;

typedef struct {
    bool           run;

    modemcf        modem;         /**< liquid-dsp M-PSK hard-decision demodulator */
    symsync_crcf   symsync;       /**< Gardner TED + polyphase RRC bank */
    nco_crcf       nco;           /**< Costas loop carrier NCO */
    msresamp_crcf  resampler;     /**< samplerate → symbol_rate × k */
    float          resamp_rate;

    int            use_ddc;
    shift_unroll_data_t ddc;
    float          ddc_phase;

    int            bits_per_symbol;

    liquid_float_complex *iq_buf;
    liquid_float_complex *resamp_buf;
    liquid_float_complex *sync_buf;
    size_t         resamp_buf_size;
    size_t         sync_buf_size;

    FILE          *output;
    size_t         samples_total;
    size_t         symbols_total;
    size_t         units_written;
    time_t         start_time;
    time_t         last_report;
} ctx_t;

static args_t args;
static ctx_t  ctx;

/* ------------------------------------------------------------------ helpers */

static void signal_handler(int signum) {
    static int count = 0;
    fprintf(stderr, "received signal %d\n", signum);
    if (count) exit(-1);
    count++;
    ctx.run = false;
}

static unsigned int gray_to_natural(unsigned int g) {
    unsigned int n = g;
    while (g >>= 1) n ^= g;
    return n;
}

static modulation_scheme m_to_psk(int M) {
    switch (M) {
        case 2:   return LIQUID_MODEM_PSK2;
        case 4:   return LIQUID_MODEM_PSK4;
        case 8:   return LIQUID_MODEM_PSK8;
        case 16:  return LIQUID_MODEM_PSK16;
        case 32:  return LIQUID_MODEM_PSK32;
        case 64:  return LIQUID_MODEM_PSK64;
        default:
            fprintf(stderr, "unsupported M=%d — use 2, 4, 8, 16, 32, or 64\n", M);
            exit(EXIT_FAILURE);
    }
}

static void print_help(void) {
    fprintf(stderr, "demod_psk — M-PSK demodulator using Fobos SDR + liquid-dsp\n");
    fprintf(stderr, "usage: demod_psk [options]\n\n");
    fprintf(stderr, "  -F, --frequency   <hz>       SDR tuning frequency (required)\n");
    fprintf(stderr, "  -s, --samplerate  <hz>       SDR capture rate (default: %d)\n", DEFAULT_SAMPLERATE);
    fprintf(stderr, "  -R, --symrate     <baud>     PSK symbol rate (default: %d)\n", DEFAULT_SYMBOL_RATE);
    fprintf(stderr, "  -k, --sps         <n>        samples per symbol into symsync (default: %d)\n", DEFAULT_SPS);
    fprintf(stderr, "  -M, --symbols     <n>        PSK order: 2,4,8,16,32,64 (default: %d = QPSK)\n", DEFAULT_M);
    fprintf(stderr, "  -r, --rolloff     <0.0-1.0>  RRC rolloff factor β (default: %.2f)\n", DEFAULT_ROLLOFF);
    fprintf(stderr, "  -A, --pll-bw      <0.0-1.0>  Costas loop bandwidth, norm. to symbol rate\n");
    fprintf(stderr, "                               (default: %.2f); wider=faster but noisier\n", DEFAULT_PLL_BW);
    fprintf(stderr, "  -O, --offset      <hz>       DDC frequency offset from centre (default: 0)\n");
    fprintf(stderr, "  -c, --coding      <natural|gray>     symbol mapping (default: natural)\n");
    fprintf(stderr, "  -t, --type        <bits|symbols>     output content (default: bits)\n");
    fprintf(stderr, "  -f, --format      <binary|ascii>     output encoding (default: binary)\n");
    fprintf(stderr, "                                         bits+binary:    1 byte/bit (0x00/0x01)\n");
    fprintf(stderr, "                                         bits+ascii:     '0'/'1' stream\n");
    fprintf(stderr, "                                         symbols+binary: 1 byte/symbol (0..M-1)\n");
    fprintf(stderr, "                                         symbols+ascii:  decimal, one per line\n");
    fprintf(stderr, "  -o, --output      <file>             output file (default: stdout)\n");
    fprintf(stderr, "  -L, --lna-gain    <value>    LNA gain index 0..3 (0,1: 0 dB, 2: +16, 3: +33)\n");
    fprintf(stderr, "  -V, --vga-gain    <value>    VGA gain index 0..31 (0..+62 dB, 2 dB step)\n");
    fprintf(stderr, "  -m, --mode        <sync|async> capture mode (default: sync)\n");
    fprintf(stderr, "  -v, --verbose                print runtime statistics to stderr\n");
    fprintf(stderr, "  -h, --help                   print this message\n\n");
    fprintf(stderr, "Example — 9600 baud QPSK at 433.92 MHz, 2 MSPS:\n");
    fprintf(stderr, "  demod_psk -F 433920000 -s 2000000 -R 9600 -k 8 -M 4 | xxd\n\n");
    fprintf(stderr, "Example — BPSK signal 50 kHz above centre, Gray-coded bits:\n");
    fprintf(stderr, "  demod_psk -F 868000000 -s 2000000 -R 4800 -M 2 -O 50000 -c gray | xxd\n");
}

static void dump_info(void) {
    const char *scheme_names[] = {"BPSK","QPSK","8-PSK","16-PSK","32-PSK","64-PSK"};
    int mi = __builtin_ctz((unsigned)args.M) - 1;
    fprintf(stderr, "Center frequency:    %lu Hz\n", args.frequency);
    fprintf(stderr, "SDR sample rate:     %d Hz\n", args.samplerate);
    fprintf(stderr, "Symbol rate:         %d baud\n", args.symbol_rate);
    fprintf(stderr, "Modulation:          M=%d  %s  (%d bits/symbol)\n",
            args.M, (mi >= 0 && mi < 6) ? scheme_names[mi] : "?", ctx.bits_per_symbol);
    fprintf(stderr, "Samples/symbol (k):  %d\n", args.k);
    fprintf(stderr, "symsync input rate:  %d Hz  (= symbol_rate × k)\n", args.symbol_rate * args.k);
    fprintf(stderr, "Resample ratio:      %.6f  (%d → %d Hz)\n",
            ctx.resamp_rate, args.samplerate, args.symbol_rate * args.k);
    fprintf(stderr, "RRC rolloff β:       %.3f\n", args.rolloff);
    fprintf(stderr, "Costas loop bw:      %.4f  (norm. to symbol rate)\n", args.pll_bw);
    if (args.freq_offset_hz != 0.0f)
        fprintf(stderr, "DDC offset:          %+.0f Hz\n", args.freq_offset_hz);
    else
        fprintf(stderr, "DDC offset:          disabled\n");
    fprintf(stderr, "Capture mode:        %s\n",
            args.receiver_mode == receiver_mode_sync ? "sync" : "async");
    fprintf(stderr, "Symbol mapping:      %s\n", args.mapping == MAP_GRAY ? "gray" : "natural");
    fprintf(stderr, "Output content:      %s\n", args.content == OUT_SYMBOLS ? "symbols" : "bits");
    fprintf(stderr, "Output format:       %s\n", args.format  == FMT_ASCII   ? "ascii"   : "binary");
    fprintf(stderr, "Input block:         %d IQ samples\n", INPUT_BLOCK_SIZE);
    fprintf(stderr, "Resamp buf:          %zu IQ samples\n", ctx.resamp_buf_size);
    fprintf(stderr, "Sync buf:            %zu IQ samples\n", ctx.sync_buf_size);
}

static void parse_args(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"frequency",  required_argument, 0, 'F'},
        {"samplerate", required_argument, 0, 's'},
        {"symrate",    required_argument, 0, 'R'},
        {"sps",        required_argument, 0, 'k'},
        {"symbols",    required_argument, 0, 'M'},
        {"rolloff",    required_argument, 0, 'r'},
        {"pll-bw",     required_argument, 0, 'A'},
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
    while ((opt = getopt_long(argc, argv, "F:s:R:k:M:r:A:O:c:t:f:o:L:V:m:vh",
                              long_options, &long_index)) != -1) {
        switch (opt) {
            case 'F': args.frequency    = (uint64_t)atoll(optarg); break;
            case 's':
                args.samplerate = atoi(optarg);
                if (args.samplerate <= 0) {
                    fprintf(stderr, "samplerate must be > 0\n"); exit(EXIT_FAILURE);
                }
                break;
            case 'R':
                args.symbol_rate = atoi(optarg);
                if (args.symbol_rate <= 0) {
                    fprintf(stderr, "symbol rate must be > 0\n"); exit(EXIT_FAILURE);
                }
                break;
            case 'k':
                args.k = atoi(optarg);
                if (args.k < 2) {
                    fprintf(stderr, "sps must be >= 2\n"); exit(EXIT_FAILURE);
                }
                break;
            case 'M':
                args.M = atoi(optarg);
                if (args.M < 2 || args.M > 64 || (args.M & (args.M - 1)) != 0) {
                    fprintf(stderr, "M must be a power of 2 in {2,4,8,16,32,64}\n");
                    exit(EXIT_FAILURE);
                }
                break;
            case 'r':
                args.rolloff = atof(optarg);
                if (args.rolloff <= 0.0f || args.rolloff >= 1.0f) {
                    fprintf(stderr, "rolloff must be in (0, 1)\n"); exit(EXIT_FAILURE);
                }
                break;
            case 'A':
                args.pll_bw = atof(optarg);
                if (args.pll_bw <= 0.0f || args.pll_bw >= 1.0f) {
                    fprintf(stderr, "pll-bw must be in (0, 1)\n"); exit(EXIT_FAILURE);
                }
                break;
            case 'O': args.freq_offset_hz = atof(optarg); break;
            case 'c':
                if      (strcmp(optarg, "gray")    == 0) args.mapping = MAP_GRAY;
                else if (strcmp(optarg, "natural") == 0) args.mapping = MAP_NATURAL;
                else {
                    fprintf(stderr, "unknown coding '%s': use natural or gray\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 't':
                if      (strcmp(optarg, "bits")    == 0) args.content = OUT_BITS;
                else if (strcmp(optarg, "symbols") == 0) args.content = OUT_SYMBOLS;
                else {
                    fprintf(stderr, "unknown type '%s': use bits or symbols\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'f':
                if      (strcmp(optarg, "binary") == 0) args.format = FMT_BINARY;
                else if (strcmp(optarg, "ascii")  == 0) args.format = FMT_ASCII;
                else {
                    fprintf(stderr, "unknown format '%s': use binary or ascii\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'o': args.output_filename = optarg; break;
            case 'L': args.lna_gain = atoi(optarg); break;
            case 'V': args.vga_gain = atoi(optarg); break;
            case 'm':
                if      (strcmp(optarg, "async") == 0) args.receiver_mode = receiver_mode_async;
                else if (strcmp(optarg, "sync")  == 0) args.receiver_mode = receiver_mode_sync;
                else {
                    fprintf(stderr, "unknown mode '%s': use sync or async\n", optarg);
                    exit(EXIT_FAILURE);
                }
                break;
            case 'v': args.verbose = 1; break;
            case 'h': print_help(); exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "unknown option; try -h\n"); exit(EXIT_FAILURE);
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
    args.samplerate      = DEFAULT_SAMPLERATE;
    args.symbol_rate     = DEFAULT_SYMBOL_RATE;
    args.M               = DEFAULT_M;
    args.k               = DEFAULT_SPS;
    args.rolloff         = DEFAULT_ROLLOFF;
    args.pll_bw          = DEFAULT_PLL_BW;
    args.freq_offset_hz  = 0.0f;
    args.lna_gain        = -1;
    args.vga_gain        = -1;
    args.receiver_mode   = receiver_mode_sync;
    args.output_filename = NULL;
    args.mapping = MAP_NATURAL;
    args.content = OUT_BITS;
    args.format  = FMT_BINARY;

    parse_args(argc, argv);

    struct sigaction sa;
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    if (args.output_filename) {
        ctx.output = fopen(args.output_filename, "wb");
        if (!ctx.output) {
            fprintf(stderr, "cannot open '%s': %s\n",
                    args.output_filename, strerror(errno));
            return EXIT_FAILURE;
        }
    } else {
        ctx.output = stdout;
    }

    ctx.bits_per_symbol = __builtin_ctz((unsigned)args.M);

    ctx.resamp_rate = (float)(args.symbol_rate * args.k) / (float)args.samplerate;

    fprintf(stderr, "effective iteration rate: %.0f iter/s  (block=%d, ratio=%.5f)\n",
            (double)args.samplerate / INPUT_BLOCK_SIZE, INPUT_BLOCK_SIZE, ctx.resamp_rate);

    /* rational resampler: samplerate → symbol_rate × k */
    ctx.resampler = msresamp_crcf_create(ctx.resamp_rate, 60.0f);

    /* symbol timing recovery: Gardner TED + polyphase RRC bank */
    ctx.symsync = symsync_crcf_create_rnyquist(LIQUID_FIRFILT_RRC,
                                               (unsigned int)args.k,
                                               SYMSYNC_FILT_DELAY,
                                               args.rolloff,
                                               SYMSYNC_NPFB);

    /* carrier recovery: Costas loop NCO */
    ctx.nco = nco_crcf_create(LIQUID_NCO);
    nco_crcf_pll_set_bandwidth(ctx.nco, args.pll_bw);

    /* PSK hard-decision demodulator */
    ctx.modem = modemcf_create(m_to_psk(args.M));

    /* optional DDC */
    ctx.use_ddc = (args.freq_offset_hz != 0.0f);
    if (ctx.use_ddc) {
        float norm_shift = -args.freq_offset_hz / (float)args.samplerate;
        ctx.ddc       = shift_unroll_init(norm_shift, INPUT_BLOCK_SIZE);
        ctx.ddc_phase = 0.0f;
    }

    ctx.iq_buf = malloc(sizeof(liquid_float_complex) * INPUT_BLOCK_SIZE);
    if (!ctx.iq_buf) { fprintf(stderr, "malloc iq_buf failed\n"); return EXIT_FAILURE; }

    ctx.resamp_buf_size = (size_t)(INPUT_BLOCK_SIZE * fmaxf(ctx.resamp_rate, 1.0f) * 2.0f)
                          + (size_t)args.k + 64;
    ctx.resamp_buf = malloc(sizeof(liquid_float_complex) * ctx.resamp_buf_size);
    if (!ctx.resamp_buf) { fprintf(stderr, "malloc resamp_buf failed\n"); return EXIT_FAILURE; }

    /* symsync outputs ~1 symbol per k input samples */
    ctx.sync_buf_size = ctx.resamp_buf_size / (size_t)args.k + 64;
    ctx.sync_buf = malloc(sizeof(liquid_float_complex) * ctx.sync_buf_size);
    if (!ctx.sync_buf) { fprintf(stderr, "malloc sync_buf failed\n"); return EXIT_FAILURE; }

    fprintf(stderr, "initializing fobos...\n");
    if (receiver_init(FOBOS_BUFFER_SIZE, FOBOS_BUFFER_COUNT) != 0) {
        fprintf(stderr, "failed to open fobos device\n");
        return EXIT_FAILURE;
    }

    if (args.lna_gain >= 0) receiver_set_lna((uint8_t)args.lna_gain);
    if (args.vga_gain >= 0) receiver_set_vga((uint8_t)args.vga_gain);

    receiver_set_freq(args.frequency);
    receiver_set_samplerate((uint32_t)args.samplerate);

    if (args.verbose) dump_info();

    ctx.run = true;
    ctx.start_time = ctx.last_report = time(NULL);

    receiver_start(args.receiver_mode);

    while (ctx.run) {
        /* ── 1. read IQ block ─────────────────────────────────────────── */
        if (receiver_read(ctx.iq_buf, INPUT_BLOCK_SIZE) != INPUT_BLOCK_SIZE) break;
        ctx.samples_total += INPUT_BLOCK_SIZE;

        /* ── 2. DDC (optional) ──────────────────────────────────────── */
        if (ctx.use_ddc) {
            ctx.ddc_phase = shift_unroll_process(
                ctx.iq_buf, data_format_f32,
                (complexf *)ctx.iq_buf,
                INPUT_BLOCK_SIZE, &ctx.ddc, ctx.ddc_phase);
        }

        /* ── 3. resample to symbol_rate × k ─────────────────────────── */
        unsigned int n_resamp = 0;
        msresamp_crcf_execute(ctx.resampler, ctx.iq_buf, INPUT_BLOCK_SIZE,
                              ctx.resamp_buf, &n_resamp);

        /* ── 4. symbol timing recovery ───────────────────────────────── */
        unsigned int n_sync = 0;
        symsync_crcf_execute(ctx.symsync, ctx.resamp_buf, n_resamp,
                             ctx.sync_buf, &n_sync);

        /* ── 5. Costas PLL + PSK demodulate ─────────────────────────── */
        for (unsigned int i = 0; i < n_sync; i++) {
            /* rotate by NCO to correct carrier phase */
            liquid_float_complex sym;
            nco_crcf_mix_down(ctx.nco, ctx.sync_buf[i], &sym);

            /* hard-decision demodulate */
            unsigned int s;
            modemcf_demodulate(ctx.modem, sym, &s);

            /* Costas loop: phase error drives NCO */
            float phase_err = modemcf_get_demodulator_phase_error(ctx.modem);
            nco_crcf_pll_step(ctx.nco, phase_err);
            nco_crcf_step(ctx.nco);

            ctx.symbols_total++;

            /* ── 6. symbol mapping ─────────────────────────────────── */
            unsigned int val = (args.mapping == MAP_GRAY) ? gray_to_natural(s) : s;

            /* ── 7. output ─────────────────────────────────────────── */
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

        /* ── 8. verbose statistics ────────────────────────────────────── */
        if (args.verbose) {
            time_t now = time(NULL);
            if (now - ctx.last_report >= 2) {
                long elapsed = (long)(now - ctx.start_time);
                float nco_hz = nco_crcf_get_frequency(ctx.nco)
                               * (float)args.symbol_rate / (2.0f * (float)M_PI);
                fprintf(stderr, "[+%lds] samples: %zu  symbols: %zu  %s out: %zu  "
                        "NCO freq: %+.1f Hz\n",
                        elapsed, ctx.samples_total, ctx.symbols_total,
                        args.content == OUT_SYMBOLS ? "symbols" : "bits",
                        ctx.units_written, nco_hz);
                ctx.last_report = now;
            }
        }
    }

    /* ── shutdown ──────────────────────────────────────────────────────── */
    receiver_stop();
    receiver_close();

    modemcf_destroy(ctx.modem);
    symsync_crcf_destroy(ctx.symsync);
    nco_crcf_destroy(ctx.nco);
    msresamp_crcf_destroy(ctx.resampler);

    if (ctx.use_ddc) {
        free(ctx.ddc.dsin);
        free(ctx.ddc.dcos);
    }
    free(ctx.iq_buf);
    free(ctx.resamp_buf);
    free(ctx.sync_buf);

    if (ctx.output != stdout) fclose(ctx.output);

    if (args.verbose) {
        long elapsed = (long)(time(NULL) - ctx.start_time);
        fprintf(stderr, "done: %zu samples, %zu symbols, %zu %s out in %lds\n",
                ctx.samples_total, ctx.symbols_total, ctx.units_written,
                args.content == OUT_SYMBOLS ? "symbols" : "bits", elapsed);
    }

    return 0;
}
