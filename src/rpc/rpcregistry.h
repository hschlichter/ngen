#pragma once

#include "rpcprotocol.h"
#include "rpcrecords.h"
#include "rpcresponder.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

// Methods an endpoint serves. Each has a name (`area.verb`), a one-line summary, typed
// parameters checked before the handler runs, and a result description. rpc.describe returns
// all of it, so clients discover the surface without reading source.
enum class RpcType {
    Bool,
    Int,
    Float, // also accepts integers
    String,
    Vec3, // [x, y, z]
    Enum, // a string from enumValues
    Array,
    Object,
};

struct RpcField {
    std::string name;
    RpcType type = RpcType::String;
    bool required = false;
    std::string description;
    std::vector<std::string> enumValues;
};

struct RpcMethodDesc {
    std::string name;
    std::string summary;
    std::vector<RpcField> params;
    std::string result; // prose: what the result holds
};

using RpcHandler = std::function<void(const rpc::Json& params, RpcResponder responder)>;

class RpcRegistry {
public:
    auto add(RpcMethodDesc desc, RpcHandler handler) -> void;
    auto find(std::string_view name) const -> const RpcMethodDesc*;
    // Checks params against the method's schema, then runs the handler. Unknown methods fail
    // with methodNotFound, schema mismatches with invalidParams naming the field. A handler
    // that throws a JSON error fails with internalError.
    auto invoke(std::string_view name, const rpc::Json& params, RpcResponder responder) const -> void;
    auto describe() const -> rpc::Json;

private:
    struct Entry {
        RpcMethodDesc desc;
        RpcHandler handler;
    };
    std::map<std::string, Entry, std::less<>> methods;
};

// rpc.describe, rpc.ping and rpc.version. `kind` is the endpoint kind ("view", …).
auto registerRpcBuiltins(RpcRegistry& registry, std::string kind) -> void;

// introspect.list and introspect.get, answered from `records`, which must outlive the registry.
auto registerRpcRecords(RpcRegistry& registry, const RpcRecords& records) -> void;
