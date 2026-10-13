#pragma once

#include <cstdint>

struct PrimHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(PrimHandle a, PrimHandle b) = default;
};

struct LayerHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(LayerHandle a, LayerHandle b) = default;
};

struct MeshHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(MeshHandle a, MeshHandle b) = default;
};

struct MaterialHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(MaterialHandle a, MaterialHandle b) = default;
};

struct TextureHandle {
    uint32_t index = 0;

    explicit operator bool() const { return index != 0; }
    friend bool operator==(TextureHandle a, TextureHandle b) = default;
};
