#include "pipelineregistry.h"

#include "assetclient.h"
#include "deletionqueue.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <cassert>
#include <format>

auto PipelineRegistry::init(RhiDevice* rhiDevice, AssetClient* shaders, DeletionQueue* deletionQueue) -> void {
    device = rhiDevice;
    source = shaders;
    retired = deletionQueue;
}

auto PipelineRegistry::registerGraphics(std::string name, GraphicsPipelineRequest request, PipelineUse use) -> PipelineHandle {
    Entry entry;
    entry.name = std::move(name);
    entry.use = use;
    entry.graphics = std::move(request);
    std::lock_guard lock(infoMutex);
    entries.push_back(std::move(entry));
    return {.index = (uint32_t) entries.size()};
}

auto PipelineRegistry::registerCompute(std::string name, ComputePipelineRequest request, PipelineUse use) -> PipelineHandle {
    Entry entry;
    entry.name = std::move(name);
    entry.use = use;
    entry.compute = std::move(request);
    std::lock_guard lock(infoMutex);
    entries.push_back(std::move(entry));
    return {.index = (uint32_t) entries.size()};
}

auto PipelineRegistry::shadersOf(const Entry& entry) -> std::vector<ShaderRef> {
    std::vector<ShaderRef> out;
    if (entry.graphics) {
        out.push_back(entry.graphics->vertex);
        out.push_back(entry.graphics->fragment);
        if (entry.graphics->geometry) {
            out.push_back(*entry.graphics->geometry);
        }
    }
    if (entry.compute) {
        out.push_back(entry.compute->shader);
    }
    return out;
}

auto PipelineRegistry::arrived(const Entry& entry) const -> bool {
    return std::ranges::all_of(shadersOf(entry), [this](const ShaderRef& shader) { return modules.contains(shader.id) || source->find(shader.id) != nullptr; });
}

// The first of the entry's shaders the asset server couldn't pack, with its errors; empty when none failed.
auto PipelineRegistry::failedShader(const Entry& entry) const -> std::string {
    for (const auto& shader : shadersOf(entry)) {
        if (modules.contains(shader.id) || source->find(shader.id) != nullptr) {
            continue;
        }
        auto errors = source->errors(shader.id);
        if (errors.empty()) {
            continue;
        }
        std::string text = std::format("{} was not packed", shader.id);
        for (const auto& line : errors) {
            text += "\n  " + line;
        }
        return text;
    }
    return {};
}

auto PipelineRegistry::module(const ShaderRef& shader, std::string& error) -> RhiShaderModule* {
    if (auto it = modules.find(shader.id); it != modules.end()) {
        return it->second.module;
    }
    auto asset = source->find(shader.id);
    if (asset == nullptr) {
        return nullptr;
    }
    RhiShaderDesc desc = {
        .stage = shader.stage,
        .code = asset->bytes,
        .debugName = shader.id.c_str(),
    };
    auto* created = device->createShaderModule(desc);
    if (created == nullptr) {
        error = std::format("cannot create the shader module {}", shader.id);
        return nullptr;
    }
    {
        std::lock_guard lock(infoMutex);
        modules[shader.id] = {.module = created, .version = asset->version};
    }
    TRACE_EVENT("Render", "ShaderLoaded", shader.id).text(std::format("loaded shader {} ({})", shader.id, asset->version)).field("version", asset->version);
    return created;
}

auto PipelineRegistry::build(const Entry& entry, std::string& error) -> RhiPipeline* {
    RhiPipeline* pipeline = nullptr;
    if (entry.graphics) {
        const auto& request = *entry.graphics;
        auto* vertex = module(request.vertex, error);
        auto* fragment = module(request.fragment, error);
        RhiShaderModule* geometry = nullptr;
        if (request.geometry) {
            geometry = module(*request.geometry, error);
        }
        if (vertex != nullptr && fragment != nullptr && (!request.geometry || geometry != nullptr)) {
            RhiGraphicsPipelineDesc desc = {
                .vertexShader = vertex,
                .fragmentShader = fragment,
                .geometryShader = geometry,
                .descriptorSetLayouts = request.descriptorSetLayouts,
                .pushConstant = request.pushConstant,
                .colorFormats = request.colorFormats,
                .depthFormat = request.depthFormat,
                .vertexStride = request.vertexStride,
                .vertexAttributes = request.vertexAttributes,
                .topology = request.topology,
                .raster = request.raster,
                .depth = request.depth,
                .blend = request.blend,
            };
            pipeline = device->createGraphicsPipeline(desc);
        }
    } else if (entry.compute) {
        const auto& request = *entry.compute;
        if (auto* shader = module(request.shader, error)) {
            RhiComputePipelineDesc desc = {
                .shader = shader,
                .descriptorSetLayouts = request.descriptorSetLayouts,
                .pushConstant = request.pushConstant,
            };
            pipeline = device->createComputePipeline(desc);
        }
    }
    if (pipeline == nullptr && error.empty()) {
        error = "the device couldn't create the pipeline";
    }
    return pipeline;
}

