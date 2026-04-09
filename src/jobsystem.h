#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <thread>
#include <vector>

using JobFunc = std::function<void()>;

class JobFence {
    friend class JobSystem;
    std::shared_ptr<std::atomic<bool>> done;

public:
    auto ready() const -> bool { return done && done->load(std::memory_order_acquire); }
};

class JobSystem {
public:
    static auto init(uint32_t numWorkers = 0) -> void;
    static auto shutdown() -> void;

    static auto submit(JobFunc job) -> JobFence;

    static auto wait(const JobFence& fence) -> void;
    static auto waitAll(std::span<const JobFence> fences) -> void;

private:
    struct Job {
        JobFunc func;
        std::shared_ptr<std::atomic<bool>> done;
    };

    static auto workerLoop() -> void;
    static auto tryExecuteOne() -> bool;

    static std::vector<std::jthread> workers;
    static std::deque<Job> queue;
    static std::mutex queueMutex;
    static std::condition_variable queueCV;
    static std::condition_variable doneCV;
    static bool stopping;
};
