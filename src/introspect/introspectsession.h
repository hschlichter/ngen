#pragma once

#include "rpcdiscovery.h"
#include "rpcprotocol.h"
#include "rpcserver.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

// The tool's view of the running processes: it finds them through .ngen-discovery/ in the working directory, keeps
// one connection to each, fetches their records and subscribes to their traces. Nothing in the processes knows about
// it. A process is connected to once: one that goes away and comes back has a new pid, so a new name.
//
// Calls go out on an RpcServer that only connects out; answers and trace batches arrive on its I/O thread and are
// stored. The owner polls and reads from its own thread, getting copies; record values are shared, not copied.
class IntrospectSession {
public:
    struct Options {
        bool trace = false;        // subscribe to every process's trace
        uint64_t traceSinceNs = 0; // events from this CLOCK_MONOTONIC time on; 0 = all each process still holds
        // Only processes matching one of these targets (a kind, kind:pid or pid); empty = every process.
        std::vector<std::string> processes;
    };

    // One trace event as it arrived: the JSONL line's fields plus seq, from `process`.
    struct TraceEvent {
        uint64_t tsNs = 0;
        std::string process;
        std::shared_ptr<const rpc::Json> event;
    };

    struct RecordInfo {
        std::string name;
        std::string description;
    };

    struct Process {
        std::string name; // "<kind>:<pid>"
        RpcEndpointInfo endpoint;
        bool connected = false;
        std::string error;
        std::vector<RecordInfo> records;
    };

    struct RecordValue {
        std::shared_ptr<const rpc::Json> value; // null until the first answer
        std::string error;
        bool pending = false;
        double roundTripMs = 0.0;
        std::chrono::steady_clock::time_point received;
    };

    // How often poll() reads the discovery directory.
    static constexpr std::chrono::milliseconds discoveryInterval{250};

    explicit IntrospectSession(Options options);
    IntrospectSession(const IntrospectSession&) = delete;
    auto operator=(const IntrospectSession&) -> IntrospectSession& = delete;
    ~IntrospectSession();

    // Reads the discovery directory (at most every discoveryInterval): connects to new processes and forgets the
    // ones whose discovery file is gone or whose connection closed.
    auto poll() -> void;
    // Live processes, sorted by kind then pid.
    auto processes() const -> std::vector<Process>;
    // Asks a process for a record; the answer replaces the stored value when it arrives. Ignored while the same
    // record is still pending.
    auto fetch(const std::string& process, const std::string& record) -> void;
    // The record's latest value, empty if never fetched.
    auto record(const std::string& process, const std::string& record) const -> RecordValue;
    // Moves the trace events received since the last call into `out` (appended), in arrival order: each process's in
    // seq order, processes interleaved as their batches came. At most maxPendingTraceEvents wait; older ones are
    // dropped and counted.
    auto takeTraceEvents(std::vector<TraceEvent>& out) -> void;
    // Events dropped so far, by process: overwritten in a process's ring before they were sent, or past the cap here.
    auto traceDropped() const -> std::map<std::string, uint64_t>;
    // Names of every process connected to so far, gone or not.
    auto everConnected() const -> std::set<std::string>;
    static constexpr size_t maxPendingTraceEvents = 1'000'000;

private:
    struct Connected {
        Process process;
        RpcServer::ConnectionId connection = 0;
        std::map<std::string, RecordValue> values;
    };

    auto onDisconnected(RpcServer::ConnectionId connection) -> void;
    auto onRecordList(const std::string& process, const rpc::Json& response) -> void;
    auto onTraceEvents(const rpc::Json& params) -> void;
    auto wanted(const RpcEndpointInfo& endpoint) const -> bool;

    Options options;
    mutable std::mutex mutex;
    std::map<std::string, Connected> connected;
    std::set<std::string> connectedOnce;
    std::vector<TraceEvent> pendingTrace;
    std::map<std::string, uint64_t> dropped;
    std::chrono::steady_clock::time_point lastDiscovery;
    // Last member: stopped first in the destructor, so no answer arrives while the rest is torn down.
    RpcServer rpc;
};