auto PipelineRegistry::create(Entry& entry) -> void {
    std::string error;
    auto* pipeline = build(entry, error);
    std::lock_guard lock(infoMutex);
    if (pipeline == nullptr) {
        entry.state = PipelineState::Failed;
        entry.error = error;
        TRACE_ERROR("Render", "PipelineCreateFailed", entry.name).text(std::format("pipeline {}: {}", entry.name, entry.error)).field("reason", entry.error);
        return;
    }
    device->setDebugName(pipeline, entry.name.c_str());
    entry.pipeline = pipeline;
    entry.state = PipelineState::Ready;
    TRACE_EVENT("Render", "PipelineCreated", entry.name).text(std::format("pipeline {} created", entry.name));
}

auto PipelineRegistry::request(std::span<Entry* const> wanted) -> void {
    std::vector<std::string> ids;
    for (Entry* entry : wanted) {
        for (const auto& shader : shadersOf(*entry)) {
            if (requestedShaders.insert(shader.id).second) {
                ids.push_back(shader.id);
            }
        }
        std::lock_guard lock(infoMutex);
        entry->requested = true;
    }
    if (ids.empty()) {
        return;
    }
    source->request(ids);
    std::string list;
    for (const auto& id : ids) {
        list += list.empty() ? id : ", " + id;
    }
    TRACE_EVENT("Render", "ShadersRequested", "pipelines").text(std::format("{} shaders requested: {}", ids.size(), list)).field("count", (int64_t) ids.size());
}

auto PipelineRegistry::reportPending(Entry& entry) -> void {
    if (entry.reportedPending) {
        return;
    }
    entry.reportedPending = true;
    TRACE_EVENT("Render", "PipelinePending", entry.name).text(std::format("pipeline {} is pending: its shaders haven't arrived", entry.name));
}

auto PipelineRegistry::get(PipelineHandle handle) -> RhiPipeline* {
    if (!handle || handle.index > entries.size()) {
        return nullptr;
    }
    auto& entry = entries[handle.index - 1];
#ifndef NDEBUG
    if (auto [it, first] = returnedThisFrame.try_emplace(handle.index, entry.pipeline); !first) {
        assert(it->second == entry.pipeline && "a pipeline changed during a frame's recording");
    }
#endif
    if (entry.pipeline != nullptr || entry.state == PipelineState::Failed) {
        return entry.pipeline;
    }
    if (!entry.requested) {
        Entry* wanted[] = {&entry};
        request(wanted);
    }
    reportPending(entry);
    return nullptr;
}

auto PipelineRegistry::ready(std::span<const PipelineHandle> handles) -> bool {
    std::vector<Entry*> unrequested;
    bool all = true;
    for (auto handle : handles) {
        if (!handle || handle.index > entries.size()) {
            all = false;
            continue;
        }
        auto& entry = entries[handle.index - 1];
        if (entry.pipeline != nullptr) {
            continue;
        }
        all = false;
        if (entry.state != PipelineState::Failed && !entry.requested) {
            unrequested.push_back(&entry);
        }
    }
    if (!unrequested.empty()) {
        request(unrequested);
        for (Entry* entry : unrequested) {
            reportPending(*entry);
        }
    }
    return all;
}

auto PipelineRegistry::startupShaders() const -> std::vector<std::string> {
    std::set<std::string> ids;
    for (const auto& entry : entries) {
        if (entry.use != PipelineUse::Startup) {
            continue;
        }
        for (const auto& shader : shadersOf(entry)) {
            ids.insert(shader.id);
        }
    }
    return {ids.begin(), ids.end()};
}

