#pragma once

// GPU zone marker on top of the RHI. Separate header so profile.h stays free of RHI
// types. `name` must outlive the command buffer's execution (string literals do).
// A detail GPU zone records only while profile::gpuDetail() is on: a timestamp makes the
// GPU wait for earlier work at that point, so fine zones change what they measure.
// The zone's site travels through the RHI as the zone's tag.

#include "profile.h"
#include "rhicommandbuffer.h"

namespace profile {

struct ScopedGpuZone {
    ScopedGpuZone(RhiCommandBuffer* cmd, const char* name, Group group, Level level, std::source_location location = std::source_location::current()) : cmd(cmd) {
        if (level == Level::Detail && !gpuDetail()) {
            return;
        }
        cmd->beginGpuZone(name, registerSite(name, group, level, location));
        active = true;
    }
    // A site registered before (a frame-graph pass's).
    ScopedGpuZone(RhiCommandBuffer* cmd, uint32_t siteId) : cmd(cmd) {
        cmd->beginGpuZone(nameOf(siteId), siteId);
        active = true;
    }
    ~ScopedGpuZone() {
        if (active) {
            cmd->endGpuZone();
        }
    }
    ScopedGpuZone(const ScopedGpuZone&) = delete;
    ScopedGpuZone& operator=(const ScopedGpuZone&) = delete;
    RhiCommandBuffer* cmd;
    bool active = false;
};

} // namespace profile

#define PROFILE_GPU_ZONE(cmd, name, group) \
    ::profile::ScopedGpuZone PROFILE_CONCAT(profileGpuZone_, __LINE__)((cmd), (name), ::profile::Group::group, ::profile::Level::Guide)
#define PROFILE_GPU_ZONE_DETAIL(cmd, name, group) \
    ::profile::ScopedGpuZone PROFILE_CONCAT(profileGpuZone_, __LINE__)((cmd), (name), ::profile::Group::group, ::profile::Level::Detail)
