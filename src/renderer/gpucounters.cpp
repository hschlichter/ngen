#include "gpucounters.h"

#include <cstdio>

auto writeGpuCountersJson(const char* path, const GpuCounters& counters) -> bool {
    auto* f = std::fopen(path, "w");
    if (f == nullptr) {
        return false;
    }
    std::fprintf(f, "{\n  \"frame\": %llu,\n  \"gpuFrameMs\": %.4f,\n  \"width\": %u,\n  \"height\": %u,\n", (unsigned long long) counters.frame, counters.gpuFrameMs, counters.width, counters.height);
    std::fprintf(f, "  \"zones\": [\n");
    for (size_t i = 0; i < counters.zones.size(); i++) {
        const auto& z = counters.zones[i];
        std::fprintf(f, "    {\"name\": \"%s\", \"depth\": %u, \"startMs\": %.4f, \"ms\": %.4f}%s\n", z.name.c_str(), (unsigned) z.depth, z.startMs, z.ms, i + 1 < counters.zones.size() ? "," : "");
    }
    std::fprintf(f, "  ],\n  \"passes\": [\n");
    for (size_t i = 0; i < counters.passes.size(); i++) {
        const auto& p = counters.passes[i];
        const auto& s = p.stats;
        std::fprintf(f,
                     "    {\"name\": \"%s\", \"iaVertices\": %llu, \"iaPrimitives\": %llu, \"vertexInvocations\": %llu, \"clippingInvocations\": %llu, "
                     "\"clippingPrimitives\": %llu, \"fragmentInvocations\": %llu, \"computeInvocations\": %llu}%s\n",
                     p.name.c_str(),
                     (unsigned long long) s.iaVertices,
                     (unsigned long long) s.iaPrimitives,
                     (unsigned long long) s.vertexInvocations,
                     (unsigned long long) s.clippingInvocations,
                     (unsigned long long) s.clippingPrimitives,
                     (unsigned long long) s.fragmentInvocations,
                     (unsigned long long) s.computeInvocations,
                     i + 1 < counters.passes.size() ? "," : "");
    }
    std::fprintf(f, "  ]\n}\n");
    std::fclose(f);
    return true;
}
