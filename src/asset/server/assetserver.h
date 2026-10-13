#pragma once

#include "assetcache.h"
#include "assettrace.h"
#include "filewatcher.h"
#include "packjobs.h"
#include "packrule.h"
#include "rpcprotocol.h"
#include "rpcrecords.h"
#include "rpcserver.h"
#include "tracestream.h"

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
        bool pollOnly = false; // poll watched sources instead of inotify
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
        double ms = 0.0;     // the server's time for it: the cache check, and packing when it ran (waiting for the file's job included)
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
        double ms = 0.0;
        std::shared_ptr<const std::vector<std::byte>> bytes;
        size_t offset = 0;
    };

    // What the trace reports when a request is done.
    struct RequestTrace {
        RpcServer::ConnectionId connection = 0;
        size_t assets = 0;
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
    auto addRecords() -> void;
    auto status() -> rpc::Json;
    auto clientsRecord() -> rpc::Json;
    auto requestsRecord() -> rpc::Json;
    auto cacheRecord() -> rpc::Json;
    auto rulesRecord() const -> rpc::Json;

    // `forceRequest` is the request that forces a repack of the asset, 0 for none.
    auto subscribe(const std::string& id, Subscriber subscriber, int64_t forceRequest) -> void;
    // Packs an asset if it is out of date: "<file>" or "<file>#<fragment>", a secondary output of <file>'s job.
    // Packs the asset when its cache is stale, or once per forcing request (`forceRequest`, 0 for none): the outputs of one file, forced
    // together, pack it once.
    auto pack(const std::string& id, int64_t forceRequest) -> Outcome;
    // Runs a file's packer and records its outputs, primary and secondary.
    auto runPacker(const std::string& primary, const PackRule& rule, uint64_t key) -> Outcome;
    auto deliver(const std::string& id, const Outcome& outcome) -> void;
    auto pump(RpcServer::ConnectionId connection) -> void;
    // One of a request's assets was answered: sent with its bytes, held (the client had it), or failed.
    enum class Answer {
        Sent,
        Held,
        Failed,
    };
    auto answered(int64_t request, Answer answer, uint64_t bytes) -> void;

    // A connection was answered for an asset: it is told when the asset's sources change, until it closes. Refreshes the files watched for
    // the asset, which change when it is packed again.
    // `failed`: the answer was a failure; a change back to what the cache holds is then news to the connection too.
    auto received(RpcServer::ConnectionId connection, const std::string& id, bool failed) -> void;
    // Drops what a closed connection was watched for.
    auto forget(RpcServer::ConnectionId connection) -> void;
    // Sets the files watched for an asset; called with watchMutex held.
    auto watchFiles(const std::string& id, std::vector<std::string> files) -> void;
    // From the watcher: sends asset.changed to each connection holding an asset whose sources changed.
    auto onFilesChanged(std::vector<std::filesystem::path> files) -> void;
    // The job key of the rule for an asset id; nullopt when no rule packs it.
    auto jobKeyFor(const std::string& id) const -> std::optional<uint64_t>;

    Options options;
    std::vector<PackRule> rules;
    std::unordered_map<std::string, size_t> ruleByExtension;
    std::filesystem::path cacheDirectory;
    AssetCache cache;
    std::unique_ptr<PackJobs> jobs;
    RpcServer rpc;
    // Answers trace.subscribe and streams the trace; created in start(), reset in stop() after rpc has stopped.
    std::unique_ptr<trace::TraceStream> traceStream;
    uint16_t boundPort = 0;
    std::filesystem::path discoveryFile;
    std::atomic<int64_t> nextRequest{1};
    std::atomic<uint64_t> packerRuns{0};

    std::mutex inFlightMutex;
    std::unordered_map<std::string, std::vector<Subscriber>> inFlight;

    // One lock per file, held while its job checks the cache and runs: requests for several outputs of one file pack it once.
    std::mutex fileLocksMutex;
    std::unordered_map<std::string, std::shared_ptr<std::mutex>> fileLocks;
    std::unordered_map<std::string, int64_t> forcedBy; // by file: the last request that forced its job; guarded by fileLocksMutex

    // The data introspect.list and introspect.get serve.
    RpcRecords records;

    std::mutex requestMutex;
    std::unordered_map<int64_t, RequestTrace> requests;
    // The most recent finished requests, oldest first, for the requests record.
    struct FinishedRequest {
        int64_t request = 0;
        RequestTrace trace;
        double ms = 0.0;
    };
    std::deque<FinishedRequest> finishedRequests;
    static constexpr size_t finishedRequestsKept = 64;

    // What each connection received, and the files watched for it (src/asset/README.md, "Change notices").
    FileWatcher watcher;
    std::mutex watchMutex;
    std::unordered_map<RpcServer::ConnectionId, std::unordered_set<std::string>> receivedBy;
    std::unordered_map<RpcServer::ConnectionId, std::unordered_set<std::string>> failedFor; // the last answer for the id was a failure
    std::unordered_map<std::string, uint32_t> holders;                                      // by asset id: how many connections received it
    std::unordered_map<std::string, std::vector<std::string>> filesOfAsset;
    std::unordered_map<std::string, std::unordered_set<std::string>> assetsOfFile;
    std::atomic<uint64_t> noticesSent{0};

    std::mutex streamMutex;
    std::unordered_set<RpcServer::ConnectionId> connections;
    std::unordered_map<RpcServer::ConnectionId, Stream> streams;
};
