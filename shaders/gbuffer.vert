#version 450
#extension GL_GOOGLE_include_directive : require

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

// Persistent instance buffer (GpuInstanceRecord); drawn with
// firstInstance = instance index, so gl_InstanceIndex selects the entry.
struct Instance {
    mat4 model;
    uint material; // material table entry
    uint mesh;
    uint indexOffset;
    uint indexCount;
    vec3 boundsMin;
    uint flags;
    vec3 boundsMax;
    uint lodLevel; // drawn LOD level + 1, 0 under no LOD root
    uint lodGroup; // its render-LOD group + 1, 0 for none
    uint pad1;
    uint lodDepth; // how many LOD roots it is under, 0 under none
    uint pad3;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

// Material table (GpuMaterial): the texture slot the fragment shader samples, and the cutout opacity (cutout.glsl).
struct Material {
    uint baseColorTexture;
    uint opacityTexture;
    uint opacityInfo;
    float opacityThreshold;
};
layout(std430, set = 0, binding = 3) readonly buffer Materials {
    Material data[];
} materials;

// Per render-LOD group, lodselect.comp's state: the level fading in and its share (lodfade.glsl).
layout(std430, set = 0, binding = 4) readonly buffer LodStates {
    uvec4 data[];
} lodStates;

#include "lodfade.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec2 fragTexCoord;
// One value per draw, so the fragment shader's array index is dynamically uniform.
layout(location = 3) flat out uint fragTexture;
// The instance index + 1, for the id target picking reads; 0 is nothing drawn.
layout(location = 4) flat out uint fragInstance;
// x: the drawn LOD level + 1 of the LOD root above it, 0 under none, with its group's flags (lodTag), and 128 per LOD root it is under
// (lodTagDepth, at most 15), for the LOD level view and the pop flash. y: the instance's share of a render-LOD fade as float bits
// (lodfade.glsl), for the cutout fragment shader; lodCoverAll when its group isn't fading. One varying, so the opaque fragment shader, which reads only x, still takes it.
layout(location = 5) flat out uvec2 fragLod;

// Invariant so the depth prepass (depthonly.vert, same expression) produces identical depth.
invariant gl_Position;

void main() {
    mat4 model = instances.data[gl_InstanceIndex].model;
    fragTexture = materials.data[instances.data[gl_InstanceIndex].material].baseColorTexture;
    fragInstance = uint(gl_InstanceIndex) + 1u;
    uint lodGroup = instances.data[gl_InstanceIndex].lodGroup;
    uint lodLevel = instances.data[gl_InstanceIndex].lodLevel;
    uint depth = min(instances.data[gl_InstanceIndex].lodDepth, 15u);
    fragLod = uvec2(lodTag(lodGroup, lodLevel) + (depth * lodTagDepth), floatBitsToUint(lodCoverage(lodGroup, lodLevel)));
    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
    // Normalise here: the model matrix carries the scene's unit scale (Kitchen_set is
    // authored in cm under a 0.01 root scale), which would leave a near-zero normal for
    // the fragment shader's zero guard to swallow.
    fragNormal = normalize(mat3(model) * inNormal);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
