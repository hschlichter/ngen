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

auto DebugDraw::sphere(glm::vec3 center, float radius, glm::vec4 color, int segments) -> void {
    for (int i = 0; i < segments; i++) {
        float a0 = (float) i / (float) segments * glm::two_pi<float>();
        float a1 = (float) (i + 1) / (float) segments * glm::two_pi<float>();
        float c0 = cosf(a0) * radius, s0 = sinf(a0) * radius;
        float c1 = cosf(a1) * radius, s1 = sinf(a1) * radius;
        // XY circle
        line(center + glm::vec3(c0, s0, 0), center + glm::vec3(c1, s1, 0), color);
        // XZ circle
        line(center + glm::vec3(c0, 0, s0), center + glm::vec3(c1, 0, s1), color);
        // YZ circle
        line(center + glm::vec3(0, c0, s0), center + glm::vec3(0, c1, s1), color);
    }
}

auto DebugDraw::grid(glm::vec3 cameraPos, glm::vec3 worldUp, float spacing, int halfCount, glm::vec4 color) -> void {
    auto up = glm::normalize(worldUp);
    // Build two orthonormal axes in the plane perpendicular to up. Pick a seed that isn't
    // parallel to up, then Gram–Schmidt.
    auto seed = std::abs(up.x) > 0.9f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0);
    auto axisA = glm::normalize(seed - up * glm::dot(seed, up));
    auto axisB = glm::normalize(glm::cross(up, axisA));

    // Project camera onto the grid plane (plane passes through origin, normal = up).
    auto planePos = cameraPos - up * glm::dot(cameraPos, up);
    float centerA = std::round(glm::dot(planePos, axisA) / spacing) * spacing;
    float centerB = std::round(glm::dot(planePos, axisB) / spacing) * spacing;
    float extent = halfCount * spacing;
    float maxDist = extent;

    for (int i = -halfCount; i <= halfCount; i++) {
        float offset = i * spacing;
        float distA = std::abs(centerA + offset - glm::dot(planePos, axisA));
        float distB = std::abs(centerB + offset - glm::dot(planePos, axisB));
        bool majorA = std::abs(std::fmod(centerA + offset, spacing * 10.0f)) < spacing * 0.5f;
        bool majorB = std::abs(std::fmod(centerB + offset, spacing * 10.0f)) < spacing * 0.5f;

        // Line parallel to axisB (varies along axisA)
        {
            float fade = std::max(1.0f - (distA / maxDist), 0.0f);
            fade *= fade;
            auto c = color;
            if (majorA) c = glm::vec4(color.r * 1.5f, color.g * 1.5f, color.b * 1.5f, color.a);
            c.r *= fade;
            c.g *= fade;
            c.b *= fade;
            auto base = axisA * (centerA + offset) + axisB * centerB;
            line(base - axisB * extent, base + axisB * extent, c);
        }

        // Line parallel to axisA (varies along axisB)
        {
            float fade = std::max(1.0f - (distB / maxDist), 0.0f);
            fade *= fade;
            auto c = color;
            if (majorB) c = glm::vec4(color.r * 1.5f, color.g * 1.5f, color.b * 1.5f, color.a);
            c.r *= fade;
            c.g *= fade;
            c.b *= fade;
            auto base = axisA * centerA + axisB * (centerB + offset);
            line(base - axisA * extent, base + axisA * extent, c);
        }
    }
}

auto DebugDraw::newFrame() -> void {
    frameData.lines.clear();
}
