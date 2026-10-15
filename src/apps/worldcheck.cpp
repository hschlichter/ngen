// ngen-worldcheck: composes a scene's World from its packs, fetched from this variant's asset server, and prints its fingerprint
// (src/world/fingerprint.h), to compare with ngen-usdcheck's for the same scene.
//
//   ngen-worldcheck [--lod <root path>=<level>]... [--transforms] [--instancing] [--output <file>] <scene id>
//
// --lod sets a LOD root's wanted level before composing (the path with instances expanded); --transforms adds world matrices;
// --instancing prints instance and prototype counts instead of the fingerprint. A summary and what couldn't compose go to stderr.
// Exit codes: 0 composed, 1 the server or the scene's root failed, 3 usage.

#include "assetclient.h"
#include "fingerprint.h"
#include "worldcomposer.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <print>
#include <set>
#include <string>
#include <vector>

namespace {

auto usage() -> int {
    std::println(stderr, "usage: ngen-worldcheck [--lod <root path>=<level>]... [--transforms] [--instancing] [--output <file>] <scene id>");
    return 3;
}

// Instances as USD's default traversal finds them on the stage: active, defined, concrete and loaded prims, not inside other instances.
auto countInstances(const World& world) -> size_t {
    size_t count = 0;
    std::vector<std::pair<const WorldContent*, EntityId>> pending = {{&world.stage, 0}};
    while (!pending.empty()) {
        auto [content, id] = pending.back();
        pending.pop_back();
        const auto& entity = content->entities[id];
        if (id != 0 || content != &world.stage) {
            if (!entity.active || !entity.defined || entity.abstract) {
                continue;
            }
            if (auto payload = content->payloads.find(id); payload != content->payloads.end() && !payload->second.loaded) {
                continue;
            }
            if (entity.prototype >= 0) {
                count++;
                continue;
            }
        }
        for (EntityId child : entity.children) {
            // A LOD level is its content's root, the level prim with everything under it.
            if (auto level = content->entities[child].levelContent; level >= 0) {
                pending.emplace_back(&world.levelContents[(size_t) level], 0);
                continue;
            }
            pending.emplace_back(content, child);
        }
    }
    return count;
}

} // namespace

auto main(int argc, char** argv) -> int {
    bool transforms = false;
    bool instancing = false;
    std::string output;
    std::string scene;
    std::vector<std::pair<std::string, uint32_t>> levels;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--transforms") {
            transforms = true;
        } else if (arg == "--instancing") {
            instancing = true;
        } else if (arg == "--output" && i + 1 < argc) {
            output = argv[++i];
        } else if (arg == "--lod" && i + 1 < argc) {
            std::string value = argv[++i];
            auto equals = value.rfind('=');
            if (equals == std::string::npos) {
                return usage();
            }
            levels.emplace_back(value.substr(0, equals), (uint32_t) std::stoul(value.substr(equals + 1)));
        } else if (arg.starts_with("--") || !scene.empty()) {
            return usage();
        } else {
            scene = arg;
        }
    }
    if (scene.empty()) {
        return usage();
    }

    // The variant is the folder the binary is in, _out/<platform>/<config>/, as for ngen-view.
    std::error_code ec;
    auto bin = std::filesystem::canonical("/proc/self/exe", ec).parent_path();
    auto variant = bin.parent_path().filename().string() + "/" + bin.filename().string();
    AssetClient client;
    if (auto connected = client.connect(variant); !connected) {
        std::println(stderr, "ngen-worldcheck: {}", connected.error());
        return 1;
    }

    auto started = std::chrono::steady_clock::now();
    WorldComposer composer(scene);
    for (const auto& [path, level] : levels) {
        composer.setLodLevel(path, level);
    }
    std::set<std::string> requested;
    size_t packs = 0;
    size_t failed = 0;
    for (;;) {
        std::vector<std::string> fresh;
        for (const auto& id : composer.compose()) {
            if (requested.insert(id).second) {
                fresh.push_back(id);
            }
        }
        if (fresh.empty()) {
            break;
        }
        client.request(fresh);
        client.wait(fresh);
        for (const auto& id : fresh) {
            auto asset = client.take(id);
            if (!asset) {
                failed++;
                std::println(stderr, "ngen-worldcheck: {} failed", id);
                for (const auto& line : client.errors(id)) {
                    std::println(stderr, "  {}", line);
                }
                composer.markFailed(id);
                if (id == scene) {
                    return 1;
                }
                continue;
            }
            packs++;
            if (auto error = composer.addPack(id, asset->bytes)) {
                failed++;
                std::println(stderr, "ngen-worldcheck: {}", *error);
            }
        }
    }
    auto composed = std::chrono::steady_clock::now();
    const World& world = composer.world();
    for (const auto& message : composer.messages()) {
        std::println(stderr, "ngen-worldcheck: {}", message);
    }

    std::vector<std::string> lines;
    if (instancing) {
        lines.push_back(std::format("instances\t{}", countInstances(world)));
        lines.push_back(std::format("prototypes\t{}", world.prototypes.size()));
    } else {
        world.visit([&](const VisitedEntity& visited) {
            int64_t points = std::max<int64_t>(visited.entity->pointCount, 0);
            if (transforms) {
                double matrix[16];
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        matrix[(i * 4) + j] = visited.worldTransform[i][j];
                    }
                }
                lines.push_back(fingerprintLine(visited.path, visited.entity->typeName, points, matrix));
            } else {
                lines.push_back(fingerprintLine(visited.path, visited.entity->typeName, points, nullptr));
            }
            return true;
        });
        std::ranges::sort(lines);
    }

    size_t meshes = 0;
    int64_t totalPoints = 0;
    world.visit([&](const VisitedEntity& visited) {
        if (visited.entity->typeName == "Mesh") {
            meshes++;
        }
        totalPoints += std::max<int64_t>(visited.entity->pointCount, 0);
        return true;
    });
    auto ms = [](auto from, auto to) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    };
    std::println(stderr,
                 "ngen-worldcheck: {}: {} packs ({} failed), {} prototypes, {} meshes, {} points; composed in {} ms",
                 scene,
                 packs,
                 failed,
                 world.prototypes.size(),
                 meshes,
                 totalPoints,
                 ms(started, composed));

    std::string text;
    for (const auto& line : lines) {
        text += line;
        text += '\n';
    }
    if (output.empty()) {
        std::print("{}", text);
        return 0;
    }
    std::ofstream out(output, std::ios::trunc);
    out << text;
    if (!out) {
        std::println(stderr, "ngen-worldcheck: cannot write {}", output);
        return 1;
    }
    return 0;
}
