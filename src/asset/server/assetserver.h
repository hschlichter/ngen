#pragma once

#include "assetcache.h"
#include "assettrace.h"
#include "packjobs.h"
#include "packrule.h"
#include "rpcprotocol.h"
#include "rpcserver.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <deque>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// ngen-asset-server's server: takes asset requests over RPC, packs what is out of date with the rules from
// pack.cpp, and streams the packed bytes back. See src/asset/README.md.
class AssetServer {
public:
    struct Options {
        std::filesystem::path binDirectory; // the server's own directory, where the packers are too
        std::string platform;
        std::string config;
    };

    // Packed data is sent in chunks of at most this many bytes, and no more than packWindow bytes are queued on
    // a connection at once.
    static constexpr size_t packChunk = 1024 * 1024;
    static constexpr size_t packWindow = 8 * 1024 * 1024;

    AssetServer() = default;
    AssetServer(const AssetServer&) = delete;
    auto operator=(const AssetServer&) -> AssetServer& = delete;

    // Loads the rules and the cache, listens, and writes the discovery file.
    auto start(const Options& options) -> std::expected<void, std::string>;
    // Stops listening and removes the discovery file.
    auto stop() -> void;

    auto port() const -> uint16_t { return boundPort; }
    auto variant() const -> std::string { return options.platform + "/" + options.config; }

private:
    struct Outcome {
        bool ok = false;
        bool packed = false; // a packer ran for it; false when the cache was up to date
        uint64_t version = 0;
        std::string path;
        std::vector<std::string> errors;
    };

    struct Subscriber {
        RpcServer::ConnectionId connection = 0;
        int64_t request = 0;
        std::optional<std::string> held; // the version the client already has
    };

    struct Outgoing {
        int64_t request = 0;
        std::string id;
        std::string version;
        bool packed = false;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        size_t offset = 0;
    };

    // What the trace reports when a request is done.
    struct RequestTrace {
        RpcServer::ConnectionId connection = 0;
        size_t remaining = 0;
        size_t sent = 0;
        size_t held = 0;
        size_t failed = 0;
        uint64_t bytes = 0;
        std::chrono::steady_clock::time_point start;
    };

    struct Stream {
        std::deque<Outgoing> queue;
        size_t peakQueued = 0;
    };

    auto onMessage(RpcServer::ConnectionId connection, const rpc::Json& message) -> void;
    auto onAssetRequest(RpcServer::ConnectionId connection, const rpc::Json& id, const rpc::Json& params) -> void;
    auto status() -> rpc::Json;

    auto subscribe(const std::string& id, Subscriber subscriber) -> void;
    auto pack(const std::string& id) -> Outcome;
    auto deliver(const std::string& id, const Outcome& outcome) -> void;
    auto pump(RpcServer::ConnectionId connection) -> void;
    // One of a request's assets was answered: sent with its bytes, held (the client had it), or failed.
    enum class Answer {
        Sent,
        Held,
        Failed,
    };
    auto answered(int64_t request, Answer answer, uint64_t bytes) -> void;

    Options options;
    std::vector<PackRule> rules;
    std::unordered_map<std::string, size_t> ruleByExtension;
    std::filesystem::path cacheDirectory;
    AssetCache cache;
    std::unique_ptr<PackJobs> jobs;
    RpcServer rpc;
    uint16_t boundPort = 0;
    std::filesystem::path discoveryFile;
    std::atomic<int64_t> nextRequest{1};
    std::atomic<uint64_t> packerRuns{0};

    std::mutex inFlightMutex;
    std::unordered_map<std::string, std::vector<Subscriber>> inFlight;

    std::mutex requestMutex;
    std::unordered_map<int64_t, RequestTrace> requests;

    std::mutex streamMutex;
    std::unordered_set<RpcServer::ConnectionId> connections;
    std::unordered_map<RpcServer::ConnectionId, Stream> streams;
};
