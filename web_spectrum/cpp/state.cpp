#include "state.h"

#include <algorithm>
#include <fstream>
#include <sstream>

#include "json.h"

State st;
std::mutex st_mtx;

namespace {
const char* kStateFile = "state.json";
}  // namespace

std::string state_json_locked() {
    std::ostringstream o;
    o << "{"
      << "\"mode\":" << js::quote(st.mode)
      << ",\"freq\":" << js::num_to_str(st.freq)
      << ",\"rate\":" << js::num_to_str(st.rate)
      << ",\"lna\":" << st.lna
      << ",\"vga\":" << st.vga
      << ",\"fft_size\":" << st.fft_size
      << ",\"overlap\":" << st.overlap
      << ",\"scan_overlap\":" << st.scan_overlap
      << ",\"accum_n\":" << st.accum_n
      << ",\"scan_from\":" << js::num_to_str(st.scan_from)
      << ",\"scan_to\":" << js::num_to_str(st.scan_to)
      << ",\"dc_reject\":" << (st.dc_reject ? "true" : "false")
      << ",\"dc_reject_hz\":" << js::num_to_str(st.dc_reject_hz)
      << ",\"running\":" << (st.running ? "true" : "false")
      << ",\"scan_freqs\":[";
    for (size_t i = 0; i < st.scan_freqs.size(); i++) {
        if (i) {
            o << ",";
        }
        o << js::num_to_str(st.scan_freqs[i]);
    }
    o << "]}";
    return o.str();
}

void save_state() {
    std::lock_guard<std::mutex> lk(st_mtx);
    std::ofstream f(kStateFile);
    if (f) {
        f << state_json_locked();
    }
}

void load_state() {
    std::ifstream f(kStateFile);
    if (!f) {
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    js::ValuePtr v = js::parse(ss.str());
    if (!v) {
        return;
    }
    std::lock_guard<std::mutex> lk(st_mtx);
    st.mode = v->str_or("mode", st.mode);
    st.freq = v->num_or("freq", st.freq);
    st.rate = v->num_or("rate", st.rate);
    // Clamp on load: a state.json written before the gain range was widened (or by hand)
    // must not push an out-of-range step into the UI.
    st.lna = std::min(LNA_MAX, std::max(0, (int)v->num_or("lna", st.lna)));
    st.vga = std::min(VGA_MAX, std::max(0, (int)v->num_or("vga", st.vga)));
    st.fft_size = (int)v->num_or("fft_size", st.fft_size);
    st.overlap = (int)v->num_or("overlap", st.overlap);
    st.scan_overlap = (int)v->num_or("scan_overlap", st.scan_overlap);
    st.accum_n = (int)v->num_or("accum_n", st.accum_n);
    st.scan_from = v->num_or("scan_from", st.scan_from);
    st.scan_to = v->num_or("scan_to", st.scan_to);
    st.dc_reject = v->bool_or("dc_reject", st.dc_reject);
    st.dc_reject_hz = v->num_or("dc_reject_hz", st.dc_reject_hz);
    js::ValuePtr sf = v->get("scan_freqs");
    if (sf && sf->is_arr()) {
        st.scan_freqs.clear();
        for (auto& e : sf->arr) {
            if (e) {
                st.scan_freqs.push_back(e->num);
            }
        }
    }
}
