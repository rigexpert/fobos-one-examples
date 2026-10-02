#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <png.h>

#include "sgram.h"

/* ---- 5×7 bitmap font (Adafruit GFX classic, MIT license) ----------------
 * 95 glyphs for ASCII 0x20–0x7E.  Each entry = 5 column bytes.
 * Bit 0 of each byte = topmost pixel row; bit 6 = bottommost.
 * A 6th blank column is implied (provides inter-character spacing). */
static const uint8_t font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 0x20 ' '  */
    {0x00,0x00,0x5F,0x00,0x00}, /* 0x21 '!'  */
    {0x00,0x07,0x00,0x07,0x00}, /* 0x22 '"'  */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 0x23 '#'  */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 0x24 '$'  */
    {0x23,0x13,0x08,0x64,0x62}, /* 0x25 '%'  */
    {0x36,0x49,0x55,0x22,0x50}, /* 0x26 '&'  */
    {0x00,0x05,0x03,0x00,0x00}, /* 0x27 '\'' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 0x28 '('  */
    {0x00,0x41,0x22,0x1C,0x00}, /* 0x29 ')'  */
    {0x14,0x08,0x3E,0x08,0x14}, /* 0x2A '*'  */
    {0x08,0x08,0x3E,0x08,0x08}, /* 0x2B '+'  */
    {0x00,0x50,0x30,0x00,0x00}, /* 0x2C ','  */
    {0x08,0x08,0x08,0x08,0x08}, /* 0x2D '-'  */
    {0x00,0x60,0x60,0x00,0x00}, /* 0x2E '.'  */
    {0x20,0x10,0x08,0x04,0x02}, /* 0x2F '/'  */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0x30 '0'  */
    {0x00,0x42,0x7F,0x40,0x00}, /* 0x31 '1'  */
    {0x42,0x61,0x51,0x49,0x46}, /* 0x32 '2'  */
    {0x21,0x41,0x45,0x4B,0x31}, /* 0x33 '3'  */
    {0x18,0x14,0x12,0x7F,0x10}, /* 0x34 '4'  */
    {0x27,0x45,0x45,0x45,0x39}, /* 0x35 '5'  */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 0x36 '6'  */
    {0x01,0x71,0x09,0x05,0x03}, /* 0x37 '7'  */
    {0x36,0x49,0x49,0x49,0x36}, /* 0x38 '8'  */
    {0x06,0x49,0x49,0x29,0x1E}, /* 0x39 '9'  */
    {0x00,0x36,0x36,0x00,0x00}, /* 0x3A ':'  */
    {0x00,0x56,0x36,0x00,0x00}, /* 0x3B ';'  */
    {0x08,0x14,0x22,0x41,0x00}, /* 0x3C '<'  */
    {0x14,0x14,0x14,0x14,0x14}, /* 0x3D '='  */
    {0x00,0x41,0x22,0x14,0x08}, /* 0x3E '>'  */
    {0x02,0x01,0x51,0x09,0x06}, /* 0x3F '?'  */
    {0x32,0x49,0x79,0x41,0x3E}, /* 0x40 '@'  */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 0x41 'A'  */
    {0x7F,0x49,0x49,0x49,0x36}, /* 0x42 'B'  */
    {0x3E,0x41,0x41,0x41,0x22}, /* 0x43 'C'  */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 0x44 'D'  */
    {0x7F,0x49,0x49,0x49,0x41}, /* 0x45 'E'  */
    {0x7F,0x09,0x09,0x09,0x01}, /* 0x46 'F'  */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 0x47 'G'  */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 0x48 'H'  */
    {0x00,0x41,0x7F,0x41,0x00}, /* 0x49 'I'  */
    {0x20,0x40,0x41,0x3F,0x01}, /* 0x4A 'J'  */
    {0x7F,0x08,0x14,0x22,0x41}, /* 0x4B 'K'  */
    {0x7F,0x40,0x40,0x40,0x40}, /* 0x4C 'L'  */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 0x4D 'M'  */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 0x4E 'N'  */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 0x4F 'O'  */
    {0x7F,0x09,0x09,0x09,0x06}, /* 0x50 'P'  */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 0x51 'Q'  */
    {0x7F,0x09,0x19,0x29,0x46}, /* 0x52 'R'  */
    {0x46,0x49,0x49,0x49,0x31}, /* 0x53 'S'  */
    {0x01,0x01,0x7F,0x01,0x01}, /* 0x54 'T'  */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 0x55 'U'  */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 0x56 'V'  */
    {0x3F,0x40,0x38,0x40,0x3F}, /* 0x57 'W'  */
    {0x63,0x14,0x08,0x14,0x63}, /* 0x58 'X'  */
    {0x07,0x08,0x70,0x08,0x07}, /* 0x59 'Y'  */
    {0x61,0x51,0x49,0x45,0x43}, /* 0x5A 'Z'  */
    {0x00,0x7F,0x41,0x41,0x00}, /* 0x5B '['  */
    {0x02,0x04,0x08,0x10,0x20}, /* 0x5C '\\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* 0x5D ']'  */
    {0x04,0x02,0x01,0x02,0x04}, /* 0x5E '^'  */
    {0x40,0x40,0x40,0x40,0x40}, /* 0x5F '_'  */
    {0x00,0x01,0x02,0x04,0x00}, /* 0x60 '`'  */
    {0x20,0x54,0x54,0x54,0x78}, /* 0x61 'a'  */
    {0x7F,0x44,0x44,0x44,0x38}, /* 0x62 'b'  */
    {0x38,0x44,0x44,0x44,0x20}, /* 0x63 'c'  */
    {0x38,0x44,0x44,0x44,0x7F}, /* 0x64 'd'  */
    {0x38,0x54,0x54,0x54,0x18}, /* 0x65 'e'  */
    {0x08,0x7E,0x09,0x01,0x02}, /* 0x66 'f'  */
    {0x08,0x54,0x54,0x54,0x3C}, /* 0x67 'g'  */
    {0x7F,0x08,0x04,0x04,0x78}, /* 0x68 'h'  */
    {0x00,0x44,0x7D,0x40,0x00}, /* 0x69 'i'  */
    {0x20,0x40,0x44,0x3D,0x00}, /* 0x6A 'j'  */
    {0x7F,0x10,0x28,0x44,0x00}, /* 0x6B 'k'  */
    {0x00,0x41,0x7F,0x40,0x00}, /* 0x6C 'l'  */
    {0x7C,0x04,0x18,0x04,0x7C}, /* 0x6D 'm'  */
    {0x7C,0x08,0x04,0x04,0x78}, /* 0x6E 'n'  */
    {0x38,0x44,0x44,0x44,0x38}, /* 0x6F 'o'  */
    {0x7C,0x14,0x14,0x14,0x08}, /* 0x70 'p'  */
    {0x08,0x14,0x14,0x18,0x7C}, /* 0x71 'q'  */
    {0x7C,0x08,0x04,0x04,0x08}, /* 0x72 'r'  */
    {0x48,0x54,0x54,0x54,0x20}, /* 0x73 's'  */
    {0x04,0x3F,0x44,0x40,0x20}, /* 0x74 't'  */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 0x75 'u'  */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 0x76 'v'  */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 0x77 'w'  */
    {0x44,0x28,0x10,0x28,0x44}, /* 0x78 'x'  */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 0x79 'y'  */
    {0x44,0x64,0x54,0x4C,0x44}, /* 0x7A 'z'  */
    {0x00,0x08,0x36,0x41,0x00}, /* 0x7B '{'  */
    {0x00,0x00,0x7F,0x00,0x00}, /* 0x7C '|'  */
    {0x00,0x41,0x36,0x08,0x00}, /* 0x7D '}'  */
    {0x08,0x08,0x2A,0x1C,0x08}, /* 0x7E '~'  */
};

