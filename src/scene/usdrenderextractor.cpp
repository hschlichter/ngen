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

        out.meshInstances.push_back({
            .mesh = binding->mesh,
            .material = binding->material,
            .worldTransform = xf->world,
            .worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world),
        });
    }
}
