#include "broadcast.h"

#include "json.h"
#include "wshttp.h"

namespace {
ws::Server* g_server = nullptr;
}  // namespace

void broadcast_set_server(ws::Server* server) {
    g_server = server;
}

void emit_event(const std::string& event, const std::string& data_json) {
    if (!g_server) {
        return;
    }
    std::string msg = "{\"event\":" + js::quote(event) + ",\"data\":" + data_json + "}";
    g_server->broadcast_text(msg);
}

void emit_error(const std::string& msg) {
    emit_event("error", "{\"msg\":" + js::quote(msg) + "}");
}

void broadcast_spectrum(const void* data, size_t len) {
    if (g_server) {
        g_server->broadcast_binary(data, len);
    }
}
