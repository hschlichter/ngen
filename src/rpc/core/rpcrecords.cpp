#include "rpcrecords.h"

#include <nlohmann/json.hpp>

#include <format>

auto RpcRecords::add(std::string name, std::string description, Producer producer) -> void {
    records[std::move(name)] = Entry{.description = std::move(description), .producer = std::move(producer)};
}

auto RpcRecords::list() const -> rpc::Json {
    auto result = rpc::Json::array();
    for (const auto& [name, entry] : records) {
        result.push_back({{"name", name}, {"description", entry.description}});
    }
    return result;
}

auto RpcRecords::contains(std::string_view name) const -> bool {
    return records.contains(name);
}

auto RpcRecords::get(std::string_view name, RpcResponder responder) const -> void {
    auto it = records.find(name);
    if (it == records.end()) {
        responder.fail(rpc::invalidParams, std::format("no record '{}'", name));
        return;
    }
    it->second.producer(std::move(responder));
}

auto serveRecordsCall(const RpcRecords& records, std::string_view method, const rpc::Json& params, RpcResponder responder) -> bool {
    if (method == "introspect.list") {
        responder.respond({{"records", records.list()}});
        return true;
    }
    if (method == "introspect.get") {
        if (!params.is_object() || !params.contains("name") || !params["name"].is_string()) {
            responder.fail(rpc::invalidParams, "introspect.get needs \"name\", a record name");
            return true;
        }
        records.get(params["name"].get<std::string>(), std::move(responder));
        return true;
    }
    return false;
}
