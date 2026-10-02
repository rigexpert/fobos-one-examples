#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>

#include "ringbuffer.h"

/* ── helpers ──────────────────────────────────────────────────────────────── */

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (uint8_t)(seed + i);
}

static int check_pattern(const uint8_t *buf, size_t len, uint8_t seed)
{
    for (size_t i = 0; i < len; i++)
        if (buf[i] != (uint8_t)(seed + i)) return 0;
    return 1;
}

static int g_tests_run, g_tests_passed;

#define ASSERT(cond, msg) do { \
    if (!(cond)) { fprintf(stderr, "\n    FAIL: %s\n", msg); return 0; } \
} while (0)

#define RUN(fn) do { \
    g_tests_run++; \
    printf("  %-50s", #fn "..."); fflush(stdout); \
    if (fn()) { printf("PASS\n"); g_tests_passed++; } \
    else       { printf("FAIL\n"); } \
} while (0)

#define CS  256   /* chunk size used by most tests */
#define CC    8   /* default chunk count            */

/* ── test 1: single write/read ────────────────────────────────────────────── */

static int test_basic_write_read(void)
{
    ringbuffer_t rb;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    uint8_t src[CS], dst[CS];
    fill_pattern(src, CS, 0xAA);

    ASSERT(rb_write(&rb, src, CS) == (int)CS, "rb_write return value");
    ASSERT(rb_read(&rb, dst, CS) == CS,       "rb_read return value");
    ASSERT(memcmp(src, dst, CS) == 0,          "data mismatch");

    rb_destroy(&rb);
    return 1;
}

/* ── test 2: multiple sequential chunks, FIFO order ──────────────────────── */

static int test_sequential_chunks(void)
{
    ringbuffer_t rb;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    uint8_t src[CS], dst[CS];
    for (int i = 0; i < 4; i++) {
        fill_pattern(src, CS, (uint8_t)(i * 37));
        ASSERT(rb_write(&rb, src, CS) == (int)CS, "rb_write failed");
    }
    for (int i = 0; i < 4; i++) {
        ASSERT(rb_read(&rb, dst, CS) == CS, "rb_read failed");
        ASSERT(check_pattern(dst, CS, (uint8_t)(i * 37)), "data mismatch");
    }

    rb_destroy(&rb);
    return 1;
}

/* ── test 3: partial reads crossing chunk boundaries ─────────────────────── */

static int test_partial_read_across_chunks(void)
{
    ringbuffer_t rb;
    const size_t cs = 64;
    ASSERT(rb_init(&rb, CC, cs) == 0, "rb_init failed");

    uint8_t expected[256];
    for (int i = 0; i < 4; i++) {
        uint8_t src[64];
        fill_pattern(src,              64, (uint8_t)(i * 64));
        fill_pattern(expected + i * 64, 64, (uint8_t)(i * 64));
        ASSERT(rb_write(&rb, src, 64) == 64, "rb_write failed");
    }

    /* read in 80-byte pieces – each call straddles a chunk boundary */
    uint8_t got[256];
    size_t total = 0;
    while (total < 256) {
        size_t want = (256 - total < 80) ? 256 - total : 80;
        total += rb_read(&rb, got + total, want);
    }
    ASSERT(memcmp(expected, got, 256) == 0, "data mismatch across chunk boundaries");

    rb_destroy(&rb);
    return 1;
}

/* ── test 4: overflow – oldest chunk is dropped ───────────────────────────── */

static int test_overflow_drops_oldest(void)
{
    ringbuffer_t rb;
    const size_t cc = 4;
    ASSERT(rb_init(&rb, cc, CS) == 0, "rb_init failed");

    /* Write cc+1 chunks; the 5th write overwrites slot 0 and advances
       read_idx to 1, so the oldest surviving chunk has seed=10. */
    for (size_t i = 0; i < cc + 1; i++) {
        uint8_t src[CS];
        fill_pattern(src, CS, (uint8_t)(i * 10));
        rb_write(&rb, src, CS);
    }

    size_t cnt = atomic_load(&rb.count);
    ASSERT(cnt == cc, "count must stay at chunk_count after overflow");

    uint8_t dst[CS];
    ASSERT(rb_read(&rb, dst, CS) == CS, "rb_read failed");
    ASSERT(check_pattern(dst, CS, 10), "oldest chunk not dropped (expected seed=10)");

    rb_destroy(&rb);
    return 1;
}

/* ── test 5: index wrap-around (fill, drain, refill, drain) ──────────────── */

