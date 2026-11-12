#include "os.h"

#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <pthread.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace os {

namespace {

auto stopSignals() -> sigset_t {
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    return signals;
}

auto toArgv(const std::vector<std::string>& args) -> std::vector<char*> {
    std::vector<char*> argv;
    for (const auto& arg : args) {
        argv.push_back(const_cast<char*>(arg.c_str())); // NOLINT(cppcoreguidelines-pro-type-const-cast): exec does not write through them
    }
    argv.push_back(nullptr);
    return argv;
}

auto exitCodeOf(int status) -> int {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    if (WIFSIGNALED(status)) {
        return 128 + WTERMSIG(status);
    }
    return -1;
}

} // namespace

auto processId() -> int {
    return (int) getpid();
}

auto processAlive(int pid) -> bool {
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

auto executablePath() -> std::expected<std::filesystem::path, std::string> {
    std::error_code ec;
    auto path = std::filesystem::canonical("/proc/self/exe", ec);
    if (ec) {
        return std::unexpected(ec.message());
    }
    return path;
}

auto residentBytes() -> uint64_t {
    // statm: total size, then resident, in pages.
    std::ifstream in("/proc/self/statm");
    uint64_t size = 0;
    uint64_t resident = 0;
    if (!(in >> size >> resident)) {
        return 0;
    }
    return resident * (uint64_t) sysconf(_SC_PAGESIZE);
}

auto setThreadName(const char* name) -> void {
    std::array<char, 16> systemName = {};
    std::strncpy(systemName.data(), name, systemName.size() - 1);
    pthread_setname_np(pthread_self(), systemName.data());
}

auto blockStopSignals() -> void {
    auto signals = stopSignals();
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
}

auto waitForStopSignal() -> std::string {
    auto signals = stopSignals();
    int received = 0;
    sigwait(&signals, &received);
    return received == SIGINT ? "SIGINT" : "SIGTERM";
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
    auto args = toArgv(argv);
    pid_t pid = 0;
    int spawned = posix_spawnp(&pid, args[0], &actions, nullptr, args.data(), environ);
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
    result.exitCode = exitCodeOf(status);
    return result;
}

auto runProcessInteractive(const std::filesystem::path& program, std::span<const std::string> args) -> int {
    std::vector<std::string> all = {program.string()};
    all.insert(all.end(), args.begin(), args.end());
    auto argv = toArgv(all);
    auto pid = fork();
    if (pid < 0) {
        return 127;
    }
    if (pid == 0) {
        execv(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return 127;
        }
    }
    return exitCodeOf(status);
}

} // namespace os
