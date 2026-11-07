// The occlusion test against the depth pyramid (DepthPyramid, hzbbuild.comp), shared by instancecull.comp (instances) and meshlet.task
// (meshlets). The includer declares `params` (with cameraViewProj, hzbWidth and hzbHeight: DrawLists::CullParams) and `hzb`, the pyramid.
//
// A box is hidden when its nearest point is farther than the farthest depth drawn over its whole screen rectangle (reversed-Z: nearer is
// larger). The rectangle picks the level where it spans at most two texels a side; four texels cover it there. A box reaching the camera
// plane, or with nothing to compare against, is never hidden.

bool boxOccluded(vec3 boundsMin, vec3 boundsMax) {
    vec2 low = vec2(1e30);
    vec2 high = vec2(-1e30);
    float nearest = 0.0;
    for (uint i = 0; i < 8u; i++) {
        vec3 corner = vec3((i & 1u) != 0u ? boundsMax.x : boundsMin.x, (i & 2u) != 0u ? boundsMax.y : boundsMin.y, (i & 4u) != 0u ? boundsMax.z : boundsMin.z);
        vec4 clip = params.cameraViewProj * vec4(corner, 1.0);
        if (clip.w <= 1e-5) {
            return false;
        }
        vec3 ndc = clip.xyz / clip.w;
        low = min(low, ndc.xy);
        high = max(high, ndc.xy);
        nearest = max(nearest, ndc.z);
    }
    vec2 uvLow = clamp(low * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 uvHigh = clamp(high * 0.5 + 0.5, vec2(0.0), vec2(1.0));
    vec2 base = vec2(params.hzbWidth, params.hzbHeight);
    vec2 pixels = (uvHigh - uvLow) * base;
    int mips = int(floor(log2(max(base.x, base.y)))) + 1;
    int level = clamp(int(ceil(log2(max(max(pixels.x, pixels.y), 1.0)))), 0, mips - 1);
    ivec2 levelSize = max(ivec2(base) >> level, ivec2(1));
    ivec2 a = clamp(ivec2(uvLow * vec2(levelSize)), ivec2(0), levelSize - 1);
    ivec2 b = clamp(ivec2(uvHigh * vec2(levelSize)), ivec2(0), levelSize - 1);
    float farthest = min(min(texelFetch(hzb, a, level).r, texelFetch(hzb, ivec2(b.x, a.y), level).r), min(texelFetch(hzb, ivec2(a.x, b.y), level).r, texelFetch(hzb, b, level).r));
    return nearest < farthest;
}
