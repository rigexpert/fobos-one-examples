#include "worker.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

#include "broadcast.h"
#include "dsp.h"
#include "fobos.h"
#include "frame.h"
#include "log.h"
#include "state.h"
#include "timeutil.h"

namespace {

// libusb buffer sizing; scan mode requires >= 65536.
const uint32_t kScanBufLen = 65536;
const uint32_t kFftBufLen = 65536;
const double kEmitMinInterval = 1.0 / 25.0;  // 25 fps cap

// Worker control. g_stop / g_hwapply are polled by the read callbacks; g_life_mtx
// serializes start/stop/detach so overlapping requests can't race on g_rx.
std::atomic<bool> g_stop{false};
std::atomic<bool> g_hwapply{false};
// FFTs averaged per emitted frame, and the window overlap percent that sets the FFT
// stride. Held outside FftCtx so /api/params can retune them mid-stream; the read
// callback re-reads both per callback.
std::atomic<int> g_accum_n{4};
std::atomic<int> g_overlap{0};
std::thread g_rx;
std::mutex g_life_mtx;

// Last spectrum meta (for REST /api/spectrum).
std::mutex g_last_mtx;
std::string g_last_json;

/// @brief Store the latest spectrum meta JSON for GET /api/spectrum. @param j The JSON.
void set_last_spectrum(const std::string& j) {
    std::lock_guard<std::mutex> lk(g_last_mtx);
    g_last_json = j;
}

// ── FFT worker ──────────────────────────────────────────────────────────────
/// @brief Per-run state for the FFT worker: plans, window, accumulator, and leftovers.
struct FftCtx {
    int fft_size = 0;   ///< FFT length.
    std::vector<float> win;  ///< Window coefficients.
    double win_power = 1;    ///< Sum of squared window (normalization).
    FFT fft;   ///< Display FFT (size fft_size).
    FFT cfft;  ///< Scratch FFT for the coherent carrier meter.
    std::vector<double> psd_acc;  ///< Power accumulator, fftshifted (DC at center).
    std::vector<std::complex<float>> leftover;  ///< Samples spanning callback boundaries.
    int frames = 0;      ///< FFTs accumulated so far.
    double last_emit = 0;  ///< mono() time of the last emitted frame.
};
FftCtx* g_fft = nullptr;  ///< Active FFT context (valid only while fft_worker runs).

/// @brief Convert the accumulated PSD to dB, broadcast it, and publish the meta JSON.
/// @param c The FFT context (psd_acc holds exactly c.frames averaged FFTs).
/// @param buf Raw callback buffer, for the coherent carrier meter. @param n Complex samples.
void emit_fft_frame(FftCtx& c, float* buf, int n) {
    int N = c.fft_size;
    double cur_freq, cur_rate, dcw;
    bool dcrej;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        cur_freq = st.freq;
        cur_rate = st.rate;
        dcrej = st.dc_reject;
        dcw = st.dc_reject_hz;
    }
    std::vector<float> psd_db(N);
    double norm = c.frames * c.win_power;
    for (int j = 0; j < N; j++) {
        psd_db[j] = (float)(10.0 * std::log10(c.psd_acc[j] / norm + 1e-30));
    }
    if (dcrej) {
        notch_dc(psd_db, N, cur_rate, dcw);
    }

    double cdbfs, cfreq;
    carrier_meter(c.cfft, buf, n, cur_rate, cur_freq, &cdbfs, &cfreq);

    Buf b;
    write_header(b, 0, (uint32_t)N, (uint32_t)N, 0, 0, 0,
                 cur_freq, cur_rate, cfreq, (float)cdbfs);
    for (int j = 0; j < N; j++) {
        b.u8(db_to_u8(psd_db[j]));
    }
    broadcast_spectrum(b.d.data(), b.d.size());

    double peak = -1e9;
    double sum = 0;
    for (float v : psd_db) {
        if (v > peak) {
            peak = v;
        }
        sum += v;
    }
    std::ostringstream mj;
    mj << "{\"ts\":" << js::num_to_str(now_wall())
       << ",\"center\":" << js::num_to_str(cur_freq)
       << ",\"rate\":" << js::num_to_str(cur_rate)
       << ",\"fft_size\":" << N
       << ",\"accum_n\":" << c.frames
       << ",\"peak_db\":" << js::num_to_str(peak)
       << ",\"avg_db\":" << js::num_to_str(sum / N)
       << ",\"carrier_dbfs\":" << (std::isnan(cdbfs) ? "null" : js::num_to_str(cdbfs))
       << ",\"carrier_freq\":" << (std::isnan(cfreq) ? "null" : js::num_to_str(cfreq))
       << "}";
    set_last_spectrum(mj.str());
}

