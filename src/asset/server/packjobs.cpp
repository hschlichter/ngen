#include "packjobs.h"

#include "profile.h"

PackJobs::PackJobs(unsigned workers) {
    for (unsigned i = 0; i < workers; i++) {
        threads.emplace_back([this, i] { work(i); });
    }
}

PackJobs::~PackJobs() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    wake.notify_all();
    for (auto& thread : threads) {
        thread.join();
    }
}

auto PackJobs::submit(std::function<void()> task) -> void {
    {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
    }
    wake.notify_one();
}

auto PackJobs::running() -> size_t {
    std::lock_guard lock(mutex);
    return active;
}

auto PackJobs::queued() -> size_t {
    std::lock_guard lock(mutex);
    return tasks.size();
}

auto PackJobs::work(unsigned index) -> void {
    auto name = "Pack " + std::to_string(index);
    profile::registerThread(name.c_str());
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex);
            bool woken = false;
            while (!woken) {
                PROFILE_ZONE_DETAIL("Idle", Wait);
                woken = wake.wait_for(lock, profile::idleSlice, [this] { return stopping || !tasks.empty(); });
            }
            if (stopping && tasks.empty()) {
                return;
            }
            task = std::move(tasks.front());
            tasks.pop_front();
            active++;
        }
        task();
        std::lock_guard lock(mutex);
        active--;
    }
}
