#include "jobsystem.h"
#include "profile.h"

#include <algorithm>
#include <array>
#include <stop_token>
#include <string>

std::vector<std::jthread> JobSystem::workers;
std::deque<JobSystem::Job> JobSystem::frameQueue;
std::deque<JobSystem::Job> JobSystem::backgroundQueue;
std::mutex JobSystem::queueMutex;
std::condition_variable_any JobSystem::queueCV;
std::condition_variable JobSystem::doneCV;
bool JobSystem::stopping = false;

std::mutex JobSystem::runningMutex;
std::vector<JobSystem::Running> JobSystem::runningBackground;
std::atomic<uint32_t> JobSystem::frameRunning = 0;
std::atomic<uint32_t> JobSystem::backgroundRunning = 0;
std::atomic<uint64_t> JobSystem::frameCompleted = 0;
std::atomic<uint64_t> JobSystem::backgroundCompleted = 0;

auto JobSystem::init(uint32_t numWorkers) -> void {
    if (numWorkers == 0) {
        numWorkers = std::max(1u, std::thread::hardware_concurrency() - 1);
    }
    {
        std::lock_guard lock(runningMutex);
        runningBackground.assign(numWorkers, Running{});
    }
    for (uint32_t i = 0; i < numWorkers; i++) {
        workers.emplace_back([i](std::stop_token stopToken) {
            static std::array<std::string, 64> names; // lane names must outlive the thread
            auto& name = names[i % names.size()];
            name = "Worker " + std::to_string(i);
            profile::registerThread(name.c_str());
            workerLoop(i, stopToken);
        });
    }
}

auto JobSystem::shutdown() -> void {
    {
        std::lock_guard lock(queueMutex);
        stopping = true;
    }
    queueCV.notify_all();
    doneCV.notify_all();
    workers.clear();
    stopping = false;
}

auto JobSystem::submit(JobFunc job, const char* name, JobKind kind, std::source_location location) -> JobFence {
    auto done = std::make_shared<std::atomic<bool>>(false);
    auto siteId = profile::registerSite(name, profile::Group::Jobs, profile::Level::Guide, location);
    JobFence fence;
    fence.done = done;
    Job entry{.func = std::move(job), .done = done, .profileNameId = siteId, .name = name, .kind = kind};
    if (workers.empty()) {
        execute(entry, noWorker);
        return fence;
    }
    {
        std::lock_guard lock(queueMutex);
        if (kind == JobKind::Frame) {
            frameQueue.push_back(std::move(entry));
        } else {
            backgroundQueue.push_back(std::move(entry));
        }
    }
    queueCV.notify_one();
    return fence;
}

auto JobSystem::wait(const JobFence& fence) -> void {
    if (!fence.done) {
        return;
    }
    PROFILE_ZONE("JobWait", Wait);

    while (!fence.done->load(std::memory_order_acquire)) {
        if (!tryExecuteFrameJob()) {
            std::unique_lock lock(queueMutex);
            doneCV.wait(lock, [&] { return fence.done->load(std::memory_order_acquire) || stopping; });
        }
    }
}

auto JobSystem::waitAll(std::span<const JobFence> fences) -> void {
    for (const auto& fence : fences) {
        wait(fence);
    }
}

auto JobSystem::parallelFor(size_t count, size_t grain, const std::function<void(size_t, size_t)>& body, const char* name, std::source_location location) -> void {
    if (count == 0) {
        return;
    }
    grain = std::max<size_t>(grain, 1);
    auto chunks = (count + grain - 1) / grain;
    auto siteId = profile::registerSite(name, profile::Group::Jobs, profile::Level::Guide, location);
    if (chunks == 1 || workers.empty()) {
        profile::ScopedZone zone(siteId);
        body(0, count);
        return;
    }
    // Every runner takes the next chunk until none is left, so a slow chunk doesn't hold the others back.
    std::atomic<size_t> next = 0;
    auto run = [&] {
        while (true) {
            auto chunk = next.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= chunks) {
                return;
            }
            auto begin = chunk * grain;
            auto end = std::min(count, begin + grain);
            body(begin, end);
        }
    };
    auto helpers = std::min<size_t>(chunks - 1, workers.size());
    std::vector<JobFence> fences;
    fences.reserve(helpers);
    for (size_t i = 0; i < helpers; i++) {
        fences.push_back(submit(run, name, JobKind::Frame, location));
    }
    {
        profile::ScopedZone zone(siteId);
        run();
    }
    waitAll(fences);
}

