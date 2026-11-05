#pragma once

#include <cstdint>

class RhiSampler;
class RhiTexture;

// Where the textures the renderer makes for inspection are shown: capture previews, the frame graph's previews, the texture inspector. A
// texture registered here (in ShaderReadOnly) gets the id the UI draws it by. The UI implements it (src/ui); without one the renderer
// makes no preview textures, and the ids stay 0.
class PreviewTextures {
public:
    PreviewTextures() = default;
    PreviewTextures(const PreviewTextures&) = delete;
    PreviewTextures& operator=(const PreviewTextures&) = delete;
    PreviewTextures(PreviewTextures&&) = default;
    PreviewTextures& operator=(PreviewTextures&&) = default;
    virtual ~PreviewTextures() = default;

    virtual auto registerTexture(RhiTexture* texture, RhiSampler* sampler) -> uint64_t = 0;
    virtual auto unregisterTexture(uint64_t id) -> void = 0;
};
