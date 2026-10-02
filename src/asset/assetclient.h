#pragma once

#include <cstddef>
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

    // The packer's errors for an asset that failed; empty otherwise.
    auto errors(const std::string& id) const -> std::vector<std::string>;

private:
    struct State;
    std::unique_ptr<State> state;
};
