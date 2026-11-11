#pragma once

#include "config.h"

// The profiler's configuration values (src/config/README.md). They belong to the main thread; the main loop hands them to the
// collector when they change (profile::setHistorySeconds, profile::setGpuDetail), which any thread reads.
namespace profileconfig {

extern config::Float historySeconds;  // profile.history.seconds
extern config::Bool gpuDetail;        // profile.gpu.detail
extern config::Bool tracyFrameImages; // profile.tracy.frameImages
extern config::Float frameBudgetMs;   // profile.budget.frameMs

} // namespace profileconfig
