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

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr, "Usage: %s FREQ_HZ RATE_HZ N_SAMPLES [LNA [VGA]]\n", argv[0]);
        return 1;
    }
    uint64_t freq    = (uint64_t)atof(argv[1]);
    int      rate    = (int)atof(argv[2]);
    int      n       = atoi(argv[3]);
    int      lna     = argc > 4 ? atoi(argv[4]) : 0;
    int      vga     = argc > 5 ? atoi(argv[5]) : 0;

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

    /* discard one buffer for AGC/PLL settle */
    fobos_start_sync(dev, n);
    fobos_read_samples_sync(dev, buf, n);
    /* measurement buffer */
    fobos_read_samples_sync(dev, buf, n);
    fobos_stop_sync(dev);

    fwrite(buf, sizeof(float), (size_t)n * 2, stdout);

    free(buf);
    fobos_close(dev);
    return 0;
}
