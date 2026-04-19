#version 450

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform LightUBO {
    vec4 lightDirection;
    vec4 lightColor;
    vec4 depthParams;
    vec4 shadowTint;
    mat4 invViewProj;
    mat4 lightViewProj;
} light;
layout(set = 0, binding = 3) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 4) uniform sampler2D shadowMap;

layout(push_constant) uniform Push {
    int viewMode;
    int showOverlay;
    int showShadowOverlay;
} push;

layout(location = 0) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;

const float STRIP_HEIGHT = 0.25;
const float GBUFFER_PREVIEW_WIDTH = 0.25;
const float SHADOW_PREVIEW_WIDTH = 0.2;
const float BORDER = 0.003;
const int NUM_GBUFFER_PREVIEWS = 4;
const int NUM_SHADOW_PREVIEWS = 3;

float linearizeDepth(float d) {
    float near = light.depthParams.x;
    float far = light.depthParams.y;
    return near * far / (far - d * (far - near));
}

const vec3 BACKGROUND_COLOR = vec3(0.12, 0.12, 0.15);

bool isBackground(vec2 uv) {
    return texture(gbufferDepth, uv).r >= 1.0;
}

// Reconstruct world-space position from screen UV + sampled depth using the inverse
// of (proj * view). Matches whatever Y convention the gbuffer was rendered with, since
// invViewProj came from the same matrices.
vec3 reconstructWorld(vec2 uv, float depth) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, depth, 1.0);
    vec4 world = light.invViewProj * ndc;
    return world.xyz / world.w;
}

// Returns 1.0 when lit, 0.0 when shadowed. Caller blends between the light's radiance
// and the UsdLuxShadowAPI shadowColor tint based on the result.
//
// Slope-scale depth bias: the bias scales with the angle between the receiver normal
// and the light direction, so surfaces facing the light get a tiny floor bias (just
// enough to cover fp precision) while grazing surfaces get a larger bias to avoid
// acne on near-tangent polygons. A fixed NDC-Z bias would scale with the ortho far
// plane and swamp small casters — e.g. a 10cm cup in a ~30m frustum at bias 0.005.
float sampleShadow(vec3 worldPos, vec3 normal) {
    vec4 lightClip = light.lightViewProj * vec4(worldPos, 1.0);
    vec3 lightNdc = lightClip.xyz / lightClip.w;
    vec2 shadowUV = lightNdc.xy * 0.5 + 0.5;
    if (any(lessThan(shadowUV, vec2(0.0))) || any(greaterThan(shadowUV, vec2(1.0))) || lightNdc.z < 0.0 || lightNdc.z > 1.0) {
        return 1.0; // outside shadow frustum — treat as lit
    }
    float sampled = texture(shadowMap, shadowUV).r;
    vec3 N = normalize(normal);
    vec3 L = normalize(light.lightDirection.xyz);
    float slope = clamp(1.0 - dot(N, L), 0.0, 1.0);
    float bias = max(0.001 * slope, 0.00005);
    return (lightNdc.z - bias) > sampled ? 0.0 : 1.0;
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
    if (mode == 5) {
        // Raw shadow map visualization: screen UV → shadow map UV.
        return vec3(texture(shadowMap, uv).r);
    }
    if (isBackground(uv)) return BACKGROUND_COLOR;
    vec3 albedo = texture(gbufferAlbedo, uv).rgb;
    if (mode == 1) return albedo;
    vec3 normal = texture(gbufferNormal, uv).rgb * 2.0 - 1.0;
    if (mode == 2) return normalize(normal) * 0.5 + 0.5;

    float depth = texture(gbufferDepth, uv).r;
    vec3 worldPos = reconstructWorld(uv, depth);
    float shadow = sampleShadow(worldPos, normal);
    // shadow factor per channel: full light where lit, shadowTint where shadowed
    vec3 shadowFactor = mix(light.shadowTint.rgb, vec3(1.0), shadow);
    if (mode == 4) return vec3(shadow);
    if (mode == 6) {
        // Visualize where this fragment lands in shadow-map UV space.
        // Red = shadowUV.x, Green = shadowUV.y, Blue = 1 if inside frustum else 0.
        vec4 lightClip = light.lightViewProj * vec4(worldPos, 1.0);
        vec3 lightNdc = lightClip.xyz / lightClip.w;
        vec2 sUV = lightNdc.xy * 0.5 + 0.5;
        float inside = (all(greaterThanEqual(sUV, vec2(0.0))) && all(lessThanEqual(sUV, vec2(1.0)))) ? 1.0 : 0.0;
        return vec3(sUV, inside);
    }
    if (mode == 7) {
        // Reconstructed world position mapped from world range [-sceneRadius, +sceneRadius]
        // to color [0, 1]. Scene origin is medium gray; red rising = +X, green = +Y, blue = +Z.
        // Saturates when world distance exceeds the hardcoded scale below. Tweak the scale
        // for scenes larger than ~50 units.
        return clamp(worldPos * (1.0 / 50.0) + 0.5, 0.0, 1.0);
    }

    vec3 lightDir = normalize(light.lightDirection.xyz);
    float diff = max(dot(normalize(normal), lightDir), 0.0);
    float ambient = light.lightColor.w;

    return albedo * (ambient * light.lightColor.rgb + diff * light.lightColor.rgb * shadowFactor);
}

