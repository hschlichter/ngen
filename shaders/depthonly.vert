#version 450

// Depth prepass. Same UBO, push layout and position expression as gbuffer.vert, and both
// declare gl_Position invariant, so the geometry pass's Equal depth test matches exactly.
layout(set = 0, binding = 0) uniform UBO {
    mat4 view;
    mat4 proj;
} ubo;

layout(push_constant) uniform Push {
    mat4 model;
} push;

layout(location = 0) in vec3 inPosition;

invariant gl_Position;

void main() {
    gl_Position = ubo.proj * ubo.view * push.model * vec4(inPosition, 1.0);
}
