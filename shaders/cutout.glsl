// Cutout opacity, shared by every pipeline that draws cutout materials and by the debug views (src/renderer/README.md, "Cutout
// materials"). The includer declares the material table (`materials`, GpuMaterial) and the texture array (`textures`).
//
// A material is cutout when opacityThreshold > 0. Its opacity is a channel of the opacity texture: opacityInfo bits 0-1 the channel,
// bit 2 set when the texture is authored raw, so the sampler's sRGB decode is undone (alpha isn't decoded). Opacity texture 0 means a
// constant opacity below the threshold: the surface is cut away entirely.

float cutoutEncodeSrgb(float c) {
    c = clamp(c, 0.0, 1.0);
    return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

// Whether a fragment of this material at uv is cut away.
bool cutAway(uint material, vec2 uv) {
    float threshold = materials.data[material].opacityThreshold;
    if (threshold <= 0.0) {
        return false;
    }
    uint slot = materials.data[material].opacityTexture;
    if (slot == 0u) {
        return true;
    }
    uint info = materials.data[material].opacityInfo;
    uint channel = info & 3u;
    float value = texture(textures[slot], uv)[channel];
    if ((info & 4u) != 0u && channel != 3u) {
        value = cutoutEncodeSrgb(value);
    }
    return value < threshold;
}
