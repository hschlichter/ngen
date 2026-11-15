#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>
#include <thread>

// The remote view's listener: a small HTTP/1.1 server for the watch page and its few requests. One request per connection, served one
// after another on its own thread ("Remote View Http"); nothing of the engine waits for it. Unlike the RPC endpoint it binds the
// address it is told to, so another device can reach it; it is off unless the remote view is on.

struct VideoHttpRequest {
    std::string method; // GET, POST
    std::string path;   // without a query
    std::string body;
    std::string peer; // the client's address
};

struct VideoHttpResponse {
    int status = 200;
    std::string contentType = "text/plain; charset=utf-8";
    std::string body;
};

class VideoHttp {
public:
    using Handler = std::function<VideoHttpResponse(const VideoHttpRequest&)>;

    VideoHttp() = default;
    ~VideoHttp();
    VideoHttp(const VideoHttp&) = delete;
    auto operator=(const VideoHttp&) -> VideoHttp& = delete;

    // Listens on `address` (an IPv4 address; 0.0.0.0 for every interface) and `port`, and serves until stop. `handler` runs on the
    // listener's thread.
    auto start(const std::string& address, uint16_t port, Handler handler) -> std::expected<void, std::string>;
    auto stop() -> void;
    auto listening() const -> bool { return listenSocket >= 0; }

    // A request's head and body together are refused beyond this.
    static constexpr size_t maxRequestBytes = 16 * 1024 * 1024;

private:
    auto run() -> void;
    auto serve(int client, const std::string& peer) -> void;

    int listenSocket = -1;
    std::atomic<bool> running{false};
    std::thread thread;
    Handler handler;
};
