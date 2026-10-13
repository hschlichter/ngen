#pragma once

#include "rpcdiscovery.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// A packed asset as the asset server streamed it.
struct PackedAsset {
    std::string id;
    std::string version;
    std::vector<std::byte> bytes;
};

// What a client has asked for and received since it connected. An id requested twice counts twice.
struct AssetClientStats {
    uint64_t requested = 0;
    uint64_t received = 0; // arrived with its bytes, or as already held
    uint64_t packed = 0;   // of the received: the server ran a packer for it
    uint64_t cached = 0;   // of the received: the server's cache was up to date
    uint64_t failed = 0;
    uint64_t bytes = 0; // bytes received in asset.data
    auto inFlight() const -> uint64_t { return requested - received - failed; }
};

// One asset as the client has seen it: every request for its id, and how the last one ended. Times are milliseconds since the client was
// created; -1 for what hasn't happened yet.
enum class AssetLogState : uint8_t {
    InFlight,
    Received,
    Failed,
};

struct AssetLogEntry {
    std::string id;
    AssetLogState state = AssetLogState::InFlight;
    uint32_t requests = 0; // how often the id was requested
    bool packed = false;   // the last arrival: a packer ran for it (else the server's cache was up to date)
    std::string version;
    uint64_t bytes = 0;      // the last arrival's size
    double requestedMs = -1; // the last request
    double firstDataMs = -1; // its first asset.data
    double doneMs = -1;      // its asset.ready or asset.failed
    double serverMs = -1;    // the server's time for it (the cache check, and packing when it ran), as the server reports it
    std::vector<std::string> errors;
    bool stale = false;   // the server said its sources changed since the last arrival
    uint32_t changes = 0; // how often the server said so
};

// The engine side of the asset server's stream: sends pack requests and collects the streamed assets. No file
// of the server's is ever read. Thread-safe. See src/asset/README.md.
class AssetClient {
public:
    AssetClient();
    AssetClient(const AssetClient&) = delete;
    auto operator=(const AssetClient&) -> AssetClient& = delete;
    ~AssetClient();

    // Finds the asset server of this variant ("linux-vulkan/debug") running in the working directory, through its
    // discovery file in .ngen-discovery/, and connects; returns the server it connected to, or an error when none is
    // running.
    auto connect(const std::string& variant) -> std::expected<RpcEndpointInfo, std::string>;

    // Sends one asset.request for all of `ids`; returns at once. `force` has the server pack them even when its cache is up to date (each
    // file once, for the outputs of one file).
    auto request(std::span<const std::string> ids, bool force = false) -> void;

    // Blocks until every request for the ids in `ids` has been answered: each has arrived or failed.
    auto wait(std::span<const std::string> ids) -> void;

    // The asset once it has arrived; nullptr before that, or when it failed. A new version replaces it when it is ready, not when it starts
    // arriving; the pointer keeps the version it holds alive. A request that fails keeps the version that had arrived.
    auto find(const std::string& id) const -> std::shared_ptr<const PackedAsset>;

    // Moves an arrived asset out of the client; nullopt if it hasn't arrived. A later request for the id streams it again.
    auto take(const std::string& id) -> std::optional<PackedAsset>;

    auto stats() const -> AssetClientStats;

    // Every id requested so far, in the order first requested.
    auto log() const -> std::vector<AssetLogEntry>;

    // Whether a request for the id hasn't been answered yet.
    auto pending(const std::string& id) const -> bool;

    // The packer's errors for an asset that failed; empty otherwise.
    auto errors(const std::string& id) const -> std::vector<std::string>;

    // The ids the server said changed at their source since the last call (asset.changed), each once, in the order told. The client
    // requests nothing by itself: what still wants an asset requests it again.
    auto takeChanged() -> std::vector<std::string>;

private:
    struct State;
    std::unique_ptr<State> state;
};
