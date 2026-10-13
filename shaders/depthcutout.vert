#version 450

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
    uint pad;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint fragMaterial;

invariant gl_Position;

void main() {
    mat4 model = instances.data[gl_InstanceIndex].model;
    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragMaterial = instances.data[gl_InstanceIndex].material;
}
