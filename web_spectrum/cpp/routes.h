/**
 * @file routes.h
 * @brief HTTP routing and the WebSocket connect handler. Wired into ws::Server by main.
 */
#pragma once

#include "wshttp.h"

/**
 * @brief Handle one HTTP request.
 * @param req The parsed request.
 * @return The response (404 for unknown routes).
 */
ws::Response route(const ws::Request& req);

/**
 * @brief Called when a client finishes the WebSocket handshake.
 * @param c The new client; the current state is sent to it as a "connected" event.
 */
void on_ws_connect(const ws::ClientPtr& c);
