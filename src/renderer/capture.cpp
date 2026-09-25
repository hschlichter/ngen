#include "capture.h"

#include "deletionqueue.h"
#include "framegraphdebug.h"
#include "gpuschema.h"
#include "gpuuploader.h"
#include "imguibackend.h"
#include "observationmacros.h"
#include "rhicommandbuffer.h"
#include "rhidevice.h"
#include "screenshot.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <glm/gtc/packing.hpp>
#include <limits>

namespace {

constexpr std::array<const char*, 2> staticBufferNames = {"gpuscene.meshtable", "gpuscene.materials"};
constexpr uint32_t maxPreviewSide = 1024;

} // namespace

auto captureStaticBufferNames() -> std::span<const char* const> {
    return staticBufferNames;
}

auto captureTexelBytes(RhiFormat format) -> uint32_t {
    switch (format) {
        case RhiFormat::R8_UNORM:
            return 1;
        case RhiFormat::R8G8_UNORM:
            return 2;
        case RhiFormat::R8G8B8A8_UNORM:
        case RhiFormat::R8G8B8A8_SRGB:
        case RhiFormat::B8G8R8A8_UNORM:
        case RhiFormat::B8G8R8A8_SRGB:
        case RhiFormat::D32_SFLOAT:
            return 4;
        case RhiFormat::R16G16B16A16_SFLOAT:
        case RhiFormat::R32G32_SFLOAT:
            return 8;
        case RhiFormat::R32G32B32_SFLOAT:
            return 12;
        case RhiFormat::R32G32B32A32_SFLOAT:
            return 16;
        default:
            return 0;
    }
}

auto captureChannelCount(RhiFormat format) -> uint32_t {
    switch (format) {
        case RhiFormat::R8_UNORM:
        case RhiFormat::D32_SFLOAT:
            return 1;
        case RhiFormat::R8G8_UNORM:
        case RhiFormat::R32G32_SFLOAT:
            return 2;
        case RhiFormat::R32G32B32_SFLOAT:
            return 3;
        default:
            return 4;
    }
}

auto decodeTexel(RhiFormat format, const std::byte* texel) -> glm::vec4 {
    auto u8 = [&](int i) {
        return (float) std::to_integer<uint8_t>(texel[i]) / 255.0f;
    };
    auto f32 = [&](int i) {
        float v = 0.0f;
        std::memcpy(&v, texel + (i * 4), 4);
        return v;
    };
    switch (format) {
        case RhiFormat::R8_UNORM:
            return {u8(0), 0.0f, 0.0f, 1.0f};
        case RhiFormat::R8G8_UNORM:
            return {u8(0), u8(1), 0.0f, 1.0f};
        case RhiFormat::R8G8B8A8_UNORM:
        case RhiFormat::R8G8B8A8_SRGB:
            return {u8(0), u8(1), u8(2), u8(3)};
        case RhiFormat::B8G8R8A8_UNORM:
        case RhiFormat::B8G8R8A8_SRGB:
            return {u8(2), u8(1), u8(0), u8(3)};
        case RhiFormat::D32_SFLOAT:
            return {f32(0), 0.0f, 0.0f, 1.0f};
        case RhiFormat::R16G16B16A16_SFLOAT: {
            uint64_t packed = 0;
            std::memcpy(&packed, texel, 8);
            return glm::unpackHalf4x16(packed);
        }
        case RhiFormat::R32G32_SFLOAT:
            return {f32(0), f32(1), 0.0f, 1.0f};
        case RhiFormat::R32G32B32_SFLOAT:
            return {f32(0), f32(1), f32(2), 1.0f};
        case RhiFormat::R32G32B32A32_SFLOAT:
            return {f32(0), f32(1), f32(2), f32(3)};
        default:
            return glm::vec4(0.0f);
    }
}

auto CaptureService::init(RhiDevice* rhiDevice, ImGuiBackend* imguiBackend, DeletionQueue* queue, GpuUploader* gpuUploader, uint32_t frameSlots) -> void {
    device = rhiDevice;
    imgui = imguiBackend;
    deletionQueue = queue;
    uploader = gpuUploader;
    pending.assign(frameSlots, {});
    previewSampler = device->createSampler({.magFilter = RhiFilter::Nearest, .minFilter = RhiFilter::Linear, .addressU = RhiAddressMode::ClampToEdge, .addressV = RhiAddressMode::ClampToEdge, .debugName = "capture.preview.sampler"});
}

