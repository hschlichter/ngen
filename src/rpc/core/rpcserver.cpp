#include "rpcserver.h"

#include "rpcframe.h"
#include "rpcsocket.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cerrno>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

struct RpcServer::Connection {
    int fd = -1;
    RpcFrameReader reader;
    std::vector<std::byte> outgoing; // guarded by RpcServer::mutex
    size_t outgoingOffset = 0;
    bool closing = false;
};

RpcServer::RpcServer() = default;

RpcServer::~RpcServer() {
    stop();
}

auto RpcServer::setRequestHandler(RequestHandler handler) -> void {
    requestHandler = std::move(handler);
}

auto RpcServer::setConnectionHandler(ConnectionHandler handler) -> void {
    connectionHandler = std::move(handler);
}

auto RpcServer::start(uint16_t port) -> std::expected<uint16_t, std::string> {
    uint16_t bound = 0;
    auto fd = rpc::listenLoopback(port, bound);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    listenFd = *fd;
    rpc::setNonBlocking(listenFd);
    wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    running = true;
    ioThread = std::thread([this] { ioLoop(); });
    return bound;
}

auto RpcServer::stop() -> void {
    if (!running.exchange(false)) {
        return;
    }
    wake();
    if (ioThread.joinable()) {
        ioThread.join();
    }
    std::vector<ConnectionId> ids;
    {
        std::lock_guard lock(mutex);
        for (const auto& [id, connection] : connections) {
            ids.push_back(id);
        }
    }
    for (auto id : ids) {
        closeConnection(id);
    }
    rpc::closeSocket(listenFd);
    rpc::closeSocket(wakeFd);
    listenFd = -1;
    wakeFd = -1;
}

auto RpcServer::wake() -> void {
    if (wakeFd >= 0) {
        uint64_t one = 1;
        [[maybe_unused]] auto written = write(wakeFd, &one, sizeof(one));
    }
}

auto RpcServer::send(ConnectionId id, const rpc::Json& message, std::span<const std::byte> attachment) -> void {
    auto bytes = encodeRpcFrame(message.dump(), attachment);
    {
        std::lock_guard lock(mutex);
        auto it = connections.find(id);
        if (it == connections.end() || it->second->closing) {
            return;
        }
        auto& connection = *it->second;
        if (connection.outgoing.size() - connection.outgoingOffset + bytes.size() > maxQueuedBytes) {
            // The client stopped reading; drop it rather than grow without bound.
            connection.closing = true;
        } else {
            connection.outgoing.insert(connection.outgoing.end(), bytes.begin(), bytes.end());
        }
    }
    wake();
}

auto RpcServer::call(ConnectionId id, std::string_view method, const rpc::Json& params, ResponseHandler onResponse) -> void {
    int64_t callId = 0;
    {
        std::lock_guard lock(mutex);
        callId = nextCallId++;
        pendingCalls[{id, callId}] = std::move(onResponse);
    }
    send(id, rpc::makeRequest(callId, method, params));
}

auto RpcServer::closeConnection(ConnectionId id) -> void {
    std::unique_ptr<Connection> connection;
    std::vector<ResponseHandler> orphaned;
    {
        std::lock_guard lock(mutex);
        auto it = connections.find(id);
        if (it == connections.end()) {
            return;
        }
        connection = std::move(it->second);
        connections.erase(it);
        for (auto call = pendingCalls.begin(); call != pendingCalls.end();) {
            if (call->first.first == id) {
                orphaned.push_back(std::move(call->second));
                call = pendingCalls.erase(call);
            } else {
                ++call;
            }
        }
    }
    rpc::closeSocket(connection->fd);
    for (auto& handler : orphaned) {
        handler(rpc::makeError(nullptr, rpc::internalError, "connection closed"), {});
    }
    if (connectionHandler) {
        connectionHandler(id, false);
    }
}

