#version 450

// Depth prepass. Same UBO, instance buffer and position expression as gbuffer.vert, and both
// declare gl_Position invariant, so the geometry pass's Equal depth test matches exactly.
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
    uint pad;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

layout(location = 0) in vec3 inPosition;

invariant gl_Position;

void main() {
    mat4 model = instances.data[gl_InstanceIndex].model;
    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
}
