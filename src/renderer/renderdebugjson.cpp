#include "renderdebugjson.h"

#include "framegraphdebug.h"

#include <cstdio>
#include <string>

namespace {

auto escape(const std::string& s) -> std::string {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else {
            out += c;
        }
    }
    return out;
}

} // namespace

auto writeRenderDebugJson(const char* path, const RenderDebugSnapshot& s, const std::function<std::string(uint32_t)>& primPath) -> bool {
    auto* f = std::fopen(path, "w");
    if (f == nullptr) {
        return false;
    }
    const auto& l = s.limits;
    std::fprintf(f, "{\n  \"frame\": %llu,\n", (unsigned long long) s.frameIndex);
    std::fprintf(f, "  \"device\": {\"name\": \"%s\", \"driver\": \"%s\", \"validation\": %s, \"timestamps\": %s, \"calibratedTimestamps\": %s, \"maxPushConstantSize\": %u, \"minUniformBufferOffsetAlignment\": %llu},\n", escape(l.deviceName).c_str(), escape(l.driverName).c_str(), s.validation ? "true" : "false", l.timestamps ? "true" : "false", l.calibratedTimestamps ? "true" : "false", l.maxPushConstantSize, (unsigned long long) l.minUniformBufferOffsetAlignment);
    std::fprintf(f, "  \"swapchain\": {\"width\": %u, \"height\": %u, \"format\": \"%s\", \"images\": %u, \"slot\": %u},\n", s.swapchainExtent.width, s.swapchainExtent.height, toString(s.swapchainFormat), s.swapchainImages, s.currentSlot);
    std::fprintf(f, "  \"inspect\": {\"valid\": %s, \"material\": %u, \"level\": %u, \"mips\": %u, \"width\": %u, \"height\": %u},\n  \"scene\": {\"instances\": %u, \"culled\": %u, \"primFirstInstances\": %u, \"materials\": %u, \"materialsWithTexture\": %u, \"lights\": %u, \"hasSun\": %s, \"sunDirection\": [%.4f, %.4f, %.4f], \"sunRadiance\": [%.4f, %.4f, %.4f], \"shadowMap\": [%u, %u]},\n", s.inspect.valid ? "true" : "false", s.inspect.material, s.inspect.level, s.inspect.mipLevels, s.inspect.levelWidth, s.inspect.levelHeight, s.instanceCount, s.culledInstances, s.primFirstInstances, s.materialCount, s.materialsWithTexture, s.lightCount, s.hasSun ? "true" : "false", s.sunDirection.x, s.sunDirection.y, s.sunDirection.z, s.sunRadiance.x, s.sunRadiance.y, s.sunRadiance.z, s.shadowMapExtent.width, s.shadowMapExtent.height);

    std::fprintf(f, "  \"meshes\": [\n");
    for (size_t i = 0; i < s.meshes.size(); i++) {
        const auto& m = s.meshes[i];
        std::fprintf(f, "    {\"mesh\": %u, \"triangles\": %u, \"vertices\": %u, \"vertexBytes\": %llu, \"indexBytes\": %llu, \"instances\": %u}%s\n", m.meshIndex, m.indexCount / 3, m.vertexCount, (unsigned long long) m.vertexBytes, (unsigned long long) m.indexBytes, m.instances, i + 1 < s.meshes.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"textures\": [\n");
    for (size_t i = 0; i < s.textures.size(); i++) {
        const auto& t = s.textures[i];
        std::fprintf(f, "    {\"material\": %u, \"width\": %u, \"height\": %u, \"mips\": %u, \"format\": \"%s\", \"bytes\": %llu}%s\n", t.materialIndex, t.width, t.height, t.mipLevels, toString(t.format), (unsigned long long) t.bytes, i + 1 < s.textures.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"passes\": [\n");
    for (size_t i = 0; i < s.passes.size(); i++) {
        const auto& p = s.passes[i];
        std::fprintf(f, "    {\"name\": \"%s\", \"culled\": %s, \"gpuMs\": %.4f, \"draws\": %u, \"dispatches\": %u, \"barriers\": %u, \"pipelineBinds\": %u, \"descriptorBinds\": %u, \"bufferBinds\": %u, \"indirectDraws\": %u, \"copies\": %u, \"primitives\": %llu}%s\n", escape(p.name).c_str(), p.culled ? "true" : "false", p.gpuTimeMs, p.stats.draws, p.stats.dispatches, p.stats.barriers, p.stats.pipelineBinds, p.stats.descriptorBinds, p.stats.bufferBinds, p.stats.indirectDraws, p.stats.copies, (unsigned long long) p.stats.primitives, i + 1 < s.passes.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"frameTotals\": {\"draws\": %u, \"dispatches\": %u, \"barriers\": %u, \"primitives\": %llu},\n", s.frameTotals.draws, s.frameTotals.dispatches, s.frameTotals.barriers, (unsigned long long) s.frameTotals.primitives);
    std::fprintf(f, "  \"pool\": {\"allocationsTotal\": %u, \"textures\": [\n", s.poolAllocationsTotal);
    for (size_t i = 0; i < s.poolTextures.size(); i++) {
        const auto& t = s.poolTextures[i];
        std::fprintf(f, "    {\"width\": %u, \"height\": %u, \"format\": \"%s\", \"inUse\": %s}%s\n", t.width, t.height, toString(t.format), t.inUse ? "true" : "false", i + 1 < s.poolTextures.size() ? "," : "");
    }
    std::fprintf(f, "  ]},\n  \"draws\": [\n");
    for (size_t i = 0; i < s.draws.size(); i++) {
        const auto& d = s.draws[i];
        std::fprintf(f, "    {\"pass\": \"%s\", \"index\": %u, \"prim\": \"%s\", \"instance\": %u, \"mesh\": %u, \"material\": %u, \"indexOffset\": %u, \"indexCount\": %u}%s\n", escape(d.pass).c_str(), d.drawIndex, escape(primPath(d.prim)).c_str(), d.instance, d.mesh, d.material, d.indexOffset, d.indexCount, i + 1 < s.draws.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}
