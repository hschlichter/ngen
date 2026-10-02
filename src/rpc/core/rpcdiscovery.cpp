#include "rpcdiscovery.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

auto processAlive(int pid) -> bool {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

} // namespace

auto rpcDiscoveryDirectory() -> fs::path {
    return fs::current_path() / ".ngen-discovery";
}

auto writeRpcEndpoint(const RpcEndpointInfo& info) -> std::expected<fs::path, std::string> {
    auto directory = rpcDiscoveryDirectory();
    std::error_code ec;
    fs::create_directories(directory, ec);
    auto file = directory / std::format("{}-{}.json", info.kind, info.pid);
    nlohmann::json j = {
        {"kind", info.kind},
        {"pid", info.pid},
        {"port", info.port},
        {"label", info.label},
        {"protocol", info.protocol},
        {"startedUnixMs", info.startedUnixMs},
    };
    // Written to a temporary name and renamed, so a reader never sees half a file.
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << j.dump(2) << '\n';
        if (!out) {
            return std::unexpected(std::format("cannot write {}", temporary.string()));
        }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
        return std::unexpected(std::format("cannot rename {}: {}", temporary.string(), ec.message()));
    }
    return file;
}

auto removeRpcEndpoint(const fs::path& file) -> void {
    std::error_code ec;
    fs::remove(file, ec);
}

auto listRpcEndpoints() -> std::vector<RpcEndpointInfo> {
    std::vector<RpcEndpointInfo> endpoints;
    auto runDirectory = rpcDiscoveryDirectory();
    std::error_code ec;
    if (!fs::is_directory(runDirectory, ec)) {
        return endpoints;
    }
    for (const auto& entry : fs::directory_iterator(runDirectory, ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        std::stringstream text;
        text << in.rdbuf();
        auto j = nlohmann::json::parse(text.str(), nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            continue;
        }
        RpcEndpointInfo info = {
            .kind = j.value("kind", ""),
            .pid = j.value("pid", 0),
            .port = j.value("port", (uint16_t) 0),
            .label = j.value("label", ""),
            .protocol = j.value("protocol", 0),
            .startedUnixMs = j.value("startedUnixMs", (int64_t) 0),
            .file = entry.path(),
        };
        if (!processAlive(info.pid)) {
            removeRpcEndpoint(entry.path());
            continue;
        }
        endpoints.push_back(std::move(info));
    }
    std::ranges::sort(endpoints, [](const RpcEndpointInfo& a, const RpcEndpointInfo& b) {
        if (a.kind != b.kind) {
            return a.kind < b.kind;
        }
        return a.pid < b.pid;
    });
    return endpoints;
}
