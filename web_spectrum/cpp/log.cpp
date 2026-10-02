#include "log.h"

#include <cstdio>
#include <mutex>

namespace {

std::mutex g_mtx;
std::vector<std::string> g_ring;
const size_t kMaxLines = 1000;

}  // namespace

void logmsg(const std::string& message) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_ring.size() > kMaxLines) {
        g_ring.erase(g_ring.begin());
    }
    g_ring.push_back(message);
    fprintf(stderr, "%s\n", message.c_str());
}

std::vector<std::string> log_recent(size_t count) {
    std::lock_guard<std::mutex> lk(g_mtx);
    size_t start = g_ring.size() > count ? g_ring.size() - count : 0;
    return std::vector<std::string>(g_ring.begin() + start, g_ring.end());
}
