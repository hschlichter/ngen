// Render-LOD fades (src/renderer/README.md, "Render LOD"): while a group draws two levels inside an authored blend band, each pixel keeps
// one of them by a 4x4 ordered (Bayer) pattern in screen space, the two levels' tests complementary, so every pixel is covered once. The
// cull pass draws a fading group's instances in the cutout buckets; their mesh shaders read the group's state (lodCoverage, with the
// includer's `lodStates`, lodselect.comp's) and passes the coverage flat; the fragment stage, which defines LOD_FADE_FRAGMENT before
// including this, discards what the pattern leaves out (lodFadedAway).

const uint lodNoLevel = 0xFFu;
// The coverage of an instance that isn't fading: every pixel kept.
const float lodCoverAll = 2.0;

#ifdef LOD_FADE_FRAGMENT
// Whether the fade leaves this pixel to the other level.
bool lodFadedAway(float coverage) {
    if (coverage > 1.5) {
        return false;
    }
    const float bayer[16] = float[16](0.0, 8.0, 2.0, 10.0, 12.0, 4.0, 14.0, 6.0, 3.0, 11.0, 1.0, 9.0, 15.0, 7.0, 13.0, 5.0);
    uvec2 cell = uvec2(gl_FragCoord.xy) & 3u;
    float threshold = (bayer[cell.y * 4u + cell.x] + 0.5) / 16.0;
    return coverage >= 0.0 ? threshold >= coverage : threshold < -coverage;
}
#else
// An instance's coverage: lodCoverAll when its group isn't fading; for the level fading in, the fade's share (keeps the pattern's values
// below it); for the level drawn, minus the share (keeps the values at or above it).
float lodCoverage(uint lodGroup, uint lodLevel) {
    if (lodGroup == 0u || lodLevel == 0u || lodGroup > uint(lodStates.data.length())) {
        return lodCoverAll;
    }
    uvec4 state = lodStates.data[lodGroup - 1u];
    uint fadeLevel = (state.y >> 16) & 0xFFu;
    if (fadeLevel == lodNoLevel) {
        return lodCoverAll;
    }
    float share = uintBitsToFloat(state.z);
    return lodLevel - 1u == fadeLevel ? share : -share;
}

// The LOD level view's tag (the G-buffer normal target's alpha): the drawn level + 1 (0 under no LOD root), plus, for a render-LOD
// group's instance, 16 at a forced level, 32 drawing another level than it wants (the wanted one not resident), 64 while it flashes for a
// pop. A 16-bit float holds it exactly; lighting.frag reads it back.
const uint lodTagForced = 16u;
const uint lodTagFallback = 32u;
const uint lodTagFlash = 64u;
// The tag's bits from 128 up: how many LOD roots the draw is under (gbuffer.mesh adds it), at most 15 so the tag stays under 2048, which a
// 16-bit float holds exactly.
const uint lodTagDepth = 128u;
uint lodTag(uint lodGroup, uint lodLevel) {
    if (lodGroup == 0u || lodLevel == 0u || lodGroup > uint(lodStates.data.length())) {
        return lodLevel;
    }
    uint levels = lodStates.data[lodGroup - 1u].y;
    uint tag = lodLevel;
    if (((levels >> 24) & 1u) != 0u) {
        tag += lodTagForced;
    }
    if ((levels & 0xFFu) != ((levels >> 8) & 0xFFu)) {
        tag += lodTagFallback;
    }
    if ((levels >> 25) != 0u) {
        tag += lodTagFlash;
    }
    return tag;
}
#endif
