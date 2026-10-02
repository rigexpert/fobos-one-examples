/**
 * @file state.h
 * @brief Shared acquisition state (current tune/gain/mode + the open device handle)
 *        guarded by a single mutex, plus JSON (de)serialization and persistence.
 */
#pragma once

#include <mutex>
#include <string>
#include <vector>

struct fobos_sdr_dev_t;  ///< Opaque libfobos device; only ever held as a pointer here.

/// @brief Highest LNA step accepted by fobos_sdr_set_lna_gain (0,1: 0 dB, 2: +16 dB, 3: +33 dB).
constexpr int LNA_MAX = 3;

/// @brief Highest VGA step accepted by fobos_sdr_set_vga_gain (0..+62 dB in 2 dB steps).
constexpr int VGA_MAX = 31;

/**
 * @brief All mutable acquisition state.
 *
 * Access is guarded by @ref st_mtx (see the global @ref st). The user-facing fields are
 * persisted to state.json; @ref dev and @ref running are runtime-only.
 */
struct State {
    std::string mode = "fft";          ///< "fft" or "scan".
    double freq = 433.92e6;            ///< FFT-mode center frequency, Hz.
    double rate = 5e6;                 ///< Sample rate, Hz.
    int lna = 2;                       ///< LNA gain step (0..LNA_MAX).
    int vga = 8;                       ///< VGA gain step (0..VGA_MAX).
    int fft_size = 4096;               ///< FFT length.
    int overlap = 0;                   ///< FFT-mode window overlap, percent.
    int scan_overlap = 0;              ///< Scan-mode tile overlap, percent.
    int accum_n = 4;                   ///< FFTs averaged per emitted frame.
    std::vector<double> scan_freqs;    ///< Scan tile center frequencies, Hz.
    double scan_from = 430e6;          ///< Scan range start, Hz.
    double scan_to = 440e6;            ///< Scan range end, Hz.
    bool dc_reject = true;             ///< Interpolate across the DC/LO spike.
    double dc_reject_hz = 100e3;       ///< DC-notch width, Hz.
    bool running = false;              ///< Whether a worker is currently acquiring.
    fobos_sdr_dev_t* dev = nullptr;    ///< Open device handle, or nullptr if detached.
};

/// @brief The single global state instance. Lock @ref st_mtx before touching it.
extern State st;

/// @brief Mutex guarding @ref st.
extern std::mutex st_mtx;

/**
 * @brief Serialize @ref st as a JSON object.
 * @pre The caller must already hold @ref st_mtx.
 * @return A JSON object string of the current state.
 */
std::string state_json_locked();

/// @brief Persist the user-facing fields of @ref st to state.json.
void save_state();

/// @brief Restore the user-facing fields of @ref st from state.json (if present).
void load_state();