static int test_index_wraparound(void)
{
    ringbuffer_t rb;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    uint8_t src[CS], dst[CS];

    for (int cycle = 0; cycle < 3; cycle++) {
        uint8_t base = (uint8_t)(cycle * 71);
        for (int i = 0; i < CC; i++) {
            fill_pattern(src, CS, (uint8_t)(base + i));
            ASSERT(rb_write(&rb, src, CS) == (int)CS, "rb_write failed (fill)");
        }
        for (int i = 0; i < CC; i++) {
            ASSERT(rb_read(&rb, dst, CS) == CS, "rb_read failed (drain)");
            ASSERT(check_pattern(dst, CS, (uint8_t)(base + i)),
                   "data mismatch after wrap-around");
        }
    }

    rb_destroy(&rb);
    return 1;
}

/* ── test 6: async producer/consumer – data integrity ────────────────────── */

#define ASYNC_N 128

typedef struct {
    ringbuffer_t *rb;
    int           ok;
} consumer_args_t;

static void *consumer_fn(void *arg)
{
    consumer_args_t *a = arg;
    uint8_t buf[CS];
    a->ok = 1;
    for (int i = 0; i < ASYNC_N; i++) {
        if (rb_read(a->rb, buf, CS) != CS) {
            a->ok = 0;
            continue;
        }
        if (!check_pattern(buf, CS, (uint8_t)(i & 0xFF))) {
            fprintf(stderr, "\n    consumer mismatch at chunk %d\n", i);
            a->ok = 0;
        }
    }
    return NULL;
}

static int test_async_producer_consumer(void)
{
    ringbuffer_t rb;
    /* buffer larger than ASYNC_N so no overflow occurs */
    ASSERT(rb_init(&rb, ASYNC_N + 4, CS) == 0, "rb_init failed");

    consumer_args_t args = { .rb = &rb, .ok = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, consumer_fn, &args);

    uint8_t src[CS];
    for (int i = 0; i < ASYNC_N; i++) {
        fill_pattern(src, CS, (uint8_t)(i & 0xFF));
        rb_write(&rb, src, CS);
        /* occasional yield so consumer runs between bursts */
        if ((i & 0x0F) == 0x0F)
            usleep(50);
    }

    pthread_join(tid, NULL);
    rb_destroy(&rb);
    ASSERT(args.ok, "data integrity failure in async producer/consumer");
    return 1;
}

/* ── test 7: blocking wakeup – reader blocks until writer arrives ─────────── */

typedef struct {
    ringbuffer_t *rb;
    uint64_t      elapsed_us;
} block_args_t;

static void *blocking_reader_fn(void *arg)
{
    block_args_t *a = arg;
    uint8_t buf[CS];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rb_read(a->rb, buf, CS);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    a->elapsed_us = (uint64_t)(t1.tv_sec  - t0.tv_sec)  * 1000000ULL
                  + (uint64_t)(t1.tv_nsec - t0.tv_nsec) / 1000ULL;
    return NULL;
}

static int test_blocking_wakeup(void)
{
    ringbuffer_t rb;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    block_args_t args = { .rb = &rb, .elapsed_us = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, blocking_reader_fn, &args);

    usleep(50000);   /* let the reader block for ~50 ms */

    uint8_t src[CS];
    fill_pattern(src, CS, 0x55);
    rb_write(&rb, src, CS);
    pthread_join(tid, NULL);
    rb_destroy(&rb);

    /* reader must have waited at least 40 ms (10 ms scheduling slack) */
    ASSERT(args.elapsed_us >= 40000,
           "reader did not block (woke up too early)");
    return 1;
}

/* ── test 8: rb_read blocks mid-request until writer provides remaining data ─
 *
 *  Reader issues ONE rb_read for SPAN_N * CS bytes.
 *  Writer delivers chunks one at a time with 10 ms gaps.
 *  rb_read must block after each partial fill and reassemble the full buffer.
 */

#define SPAN_N 4

typedef struct {
    ringbuffer_t *rb;
    size_t        request_len;
    uint8_t      *out;
    size_t        received;
} large_read_args_t;

static void *large_read_fn(void *arg)
{
    large_read_args_t *a = arg;
    a->received = rb_read(a->rb, a->out, a->request_len);
    return NULL;
}

