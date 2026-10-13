#pragma once

#include "scenehandles.h"

#include <span>

class USDScene;
class MeshLibrary;
struct RenderWorld;

class USDRenderExtractor {
public:
    void extract(const USDScene& scene, const MeshLibrary& meshLib, RenderWorld& out);

    // Patch worldTransform + worldBounds for already-extracted instances whose
    // prims appear in `dirty`. Prims not present in `out.primToInstance` (e.g.
    // hidden or non-renderable) are silently skipped.
    void patchTransforms(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirty, RenderWorld& out);
};
