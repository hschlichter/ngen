#include "pipelineregistry.h"

#include "assetclient.h"
#include "rhidevice.h"
#include "trace.h"

#include <algorithm>
#include <cassert>
#include <format>

auto PipelineRegistry::init(RhiDevice* rhiDevice, AssetClient* shaders) -> void {
    device = rhiDevice;
    source = shaders;
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
    const auto* asset = source->find(shader.id);
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

auto PipelineRegistry::create(Entry& entry) -> void {
    std::string error;
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
    std::lock_guard lock(infoMutex);
    if (pipeline == nullptr) {
        entry.state = PipelineState::Failed;
        entry.error = error.empty() ? std::string("the device couldn't create the pipeline") : error;
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

auto PipelineRegistry::update(uint32_t budget) -> void {
#ifndef NDEBUG
    returnedThisFrame.clear();
#endif
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
