#pragma once

#include "rhitypes.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

class AssetClient;
class DeletionQueue;
class RhiDevice;

// Every pipeline the renderer draws with, created here and nowhere else (src/renderer/README.md, "Pipelines"). A pass registers each of its
// pipelines once, as a request: the pipeline description with shader ids in place of modules. It gets a stable handle and asks for the
// pipeline at record time; while the pipeline's shaders haven't arrived it is pending, and `get` returns null.
//
// Start-up pipelines are created before the first frame, from one request for all their shaders. First-use pipelines request their shaders
// the first time they are asked for and are created at `update`, a few per frame; until then the pass that wants them is skipped.
// Shaders come from the asset server through AssetClient; the render thread never waits on them after start-up.
//
// Hot reload: `reload` requests changed shaders again; `update` takes a new version when it arrives and recreates every pipeline using it.
// Handles keep their index; the old pipelines and modules go through the deletion queue. A shader that fails keeps the old module and
// pipelines, with the error in the trace and the pipeline's info.

struct PipelineHandle {
    uint32_t index = 0; // 0 = none

    explicit operator bool() const { return index != 0; }
};

enum class PipelineUse : uint8_t {
    Startup,  // created before the first frame; the view stops when one can't be
    FirstUse, // requested when first asked for; only for passes that may be skipped
};

struct ShaderRef {
    std::string id; // the shader's asset id, "shaders/gbuffer.vert"
    RhiShaderStage stage = RhiShaderStage::Vertex;
};

// RhiGraphicsPipelineDesc with shader ids for modules and owned arrays, so the registry can create it at any time. Descriptor set layouts
// stay the passes', which outlive the registry's pipelines.
struct GraphicsPipelineRequest {
    ShaderRef vertex;
    ShaderRef fragment;
    std::optional<ShaderRef> geometry;
    std::vector<RhiDescriptorSetLayout*> descriptorSetLayouts;
    RhiPushConstantRange pushConstant;
    std::vector<RhiFormat> colorFormats;
    RhiFormat depthFormat = RhiFormat::Undefined;
    uint32_t vertexStride = 0;
    std::vector<RhiVertexAttribute> vertexAttributes;
    RhiPrimitiveTopology topology = RhiPrimitiveTopology::TriangleList;
    RhiRasterState raster;
    RhiDepthState depth;
    RhiBlendState blend;
};

struct ComputePipelineRequest {
    ShaderRef shader;
    std::vector<RhiDescriptorSetLayout*> descriptorSetLayouts;
    RhiPushConstantRange pushConstant;
};

enum class PipelineState : uint8_t {
    Pending, // its shaders haven't arrived, or it waits for its turn in update's budget
    Ready,
    Failed,
};

// A pipeline as the `pipelines` record and the Render Debug window show it.
struct PipelineInfo {
    std::string name;
    PipelineUse use = PipelineUse::Startup;
    PipelineState state = PipelineState::Pending;
    bool requested = false;
    std::vector<std::pair<std::string, std::string>> shaders; // id and version ("" until it arrived)
    std::string error;
};

class PipelineRegistry {
public:
    auto init(RhiDevice* device, AssetClient* shaders, DeletionQueue* deletionQueue) -> void;

    // `name` is the pipeline's debug name, "geometry.pipeline.cullback.less".
    auto registerGraphics(std::string name, GraphicsPipelineRequest request, PipelineUse use) -> PipelineHandle;
    auto registerCompute(std::string name, ComputePipelineRequest request, PipelineUse use) -> PipelineHandle;

    // The pipeline, or null while it is pending or failed. A first-use pipeline's first call requests its shaders.
    auto get(PipelineHandle handle) -> RhiPipeline*;
    // Whether every one of these is ready; the shaders of those that aren't are requested together, in one request.
    auto ready(std::span<const PipelineHandle> handles) -> bool;

    // The shader ids of every start-up pipeline, each once, sorted.
    auto startupShaders() const -> std::vector<std::string>;
    // Sends one request for the start-up pipelines' shaders.
    auto requestStartup() -> void;
    // Waits for the start-up shaders and creates every start-up pipeline. False when one can't be, with the reason in the trace.
    auto createStartup() -> bool;

    // Requests these shaders again, the ones a module was created for; the asset server said their sources changed. On the render thread.
    auto reload(std::span<const std::string> shaderIds) -> void;

    // Once per frame on the render thread, before the frame graph is built: takes reloaded shaders that have arrived, recreating the
    // pipelines using them (what they replace is retired at `frame`), and creates pending first-use pipelines whose shaders have arrived,
    // at most `budget` of them.
    auto update(uint64_t frame, uint32_t budget = 4) -> void;

    // Thread-safe: what each pipeline is, for the `pipelines` record and the Render Debug window.
    auto infos() const -> std::vector<PipelineInfo>;

    auto destroy() -> void;

private:
    struct Entry {
        std::string name;
        PipelineUse use = PipelineUse::Startup;
        std::optional<GraphicsPipelineRequest> graphics;
        std::optional<ComputePipelineRequest> compute;
        RhiPipeline* pipeline = nullptr;
        PipelineState state = PipelineState::Pending;
        bool requested = false;
        bool reportedPending = false;
        std::string error;
    };
    struct Module {
        RhiShaderModule* module = nullptr;
        std::string version;
    };

    static auto shadersOf(const Entry& entry) -> std::vector<ShaderRef>;
    // The module for a shader that has arrived; null while it hasn't, or when it can't be created (`error` set).
    auto module(const ShaderRef& shader, std::string& error) -> RhiShaderModule*;
    auto arrived(const Entry& entry) const -> bool;
    auto failedShader(const Entry& entry) const -> std::string;
    // The entry's pipeline from the modules there are now; null with `error` set when it can't be made.
    auto build(const Entry& entry, std::string& error) -> RhiPipeline*;
    auto create(Entry& entry) -> void;
    // A reloaded shader's request was answered: swaps in its new module and recreates the pipelines using it.
    auto finishReload(const std::string& id, uint64_t frame) -> void;
    // Requests the shaders of these entries that aren't requested yet, in one request.
    auto request(std::span<Entry* const> wanted) -> void;
    auto reportPending(Entry& entry) -> void;

    RhiDevice* device = nullptr;
    AssetClient* source = nullptr;
    DeletionQueue* retired = nullptr;
    std::set<std::string> reloading; // shader ids requested again, not answered yet
    std::vector<Entry> entries;      // by handle index - 1
    std::unordered_map<std::string, Module> modules;
    std::set<std::string> requestedShaders;
#ifndef NDEBUG
    // What `get` returned for each handle since the last update: a frame's recording must see one pipeline per handle.
    std::unordered_map<uint32_t, RhiPipeline*> returnedThisFrame;
#endif
    mutable std::mutex infoMutex; // guards entries' state, read by infos() from other threads
};
