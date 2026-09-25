#version 450

// Sees each whole triangle: its screen-space size, the distance from each fragment to
// the triangle's edges (for the wireframe) and its index in the draw.
layout(triangles) in;
layout(triangle_strip, max_vertices = 3) out;

layout(push_constant) uniform Push {
    vec2 viewport; // pixels
    uint mode;     // DebugView
} push;

layout(location = 0) in vec3 vNormal[];
layout(location = 1) in vec2 vTexCoord[];
layout(location = 2) flat in uvec3 vIds[];

layout(location = 0) out vec3 gNormal;
layout(location = 1) out vec2 gTexCoord;
layout(location = 2) flat out uvec3 gIds;
layout(location = 3) noperspective out vec3 gEdgeDistance; // pixels to the edge opposite each vertex
layout(location = 4) flat out float gArea;                 // pixels
layout(location = 5) flat out uint gPrimitive;

invariant gl_Position;

void main() {
    vec2 p[3];
    bool behind = false;
    for (int i = 0; i < 3; i++) {
        vec4 clip = gl_in[i].gl_Position;
        if (clip.w <= 0.0) {
            behind = true;
        }
        p[i] = clip.xy / max(clip.w, 1e-6) * 0.5 * push.viewport;
    }
    // Twice the signed area; the heights follow from area = base * height / 2.
    float area2 = abs((p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y));
    vec3 heights = vec3(area2 / max(length(p[2] - p[1]), 1e-6), area2 / max(length(p[2] - p[0]), 1e-6), area2 / max(length(p[1] - p[0]), 1e-6));
    if (behind) {
        // Crosses the camera plane: no meaningful screen size, and no edges to draw.
        area2 = 2.0e6;
        heights = vec3(1.0e6);
    }
    for (int i = 0; i < 3; i++) {
        gl_Position = gl_in[i].gl_Position;
        gNormal = vNormal[i];
        gTexCoord = vTexCoord[i];
        gIds = vIds[i];
        gEdgeDistance = vec3(0.0);
        gEdgeDistance[i] = heights[i];
        gArea = area2 * 0.5;
        gPrimitive = uint(gl_PrimitiveIDIn);
        EmitVertex();
    }
    EndPrimitive();
}
