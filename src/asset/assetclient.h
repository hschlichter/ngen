#pragma once

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

// The engine side of the asset server's stream: sends pack requests and collects the streamed assets. No file
// of the server's is ever read. Thread-safe. See src/asset/README.md.
class AssetClient {
public:
    AssetClient();
    AssetClient(const AssetClient&) = delete;
    auto operator=(const AssetClient&) -> AssetClient& = delete;
    ~AssetClient();

    // Finds the asset server of this variant ("linux-vulkan/debug") running in the working directory, through its
    // discovery file in .ngen-discovery/, and connects; an error when none is running.
    auto connect(const std::string& variant) -> std::expected<void, std::string>;

    // Sends one asset.request for all of `ids`; returns at once.
    auto request(std::span<const std::string> ids) -> void;

    // Blocks until every id in `ids` has arrived or failed.
    auto wait(std::span<const std::string> ids) -> void;

    // The asset once it has arrived; nullptr before that, or when it failed.
    auto find(const std::string& id) const -> const PackedAsset*;

    // Moves an arrived asset out of the client; nullopt if it hasn't arrived. A later request for the id streams it again.
    auto take(const std::string& id) -> std::optional<PackedAsset>;

    auto stats() const -> AssetClientStats;

    // The packer's errors for an asset that failed; empty otherwise.
    auto errors(const std::string& id) const -> std::vector<std::string>;

private:
    struct State;
    std::unique_ptr<State> state;
};
