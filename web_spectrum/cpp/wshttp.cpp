#include "wshttp.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/sha.h>
#include <zlib.h>

#include <cctype>
#include <cstring>

namespace ws {

namespace {

/// @brief Base64-encode a byte buffer (for the WebSocket accept key).
/// @param d Bytes to encode. @param n Number of bytes. @return The base64 text.
std::string b64(const unsigned char* d, size_t n) {
    static const char* T =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16;
        if (i + 1 < n) {
            v |= d[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= d[i + 2];
        }
        o += T[(v >> 18) & 63];
        o += T[(v >> 12) & 63];
        o += (i + 1 < n) ? T[(v >> 6) & 63] : '=';
        o += (i + 2 < n) ? T[v & 63] : '=';
    }
    return o;
}

/// @brief Raw-deflate a buffer for permessage-deflate, independently per message.
///
/// No context takeover, so any frame decodes standalone — safe with mid-stream joins and
/// dropped frames. Strips the trailing 00 00 FF FF that Z_SYNC_FLUSH appends.
/// @param data Payload bytes. @param len Payload length. @return The compressed bytes.
std::string raw_deflate(const void* data, size_t len) {
    z_stream zs;
    std::memset(&zs, 0, sizeof(zs));
    if (deflateInit2(&zs, 1, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::string();
    }
    zs.next_in = (Bytef*)data;
    zs.avail_in = (uInt)len;
    std::string out;
    char buf[16384];
    do {
        zs.next_out = (Bytef*)buf;
        zs.avail_out = sizeof(buf);
        deflate(&zs, Z_SYNC_FLUSH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (zs.avail_out == 0);
    deflateEnd(&zs);
    if (out.size() >= 4) {
        out.resize(out.size() - 4);
    }
    return out;
}

/// @brief Write an entire buffer to a socket.
/// @param fd Socket. @param p Bytes. @param n Length. @return false on error/EOF.
bool write_all(int fd, const void* p, size_t n) {
    const char* c = (const char*)p;
    while (n) {
        ssize_t k = ::send(fd, c, n, MSG_NOSIGNAL);
        if (k <= 0) {
            return false;
        }
        c += k;
        n -= (size_t)k;
    }
    return true;
}

/// @brief Build a WebSocket frame header (server->client, unmasked).
/// @param opcode Frame opcode. @param len Payload length. @param rsv1 Compression flag.
/// @return The header bytes.
std::string ws_header(uint8_t opcode, size_t len, bool rsv1) {
    std::string h;
    h += (char)(0x80 | (rsv1 ? 0x40 : 0) | opcode);  // FIN [+ RSV1] + opcode
    if (len < 126) {
        h += (char)len;
    } else if (len <= 0xFFFF) {
        h += (char)126;
        h += (char)((len >> 8) & 0xFF);
        h += (char)(len & 0xFF);
    } else {
        h += (char)127;
        for (int s = 56; s >= 0; s -= 8) {
            h += (char)((len >> s) & 0xFF);
        }
    }
    return h;
}

}  // namespace

std::string Request::header(const std::string& key) const {
    auto it = headers.find(key);
    return it == headers.end() ? "" : it->second;
}

bool ws_send(Client& c, uint8_t opcode, const void* data, size_t len, bool rsv1) {
    std::lock_guard<std::mutex> lk(c.wmtx);
    std::string h = ws_header(opcode, len, rsv1);
    if (!write_all(c.fd, h.data(), h.size())) {
        return false;
    }
    if (len && !write_all(c.fd, data, len)) {
        return false;
    }
    return true;
}

Server::Server(int port, Router router, OnWsConnect on_ws)
    : port_(port), router_(std::move(router)), on_ws_(std::move(on_ws)) {}

bool Server::start() {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        return false;
    }
    fcntl(listen_fd_, F_SETFD, FD_CLOEXEC);  // release the port on backend re-exec
    int opt = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (::bind(listen_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        return false;
    }
    if (::listen(listen_fd_, 32) < 0) {
        return false;
    }
    accept_thread_ = std::thread([this] {
        accept_loop();
    });
    return true;
}

void Server::broadcast_text(const std::string& msg) {
    auto snap = snapshot();
    for (auto& c : snap) {
        if (c->alive && !ws_send(*c, 0x1, msg.data(), msg.size())) {
            c->alive = false;
        }
    }
}

void Server::broadcast_binary(const void* data, size_t len) {
    auto snap = snapshot();
    std::string comp;
    bool have_comp = false;
    for (auto& c : snap) {
        if (!c->alive) {
            continue;
        }
        bool ok;
        if (c->deflate) {
            if (!have_comp) {
                comp = raw_deflate(data, len);
                have_comp = true;
            }
            ok = ws_send(*c, 0x2, comp.data(), comp.size(), true);
        } else {
            ok = ws_send(*c, 0x2, data, len, false);
        }
        if (!ok) {
            c->alive = false;
        }
    }
}

size_t Server::client_count() {
    std::lock_guard<std::mutex> lk(cmtx_);
    return clients_.size();
}

std::vector<ClientPtr> Server::snapshot() {
    std::lock_guard<std::mutex> lk(cmtx_);
    return clients_;
}

void Server::add_client(const ClientPtr& c) {
    std::lock_guard<std::mutex> lk(cmtx_);
    clients_.push_back(c);
}

void Server::remove_client(const ClientPtr& c) {
    std::lock_guard<std::mutex> lk(cmtx_);
    for (size_t i = 0; i < clients_.size(); i++) {
        if (clients_[i] == c) {
            clients_.erase(clients_.begin() + i);
            break;
        }
    }
}

void Server::accept_loop() {
    while (true) {
        int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);  // don't leak client sockets across re-exec
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        std::thread([this, fd] {
            handle_conn(fd);
        }).detach();
    }
}

bool Server::read_request(int fd, Request& req) {
    std::string buf;
    char tmp[4096];
    size_t hdr_end = std::string::npos;
    while (hdr_end == std::string::npos) {
        ssize_t k = ::recv(fd, tmp, sizeof(tmp), 0);
        if (k <= 0) {
            return false;
        }
        buf.append(tmp, k);
        hdr_end = buf.find("\r\n\r\n");
        if (buf.size() > 1 << 20) {  // 1 MB header cap
            return false;
        }
    }
    std::string head = buf.substr(0, hdr_end);

    // Request line.
    size_t eol = head.find("\r\n");
    std::string line = head.substr(0, eol);
    size_t sp1 = line.find(' ');
    size_t sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) {
        return false;
    }
    req.method = line.substr(0, sp1);
    std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    if (q != std::string::npos) {
        req.path = target.substr(0, q);
        req.query = target.substr(q + 1);
    } else {
        req.path = target;
    }

    // Headers.
    size_t pos = eol + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) {
            e = head.size();
        }
        std::string h = head.substr(pos, e - pos);
        size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string k = h.substr(0, colon);
            std::string v = h.substr(colon + 1);
            for (auto& ch : k) {
                ch = tolower(ch);
            }
            size_t s = v.find_first_not_of(" \t");
            if (s != std::string::npos) {
                v = v.substr(s);
            }
            req.headers[k] = v;
        }
        pos = e + 2;
    }

    // Body (up to Content-Length).
    size_t body_start = hdr_end + 4;
    req.body = buf.substr(body_start);
    size_t clen = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        clen = strtoul(it->second.c_str(), nullptr, 10);
    }
    while (req.body.size() < clen) {
        ssize_t k = ::recv(fd, tmp, sizeof(tmp), 0);
        if (k <= 0) {
            break;
        }
        req.body.append(tmp, k);
    }
    return true;
}

