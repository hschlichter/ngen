#version 450

layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

// Persistent instance buffer (docs/plan_frame_graph_buffers.md); drawn with
// firstInstance = instance index, so gl_InstanceIndex selects the entry.
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    mat4 model[];
} instances;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 fragNormal;
layout(location = 1) out vec3 fragColor;
layout(location = 2) out vec2 fragTexCoord;

// Invariant so the depth prepass (depthonly.vert, same expression) produces identical depth.
invariant gl_Position;

void main() {
    mat4 model = instances.model[gl_InstanceIndex];
    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
    // Normalise here: the model matrix carries the scene's unit scale (Kitchen_set is
    // authored in cm under a 0.01 root scale), which would leave a near-zero normal for
    // the fragment shader's zero guard to swallow.
    fragNormal = normalize(mat3(model) * inNormal);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}
