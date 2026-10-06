#include "rpcendpoint.h"

#include "rpcdiscovery.h"
#include "trace.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <format>
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
    traceStream = std::make_unique<trace::TraceStream>(*server, std::format("{}:{}", kind, getpid()));
    server->setRequestHandler([this](RpcServer::ConnectionId connection, const rpc::Json& message, std::vector<std::byte> attachment) {
        if (traceStream->handle(connection, message)) {
            return;
        }
        std::lock_guard lock(queueMutex);
        queue.push_back(std::make_unique<Queued>(Queued{.connection = connection, .message = message, .attachment = std::move(attachment)}));
    });
    server->setConnectionHandler([this](RpcServer::ConnectionId connection, bool connected) {
        if (connected) {
            TRACE_EVENT("Engine", "RpcConnected", kind).text(std::format("rpc: connection {} opened", connection)).field("connection", (int64_t) connection);
        } else {
            traceStream->disconnected(connection);
            TRACE_EVENT("Engine", "RpcDisconnected", kind).text(std::format("rpc: connection {} closed", connection)).field("connection", (int64_t) connection);
        }
    });
    auto port = server->start(0);
    if (!port) {
        TRACE_ERROR("Engine", "RpcListenFailed", kind).text(std::format("rpc: cannot listen: {}", port.error()));
        traceStream.reset();
        server.reset();
        return false;
    }
    boundPort = *port;
    RpcEndpointInfo info = {
        .kind = kind,
        .pid = (int) getpid(),
        .port = boundPort,
        .label = std::move(label),
        .protocol = rpc::protocolVersion,
        .startedUnixMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
    };
    auto file = writeRpcEndpoint(info);
    if (!file) {
        TRACE_ERROR("Engine", "RpcDiscoveryFailed", kind).text(std::format("rpc: {}", file.error()));
    } else {
        discoveryFile = *file;
    }
    TRACE_EVENT("Engine", "RpcListening", kind)
        .text(std::format("rpc: listening on 127.0.0.1:{}", boundPort))
        .field("port", (int64_t) boundPort)
        .field("discovery", discoveryFile.string());
    return true;
}

auto RpcEndpoint::finishTrace(std::chrono::milliseconds timeout) -> void {
    if (traceStream) {
        traceStream->finish(timeout);
    }
}

auto RpcEndpoint::stop() -> void {
    // The server stops first (its connection handler still reaches the trace stream as connections close); the
    // stream goes before the server object it sends on.
    if (server) {
        server->stop();
    }
    traceStream.reset();
    server.reset();
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
        auto* srv = server.get();
        auto connection = call.connection;
        RpcResponder responder([srv, connection, id, notification, method](const RpcReply& reply) {
            if (!reply.ok) {
                TRACE_WARNING("Engine", "RpcCallFailed", method).text(reply.message).field("code", (int64_t) reply.code);
            }
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