/// @brief libfobos read callback (FFT mode): accumulate windowed FFTs and emit frames.
///
/// One callback delivers kFftBufLen samples, i.e. many FFTs at once, so the accumulation
/// gate lives inside the FFT loop: a frame is emitted every accum_n FFTs exactly. Testing
/// the count once per callback instead would quantise accum_n to the callback batch size
/// (kFftBufLen/hop -- 16 FFTs at N=4096), making every setting below that identical.
///
/// @param buf Interleaved I/Q floats. @param buf_length Complex sample count.
/// @param sender The device (for cancel). Runs on the async read thread.
void fft_cb(float* buf, uint32_t buf_length, fobos_sdr_dev_t* sender, void*) {
    if (g_stop || g_hwapply) {
        fobos_sdr_cancel_async(sender);
        return;
    }
    FftCtx& c = *g_fft;
    int n = (int)buf_length;
    int N = c.fft_size;
    // Both picked up live so the UI's Avg-frames and Overlap controls apply without
    // restarting the stream. Overlap sets the stride between successive windows.
    int accum_n = std::max(1, g_accum_n.load(std::memory_order_relaxed));
    int overlap = std::min(95, std::max(0, g_overlap.load(std::memory_order_relaxed)));
    int hop = std::max(1, N - (int)((long)N * overlap / 100));

    size_t base = c.leftover.size();
    c.leftover.resize(base + n);
    for (int i = 0; i < n; i++) {
        c.leftover[base + i] = std::complex<float>(buf[2 * i], buf[2 * i + 1]);
    }

    size_t pos = 0;
    auto& L = c.leftover;
    while (pos + N <= L.size()) {
        for (int i = 0; i < N; i++) {
            c.fft.in[i][0] = L[pos + i].real() * c.win[i];
            c.fft.in[i][1] = L[pos + i].imag() * c.win[i];
        }
        fftwf_execute(c.fft.plan);
        for (int k = 0; k < N; k++) {
            double re = c.fft.out[k][0];
            double im = c.fft.out[k][1];
            c.psd_acc[(k + N / 2) % N] += re * re + im * im;
        }
        c.frames++;
        pos += hop;

        if (c.frames >= accum_n) {
            // The display is capped at kEmitMinInterval, so a group that lands inside the
            // cap window is dropped rather than merged into the next one: merging would
            // average more than the accum_n the user asked for.
            double t = mono();
            if (t - c.last_emit >= kEmitMinInterval) {
                c.last_emit = t;
                emit_fft_frame(c, buf, n);
            }
            std::fill(c.psd_acc.begin(), c.psd_acc.end(), 0.0);
            c.frames = 0;
        }
    }
    L.erase(L.begin(), L.begin() + pos);
}

/// @brief FFT-mode worker thread: apply settings and run the async read loop, restarting
///        on live parameter changes until stopped.
void fft_worker() {
    int fft_size, accum_n, overlap;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        fft_size = st.fft_size;
        accum_n = st.accum_n;
        overlap = st.overlap;
    }

    FftCtx ctx;
    ctx.fft_size = fft_size;
    g_accum_n.store(std::max(1, accum_n), std::memory_order_relaxed);
    g_overlap.store(std::max(0, overlap), std::memory_order_relaxed);
    ctx.win = hamming(fft_size);
    ctx.win_power = 0;
    for (float w : ctx.win) {
        ctx.win_power += (double)w * w;
    }
    ctx.fft.ensure(fft_size);
    ctx.psd_acc.assign(fft_size, 0.0);
    g_fft = &ctx;

    fobos_sdr_dev_t* dev;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dev = st.dev;
    }

    while (!g_stop) {
        g_hwapply = false;
        ctx.leftover.clear();
        std::fill(ctx.psd_acc.begin(), ctx.psd_acc.end(), 0.0);
        ctx.frames = 0;
        double cf, cr;
        int cl, cv;
        {
            std::lock_guard<std::mutex> lk(st_mtx);
            cf = st.freq;
            cr = st.rate;
            cl = st.lna;
            cv = st.vga;
        }
        logmsg("fft_worker: apply freq=" + js::num_to_str(cf / 1e6) + "MHz rate=" +
               js::num_to_str(cr / 1e6) + "M lna=" + std::to_string(cl) +
               " vga=" + std::to_string(cv));
        force_gain(dev, cl, cv);
        fobos_sdr_set_samplerate(dev, cr);
        fobos_sdr_set_frequency(dev, cf);

        int r = fobos_sdr_read_async(dev, fft_cb, nullptr, 16, kFftBufLen);
        if (r != 0 && !g_stop && !g_hwapply) {
            emit_error("fft async error " + std::to_string(r));
            break;
        }
    }
    g_fft = nullptr;
    logmsg("fft_worker: exit");
}

