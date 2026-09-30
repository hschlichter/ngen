#pragma once

#include "rpcprotocol.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// An RPC endpoint with its own I/O thread: it listens, opens connections to other endpoints, or both.
//
// The I/O thread accepts connections, reads frames and hands every incoming request and
// notification to the request handler, on the I/O thread. Outgoing connections from connect()
// are served by the same thread and behave like accepted ones. Anything may call send() or
// call() from any thread; those only queue bytes and wake the I/O thread, so a caller never
// blocks on a slow client. A client that stops reading is dropped once its queue passes
// maxQueuedBytes.
class RpcServer {
public:
    using ConnectionId = uint32_t;
    using RequestHandler = std::function<void(ConnectionId connection, const rpc::Json& message, std::vector<std::byte> attachment)>;
    using ConnectionHandler = std::function<void(ConnectionId connection, bool connected)>;
    using ResponseHandler = std::function<void(const rpc::Json& response, std::vector<std::byte> attachment)>;
    using DrainHandler = std::function<void(ConnectionId connection)>;

    static constexpr size_t maxQueuedBytes = 256ull * 1024 * 1024;

    RpcServer();
    RpcServer(const RpcServer&) = delete;
    auto operator=(const RpcServer&) -> RpcServer& = delete;
    ~RpcServer();

    // Handlers must be set before start().
    auto setRequestHandler(RequestHandler handler) -> void;
    auto setConnectionHandler(ConnectionHandler handler) -> void;
    // Called on the I/O thread after bytes were written to a connection whose queue is then at or
    // below `threshold` bytes. A sender that paces itself sends more from here.
    auto setDrainHandler(DrainHandler handler, size_t threshold) -> void;
    // Listens on 127.0.0.1:port (0 = any) and starts the I/O thread. Returns the bound port.
    auto start(uint16_t port) -> std::expected<uint16_t, std::string>;
    // Starts the I/O thread without a listening socket, for an endpoint that only connects out.
    auto startWithoutListening() -> void;
    // Connects to 127.0.0.1:port. The connection handler runs for it as for an accepted one.
    auto connect(uint16_t port) -> std::expected<ConnectionId, std::string>;
    auto stop() -> void;

    // Queues a message on a connection. Ignored if the connection is gone.
    auto send(ConnectionId connection, const rpc::Json& message, std::span<const std::byte> attachment = {}) -> void;
    // Sends a request to a connected peer; onResponse runs on the I/O thread when the answer
    // arrives, or with an error response if the connection closes first.
    auto call(ConnectionId connection, std::string_view method, const rpc::Json& params, ResponseHandler onResponse) -> void;
    // Bytes queued on a connection and not yet written; 0 if the connection is gone.
    auto queuedBytes(ConnectionId connection) -> size_t;

private:
    struct Connection;

    auto ioLoop() -> void;
    auto wake() -> void;
    auto closeConnection(ConnectionId id) -> void;
    auto handleFrame(ConnectionId id, std::string json, std::vector<std::byte> attachment) -> void;

    RequestHandler requestHandler;
    ConnectionHandler connectionHandler;
    DrainHandler drainHandler;
    size_t drainThreshold = 0;
    int listenFd = -1;
    int wakeFd = -1;
    std::atomic<bool> running{false};
    std::thread ioThread;

    std::mutex mutex; // guards connections' outgoing queues, pendingCalls and nextCallId
    std::unordered_map<ConnectionId, std::unique_ptr<Connection>> connections;
    ConnectionId nextConnectionId = 1;
    std::map<std::pair<ConnectionId, int64_t>, ResponseHandler> pendingCalls;
    int64_t nextCallId = 1;
};
