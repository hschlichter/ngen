#include "rpcserver.h"

#include "profile.h"
#include "rpcframe.h"
#include "rpcsocket.h"

#include <nlohmann/json.hpp>

#include <array>

struct RpcServer::Connection {
    rpc::Socket fd = rpc::invalidSocket;
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

auto RpcServer::setThreadName(std::string name) -> void {
    threadName = std::move(name);
}

auto RpcServer::setConnectionHandler(ConnectionHandler handler) -> void {
    connectionHandler = std::move(handler);
}

auto RpcServer::setDrainHandler(DrainHandler handler, size_t threshold) -> void {
    drainHandler = std::move(handler);
    drainThreshold = threshold;
}

auto RpcServer::startWithoutListening() -> void {
    waker.open();
    running = true;
    ioThread = std::thread([this] { ioLoop(); });
}

auto RpcServer::connect(uint16_t port) -> std::expected<ConnectionId, std::string> {
    auto fd = rpc::connectLoopback(port);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    rpc::setNonBlocking(*fd);
    ConnectionId id = 0;
    {
        std::lock_guard lock(mutex);
        id = nextConnectionId++;
        auto connection = std::make_unique<Connection>();
        connection->fd = *fd;
        connections[id] = std::move(connection);
    }
    if (connectionHandler) {
        connectionHandler(id, true);
    }
    wake();
    return id;
}

auto RpcServer::queuedBytes(ConnectionId id) -> size_t {
    std::lock_guard lock(mutex);
    auto it = connections.find(id);
    if (it == connections.end()) {
        return 0;
    }
    return it->second->outgoing.size() - it->second->outgoingOffset;
}

auto RpcServer::start(uint16_t port) -> std::expected<uint16_t, std::string> {
    uint16_t bound = 0;
    auto fd = rpc::listenLoopback(port, bound);
    if (!fd) {
        return std::unexpected(fd.error());
    }
    listenSocket = *fd;
    rpc::setNonBlocking(listenSocket);
    waker.open();
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
    rpc::closeSocket(listenSocket);
    listenSocket = rpc::invalidSocket;
    waker.close();
}

auto RpcServer::wake() -> void {
    waker.signal();
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
    profile::registerThread(threadName.c_str());
    std::array<std::byte, 64 * 1024> readBuffer = {};
    // The poll set: the waker, the listener when there is one, then one entry per connection.
    bool listening = listenSocket != rpc::invalidSocket;
    size_t firstConnection = listening ? 2 : 1;
    while (running) {
        std::vector<rpc::PollEntry> fds;
        std::vector<ConnectionId> ids;
        fds.push_back({.socket = waker.socket(), .wantRead = true});
        if (listening) {
            fds.push_back({.socket = listenSocket, .wantRead = true});
        }
        std::vector<ConnectionId> toClose;
        {
            std::lock_guard lock(mutex);
            for (const auto& [id, connection] : connections) {
                if (connection->closing) {
                    toClose.push_back(id);
                    continue;
                }
                bool wantWrite = connection->outgoingOffset < connection->outgoing.size();
                fds.push_back({.socket = connection->fd, .wantRead = true, .wantWrite = wantWrite});
                ids.push_back(id);
            }
        }
        for (auto id : toClose) {
            closeConnection(id);
        }
        int polled = 0;
        {
            // Idle in slices, so an idle I/O thread has zones too (profile::idleSlice).
            PROFILE_ZONE_DETAIL("RpcPoll", Wait);
            polled = rpc::pollSockets(fds, (int) profile::idleSlice.count());
        }
        if (polled < 0) {
            if (rpc::lastErrorInterrupted()) {
                continue;
            }
            break;
        }
        if (fds[0].readable) {
            waker.drain();
        }
        if (listening && fds[1].readable) {
            PROFILE_ZONE_DETAIL("RpcAccept", Io);
            while (true) {
                auto fd = rpc::acceptConnection(listenSocket);
                if (fd == rpc::invalidSocket) {
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
            const auto& entry = fds[i + firstConnection];
            if (entry.writable) {
                PROFILE_ZONE_DETAIL("RpcSend", Io);
                bool drained = false;
                {
                    std::lock_guard lock(mutex);
                    auto it = connections.find(id);
                    if (it != connections.end()) {
                        auto& connection = *it->second;
                        auto pending = connection.outgoing.size() - connection.outgoingOffset;
                        auto written = rpc::sendSome(connection.fd, connection.outgoing.data() + connection.outgoingOffset, pending);
                        if (written > 0) {
                            connection.outgoingOffset += (size_t) written;
                            if (connection.outgoingOffset == connection.outgoing.size()) {
                                connection.outgoing.clear();
                                connection.outgoingOffset = 0;
                            }
                            drained = connection.outgoing.size() - connection.outgoingOffset <= drainThreshold;
                        } else if (written < 0 && !rpc::lastErrorWouldBlock()) {
                            connection.closing = true;
                        }
                    }
                }
                // Outside the lock: the handler sends more.
                if (drained && drainHandler) {
                    drainHandler(id);
                }
            }
            if (!entry.readable && !entry.closed) {
                continue;
            }
            PROFILE_ZONE_DETAIL("RpcReceive", Io);
            auto received = rpc::receiveSome(entry.socket, readBuffer.data(), readBuffer.size());
            if (received == 0 || (received < 0 && !rpc::lastErrorWouldBlock())) {
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