static int test_blocking_read_spans_multiple_writes(void)
{
    ringbuffer_t rb;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    uint8_t out[SPAN_N * CS];
    large_read_args_t args = { .rb = &rb, .request_len = SPAN_N * CS,
                               .out = out, .received = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, large_read_fn, &args);

    uint8_t expected[SPAN_N * CS];
    for (int i = 0; i < SPAN_N; i++) {
        usleep(10000);   /* 10 ms gap: forces rb_read to block between chunks */
        uint8_t src[CS];
        fill_pattern(src,              CS, (uint8_t)(i * 17));
        fill_pattern(expected + i * CS, CS, (uint8_t)(i * 17));
        rb_write(&rb, src, CS);
    }

    pthread_join(tid, NULL);
    rb_destroy(&rb);

    ASSERT(args.received == SPAN_N * CS, "did not receive all bytes");
    ASSERT(memcmp(expected, out, SPAN_N * CS) == 0, "data mismatch");
    return 1;
}

/* ── test 9: rb_read request exceeds total ring buffer capacity ──────────────
 *
 *  Reader asks for (CC + 2) * CS bytes — more than the ring can hold at once.
 *  Writer delivers one chunk every 5 ms so the reader drains each slot before
 *  the next arrives (count stays ≤ 1, no overflow).  The reader must
 *  repeatedly block and wake up, rolling through the physical ring twice.
 */

static int test_blocking_read_exceeds_capacity(void)
{
    ringbuffer_t rb;
    const int N = CC + 2;
    ASSERT(rb_init(&rb, CC, CS) == 0, "rb_init failed");

    uint8_t *expected = malloc((size_t)N * CS);
    uint8_t *out      = malloc((size_t)N * CS);
    ASSERT(expected && out, "malloc failed");

    large_read_args_t args = { .rb = &rb, .request_len = (size_t)N * CS,
                               .out = out, .received = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, large_read_fn, &args);

    for (int i = 0; i < N; i++) {
        usleep(5000);   /* 5 ms: reader empties the slot before next write */
        uint8_t src[CS];
        fill_pattern(src,              CS, (uint8_t)(i * 13));
        fill_pattern(expected + i * CS, CS, (uint8_t)(i * 13));
        rb_write(&rb, src, CS);
    }

    pthread_join(tid, NULL);

    int ok = (args.received == (size_t)N * CS) &&
             (memcmp(expected, out, (size_t)N * CS) == 0);

    free(expected);
    free(out);
    rb_destroy(&rb);

    if (!ok) {
        fprintf(stderr, "\n    FAIL: received %zu / expected %d bytes, data%s match\n",
                args.received, N * CS,
                (args.received == (size_t)N * CS) ? " did not" : "/size did not");
    }
    return ok;
}

/* ── test 10: throughput stress – many chunks, no overflow, no deadlock ────── */

#define STRESS_N 1000

typedef struct {
    ringbuffer_t *rb;
    size_t        received;
} stress_args_t;

static void *stress_consumer_fn(void *arg)
{
    stress_args_t *a = arg;
    uint8_t buf[CS];
    for (int i = 0; i < STRESS_N; i++) {
        rb_read(a->rb, buf, CS);
        a->received++;
    }
    return NULL;
}

static int test_stress_throughput(void)
{
    ringbuffer_t rb;
    /* large buffer: absorbs the full burst, no overflow */
    ASSERT(rb_init(&rb, STRESS_N + 4, CS) == 0, "rb_init failed");

    stress_args_t args = { .rb = &rb, .received = 0 };
    pthread_t tid;
    pthread_create(&tid, NULL, stress_consumer_fn, &args);

    uint8_t src[CS];
    memset(src, 0xAB, CS);
    for (int i = 0; i < STRESS_N; i++)
        rb_write(&rb, src, CS);

    pthread_join(tid, NULL);
    rb_destroy(&rb);
    ASSERT(args.received == STRESS_N, "consumer did not receive all chunks");
    return 1;
}

/* ── main ─────────────────────────────────────────────────────────────────── */

int main(void)
{
    printf("=== ringbuffer integrity tests ===\n");
    RUN(test_basic_write_read);
    RUN(test_sequential_chunks);
    RUN(test_partial_read_across_chunks);
    RUN(test_overflow_drops_oldest);
    RUN(test_index_wraparound);
    RUN(test_async_producer_consumer);
    RUN(test_blocking_wakeup);
    RUN(test_blocking_read_spans_multiple_writes);
    RUN(test_blocking_read_exceeds_capacity);
    RUN(test_stress_throughput);
    printf("==================================\n");
    printf("Result: %d / %d passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
