#pragma once

#include "videotypes.h"

#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

// The WebRTC side: one peer connection to one watcher, with one H.264 track that the encoded frames are sent on. The only file that
// includes libdatachannel is videosender.cpp, and none of that library's exceptions leave it. Any thread may call any of this.
//
// The engine offers and the watch page answers: `offer` makes a connection (closing the one there was) and returns its SDP with the
// engine's own addresses in it, `accept` takes the page's answer. The connection is up once the page's checks have reached one of
// those addresses.
class VideoSender {
public:
    VideoSender();
    ~VideoSender();
    VideoSender(const VideoSender&) = delete;
    auto operator=(const VideoSender&) -> VideoSender& = delete;

    // `bindAddress`: the one address the connection's sockets use; empty or 0.0.0.0 for every interface. With `control`, the offer
    // carries a data channel named "control" beside the video: the watcher's messages on it go to the control handler.
    auto offer(const std::string& bindAddress, bool control) -> std::expected<std::string, std::string>;
    auto accept(const std::string& answerSdp) -> std::expected<void, std::string>;
    // False when there is no watcher to send to.
    auto send(const VideoPacket& packet) -> bool;
    auto state() const -> VideoWatcherState;
    // Which watcher this is: a number that changes with every offer; 0 with none. A watch page tells by it whether it is still the
    // one, or another took its place.
    auto session() const -> uint64_t;
    // The watcher's address once connected; empty before.
    auto watcherAddress() const -> std::string;
    // True once after a watcher connected or asked for a keyframe.
    auto takeKeyframeRequest() -> bool;
    // Keyframes the watcher asked for since the sender was made.
    auto keyframeRequests() const -> uint64_t;
    // How many watchers have connected, and how many of those have gone: the difference is the one connected now.
    auto connects() const -> uint64_t;
    auto disconnects() const -> uint64_t;
    auto close() -> void;

    // The control channel (remotecontrol.h): text messages to and from the watcher, on the video's connection.
    // The handler gets each message with the session it came on. It runs on one of libdatachannel's threads and must not block; an
    // empty handler takes none. Setting it waits for a call that is running.
    using ControlHandler = std::function<void(uint64_t session, std::string message)>;
    auto setControlHandler(ControlHandler handler) -> void;
    // Sends to the watcher of `session`. False when that watcher is gone, the channel isn't open, or the message is over the limit.
    auto sendControl(uint64_t session, std::string_view message) -> bool;
    // Whether the current watcher's control channel is open.
    auto controlOpen() const -> bool;
    // The largest message the watcher takes, in bytes; 0 with no open channel.
    auto controlMessageLimit() const -> size_t;
    // Bytes handed to the channel and not yet sent.
    auto controlBuffered() const -> size_t;

private:
    struct Session;
    struct Shared;
    std::shared_ptr<Shared> shared;
};
