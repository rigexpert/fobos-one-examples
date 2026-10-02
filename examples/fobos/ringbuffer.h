/**
 * @file ringbuffer.h
 * @brief Lock-free multi-chunk ring buffer for producer/consumer IQ streaming.
 *
 * Designed for a single-producer / single-consumer model where the producer
 * (SDR capture thread) and the consumer (DSP thread) run on separate CPU cores.
 * Key properties:
 *
 * - **Lock-free fast path** — chunk handoff uses only atomic operations and
 *   memory fences; no mutex is taken in the common case.
 * - **Blocking consumer** — when no data is available, rb_read() sleeps on a
 *   POSIX condition variable and wakes immediately when the producer signals.
 * - **Overflow handling** — if the producer outpaces the consumer, the oldest
 *   unread chunk is silently discarded (drop-oldest policy).
 * - **Cache-line aligned allocations** — each chunk's data buffer is aligned
 *   to 64 bytes, enabling efficient SIMD/DMA access.
 *
 * Two write paths are available:
 * - **rb_write()** — copies data into the next chunk then signals the reader.
 *   Suitable when the source buffer is owned by the caller.
 * - **rb_get_avialable_chunk() + rb_inform_reader()** — zero-copy path for
 *   the synchronous receiver thread, which writes directly into the ring buffer
 *   chunk and then calls rb_inform_reader() to publish it.
 *
 * @note This implementation is not re-entrant: call rb_write() from exactly
 *       one thread and rb_read() from exactly one other thread.
 */

#ifndef __RINGBUFFER_H__
#define __RINGBUFFER_H__

#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>

/**
 * @brief One slot in the ring buffer.
 *
 * Holds a pointer to the chunk's data storage and the number of bytes
 * currently valid in that storage.  The @p used field is set by the writer
 * after the data copy completes, guarded by an atomic release fence.
 */
typedef struct {
    uint8_t *data; /**< 64-byte-aligned data buffer of size @c chunk_size bytes. */
    size_t   used; /**< Bytes of valid data written into @p data this turn. */
    double   freq; /**< Centre frequency (Hz) of the IQ data in this chunk; set by scan callback. */
} rb_chunk_t;

/**
 * @brief Ring buffer state.
 *
 * Contains the chunk array and all coordination variables.  Initialise with
 * rb_init() and release with rb_destroy(); do not modify members directly.
 */
typedef struct {
    rb_chunk_t    *chunks;          /**< Array of @c chunk_count chunks. */
    size_t         chunk_count;     /**< Total number of slots in the ring. */
    size_t         chunk_size;      /**< Allocated size of each chunk's data buffer (bytes). */

    atomic_size_t  write_idx;       /**< Next slot the producer will write into. */
    atomic_size_t  read_idx;        /**< Next slot the consumer will read from. */
    size_t         read_offset;     /**< Byte offset within the current read chunk
                                         (supports partial reads across chunk boundaries). */
    atomic_size_t  count;           /**< Number of fully written, not-yet-consumed chunks. */

    pthread_mutex_t wait_lock;      /**< Protects the condition variable below. */
    pthread_cond_t  data_available; /**< Signalled by writer; wakes a blocking reader. */
} ringbuffer_t;

/**
 * @brief Allocate and initialise a ring buffer.
 *
 * Allocates @p chunk_count chunks, each with a 64-byte-aligned data buffer of
 * @p chunk_size bytes.  Initialises all atomic counters and the POSIX mutex
 * and condition variable used for blocking reads.
 *
 * @param[out] rb           Ring buffer to initialise.
 * @param[in]  chunk_count  Number of slots in the ring (≥ 2 recommended).
 *                          Must be a power of two for best performance.
 * @param[in]  chunk_size   Size of each data buffer in bytes.
 * @return 0 on success, -1 on allocation failure.
 */
int rb_init(ringbuffer_t *rb, size_t chunk_count, size_t chunk_size);

/**
 * @brief Free all resources owned by a ring buffer.
 *
 * Frees the chunk data buffers, the chunk array, and destroys the POSIX mutex
 * and condition variable.  The @p rb struct itself is not freed.
 *
 * @param[in,out] rb  Ring buffer to destroy.
 */
void rb_destroy(ringbuffer_t *rb);

/**
 * @brief Copy data into the next ring-buffer slot and signal the reader.
 *
 * Copies @p len bytes from @p data into the next available write slot.  If
 * the ring is full the oldest unread chunk is discarded (drop-oldest policy)
 * and a warning is printed to stderr.
 *
 * @p len must not exceed @c chunk_size; if it does, -1 is returned immediately.
 *
 * @param[in,out] rb    Ring buffer.
 * @param[in]     data  Source data to copy.
 * @param[in]     len   Number of bytes to copy (≤ chunk_size).
 * @return @p len on success, -1 if @p len > chunk_size.
 */
int rb_write(ringbuffer_t *rb, const void *data, size_t len);

/**
 * @brief Claim the next write slot for zero-copy writing.
 *
 * Returns a pointer to the next chunk in the ring so the caller can write
 * directly into it without an intermediate copy.  The caller must set
 * @c chunk->used to the number of valid bytes written, then call
 * rb_inform_reader() to publish the chunk.
 *
 * If the ring is full the oldest unread chunk is dropped (drop-oldest policy).
 *
 * @param[in,out] rb  Ring buffer.
 * @return Pointer to the claimed chunk; never NULL.
 */
rb_chunk_t *rb_get_avialable_chunk(ringbuffer_t *rb);

/**
 * @brief Publish a zero-copy chunk written by rb_get_avialable_chunk().
 *
 * Issues a release memory fence so the reader sees the fully written data,
 * then signals the condition variable to wake a blocking rb_read() call.
 * Must be called after the caller has filled the chunk returned by
 * rb_get_avialable_chunk() and set its @c used field.
 *
 * @param[in,out] rb  Ring buffer.
 */
void rb_inform_reader(ringbuffer_t *rb);

/**
 * @brief Read bytes from the ring buffer into the caller's buffer (blocking).
 *
 * Copies exactly @p len bytes from the ring buffer into @p out, spanning
 * chunk boundaries transparently.  If no data is available the function
 * blocks on the condition variable until the producer signals.
 *
 * @param[in,out] rb   Ring buffer.
 * @param[out]    out  Destination buffer (must be at least @p len bytes).
 * @param[in]     len  Number of bytes to read.
 * @return Total bytes copied (always @p len on success).
 */
size_t rb_read(ringbuffer_t *rb, void *out, size_t len);

/**
 * @brief Read one complete chunk from the ring buffer (blocking).
 *
 * Blocks until a chunk is available, then copies up to @p max_bytes from it
 * into @p out and stores the chunk's @c freq tag in @p *freq_out (if non-NULL).
 * Unlike rb_read(), this always consumes exactly one chunk and does not use
 * the partial-read @c read_offset.  Use this in scan mode where each chunk
 * holds exactly one step's IQ data tagged with its centre frequency.
 *
 * @param[in,out] rb        Ring buffer.
 * @param[out]    out       Destination buffer (must be at least @p max_bytes).
 * @param[in]     max_bytes Maximum bytes to copy from the chunk.
 * @param[out]    freq_out  Set to the chunk's frequency tag; may be NULL.
 * @return Bytes copied (min of @p max_bytes and chunk->used).
 */
size_t rb_read_chunk(ringbuffer_t *rb, void *out, size_t max_bytes, double *freq_out);

#endif