// ── Scan worker ─────────────────────────────────────────────────────────────
/// @brief Per-run state for the scan worker: one accumulator/window per scan tile.
struct ScanCtx {
    int n_steps = 0;    ///< Number of scan tiles.
    int fft_size = 0;   ///< Per-tile FFT length.
    int accum_n = 4;    ///< FFTs averaged per tile before emitting.
    int hop = 1;        ///< Slide between successive FFT windows (samples).
    std::vector<double> freqs;  ///< Tile center frequencies, Hz.
    std::vector<float> win;     ///< Window coefficients.
    double win_power = 1;       ///< Sum of squared window (normalization).
    FFT fft;                    ///< FFT reused across tiles.
    std::vector<std::vector<double>> psd_acc;  ///< Power accumulator per tile [step][bin].
    std::vector<int> fc_count;  ///< FFTs accumulated per tile.
    std::vector<std::vector<std::complex<float>>> leftovers;  ///< Per-tile leftover samples.
    double rate = 5e6;          ///< Current sample rate, Hz.
};
ScanCtx* g_scan = nullptr;  ///< Active scan context (valid only while scan_worker runs).

/// @brief libfobos read callback (scan mode): accumulate the current tile, emit a tiled
///        frame once every tile has enough FFTs.
/// @param buf Interleaved I/Q floats. @param buf_length Complex sample count.
/// @param sender The device (for scan index / cancel).
void scan_cb(float* buf, uint32_t buf_length, fobos_sdr_dev_t* sender, void*) {
    if (g_stop || g_hwapply) {
        fobos_sdr_cancel_async(sender);
        return;
    }
    if (fobos_sdr_is_scanning(sender) != 1) {
        fobos_sdr_cancel_async(sender);
        return;
    }
    int idx = fobos_sdr_get_scan_index(sender);
    ScanCtx& c = *g_scan;
    if (idx < 0 || idx >= c.n_steps) {
        return;
    }

    int n = (int)buf_length;
    int N = c.fft_size;
    int hop = c.hop;
    auto& L = c.leftovers[idx];
    size_t base = L.size();
    L.resize(base + n);
    for (int i = 0; i < n; i++) {
        L[base + i] = std::complex<float>(buf[2 * i], buf[2 * i + 1]);
    }

    size_t pos = 0;
    int nf = 0;
    auto& acc = c.psd_acc[idx];
    while (pos + N <= L.size()) {
        for (int i = 0; i < N; i++) {
            c.fft.in[i][0] = L[pos + i].real() * c.win[i];
            c.fft.in[i][1] = L[pos + i].imag() * c.win[i];
        }
        fftwf_execute(c.fft.plan);
        for (int k = 0; k < N; k++) {
            double re = c.fft.out[k][0];
            double im = c.fft.out[k][1];
            acc[(k + N / 2) % N] += re * re + im * im;
        }
        nf++;
        pos += hop;
    }
    L.erase(L.begin(), L.begin() + pos);
    c.fc_count[idx] += nf;

    for (int cc : c.fc_count) {
        if (cc < c.accum_n) {
            return;  // wait until every tile has accumulated enough
        }
    }

    double rate = c.rate;
    double step_spacing = c.n_steps > 1 ? (c.freqs[1] - c.freqs[0]) : rate;
    int keep = (int)std::lround(std::fabs(step_spacing) / rate * N);
    keep = std::max(1, std::min(N, keep));
    int lo = (N - keep) / 2;
    bool dcrej;
    double dcw;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dcrej = st.dc_reject;
        dcw = st.dc_reject_hz;
    }

    Buf b;
    write_header(b, 1, (uint32_t)N, (uint32_t)(c.n_steps * keep),
                 (uint32_t)c.n_steps, (uint32_t)lo, (uint32_t)keep,
                 0.0, rate, std::nan(""), std::nan(""));
    for (int i = 0; i < c.n_steps; i++) {
        b.f64(c.freqs[i]);
    }
    for (int i = 0; i < c.n_steps; i++) {
        std::vector<float> psd_db(N);
        double denom = c.fc_count[i] * c.win_power;
        for (int j = 0; j < N; j++) {
            psd_db[j] = (float)(10.0 * std::log10(c.psd_acc[i][j] / denom + 1e-30));
        }
        if (dcrej) {
            notch_dc(psd_db, N, rate, dcw);
        }
        for (int j = lo; j < lo + keep; j++) {
            b.u8(db_to_u8(psd_db[j]));
        }
    }
    broadcast_spectrum(b.d.data(), b.d.size());

    for (int i = 0; i < c.n_steps; i++) {
        std::fill(c.psd_acc[i].begin(), c.psd_acc[i].end(), 0.0);
        c.fc_count[i] = 0;
    }
}

