#include "calibration.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>

#include "broadcast.h"
#include "json.h"
#include "log.h"

namespace {

std::string g_cal_dir = "calibration";
std::string g_cal_file = g_cal_dir + "/fobos_calibration.json";

// Running orchestrator process + the thread reading its stdout.
std::mutex g_mtx;
pid_t g_pid = -1;
std::thread g_reader;
std::atomic<bool> g_running{false};

/// @brief Reader thread: forward the orchestrator's stdout lines as cal_progress events,
///        then reap the process and emit a terminal event if it never reported "done".
/// @param rfd Read end of the pipe from the child's stdout/stderr.
/// @param pid The orchestrator process id (waited on here).
void cal_reader(int rfd, pid_t pid) {
    std::string buf;
    char tmp[4096];
    bool saw_done = false;
    ssize_t k;
    while ((k = read(rfd, tmp, sizeof(tmp))) > 0) {
        buf.append(tmp, k);
        size_t nl;
        while ((nl = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            if (line.empty()) {
                continue;
            }
            // Orchestrator prints JSON objects; wrap any stray (stderr) text as a message.
            std::string data = (line[0] == '{') ? line : ("{\"msg\":" + js::quote(line) + "}");
            if (line.find("\"done\"") != std::string::npos) {
                saw_done = true;
            }
            emit_event("cal_progress", data);
        }
    }
    close(rfd);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!saw_done) {
        emit_event("cal_progress",
                   "{\"done\":true,\"ok\":false,\"msg\":\"orchestrator exited unexpectedly\"}");
    }
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pid = -1;
    }
    g_running = false;
    logmsg("calibration: orchestrator finished");
}

}  // namespace

void resolve_cal_dir() {
    for (const char* c : {"calibration", "../calibration"}) {
        std::ifstream t(std::string(c) + "/calib_orchestrator.py");
        if (t) {
            g_cal_dir = c;
            break;
        }
    }
    g_cal_file = g_cal_dir + "/fobos_calibration.json";
    logmsg("spectrumd: calibration dir = " + g_cal_dir);
}

const std::string& cal_file() {
    return g_cal_file;
}

bool cal_start(const std::string& config_json, std::string& err) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_running) {
        err = "a calibration is already running";
        return false;
    }
    std::string cfg = "/tmp/fobos_calcfg.json";
    {
        std::ofstream f(cfg);
        if (!f) {
            err = "cannot write config";
            return false;
        }
        f << config_json;
    }
    std::string orch = g_cal_dir + "/calib_orchestrator.py";  // build before fork (heap-safe)
    int pfd[2];
    if (pipe(pfd) != 0) {
        err = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        close(pfd[0]);
        close(pfd[1]);
        return false;
    }
    if (pid == 0) {  // child
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[0]);
        close(pfd[1]);
        setpgid(0, 0);  // own process group for a clean group-kill
        execlp("python3", "python3", "-u", orch.c_str(), cfg.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(pfd[1]);
    g_pid = pid;
    g_running = true;
    if (g_reader.joinable()) {
        g_reader.detach();
    }
    g_reader = std::thread(cal_reader, pfd[0], pid);
    logmsg("calibration: orchestrator started pid=" + std::to_string(pid));
    return true;
}

void cal_abort() {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_pid > 0) {
        kill(-g_pid, SIGTERM);
        logmsg("calibration: SIGTERM sent");
    }
}

bool gen_probe(const std::string& ip, int port, std::string& info) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        info = "socket error";
        return false;
    }
    timeval tv{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) <= 0) {
        info = "bad IP";
        ::close(fd);
        return false;
    }
    if (::connect(fd, (sockaddr*)&a, sizeof(a)) != 0) {
        info = "connect timeout/refused";
        ::close(fd);
        return false;
    }
    std::string req = "GET /remoteconfig HTTP/1.1\r\nHost: " + ip + "\r\nConnection: close\r\n\r\n";
    ::send(fd, req.data(), req.size(), MSG_NOSIGNAL);
    char b[256];
    ssize_t n = ::recv(fd, b, sizeof(b) - 1, 0);
    ::close(fd);
    if (n > 0) {
        std::string s(b, n);
        info = s.substr(0, s.find("\r\n"));
        return true;
    }
    info = "TCP open (no HTTP reply)";
    return true;
}