void Server::send_http(int fd, const Response& r) {
    static const std::map<int, std::string> reason = {
        {200, "OK"},          {201, "Created"},
        {400, "Bad Request"}, {404, "Not Found"},
        {408, "Request Timeout"}, {500, "Internal Server Error"}};
    auto it = reason.find(r.status);
    std::string phrase = it == reason.end() ? "OK" : it->second;
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + " " + phrase + "\r\n";
    out += "Content-Type: " + r.content_type + "\r\n";
    out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n";
    out += "Access-Control-Allow-Origin: *\r\n";
    out += "Connection: close\r\n";
    for (auto& h : r.extra_headers) {
        out += h.first + ": " + h.second + "\r\n";
    }
    out += "\r\n";
    out += r.body;
    write_all(fd, out.data(), out.size());
}

void Server::handle_conn(int fd) {
    Request req;
    if (!read_request(fd, req)) {
        ::close(fd);
        return;
    }
    std::string upg = req.header("upgrade");
    for (auto& c : upg) {
        c = tolower(c);
    }
    if (upg == "websocket" && !req.header("sec-websocket-key").empty()) {
        do_ws_upgrade(fd, req);  // continues inside the client reader loop
        return;
    }
    Response r = router_(req);
    send_http(fd, r);
    ::close(fd);
}

