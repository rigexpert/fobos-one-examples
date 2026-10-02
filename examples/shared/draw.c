/**
 * @file draw.c
 * @brief Terminal spectrum display implementation.
 *
 * Renders a live FFT spectrum and frequency axis in the terminal using:
 *
 * **ANSI escape sequences** — cursor positioning (ANSI_CURSOR_ROW1_COL1),
 * line erase (ANSI_ERASE_LINE), and cursor visibility (ANSI_CURSOR_HIDE /
 * ANSI_CURSOR_SHOW) keep updates smooth without clearing the whole screen on
 * every frame.
 *
 * **Unicode block elements** (U+2581 – U+2588, ▁▂▃▄▅▆▇█) give BAR_EIGHTHS×
 * sub-character vertical resolution.  Each column's normalised height (0–1)
 * is multiplied by the display height in rows, and the fractional part selects
 * which block character fills the top partial cell.
 *
 * **Bin compression** — compress_bins() maps each screen column to the peak of
 * all FFT bins that fall within that column's frequency range.  Peak-preserving
 * compression ensures narrow signals remain visible even when many FFT bins
 * share a single terminal column.
 *
 * **Max hold** — draw_set_maxhold() registers a per-bin maximum buffer that is
 * rendered as a yellow BLOCK_LOWER_HALF tick one row above the live spectrum
 * bar.  Call draw_set_maxhold(NULL, 0) to disable it.
 *
 * **Keypress** — draw_enable_keypress() puts stdin into raw non-blocking mode
 * so single-character input can be polled via draw_check_keypress() without
 * blocking the processing loop.  Call draw_disable_keypress() before exit.
 *
 * **Frequency axis** — the x-axis step size is chosen from a list of "nice"
 * values (1k, 2k, 5k, 10k, …) such that labels are at least MIN_TICK_COLS
 * columns apart.  Label precision is adjusted automatically based on the step
 * size.  A no-overlap guard prevents adjacent labels from running together at
 * small terminal widths.
 *
 * **Header** — draw_header() centres the status line within the terminal width
 * and erases the line (ANSI_ERASE_LINE) before writing to prevent stale
 * characters when the terminal is resized.  It is redrawn on every draw_fft()
 * call so the MAX-HOLD indicator can appear and disappear dynamically.
 */

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>
#include <termios.h>

#include "draw.h"

/* ----------------------------------------------------------------- ANSI escapes */

#define ANSI_CLEAR_SCREEN       "\033[2J"
#define ANSI_CURSOR_HOME        "\033[H"
#define ANSI_CURSOR_HIDE        "\033[?25l"
#define ANSI_CURSOR_SHOW        "\033[?25h"
#define ANSI_CURSOR_ROW1_COL1   "\033[1;1H"
#define ANSI_CURSOR_ROW2_COL1   "\033[2;1H"
#define ANSI_ERASE_LINE         "\033[2K"
#define ANSI_ERASE_TO_END       "\033[J"
#define ANSI_COLOR_YELLOW       "\033[33m"
#define ANSI_COLOR_RESET        "\033[0m"

/* ----------------------------------------------------------------- Unicode block elements (UTF-8) */

#define BLOCK_EMPTY       " "              /* 0/8  — empty cell                      */
#define BLOCK_1_8         "\xe2\x96\x81"  /* U+2581 ▁ LOWER ONE EIGHTH BLOCK        */
#define BLOCK_2_8         "\xe2\x96\x82"  /* U+2582 ▂ LOWER ONE QUARTER BLOCK       */
#define BLOCK_3_8         "\xe2\x96\x83"  /* U+2583 ▃ LOWER THREE EIGHTHS BLOCK     */
#define BLOCK_4_8         "\xe2\x96\x84"  /* U+2584 ▄ LOWER HALF BLOCK              */
#define BLOCK_5_8         "\xe2\x96\x85"  /* U+2585 ▅ LOWER FIVE EIGHTHS BLOCK      */
#define BLOCK_6_8         "\xe2\x96\x86"  /* U+2586 ▆ LOWER THREE QUARTERS BLOCK    */
#define BLOCK_7_8         "\xe2\x96\x87"  /* U+2587 ▇ LOWER SEVEN EIGHTHS BLOCK     */
#define BLOCK_FULL        "\xe2\x96\x88"  /* U+2588 █ FULL BLOCK                    */
#define BLOCK_LOWER_HALF  BLOCK_4_8       /* U+2584 ▄ — max-hold tick marker        */

