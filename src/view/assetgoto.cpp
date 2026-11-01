#include "assetgoto.h"

#include "camera.h"
#include "viewscene.h"

#include <algorithm>
#include <format>

auto goToAsset(AssetGoToState& state, Camera& camera, const ViewScene& scene, std::string& selectedPath, const std::string& assetId, bool all) -> AssetGoToResult {
    AssetGoToResult result;
    auto prims = scene.drawsOf(assetId);
    if (prims.empty()) {
        result.error = std::format("nothing drawn uses {}", assetId);
        return result;
    }
    result.count = prims.size();
    if (all) {
        AABB bounds = {.min = glm::vec3(1e30f), .max = glm::vec3(-1e30f)};
        for (auto prim : prims) {
            auto b = scene.primBounds(prim);
            if (b.valid()) {
                bounds.min = glm::min(bounds.min, b.min);
                bounds.max = glm::max(bounds.max, b.max);
            }
        }
        if (bounds.valid()) {
            camera.frame(bounds, glm::radians(45.0f));
        }
        state = {};
        return result;
    }
    if (state.assetId != assetId || state.revision != scene.revision() || state.order.size() != prims.size()) {
        auto eye = camera.position;
        auto distance = [&](uint32_t prim) {
            auto b = scene.primBounds(prim);
            return b.valid() ? glm::length((b.min + b.max) * 0.5f - eye) : 1e30f;
        };
        std::ranges::stable_sort(prims, [&](uint32_t a, uint32_t b) { return distance(a) < distance(b); });
        state = {.assetId = assetId, .order = std::move(prims), .index = 0, .revision = scene.revision()};
    } else {
        state.index = (state.index + 1) % state.order.size();
    }
    auto prim = state.order[state.index];
    auto bounds = scene.primBounds(prim);
    if (bounds.valid()) {
        camera.frame(bounds, glm::radians(45.0f));
    }
    selectedPath = scene.primPath(prim);
    result.prim = selectedPath;
    result.index = state.index;
    return result;
}
