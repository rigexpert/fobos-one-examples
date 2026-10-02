/**
 * @file broadcast.h
 * @brief Server-push helpers: emit status events and binary spectrum frames to
 *        WebSocket clients. Wraps the single ws::Server instance so other modules do
 *        not touch it directly.
 */
#pragma once

#include <cstddef>
#include <string>

namespace ws {
class Server;
}

/**
 * @brief Register the running server.
 * @param server The server instance. Call once from main; must outlive all emits.
 */
void broadcast_set_server(ws::Server* server);

/**
 * @brief Push a text event to all clients.
 * @param event     Event name (e.g. "started").
 * @param data_json JSON payload for the event. A no-op before @ref broadcast_set_server.
 */
void emit_event(const std::string& event, const std::string& data_json);

/**
 * @brief Push an {"msg":...} error event to all clients.
 * @param msg Error message text.
 */
void emit_error(const std::string& msg);

/**
 * @brief Push a binary spectrum frame to all clients.
 * @param data Frame bytes.
 * @param len  Frame length.
 */
void broadcast_spectrum(const void* data, size_t len);
