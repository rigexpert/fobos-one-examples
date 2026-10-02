#include "backend_switch.h"

#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <fstream>
#include <thread>

#include "calibration.h"
#include "fobos.h"
#include "log.h"

namespace {
const char* kBackendConf = "backend.conf";
}  // namespace

std::string read_backend_conf() {
    std::ifstream f(kBackendConf);
    if (!f) {
        return "cpp";
    }
    std::string s;
    std::getline(f, s);
    s.erase(std::remove_if(s.begin(), s.end(), ::isspace), s.end());
    return s.empty() ? "cpp" : s;
}

void write_backend_conf(const std::string& backend) {
    std::ofstream f(kBackendConf);
    f << backend << "\n";
}

void schedule_reexec() {
    std::thread([] {
        logmsg("backend switch: exiting for systemd restart into new backend");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));  // flush HTTP reply
        cal_abort();
        dev_close();  // release the USB device
        _exit(0);
    }).detach();
}
