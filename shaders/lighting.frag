#version 450

layout(set = 0, binding = 0) uniform sampler2D gbufferAlbedo;
layout(set = 0, binding = 1) uniform sampler2D gbufferNormal;
layout(set = 0, binding = 2) uniform LightUBO {
    vec4 lightDirection;
    vec4 lightColor;
    vec4 depthParams;
    vec4 shadowTint;
    mat4 invViewProj;
    mat4 cascadeViewProj[4];
    vec4 cascadeRects[4];
    vec4 cascadeSplits;
    vec4 cascadeTexelDepth;
    vec4 cascadeTexelWorld;
    vec4 cascadeParams; // x = count, y = pcf, z = atlas size
} light;
layout(set = 0, binding = 3) uniform sampler2D gbufferDepth;
layout(set = 0, binding = 4) uniform sampler2D shadowMap;          // atlas, plain reads
layout(set = 0, binding = 5) uniform sampler2DShadow shadowMapCmp; // atlas, hardware compare

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

// Cascade for a view-space depth: the first whose split end is past it.
int cascadeFor(float viewDepth) {
    int count = int(light.cascadeParams.x);
    for (int c = 0; c < count - 1; c++) {
        if (viewDepth <= light.cascadeSplits[c]) {
            return c;
        }
    }
    return max(count - 1, 0);
}

// Returns 1.0 when lit, 0.0 when shadowed. Caller blends between the light's radiance
// and the UsdLuxShadowAPI shadowColor tint based on the result.
//
// Bias in texels: the receiver is pushed along its normal by a fraction of a shadow
// texel (normal offset), and the compared depth is moved toward the light by one to
// three texels of depth, more at grazing angles. Both scale with the cascade's texel
// size, so the near cascade gets a tiny bias and the far one enough to cover its
// coarser texels. Hardware compare with linear filtering gives bilinear PCF at one
// fetch; the 3x3 loop widens it. pcf off reads the depth and compares once.
float sampleShadow(vec3 worldPos, vec3 normal, float viewDepth) {
    int c = cascadeFor(viewDepth);
    vec3 N = normalize(normal);
    vec3 L = normalize(light.lightDirection.xyz);
    float nDotL = dot(N, L);
    if (nDotL <= 0.0) {
        return 0.0; // facing away from the light: shadowed, and the map compare would only show acne
    }
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    float texelWorld = light.cascadeTexelWorld[c];
    vec3 offsetPos = worldPos + N * texelWorld * slope * 1.5;

    vec4 lightClip = light.cascadeViewProj[c] * vec4(offsetPos, 1.0);
    vec3 lightNdc = lightClip.xyz / lightClip.w;
    vec2 tileUV = lightNdc.xy * 0.5 + 0.5;
    if (any(lessThan(tileUV, vec2(0.0))) || any(greaterThan(tileUV, vec2(1.0))) || lightNdc.z < 0.0 || lightNdc.z > 1.0) {
        return 1.0; // outside the cascade: lit
    }
    vec4 rect = light.cascadeRects[c];
    float atlasTexel = 1.0 / light.cascadeParams.z;
    // Stay half a texel inside the tile so filtering never reads the neighbour cascade.
    vec2 uvMin = rect.xy + vec2(atlasTexel * 0.5);
    vec2 uvMax = rect.xy + rect.zw - vec2(atlasTexel * 0.5);
    vec2 uv = clamp(rect.xy + tileUV * rect.zw, uvMin, uvMax);

    float bias = light.cascadeTexelDepth[c] * (1.0 + 2.0 * slope);
    float ref = lightNdc.z - bias;
    if (light.cascadeParams.y < 0.5) {
        float sampled = texture(shadowMap, uv).r;
        return ref > sampled ? 0.0 : 1.0;
    }
    float lit = 0.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 tap = clamp(uv + vec2(x, y) * atlasTexel, uvMin, uvMax);
            lit += texture(shadowMapCmp, vec3(tap, ref));
        }
    }
    return lit / 9.0;
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
    vec4 albedoSample = texture(gbufferAlbedo, uv);
    vec3 albedo = albedoSample.rgb;
    if (mode == 1) return albedo;
    if (mode == 8) {
        // Mip level from the gbuffer alpha: 0 red, 1 orange, 2 yellow, 3 green, 4 cyan,
        // 5 blue, 6 magenta, 7 and up white, blended between levels.
        const vec3 ramp[8] = vec3[](vec3(1.0, 0.0, 0.0), vec3(1.0, 0.5, 0.0), vec3(1.0, 1.0, 0.0), vec3(0.0, 1.0, 0.0), vec3(0.0, 1.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 1.0), vec3(1.0, 1.0, 1.0));
        float lod = albedoSample.a * 16.0;
        int i = int(clamp(floor(lod), 0.0, 7.0));
        int j = min(i + 1, 7);
        return mix(ramp[i], ramp[j], clamp(lod - float(i), 0.0, 1.0));
    }
    vec3 normal = texture(gbufferNormal, uv).rgb * 2.0 - 1.0;
    if (mode == 2) return normalize(normal) * 0.5 + 0.5;

    float depth = texture(gbufferDepth, uv).r;
    vec3 worldPos = reconstructWorld(uv, depth);
    float viewDepth = linearizeDepth(depth);
    float shadow = sampleShadow(worldPos, normal, viewDepth);
    // shadow factor per channel: full light where lit, shadowTint where shadowed
    vec3 shadowFactor = mix(light.shadowTint.rgb, vec3(1.0), shadow);
    if (mode == 4) return vec3(shadow);
    if (mode == 6) {
        // Visualize where this fragment lands in its cascade's UV space.
        // Red = shadowUV.x, Green = shadowUV.y, Blue = 1 if inside frustum else 0.
        vec4 lightClip = light.cascadeViewProj[cascadeFor(viewDepth)] * vec4(worldPos, 1.0);
        vec3 lightNdc = lightClip.xyz / lightClip.w;
        vec2 sUV = lightNdc.xy * 0.5 + 0.5;
        float inside = (all(greaterThanEqual(sUV, vec2(0.0))) && all(lessThanEqual(sUV, vec2(1.0)))) ? 1.0 : 0.0;
        return vec3(sUV, inside);
    }
    if (mode == 9) {
        // Cascade index: red, green, blue, yellow from near to far, dimmed by the lighting.
        const vec3 cascadeColors[4] = vec3[](vec3(1.0, 0.25, 0.25), vec3(0.25, 1.0, 0.25), vec3(0.25, 0.4, 1.0), vec3(1.0, 1.0, 0.25));
        float diffuse = max(dot(normalize(normal), normalize(light.lightDirection.xyz)), 0.0);
        return cascadeColors[cascadeFor(viewDepth)] * (0.35 + 0.65 * diffuse * shadow);
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
