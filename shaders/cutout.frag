#version 450
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_nonuniform_qualifier : require

// The depth-only passes for cutout materials and render-LOD fades (the depth prepass, the shadow pass): depth where the opacity reaches the
// material's threshold and the fade keeps the pixel, nothing elsewhere (cutout.glsl, lodfade.glsl).

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
#define LOD_FADE_FRAGMENT
#include "lodfade.glsl"

layout(location = 0) in vec2 fragTexCoord;
layout(location = 1) flat in uint fragMaterial;
layout(location = 2) flat in float fragLodCoverage;

void main() {
    if (lodFadedAway(fragLodCoverage) || cutAway(fragMaterial, fragTexCoord)) {
        discard;
    }
}
