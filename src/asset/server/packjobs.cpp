#include "packjobs.h"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

PackJobs::PackJobs(unsigned workers) {
    for (unsigned i = 0; i < workers; i++) {
        threads.emplace_back([this] { work(); });
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

auto PackJobs::work() -> void {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex);
            wake.wait(lock, [this] { return stopping || !tasks.empty(); });
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

auto runProcess(const std::vector<std::string>& argv) -> ProcessResult {
    ProcessResult result;
    std::array<int, 2> pipeFds = {-1, -1};
    if (pipe2(pipeFds.data(), O_CLOEXEC) != 0) {
        result.output = "cannot create a pipe";
        return result;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDERR_FILENO);
    std::vector<char*> args;
    for (const auto& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    pid_t pid = 0;
    int spawned = posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (spawned != 0) {
        close(pipeFds[0]);
        result.output = "cannot run " + argv[0];
        return result;
    }
    std::array<char, 4096> buffer = {};
    while (true) {
        auto count = read(pipeFds[0], buffer.data(), buffer.size());
        if (count > 0) {
            result.output.append(buffer.data(), (size_t) count);
            continue;
        }
        if (count < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(pipeFds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
