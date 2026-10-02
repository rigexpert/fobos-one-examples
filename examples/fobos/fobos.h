/**
 * @file fobos.h
 * @brief Fobos SDR hardware abstraction API.
 *
 * Provides device enumeration, configuration, and IQ sample acquisition for
 * the Fobos SDR receiver.  Two capture modes are available:
 *
 * - **Synchronous** — start with fobos_start_sync(), then call
 *   fobos_read_samples_sync_direct() in a loop (typically from a dedicated
 *   real-time thread).  This is the recommended mode for most applications.
 *
 * - **Asynchronous** — fobos_read_samples_async() registers a callback that
 *   the driver calls from its internal USB thread each time a transfer
 *   completes.  Suitable when you need the tightest possible integration with
 *   the USB transfer layer.
 *
 * All functions return 0 on success and a non-zero error code on failure,
 * unless the return type is a pointer (NULL = failure) or a sample count
 * (negative = error).
 *
 * Typical synchronous usage:
 * @code
 *   fobos_init();
 *   fobos_device_t *dev = fobos_open_by_idx(0);
 *   fobos_set_frequency(dev, 102500000ULL);   // 102.5 MHz
 *   fobos_set_samplerate(dev, 8000000);        // 8 MSPS
 *   fobos_start_sync(dev, FOBOS_DEFAULT_VALUE);
 *
 *   float buf[2048];   // interleaved I/Q float32: buf[0]=I0, buf[1]=Q0, ...
 *   fobos_read_samples_sync_direct(dev, buf, 1024);  // 1024 complex samples
 *
 *   fobos_stop_sync(dev);
 *   fobos_close(dev);
 * @endcode
 */

#ifndef FOBOS_H
#define FOBOS_H

#include <stdint.h>
#include <stddef.h>

/** Pass as a buffer-size argument to select the driver's built-in default. */
#define FOBOS_DEFAULT_VALUE 0

/** Highest LNA gain index (0,1: 0 dB, 2: +16 dB, 3: +33 dB). @see fobos_set_lna_gain */
#define FOBOS_LNA_GAIN_MAX 3

/** Highest VGA gain index (0..+62 dB, 2 dB per step). @see fobos_set_vga_gain */
#define FOBOS_VGA_GAIN_MAX 31

/**
 * @brief Callback invoked by the asynchronous capture path for each IQ buffer.
 *
 * Called from the driver's internal USB thread.  The callback must return
 * quickly — copy data into a queue and do all heavy processing elsewhere.
 *
 * @param buf        Interleaved float32 I/Q samples, normalised to ±1.0.
 *                   Valid only for the duration of the callback.
 * @param buf_length Total number of float values in @p buf
 *                   (= 2 × complex sample count, since each sample has I and Q).
 * @param sender     Opaque pointer to the originating device (useful when
 *                   multiple devices share the same callback).
 * @param ctx        User-supplied context pointer registered via
 *                   fobos_read_samples_async().
 */
typedef void (*fobos_data_callback_t)(float *buf, unsigned int buf_length,
                                      void *sender, void *ctx);

/**
 * @brief Opaque handle representing an open Fobos SDR device.
 *
 * Obtained from fobos_open_by_idx() or fobos_open_by_serial() and passed to
 * every subsequent API call.  Do not access or modify members directly; the
 * layout may change between firmware versions.
 */
typedef struct fobos_device_t {
    void   *dev;               /**< Internal driver handle (USB device context). */
    size_t  buffer_size;       /**< Current SDR transfer buffer size in samples. */
    size_t  samples_in_buffer; /**< Valid samples currently held in @p buffer. */
    uint8_t *buffer;           /**< Internal SDR transfer buffer. */
    uint8_t *buffer_ptr;       /**< Current read cursor into @p buffer. */
    fobos_data_callback_t async_callback; /**< Registered async callback, or NULL. */
    void   *async_user_data;   /**< User context forwarded to @p async_callback. */
} fobos_device_t;

