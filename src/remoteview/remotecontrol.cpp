#include "remotecontrol.h"

#include "log.h"
#include "logring.h"
#include "logstream.h"
#include "profile.h"
#include "remoteview.h"
#include "remoteviewcvars.h"
#include "rpcprotocol.h"
#include "rpcregistry.h"

#include <nlohmann/json.hpp>

#include <unistd.h>

#include <atomic>
#include <format>
#include <vector>

namespace {

using Json = rpc::Json;

} // namespace

struct RemoteControl::Shared {
    std::atomic<bool> alive{true};
    std::atomic<uint64_t> calls{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> tooLarge{0};
};

RemoteControl::RemoteControl(RemoteView& view, const RpcRegistry& methods)
    : remoteView(view), registry(methods), shared(std::make_shared<Shared>()), process(std::format("view:{}", getpid())) {
    remoteView.videoSender().setControlHandler([this](uint64_t session, std::string message) {
        std::lock_guard lock(queueMutex);
        if (queue.size() >= maxQueued) {
            droppedMessages++;
            return;
        }
        queue.push_back(Message{.session = session, .text = std::move(message)});
    });
}

RemoteControl::~RemoteControl() {
    remoteView.videoSender().setControlHandler({});
    shared->alive = false;
}

auto RemoteControl::drain() -> void {
    PROFILE_ZONE_DETAIL("RemoteControlDrain", Frame);
    auto& sender = remoteView.videoSender();
    bool open = sender.controlOpen();
    if (open != wasOpen) {
        wasOpen = open;
        if (open) {
            LOG_EVENT("RemoteView", "RemoteControlOpened", "remoteview")
                .text(std::format("remote control: the watcher's control channel is open (messages up to {} bytes)", sender.controlMessageLimit()))
                .field("messageLimit", (int64_t) sender.controlMessageLimit());
        } else {
            LOG_EVENT("RemoteView", "RemoteControlClosed", "remoteview").text("remote control: the watcher's control channel closed");
        }
    }
    if (logSession != 0 && logSession != sender.session()) {
        logSession = 0;
    }

    uint64_t start = profile::now();
    while (true) {
        Message message;
        {
            std::lock_guard lock(queueMutex);
            if (queue.empty()) {
                break;
            }
            message = std::move(queue.front());
            queue.pop_front();
        }
        run(message);
        if ((int64_t) (profile::now() - start) > drainBudgetNs) {
            break;
        }
    }
    sendLog();

    current.enabled = remoteviewcvars::control.get();
    current.open = open;
    current.calls = shared->calls;
    current.failed = shared->failed;
    current.tooLarge = shared->tooLarge;
    current.messageLimit = sender.controlMessageLimit();
    current.logSubscribed = logSession != 0;
    {
        std::lock_guard lock(queueMutex);
        current.dropped = droppedMessages;
    }
}

auto RemoteControl::run(const Message& message) -> void {
    auto& sender = remoteView.videoSender();
    uint64_t session = message.session;
    auto json = Json::parse(message.text, nullptr, false);
    if (json.is_discarded()) {
        sender.sendControl(session, rpc::makeError(Json(), rpc::parseError, "not JSON").dump());
        return;
    }
    auto kind = rpc::messageKind(json);
    if (kind != rpc::MessageKind::Request && kind != rpc::MessageKind::Notification) {
        sender.sendControl(session, rpc::makeError(json.contains("id") ? json["id"] : Json(), rpc::invalidRequest, "expected a request").dump());
        return;
    }
    auto method = json["method"].get<std::string>();
    auto params = json.contains("params") ? json["params"] : Json::object();
    bool notification = kind == rpc::MessageKind::Notification;
    auto id = notification ? Json() : json["id"];
    current.lastMethod = method;
    shared->calls++;

    // The log is the one thing this answers itself: its events are sent from here, a little each frame.
    if (method == "log.subscribe") {
        auto sinceNs = params.is_object() ? params.value("since_ns", (uint64_t) 0) : (uint64_t) 0;
        auto& ring = logging::ring();
        logCursor = sinceNs > 0 ? ring.firstAtOrAfter(sinceNs) : ring.oldest();
        logSession = session;
        logDropped = 0;
        if (!notification) {
            sender.sendControl(session, rpc::makeResult(id, {{"process", process}, {"from", logCursor}, {"head", ring.head()}}).dump());
        }
        return;
    }
    if (method == "log.unsubscribe") {
        logSession = 0;
        if (!notification) {
            sender.sendControl(session, rpc::makeResult(id, Json::object()).dump());
        }
        return;
    }

    auto* videoSender = &sender;
    auto state = shared;
    RpcResponder responder([videoSender, state, session, id, notification, method](const RpcReply& reply) {
        if (!reply.ok) {
            state->failed++;
            LOG_WARNING("Engine", "RpcCallFailed", method).text(reply.message).field("code", (int64_t) reply.code).field("from", "remote control");
        }
        // The sender belongs to the remote view, which outlives the remote control; an answer that completes after that is dropped.
        if (notification || !state->alive) {
            return;
        }
        if (!reply.ok) {
            videoSender->sendControl(session, rpc::makeError(id, reply.code, reply.message).dump());
            return;
        }
        auto text = rpc::makeResult(id, *reply.result).dump();
        size_t limit = videoSender->controlMessageLimit();
        if (limit != 0 && text.size() > limit) {
            state->tooLarge++;
            auto error = std::format("{}: the result is {} bytes, over the control channel's {}", method, text.size(), limit);
            videoSender->sendControl(session, rpc::makeError(id, rpc::appError, error).dump());
            return;
        }
        videoSender->sendControl(session, text);
    });
    registry.invoke(method, params, responder);
}

// The subscriber's share of the log for this frame: events from its cursor on, as log.events notifications of the shape the loopback
// endpoint's log stream sends, until logBytesPerFrame are sent or the channel has too much waiting.
auto RemoteControl::sendLog() -> void {
    if (logSession == 0) {
        return;
    }
    auto& sender = remoteView.videoSender();
    size_t limit = sender.controlMessageLimit();
    if (limit == 0 || sender.controlBuffered() > 4 * logBytesPerFrame) {
        return;
    }
    auto& ring = logging::ring();
    if (logCursor >= ring.head()) {
        return;
    }
    size_t budget = std::min(logBytesPerFrame, limit) - 256; // room for the notification around the events
    std::vector<logging::Event> events;
    auto read = ring.read(logCursor, 256, events);
    logDropped += read.dropped;
    auto list = Json::array();
    size_t bytes = 0;
    uint64_t next = logCursor + read.dropped;
    for (const auto& event : events) {
        auto json = logging::eventJson(event);
        size_t size = json.dump().size() + 1;
        if (size > budget) {
            // One event larger than a message: it can't be sent whole, so it counts as dropped.
            logDropped++;
            next = event.seq + 1;
            continue;
        }
        if (bytes + size > budget) {
            break;
        }
        bytes += size;
        list.push_back(std::move(json));
        next = event.seq + 1;
    }
    if (events.empty()) {
        next = read.next;
    }
    logCursor = next;
    if (list.empty() && logDropped == 0) {
        return;
    }
    size_t count = list.size();
    auto notification = rpc::makeNotification("log.events", {{"process", process}, {"dropped", logDropped}, {"events", std::move(list)}});
    if (sender.sendControl(logSession, notification.dump())) {
        current.logEvents += count;
        logDropped = 0;
    }
}
