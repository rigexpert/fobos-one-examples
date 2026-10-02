/**
 * @file backend_switch.h
 * @brief Backend selection (C++ vs Python).
 *
 * The choice lives in backend.conf. Switching just rewrites that file and exits, so
 * systemd (Restart=always) relaunches the launcher into the newly selected backend.
 */
#pragma once

#include <string>

/**
 * @brief Read the current backend selection.
 * @return "cpp" or "python" ("cpp" if backend.conf is missing/empty).
 */
std::string read_backend_conf();

/**
 * @brief Overwrite backend.conf with a selection.
 * @param backend "cpp" or "python".
 */
void write_backend_conf(const std::string& backend);

/**
 * @brief Release the device and exit shortly (after the HTTP reply flushes) so systemd
 *        restarts the service into the newly selected backend.
 */
void schedule_reexec();