/* Glyph dimensions including inter-character gap */
#define FONT_W  6
#define FONT_H  8

/* ---- Struct -------------------------------------------------------------- */

struct sgram_t {
    uint8_t       *pixels;       /* circular RGB pixel buffer */
    float         *row_times;    /* elapsed seconds from first row, per slot */
    long           t_start_ns;   /* CLOCK_MONOTONIC ns at first row */
    int            has_start;
    int            width;        /* panorama bins = image data width */
    int            capacity;     /* max rows in circular buffer */
    int            count;        /* total rows added */
    int            head;         /* next write slot */
    sgram_params_t params;
    int            has_params;
};

/* ---- Thermal colourmap --------------------------------------------------- */

static uint8_t cmap_r[256];
static uint8_t cmap_g[256];
static uint8_t cmap_b[256];

static void init_colormap(void) {
    static int done = 0;
    if (done)
        return;
    done = 1;

    static const struct { float t; uint8_t r, g, b; } keys[] = {
        { 0.00f,   0,   0,   0 },
        { 0.20f,   0,   0, 200 },
        { 0.40f,   0, 200, 220 },
        { 0.60f,   0, 200,   0 },
        { 0.80f, 220, 200,   0 },
        { 1.00f, 220,   0,   0 },
    };
    int nk = (int)(sizeof(keys) / sizeof(keys[0]));

    for (int i = 0; i < 256; i++) {
        float t = i / 255.0f;
        int k = 0;
        while (k < nk - 2 && keys[k + 1].t <= t)
        k++;
        float dt = (t - keys[k].t) / (keys[k + 1].t - keys[k].t);
        cmap_r[i] = (uint8_t)(keys[k].r + dt * ((int)keys[k+1].r - keys[k].r));
        cmap_g[i] = (uint8_t)(keys[k].g + dt * ((int)keys[k+1].g - keys[k].g));
        cmap_b[i] = (uint8_t)(keys[k].b + dt * ((int)keys[k+1].b - keys[k].b));
    }
}