auto RpcServer::handleFrame(ConnectionId id, std::string json, std::vector<std::byte> attachment) -> void {
    auto message = rpc::Json::parse(json, nullptr, false);
    if (message.is_discarded()) {
        send(id, rpc::makeError(nullptr, rpc::parseError, "invalid JSON"));
        return;
    }
    auto kind = rpc::messageKind(message);
    if (kind == rpc::MessageKind::Response) {
        ResponseHandler handler;
        {
            std::lock_guard lock(mutex);
            if (message["id"].is_number_integer()) {
                auto it = pendingCalls.find({id, message["id"].get<int64_t>()});
                if (it != pendingCalls.end()) {
                    handler = std::move(it->second);
                    pendingCalls.erase(it);
                }
            }
        }
        if (handler) {
            handler(message, std::move(attachment));
        }
        return;
    }
    if (kind == rpc::MessageKind::Invalid) {
        send(id, rpc::makeError(message.contains("id") ? message["id"] : rpc::Json(), rpc::invalidRequest, "not a JSON-RPC request"));
        return;
    }
    if (requestHandler) {
        requestHandler(id, message, std::move(attachment));
    }
}

auto RpcServer::ioLoop() -> void {
    std::array<std::byte, 64 * 1024> readBuffer = {};
    while (running) {
        std::vector<pollfd> fds;
        std::vector<ConnectionId> ids;
        fds.push_back({.fd = listenFd, .events = POLLIN, .revents = 0});
        fds.push_back({.fd = wakeFd, .events = POLLIN, .revents = 0});
        std::vector<ConnectionId> toClose;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, connection] : connections) {
                if (connection->closing) {
                    toClose.push_back(id);
                    continue;
                }
                short events = POLLIN;
                if (connection->outgoingOffset < connection->outgoing.size()) {
                    events |= POLLOUT;
                }
                fds.push_back({.fd = connection->fd, .events = events, .revents = 0});
                ids.push_back(id);
            }
        }
        for (auto id : toClose) {
            closeConnection(id);
        }
        if (poll(fds.data(), (nfds_t) fds.size(), 1000) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if ((fds[1].revents & POLLIN) != 0) {
            uint64_t value = 0;
            [[maybe_unused]] auto consumed = read(wakeFd, &value, sizeof(value));
        }
        if ((fds[0].revents & POLLIN) != 0) {
            while (true) {
                int fd = accept4(listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) {
                    break;
                }
                ConnectionId id = 0;
                {
                    std::lock_guard lock(mutex);
                    id = nextConnectionId++;
                    auto connection = std::make_unique<Connection>();
                    connection->fd = fd;
                    connections[id] = std::move(connection);
                }
                if (connectionHandler) {
                    connectionHandler(id, true);
                }
            }
        }
        for (size_t i = 0; i < ids.size(); i++) {
            auto id = ids[i];
            auto revents = fds[i + 2].revents;
            if ((revents & POLLOUT) != 0) {
                std::lock_guard lock(mutex);
                auto it = connections.find(id);
                if (it != connections.end()) {
                    auto& connection = *it->second;
                    auto pending = connection.outgoing.size() - connection.outgoingOffset;
                    auto written = ::send(connection.fd, connection.outgoing.data() + connection.outgoingOffset, pending, MSG_NOSIGNAL);
                    if (written > 0) {
                        connection.outgoingOffset += (size_t) written;
                        if (connection.outgoingOffset == connection.outgoing.size()) {
                            connection.outgoing.clear();
                            connection.outgoingOffset = 0;
                        }
                    } else if (written < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        connection.closing = true;
                    }
                }
            }
            if ((revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            int fd = fds[i + 2].fd;
            auto received = recv(fd, readBuffer.data(), readBuffer.size(), 0);
            if (received == 0 || (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                closeConnection(id);
                continue;
            }
            if (received < 0) {
                continue;
            }
            // Frames are parsed outside the lock: the reader belongs to the I/O thread.
            RpcFrameReader* reader = nullptr;
            {
                std::lock_guard lock(mutex);
                auto it = connections.find(id);
                if (it != connections.end()) {
                    reader = &it->second->reader;
                }
            }
            if (reader == nullptr) {
                continue;
            }
            reader->append(std::span(readBuffer.data(), (size_t) received));
            std::string error;
            while (auto frame = reader->next(error)) {
                handleFrame(id, std::move(frame->json), std::move(frame->attachment));
            }
            if (!error.empty()) {
                closeConnection(id);
            }
        }
    }
}
