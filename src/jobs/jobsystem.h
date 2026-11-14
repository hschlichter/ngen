#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <source_location>
#include <span>
#include <string>
#include <thread>
#include <vector>

using JobFunc = std::function<void()>;

// What a job is to the threads that wait for it (src/jobs/README.md).
enum class JobKind : uint8_t {
    Frame,      // short, part of the current frame's work: waited for, and the thread that waits runs frame jobs meanwhile
    Background, // long (a large allocation): its fence is polled with ready(), never waited for by the main or render thread
};

class JobFence {
    friend class JobSystem;
    std::shared_ptr<std::atomic<bool>> done;

public:
    auto ready() const -> bool { return done && done->load(std::memory_order_acquire); }
    // A fence that was never submitted; ready() is false and wait() returns at once.
    auto empty() const -> bool { return done == nullptr; }
};

// What the jobs record shows.
struct JobStats {
    uint32_t workers = 0;
    uint64_t frameQueued = 0;
    uint64_t backgroundQueued = 0;
    uint32_t frameRunning = 0;
    uint32_t backgroundRunning = 0;
    uint64_t frameCompleted = 0;
    uint64_t backgroundCompleted = 0;
    // The background job running longest, and for how long; empty name when none runs.
    std::string longestBackground;
    double longestBackgroundMs = 0.0;
};

class JobSystem {
public:
    static auto init(uint32_t numWorkers = 0) -> void;
    static auto shutdown() -> void;
    static auto workerCount() -> uint32_t { return (uint32_t) workers.size(); }

    // `name` labels the job in the profiler and the jobs record; must be a string literal or otherwise outlive the job.
    // `location` is the caller's: the job's zone points at its submit. Without workers (a program that never called init) the job runs at
    // once, on the caller.
    static auto submit(JobFunc job, const char* name = "Job", JobKind kind = JobKind::Frame, std::source_location location = std::source_location::current())
        -> JobFence;

    // Waits for a frame job, running other frame jobs meanwhile; never runs a background job.
    static auto wait(const JobFence& fence) -> void;
    static auto waitAll(std::span<const JobFence> fences) -> void;

    // Runs body(begin, end) over [0, count) in chunks of `grain` items, on the workers and on the caller, as frame jobs; returns when every
    // chunk has run. A count of one chunk or less runs on the caller alone.
    static auto parallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t)>& body, const char* name, std::source_location location = std::source_location::current())
        -> void;

    static auto stats() -> JobStats;

private:
    struct Job {
        JobFunc func;
        std::shared_ptr<std::atomic<bool>> done;
        uint32_t profileNameId = 0;
        const char* name = "";
        JobKind kind = JobKind::Frame;
    };
    // The background job a worker runs, for the jobs record.
    struct Running {
        const char* name = nullptr;
        uint64_t sinceNs = 0;
    };
    static constexpr uint32_t noWorker = UINT32_MAX; // a thread helping while it waits

    static auto workerLoop(uint32_t index, std::stop_token stopToken) -> void;
    static auto tryExecuteFrameJob() -> bool;
    static auto execute(Job& job, uint32_t worker) -> void;

    static std::vector<std::jthread> workers;
    static std::deque<Job> frameQueue;
    static std::deque<Job> backgroundQueue;
    static std::mutex queueMutex;
    static std::condition_variable_any queueCV; // _any: waits can be interrupted by a stop token
    static std::condition_variable doneCV;
    static bool stopping;

    static std::mutex runningMutex;
    static std::vector<Running> runningBackground; // per worker; a helper runs only frame jobs
    static std::atomic<uint32_t> frameRunning;
    static std::atomic<uint32_t> backgroundRunning;
    static std::atomic<uint64_t> frameCompleted;
    static std::atomic<uint64_t> backgroundCompleted;
};
