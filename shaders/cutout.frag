#version 450
#extension GL_GOOGLE_include_directive : require

// The depth-only passes for cutout materials (the depth prepass, the shadow pass): depth where the opacity reaches the material's
// threshold, nothing where it doesn't (cutout.glsl).

layout(set = 0, binding = 1) uniform sampler2D textures[1024];

struct Material {
    uint baseColorTexture;
    uint opacityTexture;
    uint opacityInfo;
    float opacityThreshold;
};
layout(std430, set = 0, binding = 3) readonly buffer Materials {
    Material data[];
} materials;

#include "cutout.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint fragMaterial;

void main() {
    if (cutAway(fragMaterial, fragTexCoord)) {
        discard;
    }
}