auto CaptureService::destroy() -> void {
    for (auto& slot : pending) {
        for (auto& p : slot) {
            if (p.readback != nullptr) {
                device->destroyBuffer(p.readback);
            }
        }
        slot.clear();
    }
    for (auto& [id, k] : kept) {
        if (k.result.previewId != 0) {
            imgui->unregisterTexture(k.result.previewId);
        }
        if (k.preview != nullptr) {
            device->destroyTexture(k.preview);
        }
    }
    kept.clear();
    device->destroySampler(previewSampler);
}

auto CaptureService::setWatches(std::vector<CaptureWatch> newWatches) -> void {
    watches = std::move(newWatches);
    // Watches that went away free their previews; changed display settings re-convert the
    // last capture without capturing again.
    for (auto it = kept.begin(); it != kept.end();) {
        auto match = std::ranges::find_if(watches, [&](const CaptureWatch& w) { return w.id == it->first; });
        if (match == watches.end()) {
            releasePreview(it->second);
            capturedTrigger.erase(it->first);
            it = kept.erase(it);
            continue;
        }
        if (it->second.result.texture && match->display != it->second.display) {
            it->second.display = match->display;
            buildPreview(it->second);
            ready.push_back(it->second.result);
        }
        ++it;
    }
}

auto CaptureService::setStaticBuffer(const char* name, RhiBuffer* buffer, uint64_t size) -> void {
    staticBuffers[name] = {.buffer = buffer, .size = size};
}

auto CaptureService::wantsCapture(const CaptureWatch& watch) const -> bool {
    if (watch.live) {
        return true;
    }
    auto it = capturedTrigger.find(watch.id);
    return it == capturedTrigger.end() ? watch.trigger != 0 : it->second != watch.trigger;
}

auto CaptureService::requestsForFrame(uint32_t frameSlot, uint64_t frame) -> std::vector<FgCaptureRequest> {
    currentFrame = frame;
    requestedThisFrame.clear();
    recordedThisFrame.clear();
    std::vector<FgCaptureRequest> requests;
    for (const auto& watch : watches) {
        if (!wantsCapture(watch) || staticBuffers.contains(watch.resource)) {
            continue;
        }
        requestedThisFrame.push_back(watch);
        requests.push_back({
            .pass = watch.pass,
            .resource = watch.resource,
            .record = [this, watch, frameSlot](RhiCommandBuffer* cmd, const FgCaptureSource& source) {
                Pending p = {.watch = watch, .frame = currentFrame};
                if (source.kind == FgResourceKind::Texture) {
                    recordTexture(cmd, source, p);
                } else {
                    recordBuffer(cmd, source.buffer, source.bufferDesc.size, source.bufferState, p);
                }
                recordedThisFrame.push_back(watch.id);
                pending[frameSlot].push_back(std::move(p));
            },
        });
    }
    return requests;
}

auto CaptureService::recordTexture(RhiCommandBuffer* cmd, const FgCaptureSource& source, Pending& p) -> void {
    const auto& desc = source.textureDesc;
    p.texture = true;
    p.width = desc.width;
    p.height = desc.height;
    p.format = desc.format;
    if (p.watch.regionWidth > 0) {
        p.regionX = std::min(p.watch.regionX, desc.width - 1);
        p.regionY = std::min(p.watch.regionY, desc.height - 1);
        p.width = std::min(p.watch.regionWidth, desc.width - p.regionX);
        p.height = std::min(std::max(p.watch.regionHeight, 1u), desc.height - p.regionY);
    }
    auto texel = captureTexelBytes(desc.format);
    if (texel == 0) {
        p.error = std::string("format ") + std::to_string((int) desc.format) + " cannot be captured";
        return;
    }
    if (source.textureState == RhiTextureState::Undefined) {
        p.error = "not written yet at this point";
        return;
    }
    p.size = (uint64_t) p.width * p.height * texel;
    p.readback = device->createBuffer({.size = p.size, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "capture.readback"});
    // Leave the texture in the state the graph tracks, so later barriers stay right.
    bool transition = source.textureState != RhiTextureState::TransferSrc;
    if (transition) {
        std::array<RhiTextureBarrierDesc, 1> toSrc = {{{.texture = source.texture, .oldState = source.textureState, .newState = RhiTextureState::TransferSrc}}};
        cmd->pipelineBarrier(toSrc);
    }
    cmd->copyTextureToBuffer(source.texture, p.readback, {.x = (int32_t) p.regionX, .y = (int32_t) p.regionY, .width = p.width, .height = p.height});
    if (transition) {
        std::array<RhiTextureBarrierDesc, 1> back = {{{.texture = source.texture, .oldState = RhiTextureState::TransferSrc, .newState = source.textureState}}};
        cmd->pipelineBarrier(back);
    }
}

