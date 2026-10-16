// ngen-usdcheck: opens a scene with OpenUSD and prints USD's fingerprint of it (src/world/fingerprint.h), the reference ngen-worldcheck's
// is compared with. It loads what the World loads: every payload, except those under a LOD root's levels other than the one it wants
// (lod:default:index, or --lod) and its coarsest.
//
//   ngen-usdcheck [--lod <root path>=<level>]... [--transforms] [--instancing] [--output <file>] <scene file>
//
// The scene file is read from disk, relative to the working directory: the asset server's, where an asset id is the path. Exit codes: 0
// printed, 1 the stage didn't open, 3 usage.
//
// Built as C++20, as OpenUSD's headers require.

#include "fingerprint.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/vt/array.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/pointBased.h>
#include <pxr/usd/usdLod/rootAPI.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Ends the process once the fingerprint is written: OpenUSD destroys a stage's data on worker threads, and static destructors running
// while those tasks do now and then abort the process ("pure virtual method called"). _Exit skips that teardown.
[[noreturn]] auto finish(int code) -> void {
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

auto usage() -> int {
    std::fputs("usage: ngen-usdcheck [--lod <root path>=<level>]... [--transforms] [--instancing] [--output <file>] <scene file>\n", stderr);
    return 3;
}

// A prim's world transform. UsdGeomXformCache isn't used: its inline destructor, compiled here as C++20, frees what the library didn't
// allocate (a layout mismatch with the library's build); ComputeLocalToWorldTransform is compiled in the library.
auto worldTransform(const UsdPrim& prim) -> GfMatrix4d {
    for (UsdPrim at = prim; at && !at.IsPseudoRoot(); at = at.GetParent()) {
        if (UsdGeomImageable imageable{at}) {
            return imageable.ComputeLocalToWorldTransform(UsdTimeCode::Default());
        }
    }
    return GfMatrix4d(1.0);
}

auto everyPrim() -> Usd_PrimFlagsPredicate {
    return UsdTraverseInstanceProxies(UsdPrimAllPrimsPredicate);
}

// The levels of a LOD root that load: the wanted one and the coarsest.
auto loadedLevels(const UsdPrim& prim, size_t levelCount, const std::map<std::string, unsigned>& levels) -> std::set<size_t> {
    std::set<size_t> out;
    if (levelCount == 0) {
        return out;
    }
    int defaultIndex = 0;
    UsdLodRootAPI(prim).GetLodDefaultIndexAttr().Get(&defaultIndex);
    size_t wanted = (size_t) std::max(defaultIndex, 0);
    if (auto it = levels.find(prim.GetPath().GetString()); it != levels.end()) {
        wanted = it->second;
    }
    out.insert(std::min(wanted, levelCount - 1));
    out.insert(levelCount - 1);
    return out;
}

// The payloads still to load: unloaded, and not under a level that doesn't load.
auto collectLoads(const UsdPrim& prim, bool allowed, const std::map<std::string, unsigned>& levels, SdfPathSet& out) -> void {
    if (!allowed) {
        return;
    }
    if (prim.HasAuthoredPayloads() && !prim.IsLoaded()) {
        out.insert(prim.GetPath());
        return;
    }
    std::vector<UsdPrim> children;
    for (const auto& child : prim.GetFilteredChildren(everyPrim())) {
        children.push_back(child);
    }
    if (!prim.IsPseudoRoot() && prim.HasAPI<UsdLodRootAPI>()) {
        // The levels are the children UsdLod counts as LOD items: active, defined and not abstract; one not loaded yet whose payload may
        // define it counts too, as the engine counts it. Any other child loads as the root does.
        std::vector<UsdPrim> items;
        for (const auto& child : children) {
            if (child.IsActive() && !child.IsAbstract() && (child.IsDefined() || child.HasAuthoredPayloads())) {
                items.push_back(child);
            } else {
                collectLoads(child, true, levels, out);
            }
        }
        auto loaded = loadedLevels(prim, items.size(), levels);
        for (size_t i = 0; i < items.size(); i++) {
            collectLoads(items[i], loaded.contains(i), levels, out);
        }
        return;
    }
    for (const auto& child : children) {
        collectLoads(child, true, levels, out);
    }
}

} // namespace

