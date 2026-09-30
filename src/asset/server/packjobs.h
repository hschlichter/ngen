#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// A fixed number of worker threads running pack tasks in submission order. A task checks the cache, runs the
// packer through runProcess when needed, and reports its result on the worker thread.
class PackJobs {
public:
    explicit PackJobs(unsigned workers);
    PackJobs(const PackJobs&) = delete;
    auto operator=(const PackJobs&) -> PackJobs& = delete;
    ~PackJobs();

    auto submit(std::function<void()> task) -> void;

    auto running() -> size_t;
    auto queued() -> size_t;

private:
    auto work() -> void;

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> tasks;
    std::vector<std::thread> threads;
    size_t active = 0;
    bool stopping = false;
};

struct ProcessResult {
    int exitCode = -1;
    // stdout and stderr together, in the order written.
    std::string output;
};

// Runs a program with arguments, no shell, and waits for it.
auto runProcess(const std::vector<std::string>& argv) -> ProcessResult;