auto CaptureService::recordBuffer(RhiCommandBuffer* cmd, RhiBuffer* buffer, uint64_t size, RhiBufferState state, Pending& p) -> void {
    p.texture = false;
    p.size = size;
    if (size == 0) {
        p.error = "empty buffer";
        return;
    }
    p.readback = device->createBuffer({.size = size, .usage = RhiBufferUsage::TransferDst, .memory = RhiMemoryUsage::CpuToGpu, .debugName = "capture.readback"});
    bool transition = state != RhiBufferState::Undefined && state != RhiBufferState::TransferSrc;
    if (transition) {
        std::array<RhiBufferBarrierDesc, 1> toSrc = {{{.buffer = buffer, .oldState = state, .newState = RhiBufferState::TransferSrc}}};
        cmd->bufferBarrier(toSrc);
    }
    cmd->copyBuffer(buffer, p.readback, {.size = size});
    if (transition) {
        std::array<RhiBufferBarrierDesc, 1> back = {{{.buffer = buffer, .oldState = RhiBufferState::TransferSrc, .newState = state}}};
        cmd->bufferBarrier(back);
    }
}

auto CaptureService::recordStatic(RhiCommandBuffer* cmd, uint32_t frameSlot, uint64_t frame) -> void {
    // Graph captures that never ran: the resource was not alive after that pass.
    for (const auto& watch : requestedThisFrame) {
        if (std::ranges::find(recordedThisFrame, watch.id) == recordedThisFrame.end()) {
            Pending p = {.watch = watch, .frame = frame, .error = "resource not alive after this pass (or not accessed in this frame)"};
            pending[frameSlot].push_back(std::move(p));
        }
    }
    for (const auto& watch : watches) {
        auto it = staticBuffers.find(watch.resource);
        if (it == staticBuffers.end() || !wantsCapture(watch)) {
            continue;
        }
        Pending p = {.watch = watch, .frame = frame};
        if (it->second.buffer == nullptr) {
            p.error = "buffer does not exist yet";
        } else {
            // Static tables are only read after their blocking upload: no barrier needed.
            recordBuffer(cmd, it->second.buffer, it->second.size, RhiBufferState::Undefined, p);
        }
        pending[frameSlot].push_back(std::move(p));
    }
    for (const auto& watch : watches) {
        if (wantsCapture(watch)) {
            capturedTrigger[watch.id] = watch.trigger;
        }
    }
}

auto CaptureService::afterFence(uint32_t frameSlot) -> void {
    for (auto& p : pending[frameSlot]) {
        CaptureResult r = {
            .id = p.watch.id,
            .frame = p.frame,
            .pass = p.watch.pass,
            .resource = p.watch.resource,
            .texture = p.texture,
            .width = p.width,
            .height = p.height,
            .regionX = p.regionX,
            .regionY = p.regionY,
            .format = p.format,
            .byteSize = p.size,
            .error = p.error,
        };
        if (p.readback != nullptr) {
            auto bytes = std::make_shared<std::vector<std::byte>>(p.size);
            auto* mapped = device->mapBuffer(p.readback);
            std::memcpy(bytes->data(), mapped, p.size);
            device->unmapBuffer(p.readback);
            // The slot's fence has passed: nothing uses the readback any more.
            device->destroyBuffer(p.readback);
            p.readback = nullptr;
            r.bytes = std::move(bytes);
        }
        if (r.texture && r.bytes) {
            auto texel = captureTexelBytes(r.format);
            glm::vec4 lo(std::numeric_limits<float>::max());
            glm::vec4 hi(std::numeric_limits<float>::lowest());
            for (uint64_t i = 0; i < (uint64_t) r.width * r.height; i++) {
                auto v = decodeTexel(r.format, r.bytes->data() + (i * texel));
                lo = glm::min(lo, v);
                hi = glm::max(hi, v);
            }
            r.channelMin = lo;
            r.channelMax = hi;
        }
        auto& k = kept[r.id];
        releasePreview(k);
        k.result = std::move(r);
        k.display = p.watch.display;
        if (k.result.texture && k.result.bytes) {
            buildPreview(k);
        }
        OBS_EVENT("Render", "CaptureResult", "capture")
            .field("id", (int64_t) k.result.id)
            .field("frame", (int64_t) k.result.frame)
            .field("pass", k.result.pass)
            .field("resource", k.result.resource)
            .field("bytes", (int64_t) k.result.byteSize)
            .field("error", k.result.error);
        ready.push_back(k.result);
    }
    pending[frameSlot].clear();
}

