#version 450

// Material textures, indexed by the slot gbuffer.vert looks up; size matches GpuScene::maxTextures.
layout(set = 0, binding = 1) uniform sampler2D textures[1024];

layout(location = 0) in vec3 fragNormal;
layout(location = 1) in vec3 fragColor;
layout(location = 2) in vec2 fragTexCoord;
layout(location = 3) flat in uint fragTexture;

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

void main() {
    vec4 texColor = texture(textures[fragTexture], fragTexCoord);
    // Alpha carries the mip level the sampler picked, for the mip level buffer view.
    float lod = textureQueryLod(textures[fragTexture], fragTexCoord).x;
    outAlbedo = vec4(texColor.rgb * fragColor, clamp(lod / 16.0, 0.0, 1.0));

    vec3 n = fragNormal;
    if (dot(n, n) < 0.0001) {
        n = vec3(0.0, 1.0, 0.0);
    }
    outNormal = vec4(normalize(n) * 0.5 + 0.5, 1.0);
}
