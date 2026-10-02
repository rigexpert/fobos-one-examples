/**
 * @file timeutil.h
 * @brief Small clock helpers used across the server.
 */
#pragma once

/**
 * @brief Monotonic clock reading, in seconds.
 * @return Seconds from an unspecified epoch; only differences are meaningful. Use for
 *         measuring intervals and rate-limiting.
 */
double mono();

/**
 * @brief Wall-clock time, in seconds since the Unix epoch.
 * @return Seconds since 1970-01-01 UTC. Use for timestamps embedded in JSON.
 */
double now_wall();
