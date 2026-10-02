/**
 * @file spectrum_png.c
 * @brief Save a 1-D FFT spectrum snapshot as a PNG image or CSV file.
 *
 * @section png_width PNG width management
 *
 * A 256-step × 8192-point scan produces ~1.7 M panorama bins.  At one pixel
 * per bin and 384 rows, the raw (uncompressed) pixel data would be ~1.9 GB —
 * enough to OOM a Raspberry Pi and exceed what most image viewers accept.
 *
 * spectrum_png_write() caps the output at @p max_px_width pixels (default
 * SPECTRUM_PNG_MAX_W = 4096).  When the actual bin count exceeds the cap, bins
 * are peak-compressed into the target width using the same algorithm as
 * draw_fft(): each output column maps to the peak of all source bins that fall
 * within its frequency range.  A diagnostic message is printed so the user
 * knows compression happened.
 *
 * At 4096 px the compressed PNG is ~4–8 kB on disk (libpng deflate), loads
 * instantly in any viewer, and retains full peak visibility.
 *
 * @section image_format Image format
 *
 * Width:  min(n_bins, max_px_width)
 * Height: SPECTRUM_PNG_IMG_H (384 px)
 * Colour: blue(low) → cyan(mid) → yellow(high) gradient bars on a dark background.
 *         The topmost pixel of each bar is white for a crisp peak line.
 */

#include "spectrum_png.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <png.h>

/* ------------------------------------------------------------------ colour */

/* 3-key gradient: t=0 → blue, t=0.5 → cyan, t=1 → yellow */
static void bar_color(float t, uint8_t *r, uint8_t *g, uint8_t *b) {
    if (t <= 0.0f) {
        *r =   0;
        *g =   0;
        *b = 200;
        return;
    }
    if (t >= 1.0f) {
        *r = 255;
        *g = 255;
        *b =   0;
        return;
    }
    if (t < 0.5f) {
        float u = t * 2.0f;
        *r = 0;
        *g = (uint8_t)(u * 255.0f + 0.5f);
        *b = 255;
    } else {
        float u = (t - 0.5f) * 2.0f;
        *r = (uint8_t)(u * 255.0f + 0.5f);
        *g = 255;
        *b = (uint8_t)((1.0f - u) * 255.0f + 0.5f);
    }
}

/* ------------------------------------------------------------------ helpers */

/* Peak-preserving bin compression: map n_src bins → n_dst columns. */
static void compress_peak(const float *src, int n_src,
                           float *dst, int n_dst) {
    double scale = (double)n_src / n_dst;
    for (int x = 0; x < n_dst; x++) {
        int start = (int)(x * scale);
        int end   = (int)((x + 1) * scale);
        if (end > n_src)
            end = n_src;
        if (start >= end)
            start = end - 1;

        float peak = src[start];
        for (int i = start + 1; i < end; i++)
            if (isfinite(src[i]) && src[i] > peak)
                peak = src[i];
        dst[x] = peak;
    }
}

/* ================================================================= public API */

