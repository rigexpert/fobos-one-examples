#include "fobos.h"

#include "state.h"

void force_gain(fobos_sdr_dev_t* dev, int lna, int vga) {
    // The library caches the last gain and skips the USB command when the new value
    // equals the cache — but the cache can be stale vs the hardware (notably on the
    // first set after open). Toggle to a different value first to force the write.
    fobos_sdr_set_lna_gain(dev, (unsigned)((lna + 1) % (LNA_MAX + 1)));
    fobos_sdr_set_lna_gain(dev, (unsigned)lna);
    fobos_sdr_set_vga_gain(dev, (unsigned)((vga + 1) % (VGA_MAX + 1)));
    fobos_sdr_set_vga_gain(dev, (unsigned)vga);
}

std::vector<double> get_samplerates() {
    fobos_sdr_dev_t* dev;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dev = st.dev;
    }
    if (!dev) {
        return {};
    }
    double buf[64];
    unsigned cnt = 64;
    if (fobos_sdr_get_samplerates(dev, buf, &cnt) != 0) {
        return {};
    }
    return std::vector<double>(buf, buf + cnt);
}

bool dev_open(std::string& msg) {
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        if (st.dev) {
            msg = "ok";
            return true;
        }
    }
    if (fobos_sdr_get_device_count() < 1) {
        msg = "No Fobos device found";
        return false;
    }
    fobos_sdr_dev_t* dev = nullptr;
    int r = fobos_sdr_open(&dev, 0);
    if (r != 0) {
        msg = "fobos_sdr_open error " + std::to_string(r);
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        st.dev = dev;
    }
    msg = "ok";
    return true;
}

void dev_close() {
    fobos_sdr_dev_t* dev;
    {
        std::lock_guard<std::mutex> lk(st_mtx);
        dev = st.dev;
    }
    if (!dev) {
        return;
    }
    fobos_sdr_cancel_async(dev);
    fobos_sdr_stop_scan(dev);
    fobos_sdr_close(dev);
    std::lock_guard<std::mutex> lk(st_mtx);
    st.dev = nullptr;
}
