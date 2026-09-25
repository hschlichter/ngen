#version 450

layout(push_constant) uniform Push {
    mat4 lightViewProj;
} push;

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
layout(std430, set = 0, binding = 0) readonly buffer Instances {
    Instance data[];
} instances;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = push.lightViewProj * instances.data[gl_InstanceIndex].model * vec4(inPosition, 1.0);
}
