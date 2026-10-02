#include "assetclient.h"

#include "rpcdiscovery.h"

#include "rpcprotocol.h"
#include "rpcserver.h"
#include <nlohmann/json.hpp>

#include <condition_variable>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

struct AssetClient::State {
    RpcServer rpc;
    RpcServer::ConnectionId connection = 0;
    bool connected = false;

    mutable std::mutex mutex;
    std::condition_variable changed;
    std::unordered_map<std::string, PackedAsset> partial; // arriving: asset.data seen, asset.ready not yet
    std::unordered_map<std::string, std::unique_ptr<PackedAsset>> ready;
    std::unordered_map<std::string, std::vector<std::string>> failed;
    AssetClientStats stats;

    auto fail(const std::string& id, std::vector<std::string> messages) -> void {
        partial.erase(id);
        failed[id] = std::move(messages);
        stats.failed++;
    }

    auto onMessage(const rpc::Json& message, std::vector<std::byte> attachment) -> void {
        if (rpc::messageKind(message) != rpc::MessageKind::Notification) {
            return;
        }
        auto method = message["method"].get<std::string>();
        const auto& params = message["params"];
        auto id = params.value("id", std::string());
        std::lock_guard lock(mutex);
        if (method == "asset.data") {
            auto& asset = partial[id];
            auto offset = params.value("offset", size_t(0));
            auto total = params.value("total", size_t(0));
            asset.id = id;
            asset.version = params.value("version", std::string());
            asset.bytes.resize(total);
            if (offset + attachment.size() <= total) {
                std::memcpy(asset.bytes.data() + offset, attachment.data(), attachment.size());
            }
            stats.bytes += attachment.size();
        } else if (method == "asset.ready") {
            if (params.value("sent", false) && params.value("size", size_t(0)) == 0 && !partial.contains(id)) {
                // An empty asset: no asset.data precedes it.
                partial[id] = PackedAsset{.id = id, .version = params.value("version", std::string()), .bytes = {}};
            }
            auto it = partial.find(id);
            if (it == partial.end() && !ready.contains(id)) {
                // Ready without data: only for a version the client said it holds, which this client never does.
                fail(id, {std::format("{}: the asset server sent no data", id)});
            } else {
                stats.received++;
                if (params.value("packed", false)) {
                    stats.packed++;
                } else {
                    stats.cached++;
                }
                if (it != partial.end()) {
                    ready[id] = std::make_unique<PackedAsset>(std::move(it->second));
                    partial.erase(it);
                    failed.erase(id);
                }
            }
        } else if (method == "asset.failed") {
            std::vector<std::string> messages;
            for (const auto& line : params.value("errors", rpc::Json::array())) {
                messages.push_back(line.get<std::string>());
            }
            fail(id, std::move(messages));
        } else {
            return;
        }
        changed.notify_all();
    }
};

AssetClient::AssetClient() : state(std::make_unique<State>()) {
}

AssetClient::~AssetClient() {
    state->rpc.stop();
}

auto AssetClient::connect(const std::string& variant) -> std::expected<void, std::string> {
    const RpcEndpointInfo* server = nullptr;
    auto endpoints = listRpcEndpoints();
    for (const auto& endpoint : endpoints) {
        if (endpoint.kind == "asset" && endpoint.label == variant) {
            server = &endpoint;
            break;
        }
    }
    if (server == nullptr) {
        return std::unexpected(std::format("no asset server for {} is running in {}; start that variant's ngen-asset-server here", variant, std::filesystem::current_path().string()));
    }
    state->rpc.setRequestHandler([this](RpcServer::ConnectionId, const rpc::Json& message, std::vector<std::byte> attachment) {
        state->onMessage(message, std::move(attachment));
    });
    state->rpc.setConnectionHandler([this](RpcServer::ConnectionId, bool connected) {
        if (connected) {
            return;
        }
        // Everything still pending fails: the server is gone.
        std::lock_guard lock(state->mutex);
        state->connected = false;
        state->changed.notify_all();
    });
    state->rpc.startWithoutListening();
    auto connection = state->rpc.connect(server->port);
    if (!connection) {
        state->rpc.stop();
        return std::unexpected(std::format("cannot reach the asset server on port {}: {}", server->port, connection.error()));
    }
    std::lock_guard lock(state->mutex);
    state->connection = *connection;
    state->connected = true;
    return {};
}

auto AssetClient::request(std::span<const std::string> ids) -> void {
    rpc::Json list = rpc::Json::array();
    for (const auto& id : ids) {
        list.push_back(id);
    }
    std::vector<std::string> copy(ids.begin(), ids.end());
    {
        // A new request supersedes an earlier failure for the same id.
        std::lock_guard lock(state->mutex);
        for (const auto& id : ids) {
            state->failed.erase(id);
        }
        state->stats.requested += ids.size();
    }
    state->rpc.call(state->connection, "asset.request", {{"ids", list}}, [this, copy](const rpc::Json& response, std::vector<std::byte>) {
        if (!response.contains("error")) {
            return;
        }
        std::lock_guard lock(state->mutex);
        auto message = response["error"].value("message", std::string("asset.request failed"));
        for (const auto& id : copy) {
            state->fail(id, {message});
        }
        state->changed.notify_all();
    });
}

auto AssetClient::wait(std::span<const std::string> ids) -> void {
    std::unique_lock lock(state->mutex);
    state->changed.wait(lock, [&] {
        if (!state->connected) {
            return true;
        }
        for (const auto& id : ids) {
            if (!state->ready.contains(id) && !state->failed.contains(id)) {
                return false;
            }
        }
        return true;
    });
    if (state->connected) {
        return;
    }
    for (const auto& id : ids) {
        if (!state->ready.contains(id) && !state->failed.contains(id)) {
            state->fail(id, {"the connection to the asset server closed"});
        }
    }
}

auto AssetClient::find(const std::string& id) const -> const PackedAsset* {
    std::lock_guard lock(state->mutex);
    auto it = state->ready.find(id);
    return it == state->ready.end() ? nullptr : it->second.get();
}

auto AssetClient::take(const std::string& id) -> std::optional<PackedAsset> {
    std::lock_guard lock(state->mutex);
    auto it = state->ready.find(id);
    if (it == state->ready.end()) {
        return std::nullopt;
    }
    auto asset = std::move(*it->second);
    state->ready.erase(it);
    return asset;
}

auto AssetClient::stats() const -> AssetClientStats {
    std::lock_guard lock(state->mutex);
    return state->stats;
}

auto AssetClient::errors(const std::string& id) const -> std::vector<std::string> {
    std::lock_guard lock(state->mutex);
    auto it = state->failed.find(id);
    return it == state->failed.end() ? std::vector<std::string>() : it->second;
}