/* ---- Public API ---------------------------------------------------------- */

sgram_t *sgram_create(int width, size_t mem_limit_bytes) {
    init_colormap();

    size_t row_bytes = (size_t)width * 3;
    int capacity = (int)(mem_limit_bytes / row_bytes);
    if (capacity < 1)
        capacity = 1;

    sgram_t *sg = calloc(1, sizeof(*sg));
    if (!sg)
        return NULL;

    sg->pixels    = malloc(row_bytes * (size_t)capacity);
    sg->row_times = malloc(sizeof(float) * (size_t)capacity);
    if (!sg->pixels || !sg->row_times) {
        free(sg->pixels);
        free(sg->row_times);
        free(sg);
        return NULL;
    }

    sg->width    = width;
    sg->capacity = capacity;
    return sg;
}

void sgram_set_params(sgram_t *sg, const sgram_params_t *p) {
    if (!sg || !p)
        return;
    sg->params     = *p;
    sg->has_params = 1;
}

void sgram_add_row(sgram_t *sg, const float *panorama,
                   float min_db, float max_db) {
    if (!sg)
        return;

    /* Record wall-clock timestamp */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    long now_ns = (long)ts.tv_sec * 1000000000L + ts.tv_nsec;
    if (!sg->has_start) {
        sg->t_start_ns = now_ns;
        sg->has_start = 1;
    }
    sg->row_times[sg->head] = (float)((now_ns - sg->t_start_ns) * 1e-9);

    /* Colourise panorama into pixel row */
    float range = max_db - min_db;
    if (range < 1e-6f)
        range = 1e-6f;

    uint8_t *row = sg->pixels + (size_t)sg->head * sg->width * 3;
    for (int i = 0; i < sg->width; i++) {
        float t = (panorama[i] - min_db) / range;
        if (t < 0.0f)
            t = 0.0f;
        if (t > 1.0f)
            t = 1.0f;
        int idx = (int)(t * 255.0f + 0.5f);
        row[i * 3 + 0] = cmap_r[idx];
        row[i * 3 + 1] = cmap_g[idx];
        row[i * 3 + 2] = cmap_b[idx];
    }

    sg->head = (sg->head + 1) % sg->capacity;
    sg->count++;
}

