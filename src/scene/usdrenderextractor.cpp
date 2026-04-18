#include "usdrenderextractor.h"
#include "mesh.h"
#include "renderworld.h"
#include "usdscene.h"

void USDRenderExtractor::extract(const USDScene& scene, const MeshLibrary& meshLib, RenderWorld& out) {
    out.clear();

    for (const auto& prim : scene.allPrims()) {
        if ((prim.flags & PrimFlagLight) != 0 && prim.visible) {
            const auto* desc = scene.getLightDesc(prim.handle);
            const auto* xf = scene.getTransform(prim.handle);
            // Only Distant lights have a real shading path today; skip everything else
            // so it doesn't masquerade as a directional source in RenderWorld::lights.
            if (desc != nullptr && xf != nullptr && desc->kind == LightKind::Distant) {
                out.lights.push_back({
                    .type = LightType::Directional,
                    .color = desc->color,
                    .intensity = desc->intensity,
                    .exposure = desc->exposure,
                    .angle = desc->angle,
                    .shadowEnable = desc->shadowEnable,
                    .shadowColor = desc->shadowColor,
                    .worldTransform = xf->world,
                });
            }
        }

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
