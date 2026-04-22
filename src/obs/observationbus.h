#pragma once

#include "observation.h"
#include "observationsink.h"

#include <atomic>
#include <memory>
#include <string_view>

namespace obs {

// Thread-safe observation bus. Producers call emit() (lock-free enqueue);
// a dedicated writer thread dequeues and hands observations to the installed
// sink. Without a sink installed, every emit() short-circuits to a no-op.
//
// Category filtering is read on the producer hot path without synchronization —
// the map must be fully populated before any producer thread starts. Mid-run
// toggling is not supported.
class ObservationBus {
public:
    ObservationBus();
    ~ObservationBus();

    ObservationBus(const ObservationBus&) = delete;
    ObservationBus& operator=(const ObservationBus&) = delete;

    // Installs a sink and starts the writer thread. Takes ownership. Must be
    // called before any producer thread begins emitting. Calling with a null
    // pointer is not supported (use shutdown() to stop).
    void setSink(std::unique_ptr<ObservationSink> sink);

    // Producer-side lock-free enqueue. Safe from any thread. No-op if no sink
    // is installed or the category is not enabled.
    void emit(Observation&& obs);

    // Block until all previously-emitted observations have been written and
    // flushed to the sink. Round-trips through the writer thread.
    void flush();

    // Drain the queue, flush, and join the writer thread. Idempotent; call
    // from main() before return.
    void shutdown();

    // Startup-only filter configuration. Writes must complete before any
    // producer thread starts — the read path is lock-free.
    void setCategoryEnabled(std::string_view category, bool enabled);

    // Lock-free hot-path check. Returns false unconditionally when no sink is
    // installed (the common zero-cost path when the user doesn't opt in).
    bool categoryEnabled(std::string_view category) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
    // Hot-path flag — true iff a sink has been installed. Read by every
    // categoryEnabled() call; set once during setSink().
    std::atomic<bool> m_hasSink{false};
};

// Process-wide bus. Constructed on first use; safe to call from any thread
// once the process has reached main().
ObservationBus& bus();

} // namespace obs
