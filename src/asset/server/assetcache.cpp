#include "assetcache.h"

#include "assethash.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace {

auto stat(const std::string& path, int64_t& size, int64_t& mtimeNs) -> bool {
    std::error_code ec;
    auto fileSize = fs::file_size(path, ec);
    if (ec) {
        return false;
    }
    auto time = fs::last_write_time(path, ec);
    if (ec) {
        return false;
    }
    size = (int64_t) fileSize;
    mtimeNs = (int64_t) std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    return true;
}

// The file as it is now; nullopt when it can't be read.
auto current(const std::string& path) -> std::optional<AssetCache::File> {
    AssetCache::File file;
    file.path = path;
    if (!stat(path, file.size, file.mtimeNs)) {
        return std::nullopt;
    }
    auto hash = hashFile(path);
    if (!hash) {
        return std::nullopt;
    }
    file.hash = *hash;
    return file;
}

// Whether a recorded file still has the same contents: the stat fast path first, then the hash.
auto unchanged(const AssetCache::File& recorded) -> bool {
    int64_t size = 0;
    int64_t mtimeNs = 0;
    if (!stat(recorded.path, size, mtimeNs)) {
        return false;
    }
    if (size == recorded.size && mtimeNs == recorded.mtimeNs) {
        return true;
    }
    auto hash = hashFile(recorded.path);
    return hash.has_value() && *hash == recorded.hash;
}

auto writeFields(std::ostream& out, const AssetCache::File& file) -> void {
    out << file.path << '\t' << file.size << '\t' << file.mtimeNs << '\t' << hashText(file.hash) << '\n';
}

auto writeFile(std::ostream& out, const char* tag, const AssetCache::File& file) -> void {
    out << tag << '\t';
    writeFields(out, file);
}

auto readFile(std::istringstream& fields, AssetCache::File& file) -> bool {
    std::string size;
    std::string mtime;
    std::string hash;
    if (!std::getline(fields, file.path, '\t') || !std::getline(fields, size, '\t') || !std::getline(fields, mtime, '\t') ||
        !std::getline(fields, hash, '\t')) {
        return false;
    }
    try {
        file.size = std::stoll(size);
        file.mtimeNs = std::stoll(mtime);
        file.hash = std::stoull(hash, nullptr, 16);
    } catch (...) {
        return false;
    }
    return true;
}

} // namespace

AssetCache::~AssetCache() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (saver.joinable()) {
        saver.join();
    }
    flush();
}

auto AssetCache::load(const fs::path& path) -> void {
    {
        std::lock_guard lock(mutex);
        file = path;
        records.clear();
        secondaryOf.clear();
        std::ifstream in(path);
        std::string line;
        // Version 1 kept a record per secondary output; its records are dropped, and their assets packed again.
        if (std::getline(in, line) && line == "ngen-assetcache 2") {
            Record* record = nullptr;
            std::string recordId;
            while (std::getline(in, line)) {
                std::istringstream fields(line);
                std::string tag;
                std::getline(fields, tag, '\t');
                if (tag == "asset") {
                    std::string key;
                    std::getline(fields, recordId, '\t');
                    std::getline(fields, key, '\t');
                    record = &records[recordId];
                    *record = Record{};
                    try {
                        record->jobKey = std::stoull(key, nullptr, 16);
                    } catch (...) {
                        records.erase(recordId);
                        record = nullptr;
                    }
                    continue;
                }
                if (record == nullptr) {
                    continue;
                }
                std::string secondaryId;
                if (tag == "secondary" && !std::getline(fields, secondaryId, '\t')) {
                    continue;
                }
                File entry;
                if (!readFile(fields, entry)) {
                    continue;
                }
                if (tag == "packer") {
                    record->packer = entry;
                } else if (tag == "input") {
                    record->inputs.push_back(entry);
                } else if (tag == "output") {
                    record->output = entry;
                } else if (tag == "secondary") {
                    record->secondaries.emplace_back(secondaryId, entry);
                    secondaryOf[secondaryId] = recordId;
                }
            }
        }
    }
    if (!saver.joinable()) {
        saver = std::thread([this] { saveLoop(); });
    }
}

auto AssetCache::flush() -> void {
    save();
}

auto AssetCache::size() -> size_t {
    std::lock_guard lock(mutex);
    return records.size() + secondaryOf.size();
}

auto AssetCache::entries() -> std::vector<std::pair<std::string, Record>> {
    std::vector<std::pair<std::string, Record>> result;
    {
        std::lock_guard lock(mutex);
        result.assign(records.begin(), records.end());
    }
    std::ranges::sort(result, [](const auto& a, const auto& b) { return a.first < b.first; });
    return result;
}

