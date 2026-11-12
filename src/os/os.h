#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// What the engine asks of the operating system, one implementation per OS (oslinux.cpp, oswindows.cpp): the process, its threads and
// memory, stop signals, and child processes. Standard library only in the interface; src/os/README.md.
namespace os {

// A program's file name on this OS: `ngen-view` on Linux, `ngen-view.exe` on Windows.
#if defined(_WIN32)
inline constexpr std::string_view executableSuffix = ".exe";
#else
inline constexpr std::string_view executableSuffix = "";
#endif

auto processId() -> int;
// Whether a process with this id is running (one owned by another user counts).
auto processAlive(int pid) -> bool;
// This program's file, with links resolved.
auto executablePath() -> std::expected<std::filesystem::path, std::string>;
// The process's resident memory, read from the OS on every call.
auto residentBytes() -> uint64_t;
// The calling thread's name as the OS and debuggers show it (Linux keeps 15 characters).
auto setThreadName(const char* name) -> void;

// Stop requests: SIGINT and SIGTERM on Linux; Ctrl+C, Ctrl+Break and closing the console on Windows. blockStopSignals() runs on the main
// thread before any other thread starts, so the requests reach waitForStopSignal() and no other thread; waitForStopSignal() blocks until one
// arrives and names it.
auto blockStopSignals() -> void;
auto waitForStopSignal() -> std::string;

struct ProcessResult {
    int exitCode = -1;
    // stdout and stderr together, in the order written.
    std::string output;
};

// Runs a program with arguments, no shell, and waits for it; argv[0] is a path or a name looked up on PATH. Safe to call from several
// threads at once: a child inherits only its own pipe.
auto runProcess(const std::vector<std::string>& argv) -> ProcessResult;
// Runs a program with this process's stdio and waits for it; returns its exit code, 127 when it could not start.
auto runProcessInteractive(const std::filesystem::path& program, std::span<const std::string> args) -> int;

} // namespace os