int sgram_row_count(const sgram_t *sg) {
    if (!sg)
        return 0;
    return sg->count < sg->capacity ? sg->count : sg->capacity;
}

/* ---- Image drawing helpers ----------------------------------------------- */

static inline void px(uint8_t *img, int img_w, int img_h,
                      int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || y < 0 || x >= img_w || y >= img_h)
        return;
    uint8_t *p = img + ((size_t)y * img_w + x) * 3;
    p[0] = r;
    p[1] = g;
    p[2] = b;
}

static void hline(uint8_t *img, int img_w, int img_h,
                  int x, int y, int len, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < len; i++)
        px(img, img_w, img_h, x + i, y, r, g, b);
}

static void vline(uint8_t *img, int img_w, int img_h,
                  int x, int y, int len, uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < len; i++)
        px(img, img_w, img_h, x, y + i, r, g, b);
}

static void fill_rect(uint8_t *img, int img_w, int img_h,
                      int x, int y, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b) {
    for (int row = 0; row < h; row++)
        hline(img, img_w, img_h, x, y + row, w, r, g, b);
}

static void draw_char(uint8_t *img, int img_w, int img_h,
                      int x, int y, char c,
                      uint8_t r, uint8_t g, uint8_t b) {
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        c = ' ';
    const uint8_t *glyph = font5x7[(unsigned char)c - 0x20];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                px(img, img_w, img_h, x + col, y + row, r, g, b);
    }
}

static void draw_str(uint8_t *img, int img_w, int img_h,
                     int x, int y, const char *s,
                     uint8_t r, uint8_t g, uint8_t b) {
    while (*s) {
        draw_char(img, img_w, img_h, x, y, *s++, r, g, b);
        x += FONT_W;
    }
}

static int str_w(const char *s) { return (int)strlen(s) * FONT_W; }

/* ---- Axis helpers -------------------------------------------------------- */

static double nice_step(double x) {
    if (x <= 0)
        return 1.0;
    double p = pow(10.0, floor(log10(x)));
    double r = x / p;
    if (r < 2.0)
        return p;
    if (r < 5.0)
        return 2.0 * p;
    return 5.0 * p;
}

static void fmt_freq(char *buf, size_t bufsz, double hz) {
    if (hz >= 1e9)
        snprintf(buf, bufsz, "%.3f GHz", hz / 1e9);
    else if (hz >= 1e6)
        snprintf(buf, bufsz, "%.3f MHz", hz / 1e6);
    else if (hz >= 1e3)
        snprintf(buf, bufsz, "%.1f kHz", hz / 1e3);
    else
        snprintf(buf, bufsz, "%.0f Hz", hz);
}

/* Compact tick label — drops unnecessary trailing zeros. */
static void fmt_freq_tick(char *buf, size_t bufsz, double hz, double step_hz) {
    if (step_hz >= 1e9)
        snprintf(buf, bufsz, "%.4g GHz", hz / 1e9);
    else if (step_hz >= 1e6)
        snprintf(buf, bufsz, "%.4g MHz", hz / 1e6);
    else if (step_hz >= 1e3)
        snprintf(buf, bufsz, "%.4g kHz", hz / 1e3);
    else
        snprintf(buf, bufsz, "%.0f Hz",  hz);
}

static void fmt_time(char *buf, size_t bufsz, float s) {
    if (s < 60.0f)
        snprintf(buf, bufsz, "%.1fs", s);
    else if (s < 3600.0f)
        snprintf(buf, bufsz, "%dm%02ds", (int)s / 60, (int)s % 60);
    else
        snprintf(buf, bufsz, "%dh%02dm", (int)s / 3600, ((int)s % 3600) / 60);
}

/* ---- PNG write ----------------------------------------------------------- */

