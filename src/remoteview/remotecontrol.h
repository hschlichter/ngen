#pragma once

#include "remoteviewstatus.h"

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

class RemoteView;
class RpcRegistry;

// Remote control (src/remoteview/README.md): the watch page's calls, run as the loopback endpoint's are. The watcher sends JSON-RPC
// messages on the control channel of its connection (VideoSender); they are queued on libdatachannel's thread and run here, on the
// main thread, through the view's registry, so a call from the page and a call over loopback are the same call. The answer goes back
// on the channel. log.subscribe is answered here: the log's events are sent from a cursor into the ring, a little each frame.
//
// There is no authentication: with remoteview.control on, whoever reaches the watch page can call every method.
class RemoteControl {
public:
    // Both must outlive this.
    RemoteControl(RemoteView& remoteView, const RpcRegistry& registry);
    ~RemoteControl();
    RemoteControl(const RemoteControl&) = delete;
    auto operator=(const RemoteControl&) -> RemoteControl& = delete;

    // Main thread, where the loopback endpoint's calls run: runs waiting calls for at most drainBudgetNs (at least one), then sends
    // the log events that are waiting.
    auto drain() -> void;
    [[nodiscard]] auto status() const -> const RemoteControlStatus& { return current; }

    static constexpr int64_t drainBudgetNs = 2'000'000;
    // Messages waiting to run at most; more are dropped and counted.
    static constexpr size_t maxQueued = 4096;
    // Log events' JSON sent in one frame at most, in bytes.
    static constexpr size_t logBytesPerFrame = 65536;

private:
    struct Message {
        uint64_t session = 0;
        std::string text;
    };
    // What answers that complete later still reach: the counts, and whether this is still here.
    struct Shared;

    auto run(const Message& message) -> void;
    auto sendLog() -> void;

    RemoteView& remoteView;
    const RpcRegistry& registry;
    std::shared_ptr<Shared> shared;
    std::mutex queueMutex;
    std::deque<Message> queue;
    uint64_t droppedMessages = 0; // under queueMutex

    RemoteControlStatus current;
    bool wasOpen = false;
    uint64_t logSession = 0; // the session that subscribed; 0 for none
    uint64_t logCursor = 0;
    uint64_t logDropped = 0;
    std::string process;
};
