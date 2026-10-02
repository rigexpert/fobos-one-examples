#include "routes.h"

#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "backend_switch.h"
#include "broadcast.h"
#include "calibration.h"
#include "fobos.h"
#include "json.h"
#include "log.h"
#include "state.h"
#include "timeutil.h"
#include "worker.h"

namespace {

/// @brief Read a whole file. @param path File path. @return Its bytes, or "" if unreadable.
std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return std::string();
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// @brief Guess a Content-Type from a file extension.
/// @param path File path. @return A MIME type (octet-stream if unknown).
std::string mime_for(const std::string& path) {
    auto ends_with = [&](const char* ext) {
        size_t n = strlen(ext);
        return path.size() > n && path.compare(path.size() - n, n, ext) == 0;
    };
    if (ends_with(".js")) {
        return "application/javascript";
    }
    if (ends_with(".css")) {
        return "text/css";
    }
    if (ends_with(".html")) {
        return "text/html";
    }
    if (ends_with(".json")) {
        return "application/json";
    }
    return "application/octet-stream";
}

/// @brief First value of a key in a URL query string.
/// @param q Query string. @param key Parameter name. @return The value, or "".
std::string query_get(const std::string& q, const std::string& key) {
    size_t p = 0;
    while (p < q.size()) {
        size_t amp = q.find('&', p);
        std::string kv = q.substr(p, amp == std::string::npos ? std::string::npos : amp - p);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key) {
            return kv.substr(eq + 1);
        }
        if (amp == std::string::npos) {
            break;
        }
        p = amp + 1;
    }
    return "";
}

/// @brief Build a JSON array of the device's supported sample rates. @return The array text.
std::string rates_json() {
    std::ostringstream o;
    o << "[";
    std::vector<double> rates = get_samplerates();
    for (size_t i = 0; i < rates.size(); i++) {
        if (i) {
            o << ",";
        }
        o << js::num_to_str(rates[i]);
    }
    o << "]";
    return o.str();
}

// ── GET handlers ────────────────────────────────────────────────────────────
/// @brief GET / — serve the single-page UI. @return The index.html response.
ws::Response get_index() {
    ws::Response r;
    r.body = read_file("templates/index.html");
    r.content_type = "text/html";
    // The page is read from disk per request, so a redeploy takes effect immediately --
    // but we send no validators, and without them browsers cache heuristically and serve
    // a stale UI after an upgrade. Tell them not to.
    r.extra_headers.push_back({"Cache-Control", "no-store, must-revalidate"});
    if (r.body.empty()) {
        r.status = 404;
        r.body = "index.html not found";
    }
    return r;
}

/// @brief GET /static/... — serve a static asset (path-traversal rejected).
/// @param path Request path. @return The asset response (404 if missing).
ws::Response get_static(const std::string& path) {
    ws::Response r;
    std::string rel = path.substr(1);  // drop leading '/'
    if (rel.find("..") != std::string::npos) {
        r.status = 400;
        r.body = "bad path";
        return r;
    }
    r.body = read_file(rel);
    r.content_type = mime_for(rel);
    if (r.body.empty()) {
        r.status = 404;
        r.body = "not found";
    }
    return r;
}

/// @brief GET /api/status — current state plus an "attached" flag. @return The status JSON.
ws::Response get_status() {
    ws::Response r;
    std::lock_guard<std::mutex> lk(st_mtx);
    std::string s = state_json_locked();
    s.insert(s.size() - 1, std::string(",\"attached\":") + (st.dev ? "true" : "false"));
    r.body = s;
    return r;
}

/// @brief GET /api/calibration — the stored calibration, if any. @return {available,cal}.
ws::Response get_calibration() {
    ws::Response r;
    std::string cal = read_file(cal_file());
    if (cal.empty()) {
        r.body = "{\"available\":false}";
    } else {
        r.body = "{\"available\":true,\"cal\":" + cal + "}";
    }
    return r;
}

/// @brief GET /api/spectrum — the latest spectrum meta. @return The meta JSON (404 if none).
ws::Response get_spectrum() {
    ws::Response r;
    std::string meta = last_spectrum_json();
    if (meta.empty()) {
        r.status = 404;
        r.body = "{\"ok\":false,\"msg\":\"no data yet\"}";
    } else {
        r.body = "{\"ok\":true," + meta.substr(1);  // splice ok into the meta object
    }
    return r;
}