/* ----------------------------------------------------------------- Layout constants */

#define Y_LABEL_WIDTH           9       /* dB y-axis column width: " -100dB|"       */
#define Y_AXIS_LABEL_STEP       5       /* print a dB label every this many rows     */
#define SPECTRUM_MIN_WIDTH      8       /* minimum spectrum width in columns         */
#define SCREEN_HEIGHT_RESERVE   4       /* rows reserved: header + tick + label + spare */
#define BAR_EIGHTHS             8       /* sub-character vertical resolution         */
#define MIN_TICK_COLS           5       /* minimum columns between frequency labels  */
#define TICK_HALF_STEP          0.5     /* lookahead fraction of step_hz for last tick */
#define TERM_WIDTH_DEFAULT      80      /* fallback terminal width when ioctl fails  */
#define TERM_HEIGHT_DEFAULT     24      /* fallback terminal height when ioctl fails */
#define HEADER_BUF_SIZE         256     /* header status line buffer in bytes        */
#define LABEL_BUF_SIZE          16      /* frequency tick label buffer in bytes      */
#define OVERLAP_PCT_FACTOR      100     /* multiplier for overlap percentage display */

/* ----------------------------------------------------------------- Frequency constants (Hz) */

#define FREQ_1KHZ     1e3
#define FREQ_2KHZ     2e3
#define FREQ_5KHZ     5e3
#define FREQ_10KHZ    10e3
#define FREQ_25KHZ    25e3
#define FREQ_50KHZ    50e3
#define FREQ_100KHZ   100e3
#define FREQ_250KHZ   250e3
#define FREQ_500KHZ   500e3
#define FREQ_1MHZ     1e6
#define FREQ_2MHZ     2e6
#define FREQ_5MHZ     5e6
#define FREQ_10MHZ    10e6
#define FREQ_25MHZ    25e6
#define FREQ_50MHZ    50e6
#define FREQ_100MHZ   100e6
#define FREQ_200MHZ   200e6
#define FREQ_500MHZ   500e6
#define FREQ_1GHZ     1e9

/* ----------------------------------------------------------------- context */

typedef struct draw_ctx_t {
    uint64_t cf;
    uint64_t rate;
    float baselavel;
    int overlap;
    int avarage;
    uint32_t fft_width;
    float min_db;
    float max_db;
    int maxhold_active;   /* 1 when a max-hold buffer is registered */
} draw_ctx_t;

static draw_ctx_t draw_ctx;

/* Max-hold source buffer (owned by the caller, not draw.c). */
static const float *draw_mh_src  = NULL;
static int          draw_mh_n    = 0;

/* Raw-terminal state for keypress polling. */
static struct termios g_orig_termios;
static int            g_keypress_enabled = 0;

/* ----------------------------------------------------------------- forward decls */

static void get_terminal_size(int *width, int *height);
static void compress_bins(const float *fft_mag, int fft_size, float *out,
                           int width, float min_db, float max_db);
static void draw_spectrum(const float *mag, const float *mh,
                          size_t width, size_t height,
                          float min_db, float max_db);
static void draw_header(draw_ctx_t *ctx);
static void draw_reset_cursor(void);

/* ================================================================= public API */

