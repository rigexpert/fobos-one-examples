/*
 * capture.c — Raw IQ capture from Fobos SDR to stdout.
 *
 * Tunes to FREQ_HZ at RATE_HZ, captures N_SAMPLES IQ samples and writes
 * them as raw float32 interleaved I/Q to stdout.
 *
 * Usage: capture FREQ_HZ RATE_HZ N_SAMPLES [LNA [VGA]]
 *   LNA default 0 (range 0..3:  0,1 = 0 dB, 2 = +16 dB, 3 = +33 dB)
 *   VGA default 0 (range 0..31: 0..+62 dB, 2 dB step)
 */
#include "fobos.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>

/* fobos_sdr_start_sync sets USB transfer_buf_size = buf_length*4 bytes,
 * but the wrapper's internal device-read buffer is only 65536*8 bytes.
 * Always pass CHUNK <= 65536 to avoid heap overflow in convert_all(). */
#define CHUNK 65536

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s FREQ_HZ RATE_HZ N_SAMPLES [LNA [VGA]]\n", argv[0]);
        return 1;
    }
    uint64_t freq = (uint64_t)atof(argv[1]);
    int      rate = (int)atof(argv[2]);
    int      n    = atoi(argv[3]);
    int      lna  = argc > 4 ? atoi(argv[4]) : 0;
    int      vga  = argc > 5 ? atoi(argv[5]) : 0;

    /* Save real stdout for binary output; redirect stdout to stderr so that
     * the fobos wrapper's printf() diagnostics do not corrupt the IQ stream. */
    int out_fd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);

    fobos_device_t *dev = fobos_open_by_idx(0);
    if (!dev) {
        fprintf(stderr, "fobos_open_by_idx failed\n");
        return 1;
    }

    fobos_set_frequency(dev, freq);
    fobos_set_samplerate(dev, rate);
    fobos_set_lna_gain(dev, lna);
    fobos_set_vga_gain(dev, vga);

    float *buf = malloc((size_t)n * 2 * sizeof(float));
    if (!buf) { fobos_close(dev); return 1; }

    fobos_start_sync(dev, CHUNK);

    /* discard one chunk for AGC/PLL settle */
    float *tmp = malloc((size_t)CHUNK * 2 * sizeof(float));
    if (!tmp) { fobos_stop_sync(dev); fobos_close(dev); free(buf); return 1; }
    fobos_read_samples_sync(dev, tmp, CHUNK);
    free(tmp);

    /* read n samples in CHUNK-sized blocks */
    int total = 0;
    while (total < n) {
        int want = n - total;
        if (want > CHUNK) want = CHUNK;
        int got = fobos_read_samples_sync(dev, buf + (size_t)total * 2, want);
        if (got <= 0) {
            fprintf(stderr, "read error at sample %d\n", total);
            break;
        }
        total += got;
    }
    fobos_stop_sync(dev);
    fobos_close(dev);

    /* write binary IQ to real stdout */
    FILE *out = fdopen(out_fd, "wb");
    fwrite(buf, sizeof(float), (size_t)total * 2, out);
    fclose(out);

    free(buf);
    return 0;
}