/// @brief GET /api/log — recent log lines. @return {"entries":[{"msg":...},...]}.
ws::Response get_log() {
    ws::Response r;
    std::ostringstream o;
    o << "{\"entries\":[";
    std::vector<std::string> lines = log_recent(100);
    for (size_t i = 0; i < lines.size(); i++) {
        if (i) {
            o << ",";
        }
        o << "{\"msg\":" << js::quote(lines[i]) << "}";
    }
    o << "]}";
    r.body = o.str();
    return r;
}

// ── POST handlers ───────────────────────────────────────────────────────────
/// @brief POST /api/attach — open the device. @return {ok,rates} or {ok:false,msg}.
ws::Response post_attach() {
    ws::Response r;
    std::string msg;
    if (!dev_open(msg)) {
        r.status = 500;
        r.body = "{\"ok\":false,\"msg\":" + js::quote(msg) + "}";
        return r;
    }
    std::string rates = rates_json();
    r.body = "{\"ok\":true,\"rates\":" + rates + "}";
    emit_event("attached", "{\"rates\":" + rates + "}");
    return r;
}

/// @brief POST /api/detach — stop the worker and close the device. @return {ok:true}.
ws::Response post_detach() {
    ws::Response r;
    signal_stop();
    join_worker();
    dev_close();
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        st.running = false;
    }
    emit_event("detached", "{}");
    r.body = "{\"ok\":true}";
    return r;
}

/// @brief POST /api/start — (re)start acquisition. @param req Request. @return {ok,mode}.
ws::Response post_start(const ws::Request& req) {
    ws::Response r;
    js::ValuePtr body = js::parse(req.body);
    std::string mode = start_action(body);
    r.body = "{\"ok\":true,\"mode\":" + js::quote(mode) + "}";
    return r;
}

/// @brief POST /api/stop — stop acquisition (?wait=false to return early).
/// @param req Request. @return {ok,elapsed_ms}.
ws::Response post_stop(const ws::Request& req) {
    ws::Response r;
    double t0 = mono();
    signal_stop();
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        st.running = false;
    }
    if (query_get(req.query, "wait") != "false") {
        join_worker();
    }
    emit_event("stopped", "{}");
    r.body = "{\"ok\":true,\"elapsed_ms\":" + std::to_string((int)((mono() - t0) * 1000)) + "}";
    return r;
}

/// @brief POST /api/params — live tune/gain change. @param req Request. @return {ok,freq,rate,lna,vga}.
ws::Response post_params(const ws::Request& req) {
    ws::Response r;
    js::ValuePtr body = js::parse(req.body);
    params_action(body);
    std::lock_guard<std::mutex> lk(st_mtx);
    std::ostringstream o;
    o << "{\"ok\":true,\"freq\":" << js::num_to_str(st.freq) << ",\"rate\":" << js::num_to_str(st.rate)
      << ",\"lna\":" << st.lna << ",\"vga\":" << st.vga << "}";
    r.body = o.str();
    return r;
}

/// @brief POST /api/cal/test_gen — probe the generator. @param req Request. @return {ok,info|msg}.
ws::Response post_cal_test_gen(const ws::Request& req) {
    ws::Response r;
    js::ValuePtr body = js::parse(req.body);
    std::string ip = body ? body->str_or("ip", "") : "";
    int port = body ? (int)body->num_or("port", 0) : 0;
    std::string info;
    bool ok = gen_probe(ip, port, info);
    r.body = std::string("{\"ok\":") + (ok ? "true" : "false") +
             (ok ? ",\"info\":" : ",\"msg\":") + js::quote(info) + "}";
    return r;
}

/// @brief POST /api/cal/start — launch the orchestrator. @param req Config JSON. @return {ok,msg}.
ws::Response post_cal_start(const ws::Request& req) {
    ws::Response r;
    std::string err;
    if (!cal_start(req.body, err)) {
        r.status = 400;
        r.body = "{\"ok\":false,\"msg\":" + js::quote(err) + "}";
    } else {
        r.body = "{\"ok\":true,\"msg\":\"started\"}";
    }
    return r;
}

