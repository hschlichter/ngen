#version 450

layout(push_constant) uniform Push {
    mat4 lightViewProj;
    mat4 model;
} push;

layout(location = 0) in vec3 inPosition;

void main() {
    gl_Position = push.lightViewProj * push.model * vec4(inPosition, 1.0);
}