/// @brief Scan-mode worker thread: build the tile plan, then start_scan + async read,
///        restarting on live gain/rate changes until stopped.
void scan_worker() {
    ScanCtx ctx;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        ctx.freqs = st.scan_freqs;
        ctx.fft_size = st.fft_size;
        ctx.accum_n = std::max(1, st.accum_n);
        ctx.hop = std::max(1, st.fft_size - (int)((long)st.fft_size * st.scan_overlap / 100));
    }
    ctx.n_steps = (int)ctx.freqs.size();
    if (ctx.n_steps < 2) {
        emit_error("scan needs at least 2 frequencies");
        return;
    }
    if (ctx.n_steps > 256) {
        emit_error("scan needs " + std::to_string(ctx.n_steps) +
                   " tiles but the hardware limit is 256 — raise the sample rate "
                   "(wider tiles) or narrow the scan range");
        return;
    }
    ctx.win = hamming(ctx.fft_size);
    ctx.win_power = 0;
    for (float w : ctx.win) {
        ctx.win_power += (double)w * w;
    }
    ctx.fft.ensure(ctx.fft_size);
    ctx.psd_acc.assign(ctx.n_steps, std::vector<double>(ctx.fft_size, 0.0));
    ctx.fc_count.assign(ctx.n_steps, 0);
    ctx.leftovers.assign(ctx.n_steps, {});
    g_scan = &ctx;

    fobos_sdr_dev_t* dev;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dev = st.dev;
    }

    while (!g_stop) {
        g_hwapply = false;
        int cl, cv;
        double cr;
        {
            std::lock_guard<std::mutex> lk(st_mtx);
            cl = st.lna;
            cv = st.vga;
            cr = st.rate;
        }
        ctx.rate = cr;
        force_gain(dev, cl, cv);
        fobos_sdr_set_samplerate(dev, cr);
        fobos_sdr_set_auto_bandwidth(dev, 0.8);
        for (int i = 0; i < ctx.n_steps; i++) {
            std::fill(ctx.psd_acc[i].begin(), ctx.psd_acc[i].end(), 0.0);
            ctx.fc_count[i] = 0;
            ctx.leftovers[i].clear();
        }
        logmsg("scan_worker: apply lna=" + std::to_string(cl) + " vga=" + std::to_string(cv) +
               " rate=" + js::num_to_str(cr / 1e6) + "M steps=" + std::to_string(ctx.n_steps));
        std::vector<double> farr = ctx.freqs;
        int r = fobos_sdr_start_scan(dev, farr.data(), (unsigned)ctx.n_steps);
        if (r != 0) {
            emit_error("start_scan error " + std::to_string(r));
            break;
        }
        r = fobos_sdr_read_async(dev, scan_cb, nullptr, 16, kScanBufLen);
        fobos_sdr_stop_scan(dev);
        if (r != 0 && !g_stop && !g_hwapply) {
            emit_error("read_async error " + std::to_string(r));
            break;
        }
    }
    g_scan = nullptr;
    logmsg("scan_worker: exit");
}

}  // namespace

// ── Public lifecycle API ────────────────────────────────────────────────────
void signal_stop() {
    g_stop = true;
    fobos_sdr_dev_t* dev;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dev = st.dev;
    }
    if (dev) {
        fobos_sdr_cancel_async(dev);
    }
}

void join_worker() {
    if (g_rx.joinable()) {
        g_rx.join();
    }
}

std::string last_spectrum_json() {
    std::lock_guard<std::mutex> lk(g_last_mtx);
    return g_last_json;
}

