/**
 * @file frame.h
 * @brief Binary spectrum frame format pushed over the WebSocket (magic 'SPC2').
 *
 * Header layout (little-endian):
 * @code
 *   magic(4) mode(u8) pad(3) fft_size(u32) n_bins(u32) n_steps(u32)
 *   crop_lo(u32) crop_keep(u32) center(f64) rate(f64) carrier_freq(f64)
 *   carrier_dbfs(f32) pad(u32) | step_centers f64*n_steps | power u8*n_bins
 * @endcode
 * Power bins are quantized to 1 dB in one byte (see @ref db_to_u8); the precise absolute
 * reading is the coherent carrier meter, carried as a float in the header.
 */
#pragma once

#include <cstdint>
#include <string>

/**
 * @brief A growable little-endian byte buffer used to assemble a frame.
 *
 * Multi-byte appends assume a little-endian host (the target Raspberry Pi is LE), so the
 * bytes go on the wire in the order the JavaScript DataView reads them.
 */
struct Buf {
    std::string d;  ///< The accumulated bytes.

    /**
     * @brief Append raw bytes.
     * @param p Pointer to the bytes.
     * @param n Number of bytes.
     */
    void bytes(const void* p, size_t n);
    /// @brief Append one byte. @param v The value.
    void u8(uint8_t v);
    /// @brief Append a little-endian uint32. @param v The value.
    void u32(uint32_t v);
    /// @brief Append a little-endian float32. @param v The value.
    void f32(float v);
    /// @brief Append a little-endian float64. @param v The value.
    void f64(double v);
};

/**
 * @brief Quantize a dB value into one byte.
 * @param db The value in dB.
 * @return clamp(round(db) + 215, 0, 255) — the byte spans -215..+40 dB.
 */
uint8_t db_to_u8(double db);

/**
 * @brief Append the 60-byte SPC2 frame header.
 * @param b            Destination buffer.
 * @param mode         0 = FFT mode, 1 = scan mode.
 * @param fft_size     FFT length used to produce the frame.
 * @param n_bins       Number of power bytes that follow the header (+ step centers).
 * @param n_steps      Number of scan tiles (0 in FFT mode).
 * @param crop_lo      First kept bin index within each tile (scan mode).
 * @param crop_keep    Number of kept bins per tile (scan mode).
 * @param center       RX center frequency in Hz (0 in scan mode).
 * @param rate         Sample rate in Hz.
 * @param carrier_freq Coherent carrier frequency in Hz, or NaN.
 * @param carrier_dbfs Coherent carrier power in dBFS, or NaN.
 */
void write_header(Buf& b, uint8_t mode, uint32_t fft_size, uint32_t n_bins,
                  uint32_t n_steps, uint32_t crop_lo, uint32_t crop_keep,
                  double center, double rate, double carrier_freq, float carrier_dbfs);