void Server::do_ws_upgrade(int fd, const Request& req) {
    std::string key = req.header("sec-websocket-key");
    std::string magic = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1((const unsigned char*)magic.data(), magic.size(), digest);
    std::string accept = b64(digest, SHA_DIGEST_LENGTH);

    // Offer permessage-deflate with no context takeover (each frame stands alone).
    bool want_deflate =
        req.header("sec-websocket-extensions").find("permessage-deflate") != std::string::npos;
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n";
    if (want_deflate) {
        resp +=
            "Sec-WebSocket-Extensions: permessage-deflate; "
            "server_no_context_takeover; client_no_context_takeover\r\n";
    }
    resp += "\r\n";
    if (!write_all(fd, resp.data(), resp.size())) {
        ::close(fd);
        return;
    }

    auto client = std::make_shared<Client>(fd);
    client->deflate = want_deflate;
    add_client(client);
    if (on_ws_) {
        on_ws_(client);
    }
    ws_reader_loop(client);  // blocks until disconnect
    remove_client(client);
    client->alive = false;
    ::close(fd);
}

void Server::ws_reader_loop(const ClientPtr& c) {
    int fd = c->fd;
    std::string buf;
    char tmp[2048];
    auto need = [&](size_t n) -> bool {
        while (buf.size() < n) {
            ssize_t k = ::recv(fd, tmp, sizeof(tmp), 0);
            if (k <= 0) {
                return false;
            }
            buf.append(tmp, k);
        }
        return true;
    };
    while (c->alive) {
        if (!need(2)) {
            break;
        }
        uint8_t b0 = buf[0];
        uint8_t b1 = buf[1];
        uint8_t opcode = b0 & 0x0F;
        bool masked = b1 & 0x80;
        uint64_t len = b1 & 0x7F;
        size_t hdr = 2;
        if (len == 126) {
            if (!need(4)) {
                break;
            }
            len = ((uint8_t)buf[2] << 8) | (uint8_t)buf[3];
            hdr = 4;
        } else if (len == 127) {
            if (!need(10)) {
                break;
            }
            len = 0;
            for (int i = 0; i < 8; i++) {
                len = (len << 8) | (uint8_t)buf[2 + i];
            }
            hdr = 10;
        }
        size_t maskoff = hdr;
        if (masked) {
            hdr += 4;
        }
        if (!need(hdr + len)) {
            break;
        }
        if (opcode == 0x8) {  // close
            break;
        }
        if (opcode == 0x9) {  // ping -> pong
            std::string payload = buf.substr(hdr, len);
            if (masked) {
                for (size_t i = 0; i < payload.size(); i++) {
                    payload[i] ^= buf[maskoff + (i & 3)];
                }
            }
            ws_send(*c, 0xA, payload.data(), payload.size());
        }
        // (Non-control client frames carry no app data — control is REST — so their
        // masked payload is simply skipped.)
        buf.erase(0, hdr + len);
    }
}

}  // namespace ws
