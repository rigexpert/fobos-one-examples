/**
 * @file log.h
 * @brief Tiny logging facility: mirrors messages to stderr (the systemd journal) and
 *        keeps a bounded in-memory ring so the /api/log endpoint can return recent lines.
 */
#pragma once

#include <string>
#include <vector>

/**
 * @brief Append a log line.
 * @param message The text to log. It is stored in the ring and also written to stderr.
 */
void logmsg(const std::string& message);

/**
 * @brief Fetch the most recent log lines.
 * @param count Maximum number of lines to return.
 * @return Up to @p count lines, oldest first.
 */
std::vector<std::string> log_recent(size_t count);
