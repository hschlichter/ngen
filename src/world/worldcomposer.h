#pragma once

#include "world.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

// Composes a World from layer packs (src/asset/pack/scenepack.h), as USD composes a stage from layers, for what the engine composes:
// the layer stack (sublayers, strongest first, per attribute), references and payloads to other files and inside a file, with namespace
// mapping and relationship targets mapped through them, instancing (one prototype per distinct set of arcs), and UsdLod roots. What packs
// report as unsupported (variant sets, inherits, specializes, relocates) isn't composed. See src/world/README.md.
//
// The composer doesn't fetch anything. The caller feeds it packs and calls compose(), which says which packs it still needs; composing
// again once they have arrived goes deeper:
//
//     WorldComposer composer("assets/layered/root.usda");
//     auto needed = composer.compose(); // the root layer
//     // ... fetch them, addPack each, compose again, until nothing is needed
//
// A pack that isn't there yet composes as a gap: the arc to it adds nothing until it arrives.
class WorldComposer {
public:
    explicit WorldComposer(std::string rootAssetId);
    ~WorldComposer();
    WorldComposer(const WorldComposer&) = delete;
    auto operator=(const WorldComposer&) -> WorldComposer& = delete;

    // A layer pack has arrived. An error when the bytes aren't one; the id then counts as failed (markFailed).
    auto addPack(const std::string& assetId, std::span<const std::byte> bytes) -> std::optional<std::string>;
    // An asset the asset server couldn't provide: arcs to it compose as if they weren't authored.
    auto markFailed(const std::string& assetId) -> void;

    // Composes the world from the packs held. Returns the layer packs it names and doesn't hold: sublayers, references, and the payloads
    // it wants (every payload except those under a LOD root's levels that aren't loaded). Empty when the world is complete.
    auto compose() -> std::vector<std::string>;

    // The level a LOD root wants, by its full path (instances expanded); lod:default:index until set. The root's coarsest level stays
    // loaded. Takes effect at the next compose, which says what the level needs.
    auto setLodLevel(const std::string& rootPath, uint32_t level) -> void;
    // Forgets a level set by hand, so the root wants lod:default:index again. Takes effect at the next compose.
    auto resetLodLevel(const std::string& rootPath) -> void;

    auto world() const -> const World&;
    // What the last compose couldn't compose, one line per arc or prim: a reference with no default prim to name, a target that isn't
    // there, a cycle.
    auto messages() const -> const std::vector<std::string>&;

    // The last compose(): contents composed (prototypes and LOD level contents; one complete from before is reused, not composed), held
    // (kept across composes) and reached (what the World draws from).
    struct Stats {
        uint32_t prototypesComposed = 0;
        uint32_t levelsComposed = 0;
        uint32_t prototypesHeld = 0;
        uint32_t levelsHeld = 0;
        uint32_t contentsReached = 0;
        double stageMs = 0.0;   // composing the stage itself
        double resolveMs = 0.0; // linking levels, composing what wasn't kept
        double finishMs = 0.0;  // bindings and messages
    };
    auto stats() const -> const Stats&;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
