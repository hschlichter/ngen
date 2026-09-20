#pragma once

// GPU zone marker on top of the RHI. Separate header so profile.h stays free of RHI
// types. `name` must outlive the command buffer's execution (string literals do).

#include "profile.h"
#include "rhicommandbuffer.h"

namespace profile {

struct ScopedGpuZone {
    ScopedGpuZone(RhiCommandBuffer* cmd, const char* name) : cmd(cmd) { cmd->beginGpuZone(name); }
    ~ScopedGpuZone() { cmd->endGpuZone(); }
    ScopedGpuZone(const ScopedGpuZone&) = delete;
    ScopedGpuZone& operator=(const ScopedGpuZone&) = delete;
    RhiCommandBuffer* cmd;
};

} // namespace profile

#define PROFILE_GPU_ZONE(cmd, name) ::profile::ScopedGpuZone PROFILE_CONCAT(profileGpuZone_, __LINE__)((cmd), (name))
