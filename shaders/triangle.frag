#version 450

layout(binding = 1) uniform sampler2D texSampler;

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec3 lightDir = normalize(vec3(1.0, 1.0, 1.0));
    float ambient = 0.15;
    float diffuse = max(dot(normalize(fragNormal), lightDir), 0.0);
    vec3 texColor = texture(texSampler, fragTexCoord).rgb;
    outColor = vec4(texColor * fragColor * (ambient + diffuse), 1.0);
}
