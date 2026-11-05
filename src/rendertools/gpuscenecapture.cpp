#include "gpuscenecapture.h"

#include "gpuscene.h"
#include "gpuschema.h"

#include <algorithm>
#include <cstdio>
#include <format>

auto capturedTable(const std::map<std::string, CaptureResult>& byResource, const char* resource) -> const CaptureResult* {
    auto it = byResource.find(resource);
    return it != byResource.end() && it->second.bytes && it->second.error.empty() ? &it->second : nullptr;
}

auto gpuSceneViewName(uint32_t view) -> std::string {
    return view == 0 ? std::string("camera") : std::format("cascade {}", view - 1);
}

auto gpuInstanceFlagText(uint32_t flags) -> std::string {
    std::string text;
    if ((flags & gpuInstancePrimFirst) != 0) {
        text += "primFirst ";
    }
    if ((flags & gpuInstanceDoubleSided) != 0) {
        text += "doubleSided ";
    }
    if ((flags & gpuInstanceBoundsValid) == 0) {
        text += "noBounds";
    }
    return text;
}

auto gpuSceneCaptures() -> const std::vector<std::pair<std::string, std::string>>& {
    // (pass, resource): each table where it is complete for the frame.
    static const std::vector<std::pair<std::string, std::string>> captures = {
        {"InstanceCullScatter", "instances"},
        {"", "gpuscene.meshtable"},
        {"", "gpuscene.materials"},
        {"InstanceCull", "cullVisibility"},
        {"InstanceCull", "cullPlanes"},
        {"InstanceCull", "cullGroupCounters"},
        {"InstanceCullScan", "cullGroupOffsets"},
        {"InstanceCullScan", "drawCounts"},
        {"InstanceCullScatter", "drawCommands"},
    };
    return captures;
}

auto primOfEachInstance(const RenderWorld& renderWorld) -> std::vector<uint32_t> {
    std::vector<uint32_t> prims(renderWorld.meshInstances.size(), 0);
    for (size_t i = 0; i < prims.size(); i++) {
        if (renderWorld.meshInstances[i].alive) {
            prims[i] = renderWorld.meshInstances[i].prim;
        }
    }
    return prims;
}

auto buildGpuSceneRows(const std::map<std::string, CaptureResult>& byResource, std::span<const uint32_t> primOfInstance, const std::function<std::string(uint32_t)>& primPath) -> std::vector<GpuSceneInstanceRow> {
    std::vector<GpuSceneInstanceRow> rows;
    const auto* instances = capturedTable(byResource, "instances");
    if (instances == nullptr) {
        return rows;
    }
    const auto* meshes = capturedTable(byResource, "gpuscene.meshtable");
    const auto* materials = capturedTable(byResource, "gpuscene.materials");
    const auto* visibility = capturedTable(byResource, "cullVisibility");
    const auto* planes = capturedTable(byResource, "cullPlanes");
    // The instance buffer is sized to its capacity; the visibility buffer's live part is the
    // instance count the culling saw, so rows stop where both agree.
    auto count = instances->bytes->size() / sizeof(GpuInstanceRecord);
    if (visibility != nullptr) {
        count = std::min(count, visibility->bytes->size() / sizeof(uint32_t));
    }
    count = std::min(count, primOfInstance.size());
    rows.reserve(count);
    for (uint32_t m = 0; m < count; m++) {
        auto record = capturedValue<GpuInstanceRecord>(*instances, (size_t) m * sizeof(GpuInstanceRecord));
        GpuSceneInstanceRow row = {
            .index = m,
            .prim = primOfInstance[m],
            .mesh = record.mesh,
            .material = record.material,
            .indexOffset = record.indexOffset,
            .indexCount = record.indexCount,
            .flags = record.flags,
            .boundsMin = record.boundsMin,
            .boundsMax = record.boundsMax,
        };
        row.primPath = primPath(row.prim);
        if (meshes != nullptr) {
            row.meshIndexCount = capturedValue<GpuMeshEntry>(*meshes, (size_t) record.mesh * sizeof(GpuMeshEntry)).indexCount;
        }
        if (materials != nullptr) {
            row.textureSlot = capturedValue<GpuMaterial>(*materials, (size_t) record.material * sizeof(GpuMaterial)).baseColorTexture;
        }
        if (visibility != nullptr) {
            row.visibleBits = capturedValue<uint32_t>(*visibility, (size_t) m * 4);
        }
        if (planes != nullptr) {
            row.cullPlanes = capturedValue<uint32_t>(*planes, (size_t) m * 4);
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

auto writeGpuSceneJoinedJson(FILE* f, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> void {
    std::fprintf(f, "{\"views\": %u, \"instances\": [\n", viewCount);
    for (size_t i = 0; i < rows.size(); i++) {
        const auto& r = rows[i];
        std::fprintf(f, "  {\"instance\": %u, \"prim\": \"%s\", \"mesh\": %u, \"meshInPool\": %s, \"material\": %u, \"textureSlot\": %u, \"indexOffset\": %u, \"indexCount\": %u, \"flags\": \"%s\", \"boundsMin\": [%g, %g, %g], \"boundsMax\": [%g, %g, %g], \"views\": [", r.index, r.primPath.c_str(), r.mesh, r.meshIndexCount > 0 ? "true" : "false", r.material, r.textureSlot, r.indexOffset, r.indexCount, gpuInstanceFlagText(r.flags).c_str(), r.boundsMin.x, r.boundsMin.y, r.boundsMin.z, r.boundsMax.x, r.boundsMax.y, r.boundsMax.z);
        for (uint32_t v = 0; v < viewCount; v++) {
            std::fprintf(f, "%s{\"view\": \"%s\", \"visible\": %s, \"reason\": \"%s\"}", v > 0 ? ", " : "", gpuSceneViewName(v).c_str(), (r.visibleBits & (1u << v)) != 0 ? "true" : "false", cullPlaneName((r.cullPlanes >> (4 * v)) & 0xFu));
        }
        std::fprintf(f, "]}%s\n", i + 1 < rows.size() ? "," : "");
    }
    std::fprintf(f, "]}\n");
}

auto writeGpuSceneJoinedJson(const std::string& path, const std::vector<GpuSceneInstanceRow>& rows, uint32_t viewCount) -> bool {
    auto* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    writeGpuSceneJoinedJson(f, rows, viewCount);
    std::fclose(f);
    return true;
}
