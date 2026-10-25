// ngen-pack: has this variant's asset server pack everything a scene can reach, so its cache is full before a view opens the scene.
//
//   ngen-pack <scene>...
//
// From each scene's root layer it follows what every pack names (namedAssets, scenepack.h): every LOD level and every variant, not only what a
// view would load. Nothing is composed: what is authored anywhere is packed. Each asset is requested and streamed back like a view's request,
// so the server's cache, its trace and its records see an ordinary client. Exit codes: 0 everything packed, 1 an asset failed or no server
// runs, 3 usage.

#include "assetclient.h"
#include "assetid.h"
#include "scenepack.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <print>
#include <set>
#include <string>
#include <vector>

namespace {

auto kindName(NamedAssetKind kind) -> const char* {
    switch (kind) {
        case NamedAssetKind::Layer:
            return "layers";
        case NamedAssetKind::Mesh:
            return "meshes";
        case NamedAssetKind::Material:
            return "materials";
        case NamedAssetKind::Texture:
            return "textures";
    }
    return "assets";
}

// Requested in batches, so the bytes streamed back are dropped batch by batch rather than held for the whole scene.
constexpr size_t batchSize = 512;

struct Walk {
    std::deque<NamedAsset> queue;
    std::set<std::string> seen;

    auto add(const NamedAsset& asset) -> void {
        if (!seen.insert(asset.id).second) {
            return;
        }
        queue.push_back(asset);
    }
};

} // namespace

auto main(int argc, char** argv) -> int {
    std::vector<std::string> scenes;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.starts_with("-")) {
            std::println(stderr, "usage: ngen-pack <scene>...");
            return 3;
        }
        auto id = assetIdForPath(arg);
        if (id.empty()) {
            std::println(stderr, "ngen-pack: {} isn't a path inside the working directory", arg);
            return 3;
        }
        scenes.push_back(id);
    }
    if (scenes.empty()) {
        std::println(stderr, "usage: ngen-pack <scene>...");
        return 3;
    }

    // The variant is the folder the binary is in, _out/<platform>/<config>/, as for ngen-view.
    std::error_code ec;
    auto bin = std::filesystem::canonical("/proc/self/exe", ec).parent_path();
    auto variant = bin.parent_path().filename().string() + "/" + bin.filename().string();
    AssetClient client;
    if (auto connected = client.connect(variant); !connected) {
        std::println(stderr, "ngen-pack: {}", connected.error());
        return 1;
    }

    auto start = std::chrono::steady_clock::now();
    Walk walk;
    for (const auto& scene : scenes) {
        walk.add({.id = scene, .kind = NamedAssetKind::Layer});
    }
    std::map<NamedAssetKind, uint64_t> answered;
    std::vector<std::string> failed;
    while (!walk.queue.empty()) {
        std::vector<NamedAsset> batch;
        while (!walk.queue.empty() && batch.size() < batchSize) {
            batch.push_back(std::move(walk.queue.front()));
            walk.queue.pop_front();
        }
        std::vector<std::string> ids;
        for (const auto& asset : batch) {
            ids.push_back(asset.id);
        }
        client.request(ids);
        client.wait(ids);
        for (const auto& [id, kind] : batch) {
            auto asset = client.take(id);
            if (!asset) {
                failed.push_back(id);
                continue;
            }
            answered[kind]++;
            std::vector<NamedAsset> named;
            std::string error;
            if (kind == NamedAssetKind::Layer) {
                auto pack = readLayerPack(asset->bytes);
                if (pack) {
                    named = namedAssets(*pack);
                } else {
                    error = pack.error();
                }
            } else if (kind == NamedAssetKind::Material) {
                auto pack = readMaterialPack(asset->bytes);
                if (pack) {
                    named = namedAssets(*pack);
                } else {
                    error = pack.error();
                }
            }
            if (!error.empty()) {
                std::println(stderr, "ngen-pack: {}: {}", id, error);
                failed.push_back(id);
                continue;
            }
            for (const auto& next : named) {
                walk.add(next);
            }
        }
        auto stats = client.stats();
        std::println("ngen-pack: {} of {} assets answered ({} packed, {} from the cache, {} failed)",
                     stats.received + stats.failed,
                     walk.seen.size(),
                     stats.packed,
                     stats.cached,
                     stats.failed);
    }

    auto stats = client.stats();
    auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::string kinds;
    for (const auto& [kind, count] : answered) {
        kinds += std::format("{}{} {}", kinds.empty() ? "" : ", ", count, kindName(kind));
    }
    std::println("ngen-pack: {} assets in {:.1f} s: {}; {} packed, {} from the cache, {:.1f} MiB streamed",
                 walk.seen.size(),
                 seconds,
                 kinds,
                 stats.packed,
                 stats.cached,
                 (double) stats.bytes / (1024.0 * 1024.0));
    if (failed.empty()) {
        return 0;
    }
    std::ranges::sort(failed);
    std::println(stderr, "ngen-pack: {} failed:", failed.size());
    for (const auto& id : failed) {
        std::println(stderr, "  {}", id);
        for (const auto& line : client.errors(id)) {
            std::println(stderr, "    {}", line);
        }
    }
    return 1;
}
