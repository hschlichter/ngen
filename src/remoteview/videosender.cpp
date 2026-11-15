#include "videosender.h"

#include <rtc/rtc.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <mutex>

namespace {

constexpr uint8_t payloadType = 96;
constexpr uint32_t ssrc = 42;
constexpr const char* cname = "ngen";
constexpr std::chrono::seconds gatherTimeout{3};

} // namespace

// One watcher's connection. libdatachannel's callbacks hold it, so it outlives a sender that has moved on to the next watcher.
struct VideoSender::Session {
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::Track> track;
    uint64_t startNs = 0; // the first frame's capture time: the track's time zero
};

struct VideoSender::Shared {
    mutable std::mutex mutex;
    std::shared_ptr<Session> session;
    uint64_t generation = 0; // which session the callbacks may speak for
    std::atomic<VideoWatcherState> state{VideoWatcherState::None};
    std::atomic<bool> keyframeWanted{false};
    std::atomic<uint64_t> keyframeRequests{0};
    std::atomic<uint64_t> connects{0};
    std::atomic<uint64_t> disconnects{0};
};

VideoSender::VideoSender() : shared(std::make_shared<Shared>()) {
}

VideoSender::~VideoSender() {
    close();
}

auto VideoSender::offer(const std::string& bindAddress) -> std::expected<std::string, std::string> {
    close();
    try {
        rtc::Configuration configuration;
        if (!bindAddress.empty() && bindAddress != "0.0.0.0") {
            configuration.bindAddress = bindAddress;
        }
        auto session = std::make_shared<Session>();
        session->connection = std::make_shared<rtc::PeerConnection>(configuration);

        uint64_t generation = 0;
        {
            std::lock_guard lock(shared->mutex);
            shared->generation++;
            generation = shared->generation;
            shared->session = session;
            shared->state = VideoWatcherState::Connecting;
        }

        // The callbacks speak only while their session is the current one: a watcher that was replaced must not mark the new one gone.
        std::weak_ptr<Shared> weak = shared;
        auto gathered = std::make_shared<std::promise<void>>();
        auto gatheredOnce = std::make_shared<std::atomic<bool>>(false);
        auto gatheredFuture = gathered->get_future();
        session->connection->onGatheringStateChange([gathered, gatheredOnce](rtc::PeerConnection::GatheringState state) {
            if (state == rtc::PeerConnection::GatheringState::Complete && !gatheredOnce->exchange(true)) {
                gathered->set_value();
            }
        });
        session->connection->onStateChange([weak, generation](rtc::PeerConnection::State state) {
            auto strong = weak.lock();
            if (strong == nullptr) {
                return;
            }
            bool gone = state == rtc::PeerConnection::State::Disconnected || state == rtc::PeerConnection::State::Failed ||
                        state == rtc::PeerConnection::State::Closed;
            if (!gone) {
                return;
            }
            std::lock_guard lock(strong->mutex);
            if (strong->generation != generation) {
                return;
            }
            if (strong->state.exchange(VideoWatcherState::None) == VideoWatcherState::Connected) {
                strong->disconnects++;
            }
        });

        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
        media.addH264Codec(payloadType);
        media.addSSRC(ssrc, cname, "ngen-remoteview", "ngen-video");
        session->track = session->connection->addTrack(media);

        auto rtpConfiguration = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, cname, payloadType, rtc::H264RtpPacketizer::ClockRate);
        // The encoder's frames come with start codes of either length.
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(rtc::NalUnit::Separator::StartSequence, rtpConfiguration);
        packetizer->addToChain(std::make_shared<rtc::RtcpSrReporter>(rtpConfiguration));
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>());
        packetizer->addToChain(std::make_shared<rtc::PliHandler>([weak, generation]() {
            auto strong = weak.lock();
            if (strong == nullptr) {
                return;
            }
            std::lock_guard lock(strong->mutex);
            if (strong->generation != generation) {
                return;
            }
            strong->keyframeWanted = true;
            strong->keyframeRequests++;
        }));
        session->track->setMediaHandler(packetizer);
        session->track->onOpen([weak, generation]() {
            auto strong = weak.lock();
            if (strong == nullptr) {
                return;
            }
            std::lock_guard lock(strong->mutex);
            if (strong->generation != generation) {
                return;
            }
            strong->keyframeWanted = true;
            strong->state = VideoWatcherState::Connected;
            strong->connects++;
        });

        session->connection->setLocalDescription(rtc::Description::Type::Offer);
        // Only this machine's own addresses are gathered (no STUN server), so this is quick; the wait is for a machine where it isn't.
        if (gatheredFuture.wait_for(gatherTimeout) != std::future_status::ready) {
            close();
            return std::unexpected("gathering this machine's addresses took more than three seconds");
        }
        auto description = session->connection->localDescription();
        if (!description) {
            close();
            return std::unexpected("no local description after gathering");
        }
        return std::string(*description);
    } catch (const std::exception& error) {
        close();
        return std::unexpected(std::format("cannot make an offer: {}", error.what()));
    }
}

auto VideoSender::accept(const std::string& answerSdp) -> std::expected<void, std::string> {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(shared->mutex);
        session = shared->session;
    }
    if (session == nullptr) {
        return std::unexpected("an answer without an offer: ask for /offer first");
    }
    try {
        session->connection->setRemoteDescription(rtc::Description(answerSdp, rtc::Description::Type::Answer));
    } catch (const std::exception& error) {
        return std::unexpected(std::format("cannot take the answer: {}", error.what()));
    }
    return {};
}

auto VideoSender::send(const VideoPacket& packet) -> bool {
    if (packet.data.empty() || shared->state != VideoWatcherState::Connected) {
        return false;
    }
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(shared->mutex);
        session = shared->session;
    }
    if (session == nullptr) {
        return false;
    }
    try {
        if (!session->track->isOpen()) {
            return false;
        }
        if (session->startNs == 0) {
            session->startNs = packet.captureNs;
        }
        std::chrono::duration<double> seconds((double) (packet.captureNs - session->startNs) / 1e9);
        session->track->sendFrame(reinterpret_cast<const rtc::byte*>(packet.data.data()), packet.data.size(), rtc::FrameInfo(seconds));
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

auto VideoSender::state() const -> VideoWatcherState {
    return shared->state;
}

auto VideoSender::session() const -> uint64_t {
    std::lock_guard lock(shared->mutex);
    return shared->session != nullptr ? shared->generation : 0;
}

auto VideoSender::watcherAddress() const -> std::string {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(shared->mutex);
        session = shared->session;
    }
    if (session == nullptr || shared->state != VideoWatcherState::Connected) {
        return {};
    }
    try {
        return session->connection->remoteAddress().value_or("");
    } catch (const std::exception&) {
        return {};
    }
}

auto VideoSender::takeKeyframeRequest() -> bool {
    return shared->keyframeWanted.exchange(false);
}

auto VideoSender::keyframeRequests() const -> uint64_t {
    return shared->keyframeRequests;
}

auto VideoSender::connects() const -> uint64_t {
    return shared->connects;
}

auto VideoSender::disconnects() const -> uint64_t {
    return shared->disconnects;
}

auto VideoSender::close() -> void {
    std::shared_ptr<Session> session;
    {
        std::lock_guard lock(shared->mutex);
        session = std::move(shared->session);
        shared->session = nullptr;
        shared->generation++;
        if (shared->state.exchange(VideoWatcherState::None) == VideoWatcherState::Connected) {
            shared->disconnects++;
        }
    }
    if (session == nullptr) {
        return;
    }
    try {
        session->connection->close();
    } catch (const std::exception&) {
    }
}
