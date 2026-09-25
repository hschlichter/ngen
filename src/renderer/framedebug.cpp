#include "framedebug.h"

#include "rhidevice.h"

#include <cstdio>
#include <unordered_map>

auto buildFrameDebugCapture(const FrameGraphDebugSnapshot& snap, const RhiDevice& device, uint64_t frame) -> FrameDebugCapture {
    FrameDebugCapture capture = {.frame = frame};
    auto resourceName = [&](uint32_t index) -> std::string {
        return index < snap.resources.size() ? snap.resources[index].name : std::string("?");
    };
    std::unordered_map<const RhiDescriptorSet*, std::string> setNames;
    for (uint32_t order = 0; order < snap.executionOrder.size(); order++) {
        const auto& src = snap.passes[snap.executionOrder[order]];
        FrameDebugPass pass = {.name = src.name, .order = order, .culled = src.culled, .gpuMs = src.gpuTimeMs, .stats = src.stats};
        for (const auto& a : src.reads) {
            pass.reads.push_back({resourceName(a.resourceIndex), toString(a.access)});
        }
        for (const auto& a : src.writes) {
            pass.writes.push_back({resourceName(a.resourceIndex), toString(a.access)});
        }
        for (const auto& b : src.barriers) {
            FrameDebugBarrier barrier = {
                .resource = resourceName(b.resourceIndex),
                .buffer = b.buffer,
                .oldAccess = toString(b.oldAccess),
                .newAccess = toString(b.newAccess),
                .oldState = b.oldState,
                .newState = b.newState,
            };
            barrier.backend = b.buffer ? device.describeTransition(b.oldBufferState, b.newBufferState) : device.describeTransition(b.oldTextureState, b.newTextureState);
            pass.barriers.push_back(std::move(barrier));
        }
        for (const auto& c : src.commands) {
            pass.commands.push_back(c.text);
            if (c.descriptorSet != nullptr) {
                // "bindDescriptorSet set N <name> (pipeline ...)": the name is the fourth word.
                auto name = c.text;
                auto start = name.find(' ', std::string("bindDescriptorSet set ").size());
                auto end = name.find(" (", start);
                name = start == std::string::npos ? name : name.substr(start + 1, end - start - 1);
                pass.descriptorSets.push_back(name);
                setNames.emplace(c.descriptorSet, name);
            }
        }
        capture.passes.push_back(std::move(pass));
    }
    for (const auto& [set, name] : setNames) {
        capture.sets.push_back({.name = name, .descriptors = device.describeDescriptorSet(set)});
    }
    return capture;
}

namespace {

auto escape(const std::string& s) -> std::string {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

auto descriptorTypeName(RhiDescriptorType type) -> const char* {
    switch (type) {
        case RhiDescriptorType::UniformBuffer:
            return "UniformBuffer";
        case RhiDescriptorType::CombinedImageSampler:
            return "CombinedImageSampler";
        case RhiDescriptorType::StorageBuffer:
            return "StorageBuffer";
        case RhiDescriptorType::StorageImage:
            return "StorageImage";
    }
    return "?";
}

} // namespace

auto writeFrameDebugJson(const char* path, const FrameDebugCapture& c) -> bool {
    auto* f = std::fopen(path, "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "{\n  \"frame\": %llu,\n  \"passes\": [\n", (unsigned long long) c.frame);
    for (size_t p = 0; p < c.passes.size(); p++) {
        const auto& pass = c.passes[p];
        std::fprintf(f, "    {\"name\": \"%s\", \"order\": %u, \"culled\": %s, \"gpuMs\": %.4f, \"draws\": %u, \"indirectDraws\": %u, \"dispatches\": %u, \"barriers\": %u, \"primitives\": %llu,\n", escape(pass.name).c_str(), pass.order, pass.culled ? "true" : "false", pass.gpuMs, pass.stats.draws, pass.stats.indirectDraws, pass.stats.dispatches, pass.stats.barriers, (unsigned long long) pass.stats.primitives);
        auto accessList = [&](const char* key, const std::vector<FrameDebugAccess>& list) {
            std::fprintf(f, "     \"%s\": [", key);
            for (size_t i = 0; i < list.size(); i++) {
                std::fprintf(f, "%s{\"resource\": \"%s\", \"access\": \"%s\"}", i > 0 ? ", " : "", escape(list[i].resource).c_str(), list[i].access.c_str());
            }
            std::fprintf(f, "],\n");
        };
        accessList("reads", pass.reads);
        accessList("writes", pass.writes);
        std::fprintf(f, "     \"barrierList\": [\n");
        for (size_t i = 0; i < pass.barriers.size(); i++) {
            const auto& b = pass.barriers[i];
            std::fprintf(f, "       {\"resource\": \"%s\", \"kind\": \"%s\", \"access\": \"%s -> %s\", \"state\": \"%s -> %s\", \"srcStages\": \"%s\", \"srcAccess\": \"%s\", \"dstStages\": \"%s\", \"dstAccess\": \"%s\", \"oldLayout\": \"%s\", \"newLayout\": \"%s\"}%s\n", escape(b.resource).c_str(), b.buffer ? "buffer" : "texture", b.oldAccess.c_str(), b.newAccess.c_str(), b.oldState.c_str(), b.newState.c_str(), b.backend.srcStages.c_str(), b.backend.srcAccess.c_str(), b.backend.dstStages.c_str(), b.backend.dstAccess.c_str(), b.backend.oldLayout.c_str(), b.backend.newLayout.c_str(), i + 1 < pass.barriers.size() ? "," : "");
        }
        std::fprintf(f, "     ],\n     \"descriptorSets\": [");
        for (size_t i = 0; i < pass.descriptorSets.size(); i++) {
            std::fprintf(f, "%s\"%s\"", i > 0 ? ", " : "", escape(pass.descriptorSets[i]).c_str());
        }
        std::fprintf(f, "],\n     \"commands\": [\n");
        for (size_t i = 0; i < pass.commands.size(); i++) {
            std::fprintf(f, "       \"%s\"%s\n", escape(pass.commands[i]).c_str(), i + 1 < pass.commands.size() ? "," : "");
        }
        std::fprintf(f, "     ]}%s\n", p + 1 < c.passes.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"descriptorSets\": [\n");
    for (size_t s = 0; s < c.sets.size(); s++) {
        const auto& set = c.sets[s];
        std::fprintf(f, "    {\"name\": \"%s\", \"descriptors\": [\n", escape(set.name).c_str());
        for (size_t i = 0; i < set.descriptors.size(); i++) {
            const auto& d = set.descriptors[i];
            std::fprintf(f, "      {\"binding\": %u, \"element\": %u, \"type\": \"%s\", \"resource\": \"%s\"}%s\n", d.binding, d.arrayElement, descriptorTypeName(d.type), escape(d.resource).c_str(), i + 1 < set.descriptors.size() ? "," : "");
        }
        std::fprintf(f, "    ]}%s\n", s + 1 < c.sets.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}
