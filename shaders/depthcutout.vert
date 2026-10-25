#version 450
#extension GL_GOOGLE_include_directive : require

// Depth prepass for cutout materials. Same UBO, instance buffer and position expression as gbuffer.vert and depthonly.vert, all with
// gl_Position invariant, so the geometry pass's Equal depth test matches exactly; reads the full vertex stream for the texture coordinate
// the cutout test samples at.
layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

struct Instance {
    mat4 model;
    uint material; // material table entry
    uint mesh;
    uint indexOffset;
    uint indexCount;
    vec3 boundsMin;
    uint flags;
    vec3 boundsMax;
    uint lodLevel; // its level + 1 in its render-LOD group
    uint lodGroup; // its render-LOD group + 1, 0 for none
    uint pad1;
    uint pad2;
    uint pad3;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

// Per render-LOD group, lodselect.comp's state: the level fading in and its share (lodfade.glsl).
layout(std430, set = 0, binding = 4) readonly buffer LodStates {
    uvec4 data[];
} lodStates;

#include "lodfade.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint fragMaterial;
layout(location = 2) flat out float fragLodCoverage; // its share of a render-LOD fade (lodfade.glsl)

invariant gl_Position;

void main() {
    mat4 model = instances.data[gl_InstanceIndex].model;
    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragMaterial = instances.data[gl_InstanceIndex].material;
    fragLodCoverage = lodCoverage(instances.data[gl_InstanceIndex].lodGroup, instances.data[gl_InstanceIndex].lodLevel);
}