int sgram_write_png(const sgram_t *sg, const char *filename) {
    int n_rows = sgram_row_count(sg);
    if (!sg || n_rows == 0) {
        fprintf(stderr, "sgram: no data to write\n");
        return -1;
    }

    /* ---- Layout ---- */
    const int PAD      = 4;
    const int Y_W      = 8 * FONT_W + PAD * 2;   /* Y-axis label column */
    const int X_H      = 6 + FONT_H + PAD;        /* X-axis row height */
    const int TICK_LEN = 5;

    int hdr_lines = (sg->has_params && sg->params.overlap_hz > 0.0) ? 4 : 3;
    int HDR_H     = PAD + hdr_lines * (FONT_H + 2) + PAD;

    /* Colours */
    const uint8_t BG_R=16,  BG_G=16,  BG_B=24;
    const uint8_t HB_R=28,  HB_G=28,  HB_B=40;
    const uint8_t TXT_R=220,TXT_G=220,TXT_B=220;
    const uint8_t TCK_R=120,TCK_G=120,TCK_B=140;
    const uint8_t AXS_R=70, AXS_G=70, AXS_B=90;

    int img_w = Y_W + sg->width + PAD;
    int img_h = HDR_H + n_rows + X_H + PAD;

    /* ---- Canvas ---- */
    uint8_t *img = malloc((size_t)img_w * img_h * 3);
    if (!img) {
        fprintf(stderr, "sgram: out of memory for PNG canvas\n");
        return -1;
    }

    for (int i = 0; i < img_w * img_h; i++) {
        img[i*3+0] = BG_R;
        img[i*3+1] = BG_G;
        img[i*3+2] = BG_B;
    }
    fill_rect(img, img_w, img_h, 0, 0, img_w, HDR_H, HB_R, HB_G, HB_B);

    /* Data area origin */
    int dx = Y_W;
    int dy = HDR_H;

    /* ---- Copy spectrogram rows (newest = top) ---- */
    for (int i = 0; i < n_rows; i++) {
        int slot = (sg->head - 1 - i + 2 * sg->capacity) % sg->capacity;
        uint8_t *src = sg->pixels + (size_t)slot * sg->width * 3;
        uint8_t *dst = img + ((size_t)(dy + i) * img_w + dx) * 3;
        memcpy(dst, src, (size_t)sg->width * 3);
    }

    /* ---- Axis lines ---- */
    vline(img, img_w, img_h, dx - 1, dy, n_rows + TICK_LEN, AXS_R, AXS_G, AXS_B);
    hline(img, img_w, img_h, dx - 1, dy + n_rows, sg->width + 2, AXS_R, AXS_G, AXS_B);

    /* ---- Header ---- */
    if (sg->has_params) {
        const sgram_params_t *p = &sg->params;
        char line[256];
        int lx = PAD, ly = PAD;

        double bin_hz     = (double)p->rate / p->fft_width;
        double span_hz    = p->freq_to_hz - p->freq_from_hz;
        int bins_per_step = (p->n_steps > 0) ? sg->width / p->n_steps : sg->width;

        char f1[32], f2[32], fspan[32], bw[32];
        fmt_freq(f1,   sizeof(f1),    p->freq_from_hz);
        fmt_freq(f2,   sizeof(f2),    p->freq_to_hz);
        fmt_freq(fspan,sizeof(fspan), span_hz);
        fmt_freq(bw,   sizeof(bw),    bin_hz);

        snprintf(line, sizeof(line), "fft-scan  %s - %s  (span %s)", f1, f2, fspan);
        draw_str(img, img_w, img_h, lx, ly, line, TXT_R, TXT_G, TXT_B);
        ly += FONT_H + 2;

        snprintf(line, sizeof(line),
                 "rate %.1f MSPS  FFT %d bins  bin bw %s  avg %d",
                 p->rate / 1e6, p->fft_width, bw, p->average);
        draw_str(img, img_w, img_h, lx, ly, line, TXT_R, TXT_G, TXT_B);
        ly += FONT_H + 2;

        snprintf(line, sizeof(line),
                 "steps %d  total bins %d  bins/step %d",
                 p->n_steps, sg->width, bins_per_step);
        draw_str(img, img_w, img_h, lx, ly, line, TXT_R, TXT_G, TXT_B);
        ly += FONT_H + 2;

        if (p->overlap_hz > 0.0) {
            char ov[32];
            fmt_freq(ov, sizeof(ov), p->overlap_hz);
            snprintf(line, sizeof(line), "overlap %s per step boundary", ov);
            draw_str(img, img_w, img_h, lx, ly, line, TXT_R, TXT_G, TXT_B);
        }
    }

    /* ---- X axis (frequency) ---- */
    if (sg->has_params) {
        const sgram_params_t *p = &sg->params;
        double span  = p->freq_to_hz - p->freq_from_hz;
        double step  = nice_step(span / 8.0);
        double f0    = ceil(p->freq_from_hz / step) * step;
        int prev_end = -999;

        for (double f = f0; f <= p->freq_to_hz + step * 0.01; f += step) {
            int x = dx + (int)((f - p->freq_from_hz) / span * (sg->width - 1));
            if (x < dx || x >= dx + sg->width)
                continue;

            vline(img, img_w, img_h, x, dy + n_rows + 1, TICK_LEN, TCK_R, TCK_G, TCK_B);

            char lbl[32];
            fmt_freq_tick(lbl, sizeof(lbl), f, step);
            int lw = str_w(lbl);
            int lx = x - lw / 2;
            if (lx > prev_end + 2) {
                draw_str(img, img_w, img_h, lx,
                         dy + n_rows + TICK_LEN + 2, lbl,
                         TXT_R, TXT_G, TXT_B);
                prev_end = lx + lw;
            }
        }
    }

    /* ---- Y axis (time) ---- */
    if (sg->has_start && n_rows >= 2) {
        int slot_top = (sg->head - 1 + 2 * sg->capacity) % sg->capacity;
        int slot_bot = (sg->head - n_rows + 2 * sg->capacity) % sg->capacity;
        float t_top  = sg->row_times[slot_top];
        float t_bot  = sg->row_times[slot_bot];
        float t_span = t_top - t_bot;

        if (t_span > 0.01f) {
            double step  = nice_step((double)t_span / 6.0);
            /* Iterate newest→oldest (decreasing t → increasing y = top→bottom)
             * so the prev_bot overlap guard works correctly. */
            double t_last = floor((double)t_top / step) * step;
            int prev_bot  = -999;

            for (double t = t_last; t >= t_bot - step * 0.01; t -= step) {
                /* y=dy → t_top (newest top), y=dy+n_rows-1 → t_bot (oldest bottom) */
                int y = dy + (int)((t_top - t) / t_span * (n_rows - 1));
                if (y < dy || y >= dy + n_rows)
                    continue;

                hline(img, img_w, img_h, dx - TICK_LEN - 1, y, TICK_LEN, TCK_R, TCK_G, TCK_B);

                char lbl[32];
                fmt_time(lbl, sizeof(lbl), (float)t);
                int lw = str_w(lbl);
                int lx = dx - TICK_LEN - 2 - lw;
                int label_top = y - FONT_H / 2;
                if (label_top > prev_bot + 1 && lx >= 0) {
                    draw_str(img, img_w, img_h, lx, label_top, lbl,
                             TXT_R, TXT_G, TXT_B);
                    prev_bot = label_top + FONT_H;
                }
            }
        }
    }

    /* ---- Write PNG ---- */
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        perror(filename);
        free(img);
        return -1;
    }

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_infop   info = png ? png_create_info_struct(png) : NULL;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        if (png)
            png_destroy_write_struct(&png, &info);
        fclose(fp);
        free(img);
        return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)img_w, (png_uint_32)img_h,
                 8, PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (int y = 0; y < img_h; y++)
        png_write_row(png, img + (size_t)y * img_w * 3);

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    free(img);
    return 0;
}

void sgram_destroy(sgram_t *sg) {
    if (!sg)
        return;
    free(sg->pixels);
    free(sg->row_times);
    free(sg);
}