int spectrum_png_write(const char *filename,
                       const float *spec, int n_bins,
                       float min_db, float max_db,
                       double freq_from_hz, double freq_to_hz,
                       int max_px_width) {
    (void)freq_from_hz; (void)freq_to_hz;   /* reserved for future axis annotation */

    if (!filename || !spec || n_bins <= 0 || min_db >= max_db) {
        fprintf(stderr, "spectrum_png_write: invalid arguments\n");
        return -1;
    }

    if (max_px_width <= 0)
        max_px_width = SPECTRUM_PNG_MAX_W;
    int img_w = (n_bins < max_px_width) ? n_bins : max_px_width;
    int img_h = SPECTRUM_PNG_IMG_H;

    /* Build working buffer: compress if needed. */
    float *work = malloc((size_t)img_w * sizeof(float));
    if (!work) {
        fprintf(stderr, "spectrum_png_write: OOM\n");
        return -1;
    }

    if (img_w == n_bins) {
        memcpy(work, spec, (size_t)n_bins * sizeof(float));
    } else {
        compress_peak(spec, n_bins, work, img_w);
        printf("  note: %d bins compressed to %d px (peak-preserving)\n",
               n_bins, img_w);
    }

    /* Normalise to [0, 1], treating non-finite as min (bar at zero). */
    float db_range = max_db - min_db;
    float *norm = malloc((size_t)img_w * sizeof(float));
    if (!norm) {
        free(work);
        return -1;
    }
    for (int x = 0; x < img_w; x++) {
        float v = work[x];
        if (!isfinite(v))
            v = min_db;
        v = (v < min_db) ? min_db : (v > max_db) ? max_db : v;
        norm[x] = (v - min_db) / db_range;
    }
    free(work);

    /* Open file and create libpng structs. */
    FILE *fp = fopen(filename, "wb");
    if (!fp) {
        fprintf(stderr, "spectrum_png_write: cannot open %s\n", filename);
        free(norm);
        return -1;
    }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              NULL, NULL, NULL);
    if (!png) {
        fclose(fp);
        free(norm);
        return -1;
    }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, NULL);
        fclose(fp);
        free(norm);
        return -1;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        free(norm);
        return -1;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info,
                 (png_uint_32)img_w, (png_uint_32)img_h,
                 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT,
                 PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    /* Row buffer */
    uint8_t *row = malloc((size_t)img_w * 3);
    if (!row) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        free(norm);
        return -1;
    }

    /* Render: y=0 is top of image (= max_db), y=img_h-1 is bottom (= min_db). */
    static const uint8_t BG_R = 10, BG_G = 10, BG_B = 24;

    for (int y = 0; y < img_h; y++) {
        for (int x = 0; x < img_w; x++) {
            /* bar_top_row: first row (from top) that is inside the bar.
             * bar fills rows [bar_top_row .. img_h-1]. */
            int bar_top_row = (norm[x] <= 0.0f)
                              ? img_h        /* no bar */
                              : (int)roundf((1.0f - norm[x]) * (img_h - 1));

            if (y < bar_top_row) {
                /* Above bar: background */
                row[x * 3 + 0] = BG_R;
                row[x * 3 + 1] = BG_G;
                row[x * 3 + 2] = BG_B;
            } else if (y == bar_top_row) {
                /* Top pixel of bar: white peak line */
                row[x * 3 + 0] = 255;
                row[x * 3 + 1] = 255;
                row[x * 3 + 2] = 255;
            } else {
                /* Interior of bar: colour by position within bar.
                 * t=1 near top (bright/hot), t=0 at bottom (dark/cold). */
                float t = (norm[x] > 0.0f)
                    ? (float)(img_h - 1 - y) / ((float)(img_h - 1) * norm[x])
                    : 0.0f;
                uint8_t r, g, b;
                bar_color(t, &r, &g, &b);
                row[x * 3 + 0] = r;
                row[x * 3 + 1] = g;
                row[x * 3 + 2] = b;
            }
        }
        png_write_row(png, row);
    }

    free(row);
    free(norm);
    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    printf("saved %s  (%d × %d px)\n", filename, img_w, img_h);
    return 0;
}

int spectrum_csv_write(const char *filename,
                       const float *spec, int n_bins,
                       double freq_from_hz, double freq_to_hz) {
    if (!filename || !spec || n_bins <= 0)
        return -1;

    FILE *fp = fopen(filename, "w");
    if (!fp) {
        fprintf(stderr, "spectrum_csv_write: cannot open %s\n", filename);
        return -1;
    }

    fprintf(fp, "freq_hz,dbfs\n");

    double hz_per_bin = (n_bins > 1)
                        ? (freq_to_hz - freq_from_hz) / (n_bins - 1)
                        : 0.0;

    for (int i = 0; i < n_bins; i++) {
        double f = freq_from_hz + hz_per_bin * i;
        float  v = spec[i];
        if (isfinite(v))
            fprintf(fp, "%.1f,%.2f\n", f, v);
        else
            fprintf(fp, "%.1f,-inf\n", f);
    }

    fclose(fp);
    printf("saved %s  (%d bins, %.3f–%.3f MHz)\n",
           filename, n_bins,
           freq_from_hz / 1e6, freq_to_hz / 1e6);
    return 0;
}
