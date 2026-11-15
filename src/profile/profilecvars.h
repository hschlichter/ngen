#pragma once

#include "cvar.h"

// The profiler's cvars (src/cvar/README.md). They belong to the main thread; the main loop hands them to the
// collector when they change (profile::setHistorySeconds, profile::setGpuDetail), which any thread reads.
namespace profilecvars {

extern cvar::Float historySeconds;  // profile.history.seconds
extern cvar::Bool gpuDetail;        // profile.gpu.detail
extern cvar::Bool tracyFrameImages; // profile.tracy.frameImages
extern cvar::Float frameBudgetMs;   // profile.budget.frameMs
extern cvar::Float stallSeconds;    // profile.stall.seconds

} // namespace profilecvars
