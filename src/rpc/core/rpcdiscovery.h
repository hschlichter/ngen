#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

// Discovery: every endpoint announces itself with one JSON file in <project root>/_out/run/,
// named <kind>-<pid>.json, written once it is listening and removed on exit. Readers drop
// files whose process is gone.
struct RpcEndpointInfo {
    std::string kind; // "view", "build", "editor", …
    int pid = 0;
    uint16_t port = 0;
    std::string projectRoot;
    std::string label; // human-readable, such as the scene path
    int protocol = 0;
    int64_t startedUnixMs = 0;
    std::filesystem::path file; // where it was read from (filled by listRpcEndpoints)
};

// The project root: three levels above an executable in _out/<platform>/<config>/, when that
// holds a build.cpp; otherwise the working directory.
auto rpcProjectRoot() -> std::filesystem::path;
auto rpcRunDirectory(const std::filesystem::path& projectRoot) -> std::filesystem::path;

auto writeRpcEndpoint(const RpcEndpointInfo& info) -> std::expected<std::filesystem::path, std::string>;
auto removeRpcEndpoint(const std::filesystem::path& file) -> void;
// Live endpoints, sorted by kind then pid. Files of dead processes are removed.
auto listRpcEndpoints(const std::filesystem::path& runDirectory) -> std::vector<RpcEndpointInfo>;
