/**
 * @file fobos.h
 * @brief Thin wrappers around libfobos_sdr device control. All operate on the device
 *        stored in the shared @ref State (see state.h).
 */
#pragma once

#include <string>
#include <vector>

extern "C" {
#include <fobos_sdr.h>
}

/**
 * @brief Set LNA/VGA gains, forcing the USB write.
 *
 * Works around the library's gain cache, which skips the write when the requested value
 * equals its (possibly stale) cached copy, by first toggling to a different value.
 * @param dev The open device.
 * @param lna LNA gain step (0..3: 0,1 = 0 dB, 2 = +16 dB, 3 = +33 dB).
 * @param vga VGA gain step (0..31: 0..+62 dB in 2 dB steps).
 */
void force_gain(fobos_sdr_dev_t* dev, int lna, int vga);

/**
 * @brief Query the device's supported sample rates.
 * @return The rates in Hz, or an empty vector if the device is closed or the query fails.
 */
std::vector<double> get_samplerates();

/**
 * @brief Open device index 0 and store it in @ref st (idempotent).
 * @param[out] msg "ok" on success, otherwise a human-readable error.
 * @return true on success (or if already open).
 */
bool dev_open(std::string& msg);

/// @brief Cancel streaming, stop scanning, close the device, and clear st.dev.
void dev_close();