auto main(int argc, char** argv) -> int {
    bool transforms = false;
    bool instancing = false;
    std::string output;
    std::string scene;
    std::map<std::string, unsigned> levels;
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
            levels[value.substr(0, equals)] = (unsigned) std::stoul(value.substr(equals + 1));
        } else if (arg.starts_with("--") || !scene.empty()) {
            return usage();
        } else {
            scene = arg;
        }
    }
    if (scene.empty()) {
        return usage();
    }

    auto started = std::chrono::steady_clock::now();
    auto stage = UsdStage::Open(scene, UsdStage::LoadNone);
    if (!stage) {
        std::fprintf(stderr, "ngen-usdcheck: cannot open %s\n", scene.c_str());
        finish(1);
    }
    size_t loads = 0;
    for (;;) {
        SdfPathSet toLoad;
        collectLoads(stage->GetPseudoRoot(), true, levels, toLoad);
        if (toLoad.empty()) {
            break;
        }
        loads += toLoad.size();
        stage->LoadAndUnload(toLoad, SdfPathSet(), UsdLoadWithoutDescendants);
    }
    auto opened = std::chrono::steady_clock::now();

    std::vector<std::string> lines;
    size_t meshes = 0;
    long long totalPoints = 0;
    std::function<void(const UsdPrim&)> visit = [&](const UsdPrim& prim) {
        if (!prim.IsPseudoRoot()) {
            long long points = 0;
            if (prim.IsA<UsdGeomPointBased>()) {
                VtArray<GfVec3f> values;
                if (UsdGeomPointBased(prim).GetPointsAttr().Get(&values)) {
                    points = (long long) values.size();
                }
            }
            if (prim.GetTypeName() == "Mesh") {
                meshes++;
            }
            totalPoints += points;
            if (transforms) {
                GfMatrix4d m = worldTransform(prim);
                double matrix[16];
                for (int i = 0; i < 4; i++) {
                    for (int j = 0; j < 4; j++) {
                        matrix[(i * 4) + j] = m[i][j];
                    }
                }
                lines.push_back(fingerprintLine(prim.GetPath().GetString(), prim.GetTypeName().GetString(), points, matrix));
            } else {
                lines.push_back(fingerprintLine(prim.GetPath().GetString(), prim.GetTypeName().GetString(), points, nullptr));
            }
        }
        for (const auto& child : prim.GetFilteredChildren(everyPrim())) {
            visit(child);
        }
    };
    visit(stage->GetPseudoRoot());

    if (instancing) {
        size_t instances = 0;
        for (const auto& prim : stage->Traverse()) {
            if (prim.IsInstance()) {
                instances++;
            }
        }
        lines.clear();
        lines.push_back(std::format("instances\t{}", instances));
        lines.push_back(std::format("prototypes\t{}", stage->GetPrototypes().size()));
    } else {
        std::ranges::sort(lines);
    }

    auto ms = [](auto from, auto to) {
        return (long long) std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
    };
    std::fprintf(stderr,
                 "ngen-usdcheck: %s: %zu payloads loaded, %zu prototypes, %zu meshes, %lld points; opened in %lld ms\n",
                 scene.c_str(),
                 loads,
                 stage->GetPrototypes().size(),
                 meshes,
                 totalPoints,
                 ms(started, opened));

    std::string text;
    for (const auto& line : lines) {
        text += line;
        text += '\n';
    }
    if (output.empty()) {
        std::fputs(text.c_str(), stdout);
        finish(0);
    }
    std::ofstream out(output, std::ios::trunc);
    out << text;
    out.close();
    if (!out) {
        std::fprintf(stderr, "ngen-usdcheck: cannot write %s\n", output.c_str());
        finish(1);
    }
    finish(0);
}
