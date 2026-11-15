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

namespace logging {

struct Event;

// An event as it travels in a log.events notification: the JSONL line's fields, plus seq.
auto eventJson(const Event& event) -> rpc::Json;

// Streams the process's ring to RPC subscribers. A client calls log.subscribe and then receives log.events
// notifications: batches of events in seq order, sent every batchInterval or once batchEvents are waiting.
//
// A slow subscriber never stalls the process: while more than maxQueuedBytes wait to be written on its connection
// nothing more is sent, and events the ring overwrites meanwhile are counted in the next batch's `dropped`.
class LogStream {
public:
    static constexpr size_t batchEvents = 1024;
    static constexpr std::chrono::milliseconds batchInterval{50};
    static constexpr size_t maxQueuedBytes = 16ull * 1024 * 1024;
    // Events per notification at most; more go out as several.
    static constexpr size_t maxEventsPerNotification = 4096;

    // `process` names this process in every batch ("view:1234"). The server must outlive the stream.
    LogStream(RpcServer& server, std::string process);
    LogStream(const LogStream&) = delete;
    auto operator=(const LogStream&) -> LogStream& = delete;
    ~LogStream();

    // Answers log.subscribe and log.unsubscribe, on whatever thread the message arrived; false for any other
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

} // namespace logging
