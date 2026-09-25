#version 450

// Debug views (DebugViewPass). Same UBO, instance buffer and position expression as
// gbuffer.vert, so the LessOrEqual test against the frame's depth keeps exactly the
// surfaces the geometry pass drew.
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
    uint pad;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec3 inColor;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vTexCoord;
layout(location = 2) flat out uvec3 vIds; // instance, mesh, material

invariant gl_Position;

void main() {
    Instance inst = instances.data[gl_InstanceIndex];
    gl_Position = ubo.proj * ubo.view * inst.model * vec4(inPosition, 1.0);
    vNormal = normalize(mat3(inst.model) * inNormal);
    vTexCoord = inTexCoord;
    vIds = uvec3(gl_InstanceIndex, inst.mesh, inst.material);
}
