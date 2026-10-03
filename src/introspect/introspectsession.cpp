#include "introspectsession.h"

#include "introspecttarget.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

IntrospectSession::IntrospectSession() {
    rpc.setConnectionHandler([this](RpcServer::ConnectionId connection, bool isConnected) {
        if (!isConnected) {
            onDisconnected(connection);
        }
    });
    rpc.startWithoutListening();
}

IntrospectSession::~IntrospectSession() {
    rpc.stop();
}

auto IntrospectSession::poll() -> void {
    auto now = std::chrono::steady_clock::now();
    if (now - lastDiscovery < discoveryInterval) {
        return;
    }
    lastDiscovery = now;
    auto endpoints = listRpcEndpoints();

    std::set<std::string> live;
    std::vector<RpcEndpointInfo> fresh;
    {
        std::lock_guard lock(mutex);
        for (const auto& endpoint : endpoints) {
            auto name = processName(endpoint);
            live.insert(name);
            if (!connected.contains(name)) {
                fresh.push_back(endpoint);
            }
        }
        for (auto it = connected.begin(); it != connected.end();) {
            if (!live.contains(it->first)) {
                it = connected.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Connected without the lock held: connect() runs the connection handler on this thread.
    for (const auto& endpoint : fresh) {
        auto name = processName(endpoint);
        auto connection = rpc.connect(endpoint.port);
        if (!connection) {
            continue;
        }
        {
            std::lock_guard lock(mutex);
            Connected entry;
            entry.process.name = name;
            entry.process.endpoint = endpoint;
            entry.process.connected = true;
            entry.connection = *connection;
            connected[name] = std::move(entry);
        }
        rpc.call(*connection, "introspect.list", rpc::Json::object(), [this, name](const rpc::Json& response, std::vector<std::byte>) {
            onRecordList(name, response);
        });
    }
}

auto IntrospectSession::onRecordList(const std::string& process, const rpc::Json& response) -> void {
    std::lock_guard lock(mutex);
    auto it = connected.find(process);
    if (it == connected.end()) {
        return;
    }
    auto& entry = it->second.process;
    if (response.contains("error")) {
        entry.error = response["error"].value("message", std::string("introspect.list failed"));
        return;
    }
    entry.records.clear();
    for (const auto& record : response["result"].value("records", rpc::Json::array())) {
        entry.records.push_back({.name = record.value("name", ""), .description = record.value("description", "")});
    }
}

auto IntrospectSession::onDisconnected(RpcServer::ConnectionId connection) -> void {
    std::lock_guard lock(mutex);
    for (auto it = connected.begin(); it != connected.end(); ++it) {
        if (it->second.connection == connection) {
            connected.erase(it);
            return;
        }
    }
}

auto IntrospectSession::processes() const -> std::vector<Process> {
    std::vector<Process> result;
    std::lock_guard lock(mutex);
    for (const auto& [name, entry] : connected) {
        result.push_back(entry.process);
    }
    std::ranges::sort(result, [](const Process& a, const Process& b) {
        if (a.endpoint.kind != b.endpoint.kind) {
            return a.endpoint.kind < b.endpoint.kind;
        }
        return a.endpoint.pid < b.endpoint.pid;
    });
    return result;
}

auto IntrospectSession::fetch(const std::string& process, const std::string& record) -> void {
    RpcServer::ConnectionId connection = 0;
    {
        std::lock_guard lock(mutex);
        auto it = connected.find(process);
        if (it == connected.end()) {
            return;
        }
        auto& value = it->second.values[record];
        if (value.pending) {
            return;
        }
        value.pending = true;
        connection = it->second.connection;
    }
    auto sent = std::chrono::steady_clock::now();
    rpc.call(connection, "introspect.get", {{"name", record}}, [this, process, record, sent](const rpc::Json& response, std::vector<std::byte>) {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(mutex);
        auto it = connected.find(process);
        if (it == connected.end()) {
            return;
        }
        auto& value = it->second.values[record];
        value.pending = false;
        value.roundTripMs = std::chrono::duration<double, std::milli>(now - sent).count();
        value.received = now;
        if (response.contains("error")) {
            value.error = response["error"].value("message", std::string("introspect.get failed"));
            return;
        }
        value.error.clear();
        value.value = std::make_shared<const rpc::Json>(response["result"]);
    });
}

auto IntrospectSession::record(const std::string& process, const std::string& record) const -> RecordValue {
    std::lock_guard lock(mutex);
    auto it = connected.find(process);
    if (it == connected.end()) {
        return {};
    }
    auto value = it->second.values.find(record);
    if (value == it->second.values.end()) {
        return {};
    }
    return value->second;
}
