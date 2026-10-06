#pragma once

#include "traceevent.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace trace {

// The process's trace: the most recent `capacity` events, always on. Producers push from any thread; readers copy
// events out by sequence number, so each reader keeps its own cursor and the ring never waits for one.
class TraceRing {
public:
    static constexpr size_t capacity = 65536;

    TraceRing();
    TraceRing(const TraceRing&) = delete;
    auto operator=(const TraceRing&) -> TraceRing& = delete;

    // Assigns the event its seq, and its time and thread if empty, and stores it over the oldest.
    auto push(Event event) -> void;

    struct Read {
        uint64_t next = 0;    // the cursor after what was read
        uint64_t dropped = 0; // events from `from` on that were overwritten before they were read
    };
    // Copies at most `max` events from seq `from` on into `out` (appended).
    auto read(uint64_t from, size_t max, std::vector<Event>& out) const -> Read;
    // The seq the next event gets.
    auto head() const -> uint64_t;
    // The seq of the oldest event still held.
    auto oldest() const -> uint64_t;
    // The seq of the oldest held event at or after tsNs; head() when there is none.
    auto firstAtOrAfter(uint64_t tsNs) const -> uint64_t;
    // Returns once head() reaches `seq`, or after `timeout`.
    auto waitFor(uint64_t seq, std::chrono::milliseconds timeout) const -> void;

private:
    mutable std::mutex mutex;
    mutable std::condition_variable grew;
    std::vector<Event> slots;
    uint64_t next = 1;
};

// The process's ring.
auto ring() -> TraceRing&;

} // namespace trace
