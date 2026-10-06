#pragma once

#include "rpcprotocol.h"
#include "rpcserver.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace trace {

// Streams the process's ring to RPC subscribers. A client calls trace.subscribe and then receives trace.events
// notifications: batches of events in seq order, sent every batchInterval or once batchEvents are waiting.
//
// A slow subscriber never stalls the process: while more than maxQueuedBytes wait to be written on its connection
// nothing more is sent, and events the ring overwrites meanwhile are counted in the next batch's `dropped`.
class TraceStream {
public:
    static constexpr size_t batchEvents = 1024;
    static constexpr std::chrono::milliseconds batchInterval{50};
    static constexpr size_t maxQueuedBytes = 16ull * 1024 * 1024;
    // Events per notification at most; more go out as several.
    static constexpr size_t maxEventsPerNotification = 4096;

    // `process` names this process in every batch ("view:1234"). The server must outlive the stream.
    TraceStream(RpcServer& server, std::string process);
    TraceStream(const TraceStream&) = delete;
    auto operator=(const TraceStream&) -> TraceStream& = delete;
    ~TraceStream();

    // Answers trace.subscribe and trace.unsubscribe, on whatever thread the message arrived; false for any other
    // message, which the caller handles.
    auto handle(RpcServer::ConnectionId connection, const rpc::Json& message) -> bool;
    // Forgets a closed connection's subscription.
    auto disconnected(RpcServer::ConnectionId connection) -> void;
    // Sends every event pushed so far to every subscriber and waits, at most `timeout`, until it is written. For
    // shutdown, so the last events reach the subscribers before the connections close.
    auto finish(std::chrono::milliseconds timeout) -> void;

private:
    struct Subscriber {
        uint64_t cursor = 0;
    };

    auto loop() -> void;
    // Sends what is waiting to every subscriber whose connection has room. True when every subscriber is caught up.
    auto pump() -> bool;

    RpcServer& server;
    std::string process;
    std::mutex mutex; // guards subscribers; pump() runs on one thread at a time under it
    std::map<RpcServer::ConnectionId, Subscriber> subscribers;
    std::atomic<bool> running{true};
    std::thread thread;
};

} // namespace trace
