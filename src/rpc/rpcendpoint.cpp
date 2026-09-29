#include "rpcendpoint.h"

#include "observationmacros.h"
#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <print>
#include <unistd.h>

namespace {

auto nowNs() -> int64_t {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

} // namespace

struct RpcEndpoint::Queued {
    RpcServer::ConnectionId connection = 0;
    rpc::Json message;
    std::vector<std::byte> attachment;
};

RpcEndpoint::RpcEndpoint() = default;

RpcEndpoint::~RpcEndpoint() {
    stop();
}

auto RpcEndpoint::start(const RpcRegistry* methods, std::string endpointKind, std::string label) -> bool {
    registry = methods;
    kind = std::move(endpointKind);
    server = std::make_unique<RpcServer>();
    server->setRequestHandler([this](RpcServer::ConnectionId connection, const rpc::Json& message, std::vector<std::byte> attachment) {
        std::lock_guard lock(queueMutex);
        queue.push_back(std::make_unique<Queued>(Queued{.connection = connection, .message = message, .attachment = std::move(attachment)}));
    });
    server->setConnectionHandler([this](RpcServer::ConnectionId connection, bool connected) {
        if (connected) {
            OBS_EVENT("Engine", "RpcConnected", kind).field("connection", (int64_t) connection);
        } else {
            OBS_EVENT("Engine", "RpcDisconnected", kind).field("connection", (int64_t) connection);
        }
    });
    auto port = server->start(0);
    if (!port) {
        std::println(stderr, "rpc: cannot listen: {}", port.error());
        server.reset();
        return false;
    }
    boundPort = *port;
    RpcEndpointInfo info = {
        .kind = kind,
        .pid = (int) getpid(),
        .port = boundPort,
        .projectRoot = rpcProjectRoot().string(),
        .label = std::move(label),
        .protocol = rpc::protocolVersion,
        .startedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
    };
    auto file = writeRpcEndpoint(info);
    if (!file) {
        std::println(stderr, "rpc: {}", file.error());
    } else {
        discoveryFile = *file;
    }
    OBS_EVENT("Engine", "RpcListening", kind).field("port", (int64_t) boundPort).field("discovery", discoveryFile.string());
    return true;
}

auto RpcEndpoint::stop() -> void {
    if (server) {
        server->stop();
        server.reset();
    }
    if (!discoveryFile.empty()) {
        removeRpcEndpoint(discoveryFile);
        discoveryFile.clear();
    }
}

auto RpcEndpoint::drain() -> void {
    auto drainStart = nowNs();
    while (true) {
        std::unique_ptr<Queued> queued;
        {
            std::lock_guard lock(queueMutex);
            if (queue.empty()) {
                return;
            }
            queued = std::move(queue.front());
            queue.pop_front();
        }
        auto& call = *queued;
        auto method = call.message["method"].get<std::string>();
        auto params = call.message.contains("params") ? call.message["params"] : rpc::Json::object();
        bool notification = !call.message.contains("id") || call.message["id"].is_null();
        auto id = notification ? rpc::Json() : call.message["id"];
        auto started = nowNs();
        auto* srv = server.get();
        auto connection = call.connection;
        RpcResponder responder([srv, connection, id, notification, method, started](const RpcReply& reply) {
            auto ms = (double) (nowNs() - started) * 1e-6;
            OBS_EVENT("Engine", "RpcCall", method).field("ms", ms).field("ok", reply.ok).field("code", (int64_t) reply.code);
            if (notification || srv == nullptr) {
                return;
            }
            if (reply.ok) {
                srv->send(connection, rpc::makeResult(id, *reply.result), reply.attachment);
            } else {
                srv->send(connection, rpc::makeError(id, reply.code, reply.message));
            }
        });
        registry->invoke(method, params, responder);
        if (nowNs() - drainStart > drainBudgetNs) {
            return;
        }
    }
}