auto PipelineRegistry::requestStartup() -> void {
    std::vector<Entry*> startup;
    for (auto& entry : entries) {
        if (entry.use == PipelineUse::Startup) {
            startup.push_back(&entry);
        }
    }
    request(startup);
}

auto PipelineRegistry::createStartup() -> bool {
    auto ids = startupShaders();
    source->wait(ids);
    bool ok = true;
    for (auto& entry : entries) {
        if (entry.use != PipelineUse::Startup || entry.pipeline != nullptr) {
            continue;
        }
        if (auto failed = failedShader(entry); !failed.empty()) {
            TRACE_ERROR("Render", "ShaderPackFailed", entry.name).text(failed);
            std::lock_guard lock(infoMutex);
            entry.state = PipelineState::Failed;
            entry.error = failed;
            TRACE_ERROR("Render", "PipelineCreateFailed", entry.name).text(std::format("pipeline {}: {}", entry.name, failed)).field("reason", failed);
            ok = false;
            continue;
        }
        create(entry);
        if (entry.state != PipelineState::Ready) {
            ok = false;
        }
    }
    return ok;
}

auto PipelineRegistry::reload(std::span<const std::string> shaderIds) -> void {
    std::vector<std::string> ids;
    for (const auto& id : shaderIds) {
        // Requested even while an earlier reload of it is in flight: that one may answer with the content before this change.
        if (modules.contains(id)) {
            reloading.insert(id);
            ids.push_back(id);
        }
    }
    if (ids.empty()) {
        return;
    }
    source->request(ids);
    std::string list;
    for (const auto& id : ids) {
        list += list.empty() ? id : ", " + id;
    }
    TRACE_EVENT("Render", "ShadersReloadRequested", "pipelines").text(std::format("{} changed shader{} requested again: {}", ids.size(), ids.size() == 1 ? "" : "s", list)).field("count", (int64_t) ids.size());
}

