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
            if (desc != nullptr && xf != nullptr) {
                // Map USD light kinds to the shading types the lighting pass supports.
                // Distant is the directional sun; the local area kinds are approximated
                // as point lights (shape ignored for now). Dome/IBL isn't shaded yet.
                bool emit = true;
                LightType type = LightType::Directional;
                switch (desc->kind) {
                    case LightKind::Distant:
                        type = LightType::Directional;
                        break;
                    case LightKind::Sphere:
                    case LightKind::Disk:
                    case LightKind::Rect:
                    case LightKind::Cylinder:
                        type = LightType::Point;
                        break;
                    case LightKind::Dome:
                        emit = false;
                        break;
                }
                if (emit) {
                    out.lights.push_back({
                        .type = type,
                        .color = desc->color,
                        .intensity = desc->intensity,
                        .exposure = desc->exposure,
                        .angle = desc->angle,
                        .shadowEnable = desc->shadowEnable,
                        .shadowColor = desc->shadowColor,
                        .worldTransform = xf->world,
                        .primHandle = prim.handle,
                    });
                }
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

        const auto* meshData = meshLib.get(binding->mesh);
        if (!meshData || meshData->submeshes.empty()) {
            continue;
        }

        auto worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world);
        auto first = (uint32_t) out.meshInstances.size();
        // Expand the mesh into one draw instance per material submesh. All share
        // the mesh buffers, transform and (conservative full-mesh) bounds.
        for (size_t s = 0; s < meshData->submeshes.size(); s++) {
            const auto& sub = meshData->submeshes[s];
            out.meshInstances.push_back({
                .mesh = binding->mesh,
                .material = sub.material,
                .worldTransform = xf->world,
                .worldBounds = worldBounds,
                .indexOffset = sub.indexOffset,
                .indexCount = sub.indexCount,
                .primFirst = (s == 0),
            });
        }
        out.primToInstance[prim.handle.index] = {.first = first, .count = (uint32_t) meshData->submeshes.size()};
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
        auto worldBounds = meshLib.bounds(binding->mesh).transformed(xf->world);
        const auto& range = it->second;
        for (uint32_t i = 0; i < range.count; i++) {
            auto& inst = out.meshInstances[range.first + i];
            inst.worldTransform = xf->world;
            inst.worldBounds = worldBounds;
        }
    }

    // Refresh light world transforms for dirty prims. Small N, linear scan.
    if (!out.lights.empty()) {
        for (auto h : dirty) {
            for (auto& light : out.lights) {
                if (light.primHandle.index != h.index) {
                    continue;
                }
                const auto* xf = scene.getTransform(h);
                if (xf) {
                    light.worldTransform = xf->world;
                }
            }
        }
    }
}