/// @brief POST /api/cal/restore — write a calibration JSON straight to disk (recovery).
/// @param req Calibration JSON body. @return {ok,bytes} or an error.
ws::Response post_cal_restore(const ws::Request& req) {
    ws::Response r;
    // Write a calibration JSON straight to the file (e.g. recovered from a browser's
    // in-memory calData). Backs up whatever is there first.
    if (req.body.size() < 50 || req.body.find("freq_response") == std::string::npos) {
        r.status = 400;
        r.body = "{\"ok\":false,\"msg\":\"not a calibration object\"}";
        return r;
    }
    std::string cur = read_file(cal_file());
    if (!cur.empty()) {
        std::ofstream bak(cal_file() + ".clobbered");
        bak << cur;
    }
    std::ofstream f(cal_file());
    if (!f) {
        r.status = 500;
        r.body = "{\"ok\":false,\"msg\":\"write failed\"}";
        return r;
    }
    f << req.body;
    r.body = "{\"ok\":true,\"bytes\":" + std::to_string(req.body.size()) + "}";
    return r;
}

/// @brief POST /api/config/backend — select the backend; switching to python restarts.
/// @param req Request with {"backend":"cpp"|"python"}. @return {ok,switching,...}.
ws::Response post_set_backend(const ws::Request& req) {
    ws::Response r;
    js::ValuePtr body = js::parse(req.body);
    std::string be = body ? body->str_or("backend", "") : "";
    if (be != "cpp" && be != "python") {
        r.status = 400;
        r.body = "{\"ok\":false,\"msg\":\"backend must be cpp or python\"}";
        return r;
    }
    write_backend_conf(be);
    if (be == "cpp") {
        r.body = "{\"ok\":true,\"switching\":false,\"msg\":\"already cpp\"}";
        return r;
    }
    schedule_reexec();
    r.body = "{\"ok\":true,\"switching\":true,\"backend\":\"python\"}";
    return r;
}

}  // namespace

ws::Response route(const ws::Request& req) {
    const std::string& p = req.path;
    const std::string& m = req.method;

    if (m == "GET" && p == "/") {
        return get_index();
    }
    if (m == "GET" && p.rfind("/static/", 0) == 0) {
        return get_static(p);
    }
    if (m == "GET" && p == "/api/samplerates") {
        ws::Response r;
        r.body = "{\"rates\":" + rates_json() + "}";
        return r;
    }
    if (m == "GET" && p == "/api/status") {
        return get_status();
    }
    if (m == "GET" && p == "/api/calibration") {
        return get_calibration();
    }
    if (m == "GET" && p == "/api/spectrum") {
        return get_spectrum();
    }
    if (m == "GET" && p == "/api/log") {
        return get_log();
    }
    if (m == "GET" && p == "/api/config/backend") {
        ws::Response r;
        r.body = "{\"running\":\"cpp\",\"selected\":" + js::quote(read_backend_conf()) + "}";
        return r;
    }
    if (m == "POST" && p == "/api/attach") {
        return post_attach();
    }
    if (m == "POST" && p == "/api/detach") {
        return post_detach();
    }
    if (m == "POST" && p == "/api/start") {
        return post_start(req);
    }
    if (m == "POST" && p == "/api/stop") {
        return post_stop(req);
    }
    if (m == "POST" && p == "/api/params") {
        return post_params(req);
    }
    if (m == "POST" && p == "/api/cal/test_gen") {
        return post_cal_test_gen(req);
    }
    if (m == "POST" && p == "/api/cal/start") {
        return post_cal_start(req);
    }
    if (m == "POST" && p == "/api/cal/abort") {
        cal_abort();
        ws::Response r;
        r.body = "{\"ok\":true}";
        return r;
    }
    if (m == "POST" && p == "/api/cal/restore") {
        return post_cal_restore(req);
    }
    if (m == "POST" && p == "/api/config/backend") {
        return post_set_backend(req);
    }

    ws::Response r;
    r.status = 404;
    r.body = "{\"error\":\"not found\"}";
    return r;
}

void on_ws_connect(const ws::ClientPtr& c) {
    std::string s;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        s = state_json_locked();
    }
    std::string msg = "{\"event\":\"connected\",\"data\":{\"state\":" + s + "}}";
    ws::ws_send(*c, 0x1, msg.data(), msg.size());
}
