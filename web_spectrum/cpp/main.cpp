// FobosOne Web Spectrum Analyzer — native C++ backend entry point.
//
// True multithreading (no GIL): the DSP worker runs on its own core while the
// HTTP/WebSocket server serves clients on others. Spectrum frames are pushed as compact
// binary (SPC2, 1 byte/bin, permessage-deflate). Control is REST; the WebSocket is
// push-only (text status events + binary spectrum frames).
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <string>
#include <thread>

#include "broadcast.h"
#include "calibration.h"
#include "fobos.h"
#include "log.h"
#include "routes.h"
#include "state.h"
#include "wshttp.h"

namespace {

/// @brief Parse the listen port from the command line (--port N or --port=N).
/// @param argc Argument count. @param argv Arguments. @return The port (default 8080).
int parse_port(int argc, char** argv) {
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--port" && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (a.rfind("--port=", 0) == 0) {
            port = atoi(a.c_str() + 7);
        }
    }
    return port;
}

}  // namespace

/// @brief Program entry point: load state, resolve paths, start the server, then park.
/// @param argc Argument count. @param argv Arguments. @return Process exit code.
int main(int argc, char** argv) {
    int port = parse_port(argc, argv);

    load_state();
    resolve_cal_dir();
    logmsg("spectrumd: fobos devices found = " + std::to_string(fobos_sdr_get_device_count()));

    ws::Server server(port, route, on_ws_connect);
    broadcast_set_server(&server);
    if (!server.start()) {
        fprintf(stderr, "failed to bind port %d\n", port);
        return 1;
    }
    logmsg("spectrumd: listening on 0.0.0.0:" + std::to_string(port));

    // Park the main thread; all work happens on the accept/worker threads.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(3600));
    }
    return 0;
}
