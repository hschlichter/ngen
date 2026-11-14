#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The stall watch (src/profile/README.md, "Stalls"): a thread that looks at every thread's open zones a few times a second and reports a
// thread that has stayed in one zone longer than profile.stall.seconds, as a Stall warning in the trace with the zones it has open. The
// history shows a zone only once it ends, so a thread that never leaves one shows nowhere else; the trace streams on its own thread, so the
// report gets out while the main thread is the one stuck.
namespace profile {

// Starts the watch's thread; a second call does nothing. Stopped before the program's statics go (the trace among them).
auto startStallWatch() -> void;
auto stopStallWatch() -> void;

struct StallInfo {
    std::string lane;
    std::string zone;  // the innermost zone open longer than the threshold
    std::string stack; // every open zone, outermost first: "Name 6.2 s > Name 5.1 s > Name 0.3 s"
    double seconds = 0.0;
};

struct StallWatchState {
    bool running = false;
    uint64_t reported = 0;          // stalls reported since start
    std::vector<StallInfo> current; // threads stalled now
};
auto stallWatchState() -> StallWatchState;

} // namespace profile
