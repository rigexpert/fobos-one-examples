#include "frame.h"

#include <cmath>

namespace {
const int kPowerOffset = 215;
}  // namespace

void Buf::bytes(const void* p, size_t n) {
    d.append((const char*)p, n);
}

void Buf::u8(uint8_t v) {
    d.push_back((char)v);
}

void Buf::u32(uint32_t v) {
    bytes(&v, 4);
}

void Buf::f32(float v) {
    bytes(&v, 4);
}

void Buf::f64(double v) {
    bytes(&v, 8);
}

uint8_t db_to_u8(double db) {
    int v = (int)std::lround(db) + kPowerOffset;
    if (v < 0) {
        v = 0;
    } else if (v > 255) {
        v = 255;
    }
    return (uint8_t)v;
}

void write_header(Buf& b, uint8_t mode, uint32_t fft_size, uint32_t n_bins,
                  uint32_t n_steps, uint32_t crop_lo, uint32_t crop_keep,
                  double center, double rate, double carrier_freq, float carrier_dbfs) {
    b.u8('S');
    b.u8('P');
    b.u8('C');
    b.u8('2');
    b.u8(mode);
    b.u8(0);
    b.u8(0);
    b.u8(0);
    b.u32(fft_size);
    b.u32(n_bins);
    b.u32(n_steps);
    b.u32(crop_lo);
    b.u32(crop_keep);
    b.f64(center);
    b.f64(rate);
    b.f64(carrier_freq);
    b.f32(carrier_dbfs);
    b.u32(0);
}
