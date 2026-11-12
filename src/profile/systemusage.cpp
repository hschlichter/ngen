#include "profile.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>

// What the system reports about the process and the GPU, read from /proc and sysfs at most every 100 ms: the readings change slowly and a
// read is a file open.

namespace profile {

namespace {

constexpr uint64_t readIntervalNs = 100'000'000;

auto readNumber(const std::filesystem::path& path) -> std::optional<uint64_t> {
    std::ifstream in(path);
    std::string text;
    if (!(in >> text)) {
        return std::nullopt;
    }
    // sysfs IDs are hexadecimal with a 0x prefix; counters are decimal.
    int base = 10;
    auto begin = text.data();
    if (text.starts_with("0x")) {
        base = 16;
        begin += 2;
    }
    uint64_t value = 0;
    auto [end, error] = std::from_chars(begin, text.data() + text.size(), value, base);
    if (error != std::errc()) {
        return std::nullopt;
    }
    return value;
}

struct GpuDriver {
    std::mutex mutex;
    bool deviceSet = false;
    bool searched = false;
    std::filesystem::path busyFile; // empty when no card matched
    std::string status = "no GPU device given yet";
    uint32_t vendorId = 0;
    uint32_t deviceId = 0;
    uint64_t readNs = 0;
    std::optional<double> busy;
};
GpuDriver g_gpuDriver;

// The DRM card whose PCI vendor and device match and that reports gpu_busy_percent (amdgpu). Several matching: none is picked.
auto findBusyFile(GpuDriver& driver) -> void {
    driver.searched = true;
    std::error_code ec;
    std::vector<std::filesystem::path> matches;
    for (const auto& entry : std::filesystem::directory_iterator("/sys/class/drm", ec)) {
        auto name = entry.path().filename().string();
        if (!name.starts_with("card") || name.contains('-')) {
            continue;
        }
        auto device = entry.path() / "device";
        auto vendor = readNumber(device / "vendor");
        auto id = readNumber(device / "device");
        if (!vendor || !id || *vendor != driver.vendorId || *id != driver.deviceId) {
            continue;
        }
        if (std::filesystem::exists(device / "gpu_busy_percent", ec)) {
            matches.push_back(device / "gpu_busy_percent");
        }
    }
    if (matches.size() == 1) {
        driver.busyFile = matches.front();
        driver.status = driver.busyFile.string();
    } else if (matches.empty()) {
        driver.status = "no DRM card with this GPU's PCI IDs reports gpu_busy_percent (amdgpu only)";
    } else {
        driver.status = "several DRM cards match this GPU's PCI IDs; none is picked";
    }
}

struct Resident {
    std::mutex mutex;
    uint64_t readNs = 0;
    uint64_t bytes = 0;
};
Resident g_resident;

} // namespace

auto setGpuDevice(uint32_t vendorId, uint32_t deviceId) -> void {
    std::lock_guard lock(g_gpuDriver.mutex);
    g_gpuDriver.deviceSet = true;
    g_gpuDriver.searched = false;
    g_gpuDriver.busyFile.clear();
    g_gpuDriver.vendorId = vendorId;
    g_gpuDriver.deviceId = deviceId;
    g_gpuDriver.busy.reset();
    g_gpuDriver.readNs = 0;
}

auto gpuDriverBusyPercent() -> std::optional<double> {
    std::lock_guard lock(g_gpuDriver.mutex);
    if (!g_gpuDriver.deviceSet) {
        return std::nullopt;
    }
    if (!g_gpuDriver.searched) {
        findBusyFile(g_gpuDriver);
    }
    if (g_gpuDriver.busyFile.empty()) {
        return std::nullopt;
    }
    auto t = now();
    if (t - g_gpuDriver.readNs >= readIntervalNs) {
        g_gpuDriver.readNs = t;
        PROFILE_ZONE_DETAIL("ReadGpuBusyFile", Io);
        if (auto value = readNumber(g_gpuDriver.busyFile)) {
            g_gpuDriver.busy = (double) *value;
        }
    }
    return g_gpuDriver.busy;
}

auto gpuDriverBusyStatus() -> std::string {
    std::lock_guard lock(g_gpuDriver.mutex);
    return g_gpuDriver.status;
}

auto residentBytes() -> uint64_t {
    std::lock_guard lock(g_resident.mutex);
    auto t = now();
    if (g_resident.readNs != 0 && t - g_resident.readNs < readIntervalNs) {
        return g_resident.bytes;
    }
    g_resident.readNs = t;
    PROFILE_ZONE_DETAIL("ReadStatm", Io);
    // statm: total size, then resident, in pages.
    std::ifstream in("/proc/self/statm");
    uint64_t size = 0;
    uint64_t resident = 0;
    if (in >> size >> resident) {
        g_resident.bytes = resident * (uint64_t) sysconf(_SC_PAGESIZE);
    }
    return g_resident.bytes;
}

} // namespace profile
