#include "tracestream.h"

#include "tracering.h"

#include <nlohmann/json.hpp>

#include <vector>

namespace trace {

namespace {

auto fieldJson(const FieldValue& value) -> rpc::Json {
    if (const auto* b = std::get_if<bool>(&value)) {
        return *b;
    }
    if (const auto* i = std::get_if<int64_t>(&value)) {
        return *i;
    }
    if (const auto* d = std::get_if<double>(&value)) {
        return *d;
    }
    return std::get<std::string>(value);
}

// An event as it travels: the JSONL line's fields, plus seq.
auto eventJson(const Event& event) -> rpc::Json {
    auto fields = rpc::Json::object();
    for (const auto& field : event.fields) {
        fields[field.key] = fieldJson(field.value);
    }
    return {
        {"seq", event.seq},
        {"ts_ns", event.tsNs},
        {"thread", event.thread},
        {"level", levelName(event.level)},
        {"category", event.category},
        {"type", event.type},
        {"name", event.name},
        {"text", event.text},
        {"fields", std::move(fields)},
    };
}

} // namespace

TraceStream::TraceStream(RpcServer& rpcServer, std::string processName) : server(rpcServer), process(std::move(processName)) {
    thread = std::thread([this] { loop(); });
}

TraceStream::~TraceStream() {
    running.store(false);
    if (thread.joinable()) {
        thread.join();
    }
}

auto TraceStream::handle(RpcServer::ConnectionId connection, const rpc::Json& message) -> bool {
    if (rpc::messageKind(message) != rpc::MessageKind::Request) {
        return false;
    }
    const auto& method = message["method"];
    if (method != "trace.subscribe" && method != "trace.unsubscribe") {
        return false;
    }
    const auto& id = message["id"];
    if (method == "trace.unsubscribe") {
        {
            std::lock_guard lock(mutex);
            subscribers.erase(connection);
        }
        server.send(connection, rpc::makeResult(id, rpc::Json::object()));
        return true;
    }
    auto params = message.contains("params") && message["params"].is_object() ? message["params"] : rpc::Json::object();
    auto sinceNs = params.value("since_ns", (uint64_t) 0);
    auto cursor = sinceNs > 0 ? ring().firstAtOrAfter(sinceNs) : ring().oldest();
    // Answered before the subscription starts, so the reply comes ahead of the first batch.
    server.send(connection, rpc::makeResult(id, {{"process", process}, {"from", cursor}, {"head", ring().head()}}));
    {
        std::lock_guard lock(mutex);
        subscribers[connection] = Subscriber{.cursor = cursor};
    }
    return true;
}

auto TraceStream::disconnected(RpcServer::ConnectionId connection) -> void {
    std::lock_guard lock(mutex);
    subscribers.erase(connection);
}

auto TraceStream::pump() -> bool {
    std::lock_guard lock(mutex);
    bool caughtUp = true;
    std::vector<Event> events;
    for (auto& [connection, subscriber] : subscribers) {
        while (subscriber.cursor < ring().head()) {
            if (server.queuedBytes(connection) > maxQueuedBytes) {
                caughtUp = false;
                break;
            }
            events.clear();
            auto read = ring().read(subscriber.cursor, maxEventsPerNotification, events);
            subscriber.cursor = read.next;
            auto list = rpc::Json::array();
            for (const auto& event : events) {
                list.push_back(eventJson(event));
            }
            server.send(connection, rpc::makeNotification("trace.events", {{"process", process}, {"dropped", read.dropped}, {"events", std::move(list)}}));
        }
    }
    return caughtUp;
}

auto TraceStream::loop() -> void {
    uint64_t sent = ring().head();
    while (running.load()) {
        ring().waitFor(sent + batchEvents, batchInterval);
        sent = ring().head();
        pump();
    }
}

auto TraceStream::finish(std::chrono::milliseconds timeout) -> void {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        bool caughtUp = pump();
        bool written = true;
        {
            std::lock_guard lock(mutex);
            for (const auto& [connection, subscriber] : subscribers) {
                if (server.queuedBytes(connection) > 0) {
                    written = false;
                }
            }
        }
        if (caughtUp && written) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

} // namespace trace
