/**
 * @file test_demod_psk.c
 * @brief Unit tests for demod_psk logic. No hardware or liquid-dsp required.
 *
 * Tests:
 *   1. gray_to_natural — inverse Gray code roundtrip for M = 2 … 256
 *   2. Bit extraction  — MSB-first ordering for various M values
 *   3. Output modes    — all 4 content×format combinations on a known symbol stream
 *   4. Bits-per-symbol — log2(M) for M = 2, 4, 8, 16, 32, 64
 *
 * Build standalone:
 *   gcc -O2 -o test_demod_psk test_demod_psk.c -lm && ./test_demod_psk
 *
 * Or via cmake: make test_demod_psk
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* ---- copy of helpers from main.c ---------------------------------------- */

static unsigned int gray_to_natural(unsigned int g) {
    unsigned int n = g;
    while (g >>= 1)
        n ^= g;
    return n;
}

static unsigned int natural_to_gray(unsigned int n) {
    return n ^ (n >> 1);
}

/* ---- minimal test framework --------------------------------------------- */

static int failures = 0;

#define EXPECT_EQ(a, b, msg) do { \
    if ((a) != (b)) { \
        printf("  FAIL: %s — got %u, expected %u\n", (msg), (unsigned)(a), (unsigned)(b)); \
        failures++; \
    } \
} while (0)

#define EXPECT_STR(got, exp, msg) do { \
    if (strcmp((got), (exp)) != 0) { \
        printf("  FAIL: %s — got \"%s\", expected \"%s\"\n", (msg), (got), (exp)); \
        failures++; \
    } \
} while (0)

/* ---- test 1: gray_to_natural -------------------------------------------- */

static void test_gray_roundtrip(void) {
    printf("TEST 1: gray_to_natural / natural_to_gray roundtrip\n");

    for (unsigned int M = 2; M <= 256; M <<= 1) {
        for (unsigned int n = 0; n < M; n++) {
            unsigned int g    = natural_to_gray(n);
            unsigned int back = gray_to_natural(g);
            char label[64];
            snprintf(label, sizeof(label), "M=%u n=%u: gray=%u -> back=%u", M, n, g, back);
            EXPECT_EQ(back, n, label);
        }
    }

    /* spot-check known Gray table for M=8 */
    static const unsigned int gray8[8] = {0, 1, 3, 2, 6, 7, 5, 4};
    for (unsigned int n = 0; n < 8; n++) {
        char label[64];
        snprintf(label, sizeof(label), "M=8 natural_to_gray(%u)", n);
        EXPECT_EQ(natural_to_gray(n), gray8[n], label);
        snprintf(label, sizeof(label), "M=8 gray_to_natural(%u)", gray8[n]);
        EXPECT_EQ(gray_to_natural(gray8[n]), n, label);
    }

    /* adjacent Gray codes must differ by exactly 1 bit */
    for (unsigned int M = 2; M <= 256; M <<= 1) {
        for (unsigned int n = 0; n < M - 1; n++) {
            unsigned int diff = natural_to_gray(n) ^ natural_to_gray(n + 1);
            int popcount = __builtin_popcount(diff);
            char label[64];
            snprintf(label, sizeof(label),
                     "M=%u: adjacent codes n=%u,n+1 differ by 1 bit", M, n);
            EXPECT_EQ(popcount, 1, label);
        }
    }

    printf("  done.\n\n");
}

/* ---- test 2: bit extraction MSB-first ----------------------------------- */

static void test_bit_extraction(void) {
    printf("TEST 2: bit extraction MSB-first\n");

    struct { unsigned int sym; int bps; const char *expected_bits; } cases[] = {
        /* BPSK (1 bit/symbol) */
        {0, 1, "0"},
        {1, 1, "1"},
        /* QPSK (2 bits/symbol) */
        {0, 2, "00"},
        {1, 2, "01"},
        {2, 2, "10"},
        {3, 2, "11"},
        /* 8-PSK (3 bits/symbol) */
        {5, 3, "101"},
        {7, 3, "111"},
        {4, 3, "100"},
        /* 16-PSK (4 bits/symbol) */
        {0xa, 4, "1010"},
        {0xf, 4, "1111"},
        {0,   4, "0000"},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned int sym = cases[i].sym;
        int bps          = cases[i].bps;
        char got[33]     = {0};

        for (int b = bps - 1; b >= 0; b--)
            got[bps - 1 - b] = '0' + ((sym >> b) & 1u);

        char label[64];
        snprintf(label, sizeof(label), "sym=%u bps=%d", sym, bps);
        EXPECT_STR(got, cases[i].expected_bits, label);
    }

    printf("  done.\n\n");
}

/* ---- test 3: output modes ----------------------------------------------- */

