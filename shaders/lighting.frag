#version 450

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform LightUBO {
    vec4 lightDirection;
    vec4 lightColor;
    vec4 depthParams;
} light;
layout(set = 0, binding = 3) uniform sampler2D gbufferDepth;

layout(push_constant) uniform Push {
    int viewMode;
    int showOverlay;
} push;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

const float STRIP_HEIGHT = 0.25;
const float PREVIEW_WIDTH = 0.25;
const float BORDER = 0.003;
const int NUM_PREVIEWS = 4;

float linearizeDepth(float d) {
    float near = light.depthParams.x;
    float far = light.depthParams.y;
    return near * far / (far - d * (far - near));
}

const vec3 BACKGROUND_COLOR = vec3(0.12, 0.12, 0.15);

bool isBackground(vec2 uv) {
    return texture(gbufferDepth, uv).r >= 1.0;
}

vec3 sampleBuffer(int mode, vec2 uv) {
    if (mode == 3) {
        float d = texture(gbufferDepth, uv).r;
        if (d >= 1.0) return vec3(0.0);
        float near = light.depthParams.x;
        float far = light.depthParams.y;
        float lin = linearizeDepth(d);
        float v = 1.0 - clamp(log(lin / near) / log(far / near), 0.0, 1.0);
        return vec3(v);
    }
    if (isBackground(uv)) return BACKGROUND_COLOR;
    vec3 albedo = texture(gbufferAlbedo, uv).rgb;
    if (mode == 1) return albedo;
    vec3 normal = texture(gbufferNormal, uv).rgb * 2.0 - 1.0;
    if (mode == 2) return normalize(normal) * 0.5 + 0.5;
    vec3 lightDir = normalize(light.lightDirection.xyz);
    float diff = max(dot(normalize(normal), lightDir), 0.0);
    float ambient = light.lightColor.w;
    return albedo * light.lightColor.rgb * (ambient + diff * light.lightDirection.w);
}

void main() {
    vec2 uv = fragTexCoord;

    if (push.showOverlay == 1 && uv.y > (1.0 - STRIP_HEIGHT)) {
        float stripLocalY = (uv.y - (1.0 - STRIP_HEIGHT)) / STRIP_HEIGHT;
        float stripLocalX = uv.x;

        int previewIdx = int(floor(stripLocalX / PREVIEW_WIDTH));
        if (previewIdx < NUM_PREVIEWS) {
            float localX = (stripLocalX - float(previewIdx) * PREVIEW_WIDTH) / PREVIEW_WIDTH;
            float localY = stripLocalY;

            float bx = BORDER / PREVIEW_WIDTH;
            float by = BORDER / STRIP_HEIGHT;
            if (localX < bx || localX > 1.0 - bx || localY < by || localY > 1.0 - by) {
                outColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            int modes[4] = int[4](1, 2, 3, 0);
            vec2 sampleUV = vec2(localX, localY);
            outColor = vec4(sampleBuffer(modes[previewIdx], sampleUV), 1.0);
            return;
        }
    }

    outColor = vec4(sampleBuffer(push.viewMode, uv), 1.0);
}
