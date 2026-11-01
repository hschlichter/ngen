#pragma once

#include <glm/glm.hpp>

// The renderer's projection convention: reversed-Z (depth 1 at the near plane, falling to 0 at infinity) with no far plane, and the Vulkan
// y flip. Depth is a 32-bit float, where reversed-Z keeps precision even far away, so nothing is ever clipped at a distance. The lighting
// pass's depth reconstruction, the shadow cascades' fit and the debug views depend on it.
inline constexpr float projectionNear = 0.1f;

// The projection for a vertical field of view (radians) and an aspect (width over height).
auto perspectiveProjection(float fovY, float aspect) -> glm::mat4;
// The depth (0..1] at which a point `distance` in front of the camera lands.
auto projectionDepthAt(float distance) -> float;
