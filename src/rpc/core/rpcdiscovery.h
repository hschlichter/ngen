#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// Discovery: every endpoint announces itself with one JSON file in .ngen-discovery/ in its working
// directory, named <kind>-<pid>.json, written once it is listening and removed on exit. Tools find
// each other when they run in the same directory. Readers drop files whose process is gone.
struct RpcEndpointInfo {
    std::string kind; // "view", "build", "editor", …
    int pid = 0;
    uint16_t port = 0;
    std::string label; // human-readable, such as the scene path
    int protocol = 0;
    int64_t startedUnixMs = 0;
    std::filesystem::path file; // where it was read from (filled by listRpcEndpoints)
};

// .ngen-discovery/ in the working directory.
auto rpcDiscoveryDirectory() -> std::filesystem::path;

auto writeRpcEndpoint(const RpcEndpointInfo& info) -> std::expected<std::filesystem::path, std::string>;
auto removeRpcEndpoint(const std::filesystem::path& file) -> void;
// Live endpoints, sorted by kind then pid. Files of dead processes are removed.
auto listRpcEndpoints() -> std::vector<RpcEndpointInfo>;