std::string start_action(const js::ValuePtr& body) {
    std::lock_guard<std::mutex> life(g_life_mtx);
    signal_stop();
    join_worker();

    std::string mode;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        if (body) {
            if (body->has("freq")) {
                st.freq = body->num_or("freq", st.freq);
            }
            if (body->has("rate")) {
                st.rate = body->num_or("rate", st.rate);
            }
            if (body->has("scan_from")) {
                st.scan_from = body->num_or("scan_from", st.scan_from);
            }
            if (body->has("scan_to")) {
                st.scan_to = body->num_or("scan_to", st.scan_to);
            }
            if (body->has("lna")) {
                st.lna = std::min(LNA_MAX, std::max(0, (int)body->num_or("lna", st.lna)));
            }
            if (body->has("vga")) {
                st.vga = std::min(VGA_MAX, std::max(0, (int)body->num_or("vga", st.vga)));
            }
            if (body->has("fft_size")) {
                st.fft_size = std::max(0, (int)body->num_or("fft_size", st.fft_size));
            }
            if (body->has("overlap")) {
                st.overlap = std::max(0, (int)body->num_or("overlap", st.overlap));
            }
            if (body->has("scan_overlap")) {
                st.scan_overlap = std::max(0, (int)body->num_or("scan_overlap", st.scan_overlap));
            }
            if (body->has("accum_n")) {
                st.accum_n = std::max(1, (int)body->num_or("accum_n", st.accum_n));
            }
            if (body->has("mode")) {
                st.mode = body->str_or("mode", st.mode);
            }
            js::ValuePtr sf = body->get("scan_freqs");
            if (sf && sf->is_arr()) {
                st.scan_freqs.clear();
                for (auto& e : sf->arr) {
                    if (e) {
                        st.scan_freqs.push_back(e->num);
                    }
                }
            }
            if (body->has("dc_reject")) {
                st.dc_reject = body->bool_or("dc_reject", st.dc_reject);
            }
            if (body->has("dc_reject_hz")) {
                st.dc_reject_hz = std::max(0.0, body->num_or("dc_reject_hz", st.dc_reject_hz));
            }
        }
        mode = st.mode;
        st.running = true;
    }
    save_state();
    g_stop = false;
    g_rx = std::thread(mode == "fft" ? fft_worker : scan_worker);
    emit_event("started", "{\"mode\":" + js::quote(mode) + "}");
    logmsg("start: worker started mode=" + mode);
    return mode;
}

void params_action(const js::ValuePtr& body) {
    fobos_sdr_dev_t* dev;
    bool running;
    double freq, rate;
    int lna, vga;
    bool has_freq = false, has_rate = false, has_lna = false, has_vga = false;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        if (body) {
            if ((has_freq = body->has("freq"))) {
                st.freq = body->num_or("freq", st.freq);
            }
            if ((has_rate = body->has("rate"))) {
                st.rate = body->num_or("rate", st.rate);
            }
            if ((has_lna = body->has("lna"))) {
                // Clamp to the library's own range so the state we echo back and persist
                // matches what the hardware actually got.
                st.lna = std::min(LNA_MAX, std::max(0, (int)body->num_or("lna", st.lna)));
            }
            if ((has_vga = body->has("vga"))) {
                st.vga = std::min(VGA_MAX, std::max(0, (int)body->num_or("vga", st.vga)));
            }
            if (body->has("accum_n")) {
                // Applies mid-stream: the FFT callback re-reads g_accum_n, so no restart.
                st.accum_n = std::max(1, (int)body->num_or("accum_n", st.accum_n));
                g_accum_n.store(st.accum_n, std::memory_order_relaxed);
            }
            if (body->has("overlap")) {
                st.overlap = std::max(0, (int)body->num_or("overlap", st.overlap));
                g_overlap.store(st.overlap, std::memory_order_relaxed);
            }
            if (body->has("dc_reject")) {
                st.dc_reject = body->bool_or("dc_reject", st.dc_reject);
            }
            if (body->has("dc_reject_hz")) {
                st.dc_reject_hz = std::max(0.0, body->num_or("dc_reject_hz", st.dc_reject_hz));
            }
        }
        dev = st.dev;
        running = st.running;
        freq = st.freq;
        rate = st.rate;
        lna = st.lna;
        vga = st.vga;
    }
    if (dev) {
        if (running) {
            // Streaming: let the worker re-apply from its own thread; break the read.
            g_hwapply = true;
            fobos_sdr_cancel_async(dev);
        } else {
            if (has_freq) {
                fobos_sdr_set_frequency(dev, freq);
            }
            if (has_rate) {
                fobos_sdr_set_samplerate(dev, rate);
            }
            if (has_lna) {
                fobos_sdr_set_lna_gain(dev, (unsigned)lna);
            }
            if (has_vga) {
                fobos_sdr_set_vga_gain(dev, (unsigned)vga);
            }
        }
    }
    save_state();
    std::ostringstream o;
    o << "{\"freq\":" << js::num_to_str(freq) << ",\"rate\":" << js::num_to_str(rate)
      << ",\"lna\":" << lna << ",\"vga\":" << vga << "}";
    emit_event("params_updated", o.str());
}
