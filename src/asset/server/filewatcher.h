#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// Reports files that changed in watched directories. A directory is watched, not each file, so a save that replaces a file through a rename
// is seen, and many files in one directory cost one watch. Only the files asked for are reported.
//
// Linux: inotify on each directory; a directory inotify can't watch (out of watches) is polled instead, by the size and modification time of
// its files once a second. Changes are collected for `settle` after the first one and reported together, so an editor's truncate, write and
// rename is one report. Thread-safe; `changed` is called on the watcher's own thread, never with the watcher's lock held.
class FileWatcher {
public:
    using Changed = std::function<void(std::vector<std::filesystem::path> files)>;
    using Warned = std::function<void(const std::string& message)>;

    static constexpr auto settle = std::chrono::milliseconds(100);
    static constexpr auto pollInterval = std::chrono::seconds(1);

    FileWatcher() = default;
    FileWatcher(const FileWatcher&) = delete;
    auto operator=(const FileWatcher&) -> FileWatcher& = delete;
    ~FileWatcher();

    // `pollOnly` polls every directory instead of using inotify.
    auto start(Changed changed, Warned warned, bool pollOnly) -> void;
    auto stop() -> void;

    // Watches the directory of each file. Paths are taken as given (relative to the working directory, or absolute).
    auto watch(std::span<const std::filesystem::path> files) -> void;
    // Stops reporting these files; a directory with no file left is no longer watched.
    auto unwatch(std::span<const std::filesystem::path> files) -> void;

    auto watchedDirectories() const -> size_t;
    auto polledDirectories() const -> size_t;

private:
    struct Stamp {
        bool exists = false;
        int64_t size = 0;
        int64_t mtimeNs = 0;

        auto operator==(const Stamp&) const -> bool = default;
    };
    struct Directory {
        int watch = -1;                               // the inotify watch, -1 when polled
        std::unordered_map<std::string, Stamp> files; // by file name; the stamp is only kept while polled
    };

    static auto stampOf(const std::filesystem::path& file) -> Stamp;
    auto run() -> void;
    // Reads the inotify events waiting; adds the watched files they name to `pending`.
    auto readEvents() -> void;
    // Stats the files of every polled directory; adds those that changed to `pending`.
    auto pollDirectories() -> void;
    auto addPending(const std::filesystem::path& file) -> void;

    Changed changed;
    Warned warned;
    bool pollOnly = false;
    int inotifyFd = -1;
    int wakeFd = -1; // an eventfd that stop() writes, so the thread's poll() returns
    std::thread thread;

    mutable std::mutex mutex;
    std::unordered_map<std::string, Directory> directories; // by directory path
    std::unordered_map<int, std::string> directoryOfWatch;
    std::set<std::filesystem::path> pending;
    std::chrono::steady_clock::time_point firstPending;
    std::chrono::steady_clock::time_point lastPoll;
};
