#pragma once

#include "framegraphresource.h"
#include "rhitypes.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

struct FgResourceAccessDebug {
    uint32_t resourceIndex = UINT32_MAX;
    FgAccessFlags access = FgAccessFlags::None;
};

struct FgPassDebug {
    std::string name;
    uint32_t executionIndex = UINT32_MAX;
    bool culled = false;
    bool hasSideEffects = false;
    std::vector<FgResourceAccessDebug> reads;
    std::vector<FgResourceAccessDebug> writes;
};

struct FgResourceDebug {
    uint32_t index = UINT32_MAX;
    uint32_t width = 0;
    uint32_t height = 0;
    const char* formatName = "";
    const char* usageName = "";
    bool external = false;
    uint32_t firstUseOrder = UINT32_MAX;
    uint32_t lastUseOrder = UINT32_MAX;
    uint32_t producerPass = UINT32_MAX;
    std::vector<uint32_t> consumerPasses;
    std::string name;
    std::string label;
    uint64_t previewTextureId = 0;
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
};

struct FrameGraphDebugSnapshot {
    uint64_t frameIndex = 0;
    std::vector<FgPassDebug> passes;
    std::vector<uint32_t> executionOrder;
    std::vector<FgResourceDebug> resources;
};

inline auto toString(RhiFormat f) -> const char* {
    switch (f) {
        case RhiFormat::Undefined:
            return "Undefined";
        case RhiFormat::R32G32_SFLOAT:
            return "R32G32_SFLOAT";
        case RhiFormat::R32G32B32_SFLOAT:
            return "R32G32B32_SFLOAT";
        case RhiFormat::R8G8B8A8_SRGB:
            return "R8G8B8A8_SRGB";
        case RhiFormat::R8G8B8A8_UNORM:
            return "R8G8B8A8_UNORM";
        case RhiFormat::B8G8R8A8_SRGB:
            return "B8G8R8A8_SRGB";
        case RhiFormat::B8G8R8A8_UNORM:
            return "B8G8R8A8_UNORM";
        case RhiFormat::R32G32B32A32_SFLOAT:
            return "R32G32B32A32_SFLOAT";
        case RhiFormat::D32_SFLOAT:
            return "D32_SFLOAT";
    }
    return "?";
}

inline auto toString(RhiTextureUsage u) -> const char* {
    switch (std::to_underlying(u)) {
        case 0:
            return "None";
        case 1u << 0:
            return "Sampled";
        case 1u << 1:
            return "ColorAttachment";
        case 1u << 2:
            return "DepthAttachment";
        case 1u << 3:
            return "Storage";
        case 1u << 4:
            return "TransferSrc";
        case 1u << 5:
            return "TransferDst";
        default:
            return "Combined";
    }
}

inline auto toString(FgAccessFlags a) -> const char* {
    switch (std::to_underlying(a)) {
        case 0:
            return "None";
        case 1u << 0:
            return "ColorAttachment";
        case 1u << 1:
            return "DepthAttachment";
        case 1u << 2:
            return "ShaderRead";
        case 1u << 3:
            return "TransferSrc";
        case 1u << 4:
            return "TransferDst";
        case 1u << 5:
            return "Present";
        default:
            return "Combined";
    }
}
