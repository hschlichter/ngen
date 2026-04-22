#include "observationbus.h"

#include "blockingconcurrentqueue.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

namespace obs {

namespace {

// Monotonic clock origin, captured on first access. Observations carry
// timestamps relative to this so two runs of the engine produce streams whose
// ts_ns values don't require wall-clock math to interpret.
auto monotonicOriginNs() -> uint64_t {
    static const auto origin = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now - origin).count());
}

// Hashable short label for the emitting thread. We avoid OS thread IDs (they
// aren't stable across runs) and instead hand the current thread's std::hash
// to a string — readers diffing two runs see the same label if the emitter
// role is the same, and short-circuit on mismatch if it isn't. Good enough
// for attribution; callers that need more can put a field on the observation.
auto currentThreadLabel() -> std::string {
    thread_local std::string label = [] {
        std::ostringstream oss;
        oss << std::this_thread::get_id();
        return oss.str();
    }();
    return label;
}

} // namespace

struct ObservationBus::Impl {
    // moodycamel::BlockingConcurrentQueue: MPSC-safe with a semaphore so the
    // writer thread can sleep when idle rather than spin. Unbounded in Phase 1.
    moodycamel::BlockingConcurrentQueue<Observation> queue;
    std::unique_ptr<ObservationSink> sink;
    std::thread writer;
    std::atomic<bool> running{false};

    // Populated before producer threads start and frozen afterwards. The read
    // path is a plain hash lookup — no mutex, no atomics.
    std::unordered_map<std::string, bool> categoryEnabled;

    // flush() round-trips through the writer thread via a sentinel. The writer
    // notifies `flushCv` after processing a flush request; callers wait on it.
    std::mutex flushMutex;
    std::condition_variable flushCv;
    uint64_t flushRequested = 0;
    uint64_t flushCompleted = 0;

    void writerLoop();
};

void ObservationBus::Impl::writerLoop() {
    using namespace std::chrono;
    auto lastFlush = steady_clock::now();
    const auto flushInterval = milliseconds(50);

    while (running.load(std::memory_order_acquire)) {
        Observation obs;
        bool gotOne = queue.wait_dequeue_timed(obs, flushInterval);
        // Empty-category observations are wake-up sentinels from flush() and
        // shutdown() — skip them rather than writing blank records.
        if (gotOne && !obs.category.empty()) {
            if (sink) {
                sink->write(obs);
            }
        }

        if (steady_clock::now() - lastFlush >= flushInterval) {
            if (sink) {
                sink->flush();
            }
            lastFlush = steady_clock::now();
        }

        // Service any pending flush request. flushRequested is monotonically
        // increasing; each call to flush() bumps it and blocks on
        // flushCompleted catching up.
        uint64_t requested;
        {
            std::lock_guard lk(flushMutex);
            requested = flushRequested;
        }
        if (requested > flushCompleted) {
            // Drain any remaining queued items first so the flush boundary is
            // honest — callers expect "everything emitted before I called
            // flush() is now on disk."
            Observation extra;
            while (queue.try_dequeue(extra)) {
                if (sink && !extra.category.empty()) {
                    sink->write(extra);
                }
            }
            if (sink) {
                sink->flush();
            }
            lastFlush = steady_clock::now();
            {
                std::lock_guard lk(flushMutex);
                flushCompleted = requested;
            }
            flushCv.notify_all();
        }
    }

    // Shutdown drain: pull everything remaining, write, flush once.
    Observation obs;
    while (queue.try_dequeue(obs)) {
        if (sink && !obs.category.empty()) {
            sink->write(obs);
        }
    }
    if (sink) {
        sink->flush();
    }
}

ObservationBus::ObservationBus() : m_impl(std::make_unique<Impl>()) {
}

ObservationBus::~ObservationBus() {
    shutdown();
}

void ObservationBus::setSink(std::unique_ptr<ObservationSink> sink) {
    if (m_hasSink.load(std::memory_order_acquire)) {
        return;
    }
    m_impl->sink = std::move(sink);
    m_impl->running.store(true, std::memory_order_release);
    m_impl->writer = std::thread([this] { m_impl->writerLoop(); });
    m_hasSink.store(true, std::memory_order_release);
}

void ObservationBus::emit(Observation&& obs) {
    if (!m_hasSink.load(std::memory_order_acquire)) {
        return;
    }
    obs.ts_ns = monotonicOriginNs();
    if (obs.thread.empty()) {
        obs.thread = currentThreadLabel();
    }
    m_impl->queue.enqueue(std::move(obs));
}

void ObservationBus::flush() {
    if (!m_hasSink.load(std::memory_order_acquire)) {
        return;
    }
    uint64_t target;
    {
        std::lock_guard lk(m_impl->flushMutex);
        target = ++m_impl->flushRequested;
    }
    // Wake the writer so it picks up the flush promptly (otherwise it could
    // sit in wait_dequeue_timed for up to flushInterval).
    m_impl->queue.enqueue(Observation{});
    std::unique_lock lk(m_impl->flushMutex);
    m_impl->flushCv.wait(lk, [&] { return m_impl->flushCompleted >= target; });
}

void ObservationBus::shutdown() {
    if (!m_hasSink.load(std::memory_order_acquire)) {
        return;
    }
    m_impl->running.store(false, std::memory_order_release);
    // Wake the writer if it's parked in wait_dequeue_timed.
    m_impl->queue.enqueue(Observation{});
    if (m_impl->writer.joinable()) {
        m_impl->writer.join();
    }
    m_impl->sink.reset();
    m_hasSink.store(false, std::memory_order_release);
}

void ObservationBus::setCategoryEnabled(std::string_view category, bool enabled) {
    m_impl->categoryEnabled[std::string(category)] = enabled;
}

bool ObservationBus::categoryEnabled(std::string_view category) const {
    if (!m_hasSink.load(std::memory_order_acquire)) {
        return false;
    }
    // Unknown categories default to enabled — opt-out, not opt-in (see §3.5).
    auto it = m_impl->categoryEnabled.find(std::string(category));
    if (it == m_impl->categoryEnabled.end()) {
        return true;
    }
    return it->second;
}

ObservationBus& bus() {
    static ObservationBus instance;
    return instance;
}

} // namespace obs
