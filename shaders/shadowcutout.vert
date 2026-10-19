#version 450

// Shadow pass for cutout materials: shadow.vert's position expression, from the full vertex stream for the texture coordinate the cutout
// test samples at. Uses the geometry pass's set (instances at binding 2, materials at 3, textures at 1).
layout(push_constant) uniform Push {
    mat4 lightViewProj;
} push;

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
    uint lodGroup; // its render-LOD group + 1, 0 for none
    uint pad1;
    uint pad2;
    uint pad3;
};
layout(std430, set = 0, binding = 2) readonly buffer Instances {
    Instance data[];
} instances;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec2 inTexCoord;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) flat out uint fragMaterial;

void main() {
    gl_Position = push.lightViewProj * instances.data[gl_InstanceIndex].model * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    fragMaterial = instances.data[gl_InstanceIndex].material;
}
