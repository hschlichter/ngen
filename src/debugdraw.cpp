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

auto DebugDraw::grid(glm::vec3 cameraPos, float spacing, int halfCount, glm::vec4 color) -> void {
    float centerX = std::round(cameraPos.x / spacing) * spacing;
    float centerZ = std::round(cameraPos.z / spacing) * spacing;
    float extent = halfCount * spacing;
    float maxDist = extent;

    for (int i = -halfCount; i <= halfCount; i++) {
        float offset = i * spacing;

        // Fade based on distance from camera
        float distX = std::abs(centerX + offset - cameraPos.x);
        float distZ = std::abs(centerZ + offset - cameraPos.z);

        // Major lines every 10 grid units
        bool majorX = std::abs(std::fmod(centerX + offset, spacing * 10.0f)) < spacing * 0.5f;
        bool majorZ = std::abs(std::fmod(centerZ + offset, spacing * 10.0f)) < spacing * 0.5f;

        // Z-aligned line (varies X)
        {
            float fade = 1.0f - (distX / maxDist);
            fade = std::max(fade, 0.0f);
            fade *= fade;
            auto c = color;
            if (majorX) c = glm::vec4(color.r * 1.5f, color.g * 1.5f, color.b * 1.5f, color.a);
            c.r *= fade;
            c.g *= fade;
            c.b *= fade;
            float x = centerX + offset;
            line({x, 0, centerZ - extent}, {x, 0, centerZ + extent}, c);
        }

        // X-aligned line (varies Z)
        {
            float fade = 1.0f - (distZ / maxDist);
            fade = std::max(fade, 0.0f);
            fade *= fade;
            auto c = color;
            if (majorZ) c = glm::vec4(color.r * 1.5f, color.g * 1.5f, color.b * 1.5f, color.a);
            c.r *= fade;
            c.g *= fade;
            c.b *= fade;
            float z = centerZ + offset;
            line({centerX - extent, 0, z}, {centerX + extent, 0, z}, c);
        }
    }
}

auto DebugDraw::newFrame() -> void {
    frameData.lines.clear();
}
