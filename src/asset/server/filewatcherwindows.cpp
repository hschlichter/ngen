#include "filewatcher.h"

#include "profile.h"

#include <chrono>

namespace fs = std::filesystem;

// Windows: every directory is polled, by the size and modification time of its watched files once a second, as Linux does with
// --watch=poll.

namespace {

// The directory a file is watched through: its parent, "" for a file in the working directory.
auto directoryOf(const fs::path& file) -> std::string {
    return file.lexically_normal().parent_path().string();
}

auto nameOf(const fs::path& file) -> std::string {
    return file.lexically_normal().filename().string();
}

// A file of a directory as the watcher reports it: the form watch() was given, made normal.
auto fileIn(const std::string& directory, const std::string& name) -> fs::path {
    if (directory.empty()) {
        return fs::path(name);
    }
    return fs::path(directory) / name;
}

} // namespace

FileWatcher::~FileWatcher() {
    stop();
}

auto FileWatcher::start(Changed changedCallback, Warned warnedCallback, bool /*pollOnlyMode*/) -> void {
    changed = std::move(changedCallback);
    warned = std::move(warnedCallback);
    pollOnly = true;
    {
        std::lock_guard lock(mutex);
        stopRequested = false;
    }
    lastPoll = std::chrono::steady_clock::now();
    thread = std::thread([this] { run(); });
}

auto FileWatcher::stop() -> void {
    if (!thread.joinable()) {
        return;
    }
    {
        std::lock_guard lock(mutex);
        stopRequested = true;
    }
    stopSignal.notify_all();
    thread.join();
    std::lock_guard lock(mutex);
    directories.clear();
    directoryOfWatch.clear();
    pending.clear();
}

auto FileWatcher::stampOf(const fs::path& file) -> Stamp {
    std::error_code ec;
    auto size = fs::file_size(file, ec);
    if (ec) {
        return {};
    }
    auto modified = fs::last_write_time(file, ec);
    if (ec) {
        return {};
    }
    return {
        .exists = true,
        .size = (int64_t) size,
        .mtimeNs = (int64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(modified.time_since_epoch()).count(),
    };
}

auto FileWatcher::watch(std::span<const fs::path> files) -> void {
    std::lock_guard lock(mutex);
    for (const auto& file : files) {
        auto& directory = directories[directoryOf(file)];
        auto name = nameOf(file);
        if (!directory.files.contains(name)) {
            directory.files.emplace(name, stampOf(file));
        }
    }
}

auto FileWatcher::unwatch(std::span<const fs::path> files) -> void {
    std::lock_guard lock(mutex);
    for (const auto& file : files) {
        auto it = directories.find(directoryOf(file));
        if (it == directories.end()) {
            continue;
        }
        it->second.files.erase(nameOf(file));
        if (it->second.files.empty()) {
            directories.erase(it);
        }
    }
}

auto FileWatcher::watchedDirectories() const -> size_t {
    return 0;
}

auto FileWatcher::polledDirectories() const -> size_t {
    std::lock_guard lock(mutex);
    return directories.size();
}

auto FileWatcher::addPending(const fs::path& file) -> void {
    if (pending.empty()) {
        firstPending = std::chrono::steady_clock::now();
    }
    pending.insert(file);
}

auto FileWatcher::readEvents() -> void {
}

auto FileWatcher::pollDirectories() -> void {
    std::lock_guard lock(mutex);
    for (auto& [directoryPath, directory] : directories) {
        for (auto& [name, stamp] : directory.files) {
            auto file = fileIn(directoryPath, name);
            auto now = stampOf(file);
            if (now != stamp) {
                stamp = now;
                addPending(file);
            }
        }
    }
}

auto FileWatcher::run() -> void {
    profile::registerThread("File Watcher");
    while (true) {
        {
            std::unique_lock lock(mutex);
            if (stopSignal.wait_for(lock, settle, [this] { return stopRequested; })) {
                return;
            }
        }
        auto now = std::chrono::steady_clock::now();
        if (now - lastPoll >= pollInterval) {
            lastPoll = now;
            pollDirectories();
        }
        std::vector<fs::path> report;
        {
            std::lock_guard lock(mutex);
            if (!pending.empty() && now - firstPending >= settle) {
                report.assign(pending.begin(), pending.end());
                pending.clear();
            }
        }
        if (!report.empty()) {
            changed(std::move(report));
        }
    }
}