auto convertCaptureForDisplay(const CaptureResult& r, const CaptureDisplay& d, uint32_t maxSide, uint32_t& outWidth, uint32_t& outHeight) -> std::vector<uint8_t> {
    std::vector<uint8_t> rgba;
    auto texel = captureTexelBytes(r.format);
    if (!r.bytes || texel == 0 || r.width == 0 || r.height == 0) {
        outWidth = 0;
        outHeight = 0;
        return rgba;
    }
    uint32_t scale = 1;
    if (maxSide > 0) {
        scale = std::max(1u, (std::max(r.width, r.height) + maxSide - 1) / maxSide);
    }
    uint32_t w = std::max(1u, r.width / scale);
    uint32_t h = std::max(1u, r.height / scale);
    // Single-channel formats (depth, R8) always show that channel as grey.
    auto channels = d.channels;
    if (captureChannelCount(r.format) == 1) {
        channels = {true, false, false, false};
    }
    float lo = d.rangeMin;
    float hi = d.rangeMax;
    if (d.autoRange) {
        lo = std::numeric_limits<float>::max();
        hi = std::numeric_limits<float>::lowest();
        for (int c = 0; c < 4; c++) {
            if (channels[c]) {
                lo = std::min(lo, r.channelMin[c]);
                hi = std::max(hi, r.channelMax[c]);
            }
        }
    }
    float span = hi > lo ? hi - lo : 1.0f;
    int enabled = (int) std::ranges::count(channels, true);
    int single = (int) (std::ranges::find(channels, true) - channels.begin());
    rgba.assign((size_t) w * h * 4, 0);
    auto norm = [&](float value) {
        return (uint8_t) std::clamp((value - lo) / span * 255.0f, 0.0f, 255.0f);
    };
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            auto src = ((uint64_t) (y * scale) * r.width) + (x * scale);
            auto v = decodeTexel(r.format, r.bytes->data() + (src * texel));
            auto* out = &rgba[((size_t) y * w + x) * 4];
            if (enabled == 1) {
                auto g = norm(v[single]);
                out[0] = g;
                out[1] = g;
                out[2] = g;
            } else {
                for (int c = 0; c < 3; c++) {
                    out[c] = channels[c] ? norm(v[c]) : 0;
                }
            }
            out[3] = 255;
        }
    }
    outWidth = w;
    outHeight = h;
    return rgba;
}

auto CaptureService::buildPreview(Kept& k) -> void {
    auto& r = k.result;
    uint32_t w = 0;
    uint32_t h = 0;
    r.previewRgba = convertCaptureForDisplay(r, k.display, maxPreviewSide, w, h);
    if (r.previewRgba.empty()) {
        return;
    }
    uploader->begin();
    k.preview = uploader->uploadTexture({.width = w, .height = h, .format = RhiFormat::R8G8B8A8_UNORM, .usage = RhiTextureUsage::Sampled, .debugName = "capture.preview"}, std::as_bytes(std::span(r.previewRgba)));
    uploader->end();
    r.previewId = imgui->registerTexture(k.preview, previewSampler);
    r.previewWidth = w;
    r.previewHeight = h;
}

auto CaptureService::releasePreview(Kept& k) -> void {
    // The main thread may still hand the old preview id to ImGui for a few frames (its
    // results lag the render thread), so the preview outlives the frames in flight by a margin.
    constexpr uint64_t previewGraceFrames = 16;
    auto frame = currentFrame + previewGraceFrames;
    if (k.result.previewId != 0) {
        deletionQueue->defer(frame, [ui = imgui, id = k.result.previewId] { ui->unregisterTexture(id); });
        k.result.previewId = 0;
    }
    if (k.preview != nullptr) {
        deletionQueue->deferTexture(frame, k.preview);
        k.preview = nullptr;
    }
}

