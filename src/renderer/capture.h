#pragma once

#include "framegraph.h"
#include "rhitypes.h"

#include <array>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class DeletionQueue;
class GpuUploader;
class ImGuiBackend;
class RhiDevice;

// How a captured texture is turned into the preview image. Values are normalized from
// [rangeMin, rangeMax] (or the captured min/max when autoRange) and shown as grey when one
// channel is enabled, as colour otherwise.
struct CaptureDisplay {
    std::array<bool, 4> channels = {true, true, true, false};
    bool autoRange = true;
    float rangeMin = 0.0f;
    float rangeMax = 1.0f;

    auto operator==(const CaptureDisplay&) const -> bool = default;
};

// A capture the main thread wants: resource `resource` right after pass `pass` ("" = after
// the last pass, which is also where static scene buffers are captured). Captured when
// `trigger` changes or every frame while `live`.
struct CaptureWatch {
    uint32_t id = 0;
    std::string pass;
    std::string resource;
    bool live = false;
    uint32_t trigger = 0;
    CaptureDisplay display;
    // Textures only: a sub-rectangle to copy (width 0 = the whole texture), e.g. one texel
    // under the cursor. Clamped to the texture.
    uint32_t regionX = 0;
    uint32_t regionY = 0;
    uint32_t regionWidth = 0;
    uint32_t regionHeight = 0;

    auto operator==(const CaptureWatch&) const -> bool = default;
};

// What came back for a watch. `bytes` is the raw copy (tightly packed texels of mip 0, or
// the whole buffer); the preview is an ImGui texture of the display conversion.
struct CaptureResult {
    uint32_t id = 0;
    uint64_t frame = 0;
    std::string pass;
    std::string resource;
    bool texture = false;
    uint32_t width = 0; // of the copy: the region when one was asked for
    uint32_t height = 0;
    uint32_t regionX = 0; // where the copy starts in the texture
    uint32_t regionY = 0;
    RhiFormat format = RhiFormat::Undefined;
    uint64_t byteSize = 0;
    std::shared_ptr<const std::vector<std::byte>> bytes;
    glm::vec4 channelMin = glm::vec4(0.0f);
    glm::vec4 channelMax = glm::vec4(0.0f);
    uint64_t previewId = 0;
    uint32_t previewWidth = 0;
    uint32_t previewHeight = 0;
    std::vector<uint8_t> previewRgba; // the preview pixels, for PNG dumps
    std::string error;
};

// Bytes per texel of a capturable format; 0 when the format cannot be captured.
auto captureTexelBytes(RhiFormat format) -> uint32_t;
// Channels a capturable format stores (1 for depth and R8).
auto captureChannelCount(RhiFormat format) -> uint32_t;
// One texel as floats (depth in x, missing channels 0, alpha 1 where absent).
auto decodeTexel(RhiFormat format, const std::byte* texel) -> glm::vec4;
// A captured texture converted for display as RGBA8, scaled down to at most maxSide texels
// on the longer side (0 keeps full size). Returns the pixels and writes the size.
auto convertCaptureForDisplay(const CaptureResult& result, const CaptureDisplay& display, uint32_t maxSide, uint32_t& outWidth, uint32_t& outHeight) -> std::vector<uint8_t>;
// Writes a capture as files: buffers as JSON rows decoded with their schema (at most
// maxRows), textures as a PNG of the display conversion plus `<path>.json` with format,
// size and channel ranges. Returns false when a file cannot be written.
auto writeCaptureFiles(const std::string& path, const CaptureResult& result, const CaptureDisplay& display, size_t maxRows) -> bool;
// The scene buffers outside the frame graph that can be captured at the end of the frame.
auto captureStaticBufferNames() -> std::span<const char* const>;

// Records copies of frame-graph resources at capture points and reads them back after the
// frame slot's fence (docs in src/renderer/README.md, "Debugging and introspection").
class CaptureService {
public:
    auto init(RhiDevice* device, ImGuiBackend* imgui, DeletionQueue* deletionQueue, GpuUploader* uploader, uint32_t frameSlots) -> void;
    auto destroy() -> void;

    auto setWatches(std::vector<CaptureWatch> watches) -> void;
    auto setStaticBuffer(const char* name, RhiBuffer* buffer, uint64_t size) -> void;

    // Capture points for this frame; the frame graph runs them. Call once per frame, after
    // the graph is built and before it executes.
    auto requestsForFrame(uint32_t frameSlot, uint64_t frame) -> std::vector<FgCaptureRequest>;
    // Copies of static buffers watched this frame; after the graph executed.
    auto recordStatic(RhiCommandBuffer* cmd, uint32_t frameSlot, uint64_t frame) -> void;
    // Once the slot's fence has passed: parse its copies into results.
    auto afterFence(uint32_t frameSlot) -> void;
    auto takeResults() -> std::vector<CaptureResult>;

private:
    struct Pending {
        CaptureWatch watch;
        uint64_t frame = 0;
        bool texture = false;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t regionX = 0;
        uint32_t regionY = 0;
        RhiFormat format = RhiFormat::Undefined;
        uint64_t size = 0;
        RhiBuffer* readback = nullptr;
        std::string error;
    };
    struct Kept {
        CaptureResult result;
        CaptureDisplay display;
        RhiTexture* preview = nullptr;
    };

    auto wantsCapture(const CaptureWatch& watch) const -> bool;
    auto recordTexture(RhiCommandBuffer* cmd, const FgCaptureSource& source, Pending& pending) -> void;
    auto recordBuffer(RhiCommandBuffer* cmd, RhiBuffer* buffer, uint64_t size, RhiBufferState state, Pending& pending) -> void;
    auto buildPreview(Kept& kept) -> void;
    auto releasePreview(Kept& kept) -> void;

    RhiDevice* device = nullptr;
    ImGuiBackend* imgui = nullptr;
    DeletionQueue* deletionQueue = nullptr;
    GpuUploader* uploader = nullptr;
    RhiSampler* previewSampler = nullptr;
    uint64_t currentFrame = 0;

    std::vector<CaptureWatch> watches;
    std::unordered_map<uint32_t, uint32_t> capturedTrigger; // watch id -> trigger last captured
    struct StaticBuffer {
        RhiBuffer* buffer = nullptr;
        uint64_t size = 0;
    };
    std::unordered_map<std::string, StaticBuffer> staticBuffers;
    std::vector<std::vector<Pending>> pending;    // per frame slot
    std::vector<CaptureWatch> requestedThisFrame; // graph captures asked for; unmatched ones become errors
    std::vector<uint32_t> recordedThisFrame;
    std::unordered_map<uint32_t, Kept> kept; // last result per watch
    std::vector<CaptureResult> ready;
};
