#include "usdrenderextractor.h"
#include "mesh.h"
#include "renderworld.h"
#include "usdscene.h"

void USDRenderExtractor::extract(const USDScene& scene, const MeshLibrary& meshLib, RenderWorld& out) {
    out.clear();

    for (const auto& prim : scene.allPrims()) {
        if (!(prim.flags & PrimFlagRenderable) || !prim.visible) {
            continue;
        }

        const auto* binding = scene.getAssetBinding(prim.handle);
        if (!binding || !binding->mesh) {
            continue;
        }

        const auto* xf = scene.getTransform(prim.handle);
        if (!xf) {
            continue;
        }

        out.primToInstance[prim.handle.index] = (uint32_t) out.meshInstances.size();
        out.meshInstances.push_back({
            .mesh = binding->mesh,
            .material = binding->material,
            .worldTransform = xf->world,
            .worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world),
        });
    }
}

void USDRenderExtractor::patchTransforms(const USDScene& scene, const MeshLibrary& meshLib, std::span<const PrimHandle> dirty, RenderWorld& out) {
    for (auto h : dirty) {
        auto it = out.primToInstance.find(h.index);
        if (it == out.primToInstance.end()) {
            continue;
        }
        const auto* xf = scene.getTransform(h);
        const auto* binding = scene.getAssetBinding(h);
        if (!xf || !binding || !binding->mesh) {
            continue;
        }
        auto& inst = out.meshInstances[it->second];
        inst.worldTransform = xf->world;
        inst.worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world);
    }
}
