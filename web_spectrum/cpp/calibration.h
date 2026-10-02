/**
 * @file calibration.h
 * @brief Calibration job control: spawns the Python orchestrator (calib_orchestrator.py),
 *        forwards its JSON progress lines to WebSocket clients, and probes the generator.
 */
#pragma once

#include <string>

/**
 * @brief Resolve the calibration directory and file path. Call once at startup.
 *
 * Chooses ./calibration (installed/dev run root) or ../calibration (running from a
 * cpp/ or build/ subdir) based on which contains calib_orchestrator.py.
 */
void resolve_cal_dir();

/// @return Path to fobos_calibration.json in the resolved calibration directory.
const std::string& cal_file();

/**
 * @brief Launch the calibration orchestrator.
 * @param config_json The calibration configuration (written to a temp file for the child).
 * @param[out] err On failure, a human-readable reason.
 * @return true if the process was started; false if one is already running or spawn failed.
 */
bool cal_start(const std::string& config_json, std::string& err);

/// @brief Terminate a running calibration (SIGTERM to its process group).
void cal_abort();

/**
 * @brief Probe the signal generator's reachability.
 * @param ip   Generator IP address.
 * @param port Generator TCP port.
 * @param[out] info Human-readable result (status line or error) either way.
 * @return true if a TCP connection succeeded.
 */
bool gen_probe(const std::string& ip, int port, std::string& info);