void draw_init(uint64_t cf, uint64_t rate, float baselavel, int overlap,
               int avarage, uint32_t fft_width, float min_db, float max_db) {
    draw_ctx.cf           = cf;
    draw_ctx.rate         = rate;
    draw_ctx.baselavel    = baselavel;
    draw_ctx.overlap      = overlap;
    draw_ctx.avarage      = avarage;
    draw_ctx.fft_width    = fft_width;
    draw_ctx.min_db       = min_db;
    draw_ctx.max_db       = max_db;
    draw_ctx.maxhold_active = 0;

    draw_header(&draw_ctx);
}

void draw_fft(float *mag, size_t fft_bins, float min_db, float max_db) {
    int screen_width, screen_height;
    get_terminal_size(&screen_width, &screen_height);

    int spectrum_width = screen_width - Y_LABEL_WIDTH;
    if (spectrum_width < SPECTRUM_MIN_WIDTH)
        spectrum_width = SPECTRUM_MIN_WIDTH;

    float compressed[spectrum_width];
    compress_bins(mag, (int)fft_bins, compressed, spectrum_width, min_db, max_db);

    /* Compress max-hold buffer if present. */
    float mh_compressed[spectrum_width];
    const float *mh_ptr = NULL;
    if (draw_mh_src && draw_mh_n > 0) {
        compress_bins(draw_mh_src, draw_mh_n, mh_compressed,
                      spectrum_width, min_db, max_db);
        mh_ptr = mh_compressed;
    }

    /* Redraw header each frame so the MAX-HOLD indicator is live. */
    draw_header(&draw_ctx);
    draw_reset_cursor();
    draw_spectrum(compressed, mh_ptr,
                  spectrum_width, (size_t)(screen_height - SCREEN_HEIGHT_RESERVE),
                  min_db, max_db);
    fflush(stdout);
}

void draw_set_maxhold(const float *maxhold, int n_bins) {
    draw_mh_src              = maxhold;
    draw_mh_n                = n_bins;
    draw_ctx.maxhold_active  = (maxhold != NULL);
}

void draw_enable_keypress(void) {
    struct termios raw;
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    raw          = g_orig_termios;
    raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);
    raw.c_cc[VMIN]  = 0;   /* non-blocking: return immediately */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    g_keypress_enabled = 1;
}

void draw_disable_keypress(void) {
    if (g_keypress_enabled) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_keypress_enabled = 0;
    }
}

int draw_check_keypress(void) {
    if (!g_keypress_enabled)
        return -1;
    unsigned char c;
    if (read(STDIN_FILENO, &c, 1) == 1)
        return (int)c;
    return -1;
}

void draw_clear_screen(void) {
    printf(ANSI_CLEAR_SCREEN);
    printf(ANSI_CURSOR_HOME);
    fflush(stdout);
}

void draw_hide_cursor(void) {
    printf(ANSI_CURSOR_HIDE);
}

void draw_show_cursor(void) {
    printf(ANSI_CURSOR_SHOW);
}

/* ================================================================= internal */

static void draw_reset_cursor(void) {
    printf(ANSI_CURSOR_ROW2_COL1);
    printf(ANSI_ERASE_TO_END);
}

static void get_terminal_size(int *width, int *height) {
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == -1) {
        *width  = TERM_WIDTH_DEFAULT;
        *height = TERM_HEIGHT_DEFAULT;
        return;
    }
    *width  = w.ws_col;
    *height = w.ws_row;
}

/* Map each screen column to a peak of the FFT bins it covers, then
 * normalise to [0, 1].  Peak-preserving compression ensures signals are
 * visible even when many FFT bins compress into a single screen column. */
static void compress_bins(const float *fft_mag, int fft_size, float *out,
                           int width, float min_db, float max_db) {
    double scale    = (double)fft_size / width;
    float  db_range = max_db - min_db;

    for (int x = 0; x < width; x++) {
        int start = (int)(x * scale);
        int end   = (int)((x + 1) * scale);
        if (end > fft_size)
            end = fft_size;
        if (start >= end)
            start = end - 1;

        float peak = fft_mag[start];
        for (int i = start + 1; i < end; i++)
            if (fft_mag[i] > peak)
                peak = fft_mag[i];

        if (peak < min_db)
            peak = min_db;
        if (peak > max_db)
            peak = max_db;

        out[x] = (peak - min_db) / db_range;   /* normalised 0..1 */
    }
}