auto PipelineRegistry::finishReload(const std::string& id, uint64_t frame) -> void {
    auto moduleIt = modules.find(id);
    if (moduleIt == modules.end()) {
        return;
    }
    auto oldVersion = moduleIt->second.version;
    if (auto errors = source->errors(id); !errors.empty()) {
        std::string text = std::format("shader {} failed to reload; the pipelines keep version {}", id, oldVersion);
        for (const auto& line : errors) {
            text += "\n  " + line;
        }
        TRACE_WARNING("Render", "ShaderReloadFailed", id).text(text);
        std::lock_guard lock(infoMutex);
        for (auto& entry : entries) {
            if (std::ranges::any_of(shadersOf(entry), [&](const ShaderRef& shader) { return shader.id == id; })) {
                entry.error = text;
            }
        }
        return;
    }
    auto asset = source->find(id);
    if (asset == nullptr) {
        return;
    }
    if (asset->version == oldVersion) {
        // Back to the version the pipelines have (a broken edit undone): an earlier reload's error no longer holds.
        std::lock_guard lock(infoMutex);
        for (auto& entry : entries) {
            if (entry.state == PipelineState::Ready && std::ranges::any_of(shadersOf(entry), [&](const ShaderRef& shader) { return shader.id == id; })) {
                entry.error.clear();
            }
        }
        TRACE_EVENT("Render", "ShaderReloaded", id).text(std::format("shader {} is back to the version its pipelines have ({}): nothing to recreate", id, oldVersion)).field("version", oldVersion).field("pipelines", (int64_t) 0);
        return;
    }
    std::optional<ShaderRef> ref;
    for (const auto& entry : entries) {
        for (const auto& shader : shadersOf(entry)) {
            if (shader.id == id) {
                ref = shader;
            }
        }
    }
    if (!ref) {
        return;
    }
    RhiShaderDesc desc = {
        .stage = ref->stage,
        .code = asset->bytes,
        .debugName = id.c_str(),
    };
    auto* created = device->createShaderModule(desc);
    if (created == nullptr) {
        TRACE_WARNING("Render", "ShaderReloadFailed", id).text(std::format("shader {} ({}): cannot create the shader module; the pipelines keep version {}", id, asset->version, oldVersion));
        return;
    }
    auto* oldModule = moduleIt->second.module;
    {
        std::lock_guard lock(infoMutex);
        moduleIt->second = {.module = created, .version = asset->version};
    }
    uint32_t recreated = 0;
    uint32_t failed = 0;
    for (auto& entry : entries) {
        if (!std::ranges::any_of(shadersOf(entry), [&](const ShaderRef& shader) { return shader.id == id; })) {
            continue;
        }
        if (entry.state == PipelineState::Failed) {
            // Failed before, perhaps for this shader: tried again by update(), like a pending first-use pipeline.
            std::lock_guard lock(infoMutex);
            entry.state = PipelineState::Pending;
            entry.error.clear();
            entry.reportedPending = false;
            continue;
        }
        if (entry.pipeline == nullptr) {
            continue;
        }
        std::string error;
        auto* pipeline = build(entry, error);
        std::lock_guard lock(infoMutex);
        if (pipeline == nullptr) {
            entry.error = std::format("recreating it for {} ({}) failed, it keeps the old pipeline: {}", id, asset->version, error);
            TRACE_WARNING("Render", "PipelineRecreateFailed", entry.name).text(std::format("pipeline {}: {}", entry.name, entry.error));
            failed++;
            continue;
        }
        device->setDebugName(pipeline, entry.name.c_str());
        // Frames in flight may still draw with the old one.
        retired->defer(frame, [rhiDevice = device, old = entry.pipeline] { rhiDevice->destroyPipeline(old); });
        entry.pipeline = pipeline;
        entry.error.clear();
        recreated++;
    }
    retired->defer(frame, [rhiDevice = device, oldModule] { rhiDevice->destroyShaderModule(oldModule); });
    TRACE_EVENT("Render", "ShaderReloaded", id)
        .text(std::format("shader {} reloaded ({} to {}): {} pipeline{} recreated{}", id, oldVersion, asset->version, recreated, recreated == 1 ? "" : "s", failed > 0 ? std::format(", {} kept the old one", failed) : ""))
        .field("version", asset->version)
        .field("pipelines", (int64_t) recreated)
        .field("failed", (int64_t) failed);
}

auto PipelineRegistry::update(uint64_t frame, uint32_t budget) -> void {
#ifndef NDEBUG
    returnedThisFrame.clear();
#endif
    for (auto it = reloading.begin(); it != reloading.end();) {
        if (source->pending(*it)) {
            ++it;
            continue;
        }
        auto id = *it;
        it = reloading.erase(it);
        finishReload(id, frame);
    }
    for (auto& entry : entries) {
        if (budget == 0) {
            return;
        }
        if (entry.pipeline != nullptr || entry.state == PipelineState::Failed || !entry.requested) {
            continue;
        }
        if (arrived(entry)) {
            create(entry);
            budget--;
            continue;
        }
        if (auto failed = failedShader(entry); !failed.empty()) {
            TRACE_ERROR("Render", "ShaderPackFailed", entry.name).text(failed);
            std::lock_guard lock(infoMutex);
            entry.state = PipelineState::Failed;
            entry.error = failed;
        }
    }
}

auto PipelineRegistry::infos() const -> std::vector<PipelineInfo> {
    std::lock_guard lock(infoMutex);
    std::vector<PipelineInfo> out;
    for (const auto& entry : entries) {
        PipelineInfo info;
        info.name = entry.name;
        info.use = entry.use;
        info.state = entry.state;
        info.requested = entry.requested;
        info.error = entry.error;
        for (const auto& shader : shadersOf(entry)) {
            auto it = modules.find(shader.id);
            info.shaders.emplace_back(shader.id, it != modules.end() ? it->second.version : std::string());
        }
        out.push_back(std::move(info));
    }
    return out;
}

auto PipelineRegistry::destroy() -> void {
    std::lock_guard lock(infoMutex);
    for (auto& entry : entries) {
        if (entry.pipeline != nullptr) {
            device->destroyPipeline(entry.pipeline);
            entry.pipeline = nullptr;
        }
    }
    for (auto& [id, module] : modules) {
        device->destroyShaderModule(module.module);
    }
    modules.clear();
    entries.clear();
}
