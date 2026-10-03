#pragma once

#include "rpcdiscovery.h"
#include "rpcprotocol.h"
#include "rpcserver.h"

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// The window's view of the running processes: it finds them through .ngen-discovery/ in the working directory,
// keeps one connection to each, and fetches their records. Nothing in the processes knows about it.
//
// Calls go out on an RpcServer that only connects out; answers arrive on its I/O thread and are stored. The window
// polls and reads from its own thread, getting copies; record values are shared, not copied.
class IntrospectSession {
public:
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

    IntrospectSession();
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

private:
    struct Connected {
        Process process;
        RpcServer::ConnectionId connection = 0;
        std::map<std::string, RecordValue> values;
    };

    auto onDisconnected(RpcServer::ConnectionId connection) -> void;
    auto onRecordList(const std::string& process, const rpc::Json& response) -> void;

    mutable std::mutex mutex;
    std::map<std::string, Connected> connected;
    std::chrono::steady_clock::time_point lastDiscovery;
    // Last member: stopped first in the destructor, so no answer arrives while the rest is torn down.
    RpcServer rpc;
};
