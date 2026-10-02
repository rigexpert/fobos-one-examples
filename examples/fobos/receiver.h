/**
 * @file receiver.h
 * @brief Thread-safe Fobos SDR receiver layer with ring-buffer back-pressure.
 *
 * Wraps the low-level Fobos SDR API (fobos.h) behind a simple pull interface.
 * A dedicated real-time capture thread runs at SCHED_FIFO priority pinned to
 * CPU core 3, ensuring the SDR hardware is serviced without OS jitter.  IQ
 * data is placed into a ring buffer (ringbuffer.h) from which the DSP thread
 * reads via receiver_read().
 *
 * Two capture modes are supported:
 * - **receiver_mode_sync** — the capture thread calls fobos_read_samples_sync_direct()
 *   in a loop.  Recommended for most use cases.
 * - **receiver_mode_async** — the Fobos driver pushes IQ data through its
 *   async callback mechanism.  Use when the tightest USB integration is needed.
 *
 * Typical usage:
 * @code
 *   receiver_init(64 * 1024, 32);   // 32 ring-buffer chunks of 64k samples each
 *   receiver_set_freq(102500000ULL);
 *   receiver_set_samplerate(8000000);
 *   receiver_set_lna(3);
 *   receiver_start(receiver_mode_sync);
 *
 *   float iq_buf[2048];
 *   receiver_read(iq_buf, 1024);    // blocks until 1024 IQ samples are ready
 *
 *   receiver_stop();
 *   receiver_close();
 * @endcode
 *
 * @note Only one device instance is supported at a time (static context).
 */

#ifndef __RECEIVER_C__
#define __RECEIVER_C__

#include <stdio.h>
#include <stdint.h>

/** Fraction of sample rate used as usable IF bandwidth (matches fobos_sdr_set_auto_bandwidth). */
#define FOBOS_AUTO_BW        0.8
/** Maximum number of scan frequencies supported by the hardware. */
#define FOBOS_MAX_FREQS_CNT  256

/**
 * @brief Selects the IQ capture mechanism used by receiver_start().
 */
typedef enum {
    receiver_mode_sync  = 0, /**< Synchronous: dedicated read thread pulls from hardware. */
    receiver_mode_async      /**< Asynchronous: driver pushes data via callback. */
} receiver_mode_t;

/**
 * @brief Initialise the receiver and allocate the ring buffer.
 *
 * Opens the first available Fobos SDR device, allocates a ring buffer with
 * @p buffer_count chunks of @p buffer_size complex samples each, and prepares
 * the hardware for capture.  Call the @c receiver_set_* functions after this
 * to configure frequency, sample rate, and gains before calling receiver_start().
 *
 * @param[in] buffer_size   Number of complex IQ samples per ring-buffer chunk.
 *                          Must be a power of two.
 * @param[in] buffer_count  Number of chunks in the ring buffer.
 *                          Larger values tolerate longer DSP stalls without dropping data.
 * @return 0 on success, non-zero on failure.
 */
int receiver_init(size_t buffer_size, size_t buffer_count);

/**
 * @brief Close the device and free all receiver resources.
 *
 * Releases the Fobos device handle.  Call receiver_stop() first.
 *
 * @return 0 on success, non-zero on failure.
 */
int receiver_close(void);

/**
 * @brief Set the SDR centre frequency.
 *
 * @param[in] freq  Centre frequency in Hz (e.g. 102500000ULL for 102.5 MHz).
 * @return 0 on success, non-zero on failure.
 */
int receiver_set_freq(uint64_t freq);

/**
 * @brief Set the ADC sample rate.
 *
 * @param[in] sr  Sample rate in Hz (e.g. 8000000 for 8 MSPS).
 * @return 0 on success, non-zero on failure.
 */
int receiver_set_samplerate(uint32_t sr);

/**
 * @brief Set the analogue IF filter bandwidth.
 *
 * @param[in] bw  Bandwidth in Hz.  Set to at least the desired channel width.
 * @return 0 on success, non-zero on failure.
 */
