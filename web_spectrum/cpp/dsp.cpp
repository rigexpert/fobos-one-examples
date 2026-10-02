#include "dsp.h"

#include <algorithm>
#include <cmath>

std::mutex& FFT::plan_mutex() {
    static std::mutex m;
    return m;
}

void FFT::ensure(int size) {
    if (n == size) {
        return;
    }
    clear();
    n = size;
    in  = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * size);
    out = (fftwf_complex*)fftwf_malloc(sizeof(fftwf_complex) * size);
    std::lock_guard<std::mutex> lk(plan_mutex());
    plan = fftwf_plan_dft_1d(size, in, out, FFTW_FORWARD, FFTW_ESTIMATE);
}

void FFT::clear() {
    if (plan) {
        std::lock_guard<std::mutex> lk(plan_mutex());
        fftwf_destroy_plan(plan);
    }
    if (in) {
        fftwf_free(in);
    }
    if (out) {
        fftwf_free(out);
    }
    plan = nullptr;
    in = nullptr;
    out = nullptr;
    n = 0;
}

FFT::~FFT() {
    clear();
}

std::vector<float> hamming(int n) {
    std::vector<float> w(n);
    for (int i = 0; i < n; i++) {
        w[i] = (float)(0.54 - 0.46 * std::cos(2 * M_PI * i / (n - 1)));
    }
    return w;
}

void notch_dc(std::vector<float>& psd, int n, double rate, double width_hz) {
    if (width_hz <= 0 || rate <= 0) {
        return;
    }
    int dc = n / 2;
    int half = (int)std::lround((width_hz / 2.0) / (rate / n));
    int lo = std::max(1, dc - half);
    int hi = std::min(n - 2, dc + half);
    if (hi <= lo) {
        if (dc >= 1 && dc <= n - 2) {
            psd[dc] = 0.5f * (psd[dc - 1] + psd[dc + 1]);
        }
        return;
    }
    float a = psd[lo - 1];
    float b = psd[hi + 1];
    int span = hi - lo + 2;
    for (int k = lo; k <= hi; k++) {
        float t = (float)(k - (lo - 1)) / span;
        psd[k] = a + (b - a) * t;
    }
}

void carrier_meter(FFT& scratch, const float* iq, int n, double rate, double center_freq,
                   double* dbfs, double* freq) {
    *dbfs = std::nan("");
    *freq = std::nan("");
    if (n < 8) {
        return;
    }
    scratch.ensure(n);
    for (int i = 0; i < n; i++) {
        scratch.in[i][0] = iq[2 * i];
        scratch.in[i][1] = iq[2 * i + 1];
    }
    fftwf_execute(scratch.plan);

    std::vector<float> mag(n);
    for (int k = 0; k < n; k++) {
        mag[k] = std::hypot(scratch.out[k][0], scratch.out[k][1]);
    }

    // FFT bin index -> baseband frequency (Hz), matching numpy fftfreq order.
    auto bin_freq = [&](int k) {
        return (k < (n + 1) / 2 ? (double)k : (double)k - n) * rate / n;
    };

    // Strongest bin outside the DC/LO region.
    int kk = -1;
    float best = -1;
    for (int k = 0; k < n; k++) {
        if (std::fabs(bin_freq(k)) > 80e3 && mag[k] > best) {
            best = mag[k];
            kk = k;
        }
    }
    if (kk < 0) {
        return;
    }

    // Require the peak to stand well above the noise median.
    std::vector<float> tmp(mag);
    std::nth_element(tmp.begin(), tmp.begin() + n / 2, tmp.end());
    float median = tmp[n / 2];
    if (mag[kk] <= 8.0f * median) {
        return;
    }

    // Log-parabolic sub-bin interpolation for the exact carrier frequency.
    double l0 = std::log(mag[(kk - 1 + n) % n] + 1e-30);
    double l1 = std::log(mag[kk] + 1e-30);
    double l2 = std::log(mag[(kk + 1) % n] + 1e-30);
    double den = l0 - 2 * l1 + l2;
    double d = std::fabs(den) > 1e-9 ? std::max(-0.5, std::min(0.5, 0.5 * (l0 - l2) / den)) : 0.0;
    double f_off = bin_freq(kk) + d * (rate / n);

    // Coherent DFT at that exact frequency -> amplitude (no scalloping).
    double sr = 0;
    double si = 0;
    for (int i = 0; i < n; i++) {
        double ph = -2 * M_PI * f_off * i / rate;
        double cr = std::cos(ph);
        double ci = std::sin(ph);
        double xr = iq[2 * i];
        double xi = iq[2 * i + 1];
        sr += xr * cr - xi * ci;
        si += xr * ci + xi * cr;
    }
    sr /= n;
    si /= n;
    double amp = std::hypot(sr, si);
    *dbfs = 20 * std::log10(amp + 1e-30);
    *freq = center_freq + f_off;
}
