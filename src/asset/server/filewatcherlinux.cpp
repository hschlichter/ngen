#include "filewatcher.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <format>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t watchMask = IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE | IN_MOVED_FROM;

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

auto FileWatcher::start(Changed changedCallback, Warned warnedCallback, bool pollOnlyMode) -> void {
    changed = std::move(changedCallback);
    warned = std::move(warnedCallback);
    pollOnly = pollOnlyMode;
    if (!pollOnly) {
        inotifyFd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotifyFd < 0) {
            warned(std::format("inotify unavailable ({}): every directory is polled", std::strerror(errno)));
            pollOnly = true;
        }
    }
    wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    lastPoll = std::chrono::steady_clock::now();
    thread = std::thread([this] { run(); });
}

auto FileWatcher::stop() -> void {
    if (!thread.joinable()) {
        return;
    }
    uint64_t one = 1;
    auto written = write(wakeFd, &one, sizeof(one));
    (void) written;
    thread.join();
    close(wakeFd);
    wakeFd = -1;
    if (inotifyFd >= 0) {
        close(inotifyFd);
        inotifyFd = -1;
    }
    std::lock_guard lock(mutex);
    directories.clear();
    directoryOfWatch.clear();
    pending.clear();
}

auto FileWatcher::stampOf(const fs::path& file) -> Stamp {
    struct stat info = {};
    if (stat(file.c_str(), &info) != 0) {
        return {};
    }
    return {
        .exists = true,
        .size = (int64_t) info.st_size,
        .mtimeNs = ((int64_t) info.st_mtim.tv_sec * 1000000000) + (int64_t) info.st_mtim.tv_nsec,
    };
}

auto FileWatcher::watch(std::span<const fs::path> files) -> void {
    std::lock_guard lock(mutex);
    for (const auto& file : files) {
        auto directoryPath = directoryOf(file);
        auto name = nameOf(file);
        auto [it, inserted] = directories.try_emplace(directoryPath);
        auto& directory = it->second;
        if (inserted && !pollOnly) {
            auto watchPath = directoryPath.empty() ? std::string(".") : directoryPath;
            directory.watch = inotify_add_watch(inotifyFd, watchPath.c_str(), watchMask);
            if (directory.watch >= 0) {
                directoryOfWatch[directory.watch] = directoryPath;
            } else if (errno == ENOSPC) {
                warned(std::format("out of inotify watches: {} is polled once a second (raise fs.inotify.max_user_watches)", watchPath));
            } else {
                warned(std::format("cannot watch {} ({}): it is polled once a second", watchPath, std::strerror(errno)));
            }
        }
        if (!directory.files.contains(name)) {
            auto stamp = directory.watch < 0 ? stampOf(file) : Stamp{};
            directory.files.emplace(name, stamp);
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
        if (!it->second.files.empty()) {
            continue;
        }
        if (it->second.watch >= 0) {
            inotify_rm_watch(inotifyFd, it->second.watch);
            directoryOfWatch.erase(it->second.watch);
        }
        directories.erase(it);
    }
}

auto FileWatcher::watchedDirectories() const -> size_t {
    std::lock_guard lock(mutex);
    return directoryOfWatch.size();
}

auto FileWatcher::polledDirectories() const -> size_t {
    std::lock_guard lock(mutex);
    return directories.size() - directoryOfWatch.size();
}

auto FileWatcher::addPending(const fs::path& file) -> void {
    if (pending.empty()) {
        firstPending = std::chrono::steady_clock::now();
    }
    pending.insert(file);
}

auto FileWatcher::readEvents() -> void {
    // Aligned for inotify_event; one read returns as many whole events as fit.
    alignas(inotify_event) std::array<char, 64 * 1024> buffer;
    while (true) {
        auto bytes = read(inotifyFd, buffer.data(), buffer.size());
        if (bytes <= 0) {
            return;
        }
        std::lock_guard lock(mutex);
        for (ssize_t offset = 0; offset < bytes;) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            offset += (ssize_t) (sizeof(inotify_event) + event->len);
            if (event->len == 0) {
                continue;
            }
            auto watchIt = directoryOfWatch.find(event->wd);
            if (watchIt == directoryOfWatch.end()) {
                continue;
            }
            const auto& directory = directories[watchIt->second];
            std::string name(event->name);
            if (directory.files.contains(name)) {
                addPending(fileIn(watchIt->second, name));
            }
        }
    }
}

auto FileWatcher::pollDirectories() -> void {
    std::lock_guard lock(mutex);
    for (auto& [directoryPath, directory] : directories) {
        if (directory.watch >= 0) {
            continue;
        }
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
    while (true) {
        std::array<pollfd, 2> fds = {{
            {.fd = wakeFd, .events = POLLIN, .revents = 0},
            {.fd = inotifyFd, .events = POLLIN, .revents = 0},
        }};
        auto count = inotifyFd >= 0 ? 2 : 1;
        ::poll(fds.data(), (nfds_t) count, (int) settle.count());
        if ((fds[0].revents & POLLIN) != 0) {
            return;
        }
        if (count == 2 && (fds[1].revents & POLLIN) != 0) {
            readEvents();
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