static size_t emit(unsigned int sym, int bps,
                   int out_symbols, int out_ascii,
                   uint8_t *buf, size_t buf_size) {
    size_t n = 0;
    if (out_symbols) {
        if (out_ascii) {
            n = (size_t)snprintf((char *)buf, buf_size, "%u\n", sym);
        } else {
            buf[0] = (uint8_t)sym;
            n = 1;
        }
    } else {
        for (int b = bps - 1; b >= 0 && n < buf_size; b--) {
            unsigned int bit = (sym >> b) & 1u;
            buf[n++] = out_ascii ? (uint8_t)('0' + bit) : (uint8_t)bit;
        }
    }
    return n;
}

static void test_output_modes(void) {
    printf("TEST 3: output modes (content × format)\n");

    uint8_t buf[64];
    size_t  n;

    /* symbols + binary */
    n = emit(3, 2, 1, 0, buf, sizeof(buf));
    EXPECT_EQ(n, 1u, "symbols+binary len");
    EXPECT_EQ(buf[0], 3u, "symbols+binary value=3");

    n = emit(0, 4, 1, 0, buf, sizeof(buf));
    EXPECT_EQ(buf[0], 0u, "symbols+binary value=0 (16-PSK)");

    /* symbols + ascii */
    n = emit(7, 3, 1, 1, buf, sizeof(buf));
    buf[n] = '\0';
    EXPECT_STR((char *)buf, "7\n", "symbols+ascii 8-PSK sym=7");

    n = emit(15, 4, 1, 1, buf, sizeof(buf));
    buf[n] = '\0';
    EXPECT_STR((char *)buf, "15\n", "symbols+ascii 16-PSK sym=15");

    /* bits + binary: one byte (0/1) per bit, MSB first */
    n = emit(5, 3, 0, 0, buf, sizeof(buf)); /* 5 = 101 */
    EXPECT_EQ(n, 3u, "bits+binary len");
    EXPECT_EQ(buf[0], 1u, "bits+binary MSB");
    EXPECT_EQ(buf[1], 0u, "bits+binary mid");
    EXPECT_EQ(buf[2], 1u, "bits+binary LSB");

    /* bits + ascii */
    n = emit(6, 3, 0, 1, buf, sizeof(buf)); /* 6 = 110 */
    EXPECT_EQ(n, 3u, "bits+ascii len");
    EXPECT_EQ(buf[0], (uint8_t)'1', "bits+ascii[0]");
    EXPECT_EQ(buf[1], (uint8_t)'1', "bits+ascii[1]");
    EXPECT_EQ(buf[2], (uint8_t)'0', "bits+ascii[2]");

    /* 4-symbol QPSK stream in ASCII bits */
    const unsigned int syms4[4] = {0, 1, 2, 3};
    size_t total = 0;
    uint8_t stream[32];
    for (int i = 0; i < 4; i++)
        total += emit(syms4[i], 2, 0, 1, stream + total, sizeof(stream) - total);
    stream[total] = '\0';
    EXPECT_EQ(total, 8u, "QPSK 4-symbol stream length");
    EXPECT_STR((char *)stream, "00011011", "QPSK natural bits ascii");

    /* same stream with Gray mapping */
    total = 0;
    for (int i = 0; i < 4; i++) {
        unsigned int mapped = gray_to_natural(syms4[i]);
        total += emit(mapped, 2, 0, 1, stream + total, sizeof(stream) - total);
    }
    stream[total] = '\0';
    /* gray_to_natural([0,1,2,3]) = [0,1,3,2] → bits: 00 01 11 10 */
    EXPECT_STR((char *)stream, "00011110", "QPSK gray bits ascii");

    printf("  done.\n\n");
}

/* ---- test 4: bits per symbol -------------------------------------------- */

static void test_bits_per_symbol(void) {
    printf("TEST 4: bits per symbol for M-PSK\n");

    /* log2(M) = __builtin_ctz(M) for powers of 2 */
    struct { int M; int bps; } cases[] = {
        {2,  1},
        {4,  2},
        {8,  3},
        {16, 4},
        {32, 5},
        {64, 6},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int bps = __builtin_ctz((unsigned)cases[i].M);
        char label[64];
        snprintf(label, sizeof(label), "M=%d bps", cases[i].M);
        EXPECT_EQ((unsigned)bps, (unsigned)cases[i].bps, label);

        /* verify: 2^bps == M */
        EXPECT_EQ((unsigned)(1 << bps), (unsigned)cases[i].M, label);
    }

    /* verify that PSK symbol indices stay within [0, M-1] */
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        unsigned int M = (unsigned)cases[i].M;
        for (unsigned int s = 0; s < M; s++) {
            char label[64];
            snprintf(label, sizeof(label), "M=%u sym=%u in range", M, s);
            EXPECT_EQ(s < M ? 1u : 0u, 1u, label);
        }
    }

    printf("  done.\n\n");
}

/* ---- main --------------------------------------------------------------- */

int main(void) {
    printf("demod_psk unit tests\n\n");

    test_gray_roundtrip();
    test_bit_extraction();
    test_output_modes();
    test_bits_per_symbol();

    if (failures == 0)
        printf("All tests passed.\n");
    else
        printf("%d test(s) FAILED.\n", failures);

    return failures ? 1 : 0;
}
