#include "jobsystem.h"

#include <algorithm>

std::vector<std::jthread> JobSystem::workers;
std::deque<JobSystem::Job> JobSystem::queue;
std::mutex JobSystem::queueMutex;
std::condition_variable JobSystem::queueCV;
std::condition_variable JobSystem::doneCV;
bool JobSystem::stopping = false;

auto JobSystem::init(uint32_t numWorkers) -> void {
    if (numWorkers == 0) {
        numWorkers = std::max(1u, std::thread::hardware_concurrency() - 1);
    }

    for (uint32_t i = 0; i < numWorkers; i++) {
        workers.emplace_back([] { workerLoop(); });
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

auto JobSystem::submit(JobFunc job) -> JobFence {
    auto done = std::make_shared<std::atomic<bool>>(false);

    {
        std::lock_guard lock(queueMutex);
        queue.push_back({std::move(job), done});
    }
    queueCV.notify_one();

    JobFence fence;
    fence.done = done;
    return fence;
}

auto JobSystem::wait(const JobFence& fence) -> void {
    if (!fence.done) {
        return;
    }

    while (!fence.done->load(std::memory_order_acquire)) {
        if (!tryExecuteOne()) {
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

auto JobSystem::workerLoop() -> void {
    while (true) {
        Job job;
        {
            std::unique_lock lock(queueMutex);
            queueCV.wait(lock, [] { return !queue.empty() || stopping; });
            if (stopping && queue.empty()) {
                return;
            }
            job = std::move(queue.front());
            queue.pop_front();
        }
        job.func();
        job.done->store(true, std::memory_order_release);
        doneCV.notify_all();
    }
}

auto JobSystem::tryExecuteOne() -> bool {
    Job job;
    {
        std::lock_guard lock(queueMutex);
        if (queue.empty()) {
            return false;
        }
        job = std::move(queue.front());
        queue.pop_front();
    }
    job.func();
    job.done->store(true, std::memory_order_release);
    doneCV.notify_all();
    return true;
}
