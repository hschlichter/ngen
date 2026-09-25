#version 450

// One raw value per pixel per debug view; debugview.comp turns it into a colour and the
// readout reads it back as is. Alpha 1 marks covered pixels (background stays 0), and
// counts fragments in the overdraw view, where blending adds.
layout(push_constant) uniform Push {
    vec2 viewport;
    uint mode; // DebugView
} push;

layout(location = 0) in vec3 gNormal;
layout(location = 1) in vec2 gTexCoord;
layout(location = 2) flat in uvec3 gIds;
layout(location = 3) noperspective in vec3 gEdgeDistance;
layout(location = 4) flat in float gArea;
layout(location = 5) flat in uint gPrimitive;

layout(location = 0) out vec4 outValue;

const uint viewWireframe = 1;
const uint viewTriangleSize = 2;
const uint viewOverdraw = 3;
const uint viewInstance = 4;
const uint viewMesh = 5;
const uint viewMaterial = 6;
const uint viewPrimitive = 7;
const uint viewUv = 8;

void main() {
    if (push.mode == viewWireframe) {
        float edge = min(gEdgeDistance.x, min(gEdgeDistance.y, gEdgeDistance.z));
        vec3 n = normalize(gNormal);
        float shade = 0.3 + 0.7 * abs(dot(n, normalize(vec3(0.4, 0.8, 0.45))));
        outValue = vec4(edge, shade, 0.0, 1.0);
    } else if (push.mode == viewTriangleSize) {
        outValue = vec4(gArea, 0.0, 0.0, 1.0);
    } else if (push.mode == viewOverdraw) {
        outValue = vec4(1.0, 0.0, 0.0, 1.0);
    } else if (push.mode == viewInstance) {
        outValue = vec4(float(gIds.x), 0.0, 0.0, 1.0);
    } else if (push.mode == viewMesh) {
        outValue = vec4(float(gIds.y), 0.0, 0.0, 1.0);
    } else if (push.mode == viewMaterial) {
        outValue = vec4(float(gIds.z), 0.0, 0.0, 1.0);
    } else if (push.mode == viewPrimitive) {
        outValue = vec4(float(gPrimitive), 0.0, 0.0, 1.0);
    } else if (push.mode == viewUv) {
        outValue = vec4(gTexCoord, 0.0, 1.0);
    } else {
        outValue = vec4(0.0);
    }
}
