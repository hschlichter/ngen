#version 450
#extension GL_GOOGLE_include_directive : require

// The geometry pass for cutout materials: gbuffer.frag's outputs where the opacity reaches the material's threshold, nothing where it
// doesn't (cutout.glsl). Kept in step with gbuffer.frag.

layout(set = 0, binding = 1) uniform sampler2D textures[1024];

// The instance buffer, for the instance's material (gbuffer.vert's Instance).
struct Instance {
    mat4 model;
    uint material;
    uint mesh;
    uint indexOffset;
    uint indexCount;
    vec3 boundsMin;
    uint flags;
    vec3 boundsMax;
    uint pad;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

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

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) flat in uint fragTexture;
layout(location = 4) flat in uint fragInstance; // the instance index + 1

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;
layout(location = 2) out uint outInstance;

void main() {
    if (cutAway(instances.data[fragInstance - 1u].material, fragTexCoord)) {
        discard;
    }
    vec4 texColor = texture(textures[fragTexture], fragTexCoord);
    // Alpha carries the mip level the sampler picked, for the mip level buffer view.
    float lod = textureQueryLod(textures[fragTexture], fragTexCoord).x;
    outAlbedo = vec4(texColor.rgb * fragColor, clamp(lod / 16.0, 0.0, 1.0));

    vec3 n = fragNormal;
    if (dot(n, n) < 0.0001) {
        n = vec3(0.0, 1.0, 0.0);
    }
    outNormal = vec4(normalize(n) * 0.5 + 0.5, 1.0);
    outInstance = fragInstance;
}
