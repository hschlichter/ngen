#pragma once

#include "rpcregistry.h"
#include "rpcserver.h"
#include "tracestream.h"

#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// An engine process's RPC endpoint: the server, discovery, and dispatch of calls onto the
// thread that owns the data.
//
// trace.subscribe and trace.unsubscribe are answered on the I/O thread by the endpoint's trace stream, which sends
// the process's events to subscribers (src/trace/README.md).
//
// Requests arrive on the server's I/O thread and are queued. drain() runs them on the calling
// thread (the main thread, at the point in the frame where session commands run) through the
// registry. Each handler gets a responder whose reply goes straight back through the I/O
// thread, whenever the handler completes it.
class RpcEndpoint {
public:
    RpcEndpoint();
    RpcEndpoint(const RpcEndpoint&) = delete;
    auto operator=(const RpcEndpoint&) -> RpcEndpoint& = delete;
    ~RpcEndpoint();

    // Listens on a free loopback port and writes the discovery file. Logs and returns false
    // on failure; the process runs on without an endpoint.
    auto start(const RpcRegistry* registry, std::string kind, std::string label) -> bool;
    auto stop() -> void;
    // Runs queued calls, in arrival order, for at most drainBudgetNs (at least one call); the
    // rest wait for the next drain, so a flood of calls can't starve the frame. Main thread only.
    auto drain() -> void;
    static constexpr int64_t drainBudgetNs = 2'000'000;
    auto port() const -> uint16_t { return boundPort; }
    // Sends the trace's last events to its subscribers, waiting at most `timeout`. Call before stop() at exit.
    auto finishTrace(std::chrono::milliseconds timeout) -> void;

private:
    struct Queued; // a received call; defined in the .cpp so this header needs no full JSON type

    const RpcRegistry* registry = nullptr;
    std::unique_ptr<RpcServer> server;
    std::unique_ptr<trace::TraceStream> traceStream; // reset before the server it sends on
    std::string kind;
    uint16_t boundPort = 0;
    std::filesystem::path discoveryFile;
    std::mutex queueMutex;
    std::deque<std::unique_ptr<Queued>> queue;
};