/**
 * @brief Frequency in Hz, stored as an unsigned 64-bit integer.
 *
 * Covers the full Fobos SDR tuning range (up to several GHz) without
 * floating-point rounding.
 * Example: 102.5 MHz = @c 102500000ULL.
 */
typedef uint64_t freq_t;

/**
 * @brief Initialise the Fobos SDR library.
 *
 * Must be called once before any other API function.
 * Safe to call multiple times; subsequent calls are no-ops.
 *
 * @return 0 on success, non-zero on failure.
 */
int fobos_init(void);

/**
 * @brief Return the number of Fobos SDR devices currently connected.
 *
 * @return Device count (≥ 0), or a negative error code.
 */
int fobos_get_device_count(void);

/**
 * @brief Retrieve serial number strings for all connected devices.
 *
 * Writes null-separated serial strings into @p serials up to @p max_len bytes.
 *
 * @param[out] serials   Destination buffer.
 * @param[in]  max_len   Size of @p serials in bytes.
 * @return Number of serials written, or a negative error code.
 */
int fobos_get_serials(char *serials, int max_len);

/**
 * @brief Open a Fobos SDR device by its position in the enumeration list.
 *
 * @param[in] index  Zero-based device index (0 = first detected device).
 * @return Open device handle, or NULL on failure.
 */
fobos_device_t *fobos_open_by_idx(int index);

/**
 * @brief Open a Fobos SDR device by its serial number string.
 *
 * @param[in] serial  Null-terminated serial string as returned by fobos_get_serials().
 * @return Open device handle, or NULL if the device was not found or on failure.
 */
fobos_device_t *fobos_open_by_serial(const char *serial);

/**
 * @brief Close a Fobos SDR device and release all associated resources.
 *
 * Stop any active capture before calling this function.
 *
 * @param[in] dev  Device handle from fobos_open_by_idx() or fobos_open_by_serial().
 * @return 0 on success, non-zero on failure.
 */
int fobos_close(fobos_device_t *dev);

/**
 * @brief Query hardware revision and firmware version strings.
 *
 * All output parameters are optional; pass NULL for fields you do not need.
 *
 * @param[in]  dev           Open device handle.
 * @param[out] hw_revision   Hardware revision string (caller-allocated).
 * @param[out] fw_version    Firmware version string (caller-allocated).
 * @param[out] manufacturer  Manufacturer name (caller-allocated).
 * @param[out] product       Product name (caller-allocated).
 * @param[out] serial        Serial number (caller-allocated).
 * @return 0 on success, non-zero on failure.
 */
int fobos_get_board_info(fobos_device_t *dev, char *hw_revision, char *fw_version,
                         char *manufacturer, char *product, char *serial);

/**
 * @brief Set the ADC sample rate.
 *
 * The hardware supports a discrete set of rates; the driver selects the
 * closest available value.  Query back after setting if the exact rate matters.
 *
 * @param[in] dev         Open device handle.
 * @param[in] samplerate  Desired sample rate in Hz (e.g. 8000000 for 8 MSPS).
 * @return 0 on success, non-zero on failure.
 */
int fobos_set_samplerate(fobos_device_t *dev, int samplerate);

/**
 * @brief Set the analogue IF filter bandwidth.
 *
 * Limits the analogue pre-filter bandwidth before the ADC.  Set to a value
 * at least as large as the channel you want to capture (e.g. 8e6f for 8 MHz).
 *
 * @param[in] dev  Open device handle.
 * @param[in] bw   Desired bandwidth in Hz.
 * @return 0 on success, non-zero on failure.
 */
int fobos_set_bandwidth(fobos_device_t *dev, float bw);

/**
 * @brief Tune the receiver to a centre frequency.
 *
 * @param[in] dev        Open device handle.
 * @param[in] frequency  Centre frequency in Hz (e.g. 102500000ULL for 102.5 MHz).
 * @return 0 on success, non-zero on failure.
 */
int fobos_set_frequency(fobos_device_t *dev, freq_t frequency);

