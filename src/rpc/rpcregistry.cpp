#include "rpcregistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>

namespace {

auto typeName(RpcType type) -> const char* {
    switch (type) {
        case RpcType::Bool:
            return "bool";
        case RpcType::Int:
            return "int";
        case RpcType::Float:
            return "float";
        case RpcType::String:
            return "string";
        case RpcType::Vec3:
            return "vec3";
        case RpcType::Enum:
            return "enum";
        case RpcType::Array:
            return "array";
        case RpcType::Object:
            return "object";
    }
    return "?";
}

auto matches(const RpcField& field, const rpc::Json& value) -> bool {
    switch (field.type) {
        case RpcType::Bool:
            return value.is_boolean();
        case RpcType::Int:
            return value.is_number_integer();
        case RpcType::Float:
            return value.is_number();
        case RpcType::String:
            return value.is_string();
        case RpcType::Vec3:
            return value.is_array() && value.size() == 3 && std::ranges::all_of(value, [](const rpc::Json& v) { return v.is_number(); });
        case RpcType::Enum:
            return value.is_string() && std::ranges::find(field.enumValues, value.get<std::string>()) != field.enumValues.end();
        case RpcType::Array:
            return value.is_array();
        case RpcType::Object:
            return value.is_object();
    }
    return false;
}

auto check(const RpcMethodDesc& desc, const rpc::Json& params) -> std::optional<std::string> {
    if (!params.is_null() && !params.is_object()) {
        return "params must be an object";
    }
    for (const auto& field : desc.params) {
        bool present = params.is_object() && params.contains(field.name) && !params[field.name].is_null();
        if (!present) {
            if (field.required) {
                return std::format("missing required parameter '{}' ({})", field.name, typeName(field.type));
            }
            continue;
        }
        if (!matches(field, params[field.name])) {
            if (field.type == RpcType::Enum) {
                std::string values;
                for (const auto& v : field.enumValues) {
                    values += values.empty() ? v : "|" + v;
                }
                return std::format("parameter '{}' must be one of {}", field.name, values);
            }
            return std::format("parameter '{}' must be {}", field.name, typeName(field.type));
        }
    }
    if (params.is_object()) {
        for (const auto& [key, value] : params.items()) {
            bool known = std::ranges::any_of(desc.params, [&](const RpcField& f) { return f.name == key; });
            if (!known) {
                return std::format("unknown parameter '{}'", key);
            }
        }
    }
    return std::nullopt;
}

} // namespace

auto RpcRegistry::add(RpcMethodDesc desc, RpcHandler handler) -> void {
    auto name = desc.name;
    methods[name] = Entry{.desc = std::move(desc), .handler = std::move(handler)};
}

auto RpcRegistry::find(std::string_view name) const -> const RpcMethodDesc* {
    auto it = methods.find(name);
    return it != methods.end() ? &it->second.desc : nullptr;
}

auto RpcRegistry::invoke(std::string_view name, const rpc::Json& params, RpcResponder responder) const -> void {
    auto it = methods.find(name);
    if (it == methods.end()) {
        responder.fail(rpc::methodNotFound, std::format("unknown method '{}'", name));
        return;
    }
    if (auto error = check(it->second.desc, params); error.has_value()) {
        responder.fail(rpc::invalidParams, *error);
        return;
    }
    try {
        it->second.handler(params.is_null() ? rpc::Json::object() : params, responder);
    } catch (const rpc::Json::exception& e) {
        responder.fail(rpc::internalError, e.what());
    }
}

auto RpcRegistry::describe() const -> rpc::Json {
    auto list = rpc::Json::array();
    for (const auto& [name, entry] : methods) {
        auto params = rpc::Json::array();
        for (const auto& field : entry.desc.params) {
            rpc::Json f = {
                {"name", field.name},
                {"type", typeName(field.type)},
                {"required", field.required},
                {"description", field.description},
            };
            if (!field.enumValues.empty()) {
                f["values"] = field.enumValues;
            }
            params.push_back(std::move(f));
        }
        list.push_back({{"name", name}, {"summary", entry.desc.summary}, {"params", params}, {"result", entry.desc.result}});
    }
    return list;
}

auto registerRpcBuiltins(RpcRegistry& registry, std::string kind) -> void {
    registry.add({.name = "rpc.describe", .summary = "Every method this endpoint serves, with parameter schemas.", .result = "array of {name, summary, params, result}"}, [&registry](const rpc::Json&, RpcResponder responder) {
        responder.respond(registry.describe());
    });
    registry.add({.name = "rpc.ping", .summary = "Answers at once; for liveness and latency checks.", .result = "\"pong\""}, [](const rpc::Json&, RpcResponder responder) {
        responder.respond("pong");
    });
    registry.add({.name = "rpc.version", .summary = "Protocol version and endpoint kind.", .result = "{protocol, kind}"}, [kind](const rpc::Json&, RpcResponder responder) {
        responder.respond({{"protocol", rpc::protocolVersion}, {"kind", kind}});
    });
}

auto registerRpcRecords(RpcRegistry& registry, const RpcRecords& records) -> void {
    registry.add({.name = "introspect.list", .summary = "The records this endpoint gives: its data, by name.", .result = "{records: [{name, description}]}"}, [&records](const rpc::Json& params, RpcResponder responder) {
        serveRecordsCall(records, "introspect.list", params, std::move(responder));
    });
    registry.add(
        {
            .name = "introspect.get",
            .summary = "One record as JSON.",
            .params = {{.name = "name", .type = RpcType::String, .required = true, .description = "record name, from introspect.list"}},
            .result = "the record",
        },
        [&records](const rpc::Json& params, RpcResponder responder) {
            serveRecordsCall(records, "introspect.get", params, std::move(responder));
        });
}
