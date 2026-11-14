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

config::Bool tracyFrameImages{"profile.tracy.frameImages",
                              true,
                              "While a Tracy server is connected, send it a 320-pixel-wide image of every presented frame, shown under that frame. A "
                              "blit and a small readback a frame; nothing without a server."};

config::Float frameBudgetMs{"profile.budget.frameMs",
                            16.667,
                            "The frame time content is measured against, in milliseconds: 16.7 is 60 frames a second, 33.3 is 30. The frame budget "
                            "overlay draws the passes against it.",
                            {.min = 1.0, .max = 1000.0}};

config::Float stallSeconds{"profile.stall.seconds",
                           5.0,
                           "How long a thread stays in one profiler zone before the stall watch reports it: a Stall warning in the trace with every "
                           "zone the thread has open, and the profile record's stalls. 0 turns the watch off.",
                           {.min = 0.0, .max = 600.0}};

} // namespace profileconfig
