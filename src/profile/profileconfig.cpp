#include "profileconfig.h"

namespace profileconfig {

config::Float historySeconds{"profile.history.seconds",
                             10.0,
                             "How long the profiler keeps zones, frames and GPU frames, in seconds: what the Performance window scrubs, the zones record "
                             "and dump-profile read. Memory follows the zone rate.",
                             {.min = 1.0, .max = 120.0}};

config::Bool gpuDetail{"profile.gpu.detail",
                       false,
                       "Record detail GPU zones (culling phases, depth pyramid levels, cascades, upload batches). Each zone's timestamps make the GPU "
                       "wait for earlier work, so fine zones change the timings they measure; the per-pass zones are always on."};

} // namespace profileconfig
