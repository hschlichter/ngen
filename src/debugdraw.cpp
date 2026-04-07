#include "debugdraw.h"

auto DebugDraw::line(glm::vec3 a, glm::vec3 b, glm::vec4 color) -> void {
    frameData.lines.push_back({a, color});
    frameData.lines.push_back({b, color});
}

auto DebugDraw::box(const AABB& box, glm::vec4 color) -> void {
    auto mn = box.min;
    auto mx = box.max;

    // bottom face
    line({mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, color);
    line({mx.x, mn.y, mn.z}, {mx.x, mn.y, mx.z}, color);
    line({mx.x, mn.y, mx.z}, {mn.x, mn.y, mx.z}, color);
    line({mn.x, mn.y, mx.z}, {mn.x, mn.y, mn.z}, color);

    // top face
    line({mn.x, mx.y, mn.z}, {mx.x, mx.y, mn.z}, color);
    line({mx.x, mx.y, mn.z}, {mx.x, mx.y, mx.z}, color);
    line({mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}, color);
    line({mn.x, mx.y, mx.z}, {mn.x, mx.y, mn.z}, color);

    // verticals
    line({mn.x, mn.y, mn.z}, {mn.x, mx.y, mn.z}, color);
    line({mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, color);
    line({mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, color);
    line({mn.x, mn.y, mx.z}, {mn.x, mx.y, mx.z}, color);
}

auto DebugDraw::newFrame() -> void {
    frameData.lines.clear();
}