/**
 * @brief Set the Low-Noise Amplifier (LNA) gain.
 *
 * Higher LNA gain improves sensitivity but may increase the noise figure when
 * the input signal is already strong.
 *
 * @param[in] dev   Open device handle.
 * @param[in] gain  LNA gain index 0..#FOBOS_LNA_GAIN_MAX:
 *                  0 and 1 = 0 dB, 2 = +16 dB, 3 = +33 dB.
 *                  Out-of-range values are clamped by the library.
 * @return 0 on success, non-zero on failure.
 */
int fobos_set_lna_gain(fobos_device_t *dev, int gain);

/**
 * @brief Set the Variable Gain Amplifier (VGA) gain.
 *
 * Adjusts IF gain.  Increase for weak signals; decrease if strong signals
 * cause ADC clipping (visible as spectral spurs across the whole band).
 *
 * @param[in] dev   Open device handle.
 * @param[in] gain  VGA gain index 0..#FOBOS_VGA_GAIN_MAX, i.e. 0..+62 dB in
 *                  2 dB steps.  Out-of-range values are clamped by the library.
 * @return 0 on success, non-zero on failure.
 */
int fobos_set_vga_gain(fobos_device_t *dev, int gain);

/**
 * @brief Start synchronous (pull-mode) IQ capture.
 *
 * Prepares the hardware for streaming.  After this call, use
 * fobos_read_samples_sync() or fobos_read_samples_sync_direct() to
 * retrieve samples.
 *
 * @param[in] dev         Open device handle.
 * @param[in] buf_length  USB transfer buffer size in samples.
 *                        Pass @c FOBOS_DEFAULT_VALUE to use the driver default.
 * @return 0 on success, non-zero on failure.
 */
int fobos_start_sync(fobos_device_t *dev, int buf_length);

/**
 * @brief Stop synchronous IQ capture.
 *
 * @param[in] dev  Open device handle.
 * @return 0 on success, non-zero on failure.
 */
int fobos_stop_sync(fobos_device_t *dev);

/**
 * @brief Stop asynchronous IQ capture.
 *
 * Signals the driver to stop delivering callbacks.  Blocks until the
 * internal callback thread has exited cleanly.
 *
 * @param[in] dev  Open device handle.
 * @return 0 on success, non-zero on failure.
 */
int fobos_stop_async(fobos_device_t *dev);

/**
 * @brief Read IQ samples synchronously from the internal driver buffer.
 *
 * Blocks until @p num_samples complex samples are available, then copies
 * them (interleaved float32 I/Q) into @p buffer.
 *
 * @param[in]  dev          Open device handle; fobos_start_sync() must have been called.
 * @param[out] buffer       Destination: interleaved float32 I/Q, @p num_samples pairs.
 * @param[in]  num_samples  Number of complex samples to read.
 * @return Number of complex samples read, or a negative error code.
 */
int fobos_read_samples_sync(fobos_device_t *dev, float *buffer, size_t num_samples);

/**
 * @brief Read IQ samples synchronously, bypassing the internal driver buffer.
 *
 * Issues a USB transfer directly and returns the data.  Preferred inside tight
 * read loops where minimising copy overhead matters.  The caller must issue
 * reads frequently enough to prevent USB FIFO overflow.
 *
 * @param[in]  dev          Open device handle.
 * @param[out] buffer       Destination: interleaved float32 I/Q, @p num_samples pairs.
 * @param[in]  num_samples  Number of complex samples to read.
 * @return Number of complex samples read, or a negative error code.
 */
int fobos_read_samples_sync_direct(fobos_device_t *dev, float *buffer, size_t num_samples);

/**
 * @brief Start asynchronous IQ capture with a callback.
 *
 * Launches a USB transfer thread that calls @p callback each time a transfer
 * completes.  Returns immediately; call fobos_stop_async() to terminate.
 *
 * @param[in] dev          Open device handle.
 * @param[in] num_samples  Complex samples per callback invocation.
 * @param[in] callback     Called for each completed USB transfer.
 * @param[in] user_data    Opaque pointer forwarded to every @p callback call.
 * @return 0 on success, non-zero on failure.
 */
int fobos_read_samples_async(fobos_device_t *dev, size_t num_samples,
                             fobos_data_callback_t callback, void *user_data);

#endif