/* Draw spectrum with fractional-block vertical resolution and a dB y-axis.
 * Uses Unicode block elements for BAR_EIGHTHS× sub-character resolution.
 * @p mh is an optional normalised [0,1] max-hold buffer the same width as
 * @p mag; when non-NULL a yellow BLOCK_LOWER_HALF tick is drawn at the
 * max-hold level for each column where the live bar has not reached it. */
static void draw_spectrum(const float *mag, const float *mh,
                          size_t width, size_t height,
                          float min_db, float max_db) {
    (void)min_db; (void)max_db;   /* reserved for future per-column colouring */

    /* UTF-8 block elements: 0/8 … 8/8 */
    static const char * const vblocks[BAR_EIGHTHS + 1] = {
        BLOCK_EMPTY,
        BLOCK_1_8, BLOCK_2_8, BLOCK_3_8, BLOCK_4_8,
        BLOCK_5_8, BLOCK_6_8, BLOCK_7_8, BLOCK_FULL
    };

    float db_range   = draw_ctx.max_db - draw_ctx.min_db;
    float db_per_row = db_range / (float)height;

    /* draw top-down; row 'height' = top = max_db, row 1 = bottom = min_db */
    for (int row = (int)height; row >= 1; row--) {
        /* y-axis label: every Y_AXIS_LABEL_STEP rows, or first/last */
        float row_db = draw_ctx.max_db - (float)(height - row + 1) * db_per_row;
        if (row == (int)height || row == 1 || (height - (size_t)row) % Y_AXIS_LABEL_STEP == 0) {
            printf("%+6.0fdB|", row_db);
        } else {
            printf("%*s|", Y_LABEL_WIDTH - 1, "");
        }

        float row_bottom = (float)(row - 1);
        float row_top    = (float)row;

        for (size_t col = 0; col < width; col++) {
            float h    = mag[col] * (float)height;   /* live bar top in px */
            float mh_h = mh ? mh[col] * (float)height : -1.0f;

            if (h >= row_top) {
                /* fully inside the bar */
                printf(BLOCK_FULL);
            } else if (h > row_bottom) {
                /* partial fill: pick the right block character.
                 * If max hold also falls here the bar visually covers it. */
                int eighths = (int)roundf((h - row_bottom) * (float)BAR_EIGHTHS);
                if (eighths >= BAR_EIGHTHS)
                    eighths = BAR_EIGHTHS - 1;
                if (eighths <= 0)
                    eighths = 0;
                printf("%s", vblocks[eighths]);
            } else if (mh && mh_h >= row_bottom && mh_h < row_top) {
                /* max-hold falls in this row, bar below → yellow tick */
                printf(ANSI_COLOR_YELLOW "%s" ANSI_COLOR_RESET, BLOCK_LOWER_HALF);
            } else {
                printf(BLOCK_EMPTY);
            }
        }
        printf("\n");
    }

    /* --- frequency x-axis --- */
    double f_start    = (double)draw_ctx.cf - (double)draw_ctx.rate / 2.0;
    double f_end      = (double)draw_ctx.cf + (double)draw_ctx.rate / 2.0;
    double hz_per_col = (double)draw_ctx.rate / (double)width;

    /* Pick smallest "nice" step that places labels at least MIN_TICK_COLS apart */
    static const double nice_steps[] = {
        FREQ_1KHZ,   FREQ_2KHZ,   FREQ_5KHZ,   FREQ_10KHZ,
        FREQ_25KHZ,  FREQ_50KHZ,  FREQ_100KHZ,  FREQ_250KHZ,  FREQ_500KHZ,
        FREQ_1MHZ,   FREQ_2MHZ,   FREQ_5MHZ,   FREQ_10MHZ,
        FREQ_25MHZ,  FREQ_50MHZ,  FREQ_100MHZ,  FREQ_200MHZ,  FREQ_500MHZ
    };
    double min_step = (double)MIN_TICK_COLS * hz_per_col;
    double step_hz  = nice_steps[sizeof(nice_steps)/sizeof(nice_steps[0]) - 1];
    for (size_t s = 0; s < sizeof(nice_steps)/sizeof(nice_steps[0]); s++) {
        if (nice_steps[s] >= min_step) {
            step_hz = nice_steps[s];
            break;
        }
    }

    /* Pick label precision based on step size */
    const char *fmt;
    double scale_div;
    const char *unit;
    if (draw_ctx.cf >= (uint64_t)FREQ_1GHZ) {
        scale_div = FREQ_1GHZ; unit = "G";
        fmt = (step_hz < FREQ_1MHZ)  ? "%.3f%s" :
              (step_hz < FREQ_10MHZ) ? "%.2f%s" : "%.1f%s";
    } else {
        scale_div = FREQ_1MHZ; unit = "M";
        fmt = (step_hz < FREQ_100KHZ) ? "%.3f%s" :
              (step_hz < FREQ_1MHZ)   ? "%.2f%s" : "%.1f%s";
    }

    /* Tick mark row */
    char tick_row[width + 1];
    memset(tick_row, '-', width);
    tick_row[width] = '\0';
    double first_tick = ceil(f_start / step_hz) * step_hz;
    for (double f = first_tick; f <= f_end + step_hz * TICK_HALF_STEP; f += step_hz) {
        int col = (int)((f - f_start) / hz_per_col);
        if (col >= 0 && col < (int)width)
            tick_row[col] = '+';
    }
    printf("%*s+%s\n", Y_LABEL_WIDTH - 1, "", tick_row);

    /* Label row — centred on each tick, no-overlap guard */
    char label_row[width + 1];
    memset(label_row, ' ', width);
    label_row[width] = '\0';
    int last_label_end = -1;
    for (double f = first_tick; f <= f_end + step_hz * TICK_HALF_STEP; f += step_hz) {
        int col = (int)((f - f_start) / hz_per_col);
        if (col < 0 || col >= (int)width)
            continue;
        char lbl[LABEL_BUF_SIZE];
        snprintf(lbl, sizeof(lbl), fmt, f / scale_div, unit);
        int llen   = (int)strlen(lbl);
        int lstart = col - llen / 2;
        if (lstart < 0)
            lstart = 0;
        if (lstart + llen > (int)width)
            lstart = (int)width - llen;
        if (lstart <= last_label_end)
            continue;   /* would overlap */
        memcpy(label_row + lstart, lbl, (size_t)llen);
        last_label_end = lstart + llen;
    }
    printf("%*s %s\n", Y_LABEL_WIDTH - 1, "", label_row);
}

static void draw_header(draw_ctx_t *ctx) {
    int width, height;
    get_terminal_size(&width, &height);

    char buf[HEADER_BUF_SIZE];
    snprintf(buf, sizeof(buf),
        "CF: %lu Hz | SR: %lu S/s | Base: %.1f dB | Ovlp: %d%% | Avg: %d | FFT: %u%s",
        (unsigned long)ctx->cf, (unsigned long)ctx->rate, ctx->baselavel,
        ctx->fft_width ? ctx->overlap * OVERLAP_PCT_FACTOR / (int)ctx->fft_width : 0,
        ctx->avarage, ctx->fft_width,
        ctx->maxhold_active ? " | MAX-HOLD (c=clear)" : "");

    int len = (int)strlen(buf);
    int pad = (width - len) / 2;
    if (pad < 0)
        pad = 0;

    printf(ANSI_CURSOR_ROW1_COL1 ANSI_ERASE_LINE);   /* move to row 1, erase line */
    printf("%*s%s\n", pad, "", buf);
}
