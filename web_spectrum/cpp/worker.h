/**
 * @file worker.h
 * @brief Acquisition workers (FFT and scan) and their lifecycle.
 *
 * A worker runs on its own thread, drives the device, and pushes spectrum frames. The
 * functions here start/stop it and apply live parameter changes. Worker internals
 * (contexts, read callbacks, control flags) are private to worker.cpp.
 */
#pragma once

#include <string>

#include "json.h"

/// @brief Ask the running worker to stop and cancel any in-flight async read.
void signal_stop();

/// @brief Join the worker thread if one is running.
void join_worker();

/**
 * @brief Apply a /api/start payload and (re)start the selected worker.
 * @param body Parsed request body (may be null); recognized keys update @ref State.
 * @return The mode that was started ("fft" or "scan").
 */
std::string start_action(const js::ValuePtr& body);

/**
 * @brief Apply a /api/params payload: a live tune/gain change while streaming, or a
 *        direct hardware set when idle.
 * @param body Parsed request body (may be null).
 */
void params_action(const js::ValuePtr& body);

/**
 * @brief The most recent spectrum meta JSON (peak/avg/carrier), for GET /api/spectrum.
 * @return A JSON object string, or "" if no frame has been produced yet.
 */
std::string last_spectrum_json();
