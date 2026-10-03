#pragma once

#include "rpcprotocol.h"
#include "rpcresponder.h"

#include <functional>
#include <map>
#include <string>
#include <string_view>

// Named records: the data a process can give, each one JSON value. Every endpoint serves its records through
// introspect.list (names and descriptions) and introspect.get (one record), so a client shows any process's data
// without knowing its types.
//
// A producer answers through its responder, at once or later: a record that needs the next frame keeps the
// responder and completes it when the data is in. Producers run on whatever thread the endpoint dispatches calls on.
class RpcRecords {
public:
    using Producer = std::function<void(RpcResponder responder)>;

    auto add(std::string name, std::string description, Producer producer) -> void;
    // [{name, description}], sorted by name.
    auto list() const -> rpc::Json;
    // Runs the record's producer; fails the responder with invalidParams when there is no record of that name.
    auto get(std::string_view name, RpcResponder responder) const -> void;

private:
    struct Entry {
        std::string description;
        Producer producer;
    };
    std::map<std::string, Entry, std::less<>> records;
};

// Answers introspect.list and introspect.get from `records`. False when `method` is neither, so the caller
// dispatches it elsewhere.
auto serveRecordsCall(const RpcRecords& records, std::string_view method, const rpc::Json& params, RpcResponder responder) -> bool;
