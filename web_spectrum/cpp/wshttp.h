/**
 * @file wshttp.h
 * @brief Self-contained HTTP/1.1 + WebSocket (RFC 6455) server on POSIX sockets.
 *
 * Thread-per-connection. A connection that upgrades to WebSocket is registered for
 * server-push broadcasts (text status events + binary spectrum frames, optionally
 * permessage-deflate compressed). No external web framework is used.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ws {

/// @brief A parsed HTTP request.
struct Request {
    std::string method;                          ///< HTTP method, e.g. "GET".
    std::string path;                            ///< Path without the query string.
    std::string query;                           ///< Raw query string (after '?').
    std::string body;                            ///< Request body (up to Content-Length).
    std::map<std::string, std::string> headers;  ///< Headers with lowercased keys.

    /**
     * @brief Look up a header value.
     * @param key Lowercased header name.
     * @return The value, or "" if absent.
     */
    std::string header(const std::string& key) const;
};

/// @brief An HTTP response to send back to the client.
struct Response {
    int status = 200;                             ///< HTTP status code.
    std::string content_type = "application/json";  ///< Content-Type header value.
    std::string body;                             ///< Response body.
    std::vector<std::pair<std::string, std::string>> extra_headers;  ///< Additional headers.
};

/// @brief A connected WebSocket client.
struct Client {
    int fd;                        ///< Socket file descriptor.
    std::mutex wmtx;               ///< Serializes writes to this client.
    std::atomic<bool> alive{true};  ///< Cleared when the client disconnects or errors.
    bool deflate = false;          ///< Whether permessage-deflate was negotiated.

    /// @brief Construct for a socket. @param f The connected socket fd.
    explicit Client(int f) : fd(f) {}
};

/// @brief Shared-ownership handle to a @ref Client.
using ClientPtr = std::shared_ptr<Client>;

/**
 * @brief Send one WebSocket frame to a client (server->client, unmasked).
 * @param c      Target client (its write mutex is taken).
 * @param opcode WebSocket opcode (0x1 text, 0x2 binary, 0xA pong, ...).
 * @param data   Payload bytes.
 * @param len    Payload length.
 * @param rsv1   Set to mark a permessage-deflate-compressed payload.
 * @return true on success, false if the write failed.
 */
bool ws_send(Client& c, uint8_t opcode, const void* data, size_t len, bool rsv1 = false);

/**
 * @brief A minimal multithreaded HTTP + WebSocket server.
 *
 * Construct with a request router and a WebSocket-connect callback, then call @ref start.
 * Each connection is handled on its own thread; WebSocket clients are tracked so frames
 * can be broadcast to all of them.
 */
class Server {
public:
    /// @brief Router: turns a request into a response.
    using Router = std::function<Response(const Request&)>;
    /// @brief Called once per client right after the WebSocket handshake completes.
    using OnWsConnect = std::function<void(const ClientPtr&)>;

    /**
     * @brief Construct a server.
     * @param port   TCP port to listen on.
     * @param router Handles HTTP requests.
     * @param on_ws  Invoked when a client finishes the WebSocket handshake.
     */
    Server(int port, Router router, OnWsConnect on_ws);

    /**
     * @brief Bind, listen, and start the accept thread.
     * @return true on success, false if binding/listening failed.
     */
    bool start();

    /**
     * @brief Push a text event to every connected WebSocket client.
     * @param msg The (JSON) text payload.
     */
    void broadcast_text(const std::string& msg);

    /**
     * @brief Push a binary frame to every client.
     *
     * The payload is compressed once and reused for all deflate-capable clients; clients
     * that did not negotiate compression receive it raw.
     * @param data Frame bytes.
     * @param len  Frame length.
     */
    void broadcast_binary(const void* data, size_t len);

    /// @return The current number of connected WebSocket clients.
    size_t client_count();

private:
    int port_;                          ///< Listen port.
    Router router_;                     ///< HTTP request handler.
    OnWsConnect on_ws_;                 ///< WebSocket connect callback.
    int listen_fd_ = -1;                ///< Listening socket.
    std::thread accept_thread_;         ///< Accept loop thread.
    std::mutex cmtx_;                   ///< Guards @ref clients_.
    std::vector<ClientPtr> clients_;    ///< Connected WebSocket clients.

    /// @return A snapshot copy of the current client list (taken under @ref cmtx_).
    std::vector<ClientPtr> snapshot();
    /// @brief Register a new client. @param c The client.
    void add_client(const ClientPtr& c);
    /// @brief Unregister a client. @param c The client.
    void remove_client(const ClientPtr& c);
    /// @brief Accept loop: accept connections and dispatch each to its own thread.
    void accept_loop();
    /**
     * @brief Read one HTTP request from a socket.
     * @param fd  The connected socket.
     * @param req Filled in on success.
     * @return true on success, false on EOF/parse error.
     */
    bool read_request(int fd, Request& req);
    /// @brief Write an HTTP response. @param fd The socket. @param r The response.
    void send_http(int fd, const Response& r);
    /// @brief Handle one connection (HTTP request or WebSocket upgrade). @param fd The socket.
    void handle_conn(int fd);
    /// @brief Perform the WebSocket handshake and enter the reader loop. @param fd The socket. @param req The upgrade request.
    void do_ws_upgrade(int fd, const Request& req);
    /// @brief Read incoming frames until disconnect (handles ping/close). @param c The client.
    void ws_reader_loop(const ClientPtr& c);
};

}  // namespace ws
