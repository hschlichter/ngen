#include "videohttp.h"

#include "profile.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <format>
#include <string_view>

namespace {

auto statusText(int status) -> const char* {
    switch (status) {
        case 200:
            return "OK";
        case 204:
            return "No Content";
        case 400:
            return "Bad Request";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 413:
            return "Content Too Large";
        case 500:
            return "Internal Server Error";
        case 503:
            return "Service Unavailable";
        default:
            return "Status";
    }
}

auto sendAll(int socket, std::string_view data) -> void {
    while (!data.empty()) {
        auto sent = send(socket, data.data(), data.size(), MSG_NOSIGNAL);
        if (sent <= 0) {
            return;
        }
        data.remove_prefix((size_t) sent);
    }
}

auto respond(int socket, const VideoHttpResponse& response) -> void {
    auto head = std::format("HTTP/1.1 {} {}\r\nContent-Type: {}\r\nContent-Length: {}\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n",
                            response.status,
                            statusText(response.status),
                            response.contentType,
                            response.body.size());
    sendAll(socket, head);
    sendAll(socket, response.body);
}

// The Content-Length among a request's header lines; 0 without one.
auto contentLength(std::string_view head) -> size_t {
    size_t line = 0;
    while (line < head.size()) {
        auto end = head.find("\r\n", line);
        if (end == std::string_view::npos) {
            end = head.size();
        }
        auto text = head.substr(line, end - line);
        constexpr std::string_view name = "content-length:";
        if (text.size() > name.size()) {
            bool matches = true;
            for (size_t i = 0; i < name.size(); i++) {
                if (std::tolower((unsigned char) text[i]) != name[i]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                auto value = text.substr(name.size());
                while (!value.empty() && value.front() == ' ') {
                    value.remove_prefix(1);
                }
                size_t length = 0;
                std::from_chars(value.data(), value.data() + value.size(), length);
                return length;
            }
        }
        line = end + 2;
    }
    return 0;
}

} // namespace

VideoHttp::~VideoHttp() {
    stop();
}

auto VideoHttp::start(const std::string& address, uint16_t port, Handler requestHandler) -> std::expected<void, std::string> {
    stop();
    sockaddr_in bound = {};
    bound.sin_family = AF_INET;
    bound.sin_port = htons(port);
    if (inet_pton(AF_INET, address.c_str(), &bound.sin_addr) != 1) {
        return std::unexpected(std::format("'{}' is not an IPv4 address", address));
    }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return std::unexpected(std::format("socket: {}", std::strerror(errno)));
    }
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    if (bind(fd, reinterpret_cast<sockaddr*>(&bound), sizeof(bound)) != 0) {
        auto message = std::format("cannot bind {}:{}: {}", address, port, std::strerror(errno));
        close(fd);
        return std::unexpected(message);
    }
    if (listen(fd, 8) != 0) {
        auto message = std::format("cannot listen on {}:{}: {}", address, port, std::strerror(errno));
        close(fd);
        return std::unexpected(message);
    }
    listenSocket = fd;
    handler = std::move(requestHandler);
    running = true;
    thread = std::thread([this] {
        run();
    });
    return {};
}

auto VideoHttp::stop() -> void {
    running = false;
    if (thread.joinable()) {
        thread.join();
    }
    if (listenSocket >= 0) {
        close(listenSocket);
        listenSocket = -1;
    }
}

auto VideoHttp::run() -> void {
    profile::registerThread("Remote View Http");
    while (running) {
        pollfd waiting = {.fd = listenSocket, .events = POLLIN, .revents = 0};
        int ready = 0;
        {
            // Idle in slices, so the thread sees a stop and has zones while idle.
            PROFILE_ZONE_DETAIL("RemoteViewHttpPoll", Wait);
            ready = poll(&waiting, 1, (int) profile::idleSlice.count());
        }
        if (ready <= 0 || (waiting.revents & POLLIN) == 0) {
            continue;
        }
        sockaddr_in from = {};
        socklen_t fromSize = sizeof(from);
        int client = accept4(listenSocket, reinterpret_cast<sockaddr*>(&from), &fromSize, SOCK_CLOEXEC);
        if (client < 0) {
            continue;
        }
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &from.sin_addr, text, sizeof(text));
        // A client that stops talking doesn't hold the listener.
        timeval timeout = {.tv_sec = 5, .tv_usec = 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        {
            PROFILE_ZONE_DETAIL("RemoteViewHttpRequest", Io);
            serve(client, text);
        }
        close(client);
    }
}

auto VideoHttp::serve(int client, const std::string& peer) -> void {
    std::string data;
    size_t headEnd = std::string::npos;
    size_t bodyLength = 0;
    char buffer[16384];
    while (true) {
        if (headEnd != std::string::npos && data.size() >= headEnd + 4 + bodyLength) {
            break;
        }
        auto received = recv(client, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            return;
        }
        data.append(buffer, (size_t) received);
        if (headEnd == std::string::npos) {
            headEnd = data.find("\r\n\r\n");
            if (headEnd != std::string::npos) {
                bodyLength = contentLength(std::string_view(data).substr(0, headEnd));
            }
        }
        if (data.size() > maxRequestBytes || bodyLength > maxRequestBytes) {
            respond(client, {.status = 413, .body = "request too large\n"});
            return;
        }
    }
    auto lineEnd = data.find("\r\n");
    auto line = std::string_view(data).substr(0, lineEnd);
    auto firstSpace = line.find(' ');
    auto secondSpace = firstSpace == std::string_view::npos ? std::string_view::npos : line.find(' ', firstSpace + 1);
    if (secondSpace == std::string_view::npos) {
        respond(client, {.status = 400, .body = "not an HTTP request\n"});
        return;
    }
    VideoHttpRequest request;
    request.method = std::string(line.substr(0, firstSpace));
    auto target = line.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    request.path = std::string(target.substr(0, target.find('?')));
    request.body = data.substr(headEnd + 4, bodyLength);
    request.peer = peer;
    respond(client, handler(request));
}