int receiver_set_bw(float bw);

/**
 * @brief Set the LNA gain.
 *
 * @param[in] gain  LNA gain index 0..#FOBOS_LNA_GAIN_MAX (see fobos_set_lna_gain()).
 * @return 0 on success, non-zero on failure.
 */
int receiver_set_lna(uint8_t gain);

/**
 * @brief Set the VGA gain.
 *
 * @param[in] gain  VGA gain index 0..#FOBOS_VGA_GAIN_MAX (see fobos_set_vga_gain()).
 * @return 0 on success, non-zero on failure.
 */
int receiver_set_vga(uint8_t gain);

/**
 * @brief Start IQ capture on a dedicated real-time thread.
 *
 * Spawns a capture thread that is pinned to CPU core 3 and scheduled with
 * SCHED_FIFO (priority 80) to minimise USB transfer jitter.  The thread feeds
 * the ring buffer continuously until receiver_stop() is called.
 *
 * Requires @c CAP_SYS_NICE (run as root or set the capability on the binary)
 * for the real-time scheduling to take effect; it degrades gracefully to
 * normal priority without it.
 *
 * @param[in] mode  Capture mode: @c receiver_mode_sync or @c receiver_mode_async.
 * @return 0 on success, non-zero on failure.
 */
int receiver_start(receiver_mode_t mode);

/**
 * @brief Stop IQ capture and join the capture thread.
 *
 * Sets the exit flag, waits for the capture thread to terminate, and stops
 * the hardware transfer.  Safe to call from a signal handler via @c ctx.run.
 *
 * @return 0 on success, non-zero on failure.
 */
int receiver_stop(void);

/**
 * @brief Read IQ samples from the ring buffer (blocking).
 *
 * Blocks until @p size complex IQ samples (interleaved float32 I/Q) are
 * available in the ring buffer, then copies them into @p buff.
 *
 * @param[out] buff  Destination buffer for interleaved float32 I/Q samples.
 * @param[in]  size  Number of complex samples to read.
 * @return Number of complex samples actually read.
 */
int receiver_read(void *buff, size_t size);

/**
 * @brief Start hardware-managed frequency scanning.
 *
 * Activates the Fobos SDR agile scanning mode: the hardware steps through
 * @p freqs[] automatically and tags each IQ buffer with its channel index.
 * Buffers received while the hardware is re-tuning (channel == -1) are
 * discarded automatically.  Call receiver_init() first to open the device
 * and allocate the ring buffer (use @p samples_per_step as buffer_size).
 *
 * @param[in] freqs            Array of centre frequencies in Hz (double).
 * @param[in] count            Number of frequencies (2..256).
 * @param[in] samples_per_step IQ samples per hardware buffer; must be a
 *                             multiple of 8192 and at least 65536.
 * @return 0 on success, non-zero on failure.
 */
int receiver_start_scan(double *freqs, int count, int samples_per_step);

/**
 * @brief Stop hardware scanning and join the capture thread.
 *
 * Cancels the async stream, disables the scan mode, and waits for the
 * capture thread to terminate.
 *
 * @return 0 on success, non-zero on failure.
 */
int receiver_stop_scan(void);

/**
 * @brief Read one complete scan chunk from the ring buffer (blocking).
 *
 * Blocks until a full chunk is available, copies up to @p samples_per_step
 * IQ samples into @p buf, and returns the centre frequency of that chunk.
 * Each chunk corresponds to one settled step in the hardware scan cycle.
 *
 * @param[out] buf              Destination buffer (interleaved float32 I/Q).
 *                              Must hold at least samples_per_step complex samples.
 * @param[in]  samples_per_step Maximum complex IQ samples to copy.
 * @param[out] freq_hz          Set to the centre frequency (Hz) of this chunk.
 * @return Number of complex IQ samples copied.
 */
int receiver_read_scan_chunk(float *buf, size_t samples_per_step, double *freq_hz);

#endif
