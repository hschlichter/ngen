#pragma once

#include "config.h"
#include "scenetypes.h"
#include "shadowcascades.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <span>

class DebugDraw;
struct RenderWorld;

// The viewport's overlays, drawn with DebugDraw: a ground grid, the origin, every instance's bounds, the instances' bounds by their GPU cull
// result (`visible`: a byte per draw, nonzero for drawn; empty for none), the frozen culling frustum, each shadow cascade's light frustum,
// the lights' directions floating before `eye`, and the selection's bounds (null for none).
auto drawOverlays(DebugDraw& draw,
                  const RenderWorld& world,
                  const glm::vec3& eye,
                  const glm::vec3& up,
                  std::span<const uint8_t> visible,
                  const std::optional<std::array<glm::vec3, 8>>& frozenFrustum,
                  std::span<const ShadowCascade> cascades,
                  const AABB* selectedBounds) -> void;

// The overlays' configuration values (src/config/README.md).
namespace toolsconfig {
extern config::Bool overlayGrid;          // tools.overlay.grid
extern config::Bool overlayOrigin;        // tools.overlay.origin
extern config::Bool overlayGizmos;        // tools.overlay.gizmos: the axis and transform gizmos
extern config::Bool overlayAabbs;         // tools.overlay.aabbs: every instance's bounds
extern config::Bool overlaySelectedAabb;  // tools.overlay.selectedAabb
extern config::Bool overlayLightGizmos;   // tools.overlay.lightGizmos
extern config::Bool overlayCulled;        // tools.overlay.culled: the instances' bounds by their cull result
extern config::Int overlayCulledView;     // tools.overlay.culledView: whose result, 0 the camera, 1.. the cascades
extern config::Bool overlayCascadeFrusta; // tools.overlay.cascadeFrusta
} // namespace toolsconfig