auto JobSystem::stats() -> JobStats {
    JobStats out;
    out.workers = (uint32_t) workers.size();
    {
        std::lock_guard lock(queueMutex);
        out.frameQueued = frameQueue.size();
        out.backgroundQueued = backgroundQueue.size();
    }
    out.frameRunning = frameRunning.load(std::memory_order_relaxed);
    out.backgroundRunning = backgroundRunning.load(std::memory_order_relaxed);
    out.frameCompleted = frameCompleted.load(std::memory_order_relaxed);
    out.backgroundCompleted = backgroundCompleted.load(std::memory_order_relaxed);
    auto now = profile::now();
    std::lock_guard lock(runningMutex);
    for (const auto& entry : runningBackground) {
        if (entry.name == nullptr) {
            continue;
        }
        auto ms = (double) (now - entry.sinceNs) * 1e-6;
        if (ms > out.longestBackgroundMs) {
            out.longestBackgroundMs = ms;
            out.longestBackground = entry.name;
        }
    }
    return out;
}

auto JobSystem::execute(Job& job, uint32_t worker) -> void {
    bool background = job.kind == JobKind::Background;
    auto& runningCount = background ? backgroundRunning : frameRunning;
    runningCount.fetch_add(1, std::memory_order_relaxed);
    if (background && worker != noWorker) {
        std::lock_guard lock(runningMutex);
        runningBackground[worker] = {.name = job.name, .sinceNs = profile::now()};
    }
    {
        profile::ScopedZone zone(job.profileNameId);
        job.func();
    }
    if (background && worker != noWorker) {
        std::lock_guard lock(runningMutex);
        runningBackground[worker] = {};
    }
    runningCount.fetch_sub(1, std::memory_order_relaxed);
    (background ? backgroundCompleted : frameCompleted).fetch_add(1, std::memory_order_relaxed);
    job.done->store(true, std::memory_order_release);
    // Under the lock, so a waiter between its check and its wait can't miss it.
    {
        std::lock_guard lock(queueMutex);
    }
    doneCV.notify_all();
}

// Exits on shutdown() or when the owning jthread requests stop (static destruction
// after an early return from main), so the process can never hang on join.
auto JobSystem::workerLoop(uint32_t index, std::stop_token stopToken) -> void {
    while (true) {
        Job job;
        {
            std::unique_lock lock(queueMutex);
            bool woken = false;
            while (!woken && !stopToken.stop_requested()) {
                PROFILE_ZONE_DETAIL("Idle", Wait);
                woken = queueCV.wait_for(lock, stopToken, profile::idleSlice, [] { return !frameQueue.empty() || !backgroundQueue.empty() || stopping; });
            }
            if (stopping || stopToken.stop_requested()) {
                if (frameQueue.empty() && backgroundQueue.empty()) {
                    return;
                }
            }
            // Frame jobs first: a frame waits for them, nothing waits for a background job.
            if (!frameQueue.empty()) {
                job = std::move(frameQueue.front());
                frameQueue.pop_front();
            } else if (!backgroundQueue.empty()) {
                job = std::move(backgroundQueue.front());
                backgroundQueue.pop_front();
            } else {
                return;
            }
        }
        execute(job, index);
    }
}

auto JobSystem::tryExecuteFrameJob() -> bool {
    Job job;
    {
        std::lock_guard lock(queueMutex);
        if (frameQueue.empty()) {
            return false;
        }
        job = std::move(frameQueue.front());
        frameQueue.pop_front();
    }
    execute(job, noWorker);
    return true;
}