auto CaptureService::takeResults() -> std::vector<CaptureResult> {
    return std::exchange(ready, {});
}

namespace {

auto jsonEscape(const std::string& text) -> std::string {
    std::string out;
    for (char c : text) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace

auto writeCaptureFiles(const std::string& path, const CaptureResult& r, const CaptureDisplay& display, size_t maxRows) -> bool {
    if (r.texture) {
        uint32_t w = 0;
        uint32_t h = 0;
        auto rgba = convertCaptureForDisplay(r, display, 0, w, h);
        if (!rgba.empty() && !writeScreenshotPng(path.c_str(), rgba, w, h)) {
            return false;
        }
        auto* f = std::fopen((path + ".json").c_str(), "w");
        if (f == nullptr) {
            return false;
        }
        std::fprintf(f, "{\"frame\": %llu, \"pass\": \"%s\", \"resource\": \"%s\", \"kind\": \"texture\", \"format\": \"%s\", \"width\": %u, \"height\": %u, \"bytes\": %llu, \"error\": \"%s\",\n", (unsigned long long) r.frame, jsonEscape(r.pass).c_str(), jsonEscape(r.resource).c_str(), toString(r.format), r.width, r.height, (unsigned long long) r.byteSize, jsonEscape(r.error).c_str());
        std::fprintf(f, " \"regionX\": %u, \"regionY\": %u,\n", r.regionX, r.regionY);
        // Small captures (a cursor readout) list every texel's decoded value.
        auto texel = captureTexelBytes(r.format);
        if (r.bytes && texel > 0 && (uint64_t) r.width * r.height <= 64) {
            std::fprintf(f, " \"texels\": [");
            for (uint32_t i = 0; i < r.width * r.height; i++) {
                auto v = decodeTexel(r.format, r.bytes->data() + (size_t) i * texel);
                std::fprintf(f, "%s{\"x\": %u, \"y\": %u, \"value\": [%g, %g, %g, %g]}", i > 0 ? ", " : "", r.regionX + i % r.width, r.regionY + i / r.width, v.x, v.y, v.z, v.w);
            }
            std::fprintf(f, "],\n");
        }
        std::fprintf(f, " \"channelMin\": [%g, %g, %g, %g], \"channelMax\": [%g, %g, %g, %g]}\n", r.channelMin.x, r.channelMin.y, r.channelMin.z, r.channelMin.w, r.channelMax.x, r.channelMax.y, r.channelMax.z, r.channelMax.w);
        std::fclose(f);
        return true;
    }
    auto* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) {
        return false;
    }
    const auto& schema = schemaForResource(r.resource);
    std::fprintf(f, "{\"frame\": %llu, \"pass\": \"%s\", \"resource\": \"%s\", \"kind\": \"buffer\", \"bytes\": %llu, \"error\": \"%s\",\n", (unsigned long long) r.frame, jsonEscape(r.pass).c_str(), jsonEscape(r.resource).c_str(), (unsigned long long) r.byteSize, jsonEscape(r.error).c_str());
    std::fprintf(f, " \"schema\": {\"name\": \"%s\", \"stride\": %u, \"fields\": [", schema.name.c_str(), schema.stride);
    for (size_t i = 0; i < schema.fields.size(); i++) {
        const auto& field = schema.fields[i];
        std::fprintf(f, "%s{\"name\": \"%s\", \"offset\": %u, \"type\": \"%s\"}", i > 0 ? ", " : "", field.name.c_str(), field.offset, fieldTypeName(field.type));
    }
    size_t rows = 0;
    if (r.bytes && schema.stride > 0) {
        rows = std::min(maxRows, r.bytes->size() / schema.stride);
    }
    std::fprintf(f, "]},\n \"rows\": [\n");
    for (size_t row = 0; row < rows; row++) {
        auto values = decodeRow(schema, *r.bytes, row);
        std::fprintf(f, "  {\"row\": %zu", row);
        for (size_t i = 0; i < values.size(); i++) {
            std::fprintf(f, ", \"%s\": \"%s\"", schema.fields[i].name.c_str(), values[i].c_str());
        }
        std::fprintf(f, "}%s\n", row + 1 < rows ? "," : "");
    }
    std::fprintf(f, " ]}\n");
    std::fclose(f);
    return true;
}
