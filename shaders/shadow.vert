#version 450

layout(push_constant) uniform Push {
    mat4 lightViewProj;
} push;

// Persistent instance buffer (docs/plan_frame_graph_buffers.md); drawn with
// firstInstance = instance index, so gl_InstanceIndex selects the entry.
layout(std430, set = 0, binding = 0) readonly buffer Instances {
    mat4 model[];
} instances;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = push.lightViewProj * instances.model[gl_InstanceIndex] * vec4(inPosition, 1.0);
}