void main() {
    vec2 uv = fragTexCoord;

    // Bottom strip: gbuffer overlay (albedo / normals / depth / lit).
    if (push.showOverlay == 1 && uv.y > (1.0 - STRIP_HEIGHT)) {
        float stripLocalY = (uv.y - (1.0 - STRIP_HEIGHT)) / STRIP_HEIGHT;
        float stripLocalX = uv.x;

        int previewIdx = int(floor(stripLocalX / GBUFFER_PREVIEW_WIDTH));
        if (previewIdx < NUM_GBUFFER_PREVIEWS) {
            float localX = (stripLocalX - float(previewIdx) * GBUFFER_PREVIEW_WIDTH) / GBUFFER_PREVIEW_WIDTH;
            float localY = stripLocalY;

            float bx = BORDER / GBUFFER_PREVIEW_WIDTH;
            float by = BORDER / STRIP_HEIGHT;
            if (localX < bx || localX > 1.0 - bx || localY < by || localY > 1.0 - by) {
                outColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            int modes[4] = int[4](1, 2, 3, 0); // albedo, normals, depth, lit
            vec2 sampleUV = vec2(localX, localY);
            outColor = vec4(sampleBuffer(modes[previewIdx], sampleUV), 1.0);
            return;
        }
    }

    // Top strip: shadow overlay (shadow map / shadow factor / shadow UV).
    if (push.showShadowOverlay == 1 && uv.y < STRIP_HEIGHT) {
        float stripLocalY = uv.y / STRIP_HEIGHT;
        float stripLocalX = uv.x;

        int previewIdx = int(floor(stripLocalX / SHADOW_PREVIEW_WIDTH));
        if (previewIdx < NUM_SHADOW_PREVIEWS) {
            float localX = (stripLocalX - float(previewIdx) * SHADOW_PREVIEW_WIDTH) / SHADOW_PREVIEW_WIDTH;
            float localY = stripLocalY;

            float bx = BORDER / SHADOW_PREVIEW_WIDTH;
            float by = BORDER / STRIP_HEIGHT;
            if (localX < bx || localX > 1.0 - bx || localY < by || localY > 1.0 - by) {
                outColor = vec4(0.0, 0.0, 0.0, 1.0);
                return;
            }

            int modes[3] = int[3](5, 4, 6); // shadow map, shadow factor, shadow UV
            vec2 sampleUV = vec2(localX, localY);
            outColor = vec4(sampleBuffer(modes[previewIdx], sampleUV), 1.0);
            return;
        }
    }

    outColor = vec4(sampleBuffer(push.viewMode, uv), 1.0);
}