auto AssetCache::find(const std::string& id) -> std::pair<const Record*, const File*> {
    if (auto it = records.find(id); it != records.end()) {
        return {&it->second, &it->second.output};
    }
    auto owner = secondaryOf.find(id);
    if (owner == secondaryOf.end()) {
        return {nullptr, nullptr};
    }
    const Record& record = records.at(owner->second);
    for (const auto& [secondaryId, secondaryFile] : record.secondaries) {
        if (secondaryId == id) {
            return {&record, &secondaryFile};
        }
    }
    return {nullptr, nullptr};
}

auto AssetCache::upToDate(const std::string& id, uint64_t jobKey) -> std::optional<uint64_t> {
    File packer;
    File output;
    std::vector<File> inputs;
    {
        std::lock_guard lock(mutex);
        auto [record, outputFile] = find(id);
        if (record == nullptr || record->jobKey != jobKey) {
            return std::nullopt;
        }
        packer = record->packer;
        output = *outputFile;
        inputs = record->inputs;
    }
    if (!unchanged(packer) || !unchanged(output)) {
        return std::nullopt;
    }
    for (const auto& input : inputs) {
        if (!unchanged(input)) {
            return std::nullopt;
        }
    }
    return output.hash;
}

auto AssetCache::record(const std::string& id,
                        uint64_t jobKey,
                        const std::string& packer,
                        const std::vector<std::string>& inputs,
                        const std::string& output,
                        const std::vector<std::pair<std::string, std::string>>& secondaries) -> std::optional<uint64_t> {
    Record record;
    record.jobKey = jobKey;
    auto packerFile = current(packer);
    auto outputFile = current(output);
    if (!packerFile || !outputFile) {
        return std::nullopt;
    }
    record.packer = *packerFile;
    record.output = *outputFile;
    for (const auto& path : inputs) {
        auto input = current(path);
        if (!input) {
            return std::nullopt;
        }
        record.inputs.push_back(*input);
    }
    for (const auto& [secondaryId, secondaryFile] : secondaries) {
        auto secondaryOutput = current(secondaryFile);
        if (!secondaryOutput) {
            return std::nullopt;
        }
        record.secondaries.emplace_back(secondaryId, *secondaryOutput);
    }
    auto version = record.output.hash;
    {
        std::lock_guard lock(mutex);
        if (auto old = records.find(id); old != records.end()) {
            for (const auto& [secondaryId, secondaryFile] : old->second.secondaries) {
                secondaryOf.erase(secondaryId);
            }
        }
        for (const auto& [secondaryId, secondaryFile] : record.secondaries) {
            secondaryOf[secondaryId] = id;
        }
        records[id] = std::move(record);
        dirty = true;
    }
    changed.notify_all();
    return version;
}

auto AssetCache::outputPath(const std::string& id) -> std::optional<std::string> {
    std::lock_guard lock(mutex);
    auto [record, outputFile] = find(id);
    if (record == nullptr) {
        return std::nullopt;
    }
    return outputFile->path;
}

// Saves once a second at most while records change.
auto AssetCache::saveLoop() -> void {
    for (;;) {
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return dirty || stopping; });
            if (stopping) {
                return;
            }
        }
        save();
        std::unique_lock lock(mutex);
        changed.wait_for(lock, std::chrono::seconds(1), [this] { return stopping; });
        if (stopping) {
            return;
        }
    }
}

auto AssetCache::save() -> void {
    std::lock_guard saving(saveMutex);
    std::unordered_map<std::string, Record> snapshot;
    fs::path path;
    {
        std::lock_guard lock(mutex);
        if (!dirty || file.empty()) {
            return;
        }
        snapshot = records;
        path = file;
        dirty = false;
    }
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << "ngen-assetcache 2\n";
        for (const auto& [id, record] : snapshot) {
            out << "asset\t" << id << '\t' << hashText(record.jobKey) << '\n';
            writeFile(out, "packer", record.packer);
            for (const auto& input : record.inputs) {
                writeFile(out, "input", input);
            }
            writeFile(out, "output", record.output);
            for (const auto& [secondaryId, secondaryFile] : record.secondaries) {
                out << "secondary\t" << secondaryId << '\t';
                writeFields(out, secondaryFile);
            }
        }
        if (!out) {
            std::lock_guard lock(mutex);
            dirty = true;
            return;
        }
    }
    fs::rename(temporary, path, ec);
}

auto parseDepfile(const std::string& path) -> std::vector<std::string> {
    std::ifstream in(path);
    std::stringstream buffer;
    buffer << in.rdbuf();
    auto text = buffer.str();
    std::vector<std::string> deps;
    auto colon = text.find(':');
    if (colon == std::string::npos) {
        return deps;
    }
    std::string current;
    for (size_t i = colon + 1; i < text.size(); i++) {
        char ch = text[i];
        if (ch == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == '\n') {
                i++;
                continue;
            }
            if (next == ' ' || next == '\\' || next == '#') {
                current += next;
                i++;
                continue;
            }
        }
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') {
            if (!current.empty()) {
                deps.push_back(current);
                current.clear();
            }
            continue;
        }
        current += ch;
    }
    if (!current.empty()) {
        deps.push_back(current);
    }
    return deps;
}
