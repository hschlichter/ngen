#include "assetcache.h"

#include "assethash.h"

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

auto writeFile(std::ostream& out, const char* tag, const AssetCache::File& file) -> void {
    out << tag << '\t' << file.path << '\t' << file.size << '\t' << file.mtimeNs << '\t' << hashText(file.hash) << '\n';
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

auto AssetCache::load(const fs::path& path) -> void {
    std::lock_guard lock(mutex);
    file = path;
    records.clear();
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "ngen-assetcache 1") {
        return;
    }
    Record* record = nullptr;
    while (std::getline(in, line)) {
        std::istringstream fields(line);
        std::string tag;
        std::getline(fields, tag, '\t');
        if (tag == "asset") {
            std::string id;
            std::string key;
            std::getline(fields, id, '\t');
            std::getline(fields, key, '\t');
            record = &records[id];
            *record = Record{};
            try {
                record->jobKey = std::stoull(key, nullptr, 16);
            } catch (...) {
                records.erase(id);
                record = nullptr;
            }
            continue;
        }
        if (record == nullptr) {
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
        }
    }
}

auto AssetCache::upToDate(const std::string& id, uint64_t jobKey) -> std::optional<uint64_t> {
    Record record;
    {
        std::lock_guard lock(mutex);
        auto it = records.find(id);
        if (it == records.end() || it->second.jobKey != jobKey) {
            return std::nullopt;
        }
        record = it->second;
    }
    if (!unchanged(record.packer) || !unchanged(record.output)) {
        return std::nullopt;
    }
    for (const auto& input : record.inputs) {
        if (!unchanged(input)) {
            return std::nullopt;
        }
    }
    return record.output.hash;
}

auto AssetCache::record(const std::string& id, uint64_t jobKey, const std::string& packer, const std::vector<std::string>& inputs, const std::string& output)
    -> std::optional<uint64_t> {
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
    auto version = record.output.hash;
    std::lock_guard lock(mutex);
    records[id] = std::move(record);
    save();
    return version;
}

auto AssetCache::save() -> void {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    auto temporary = file;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::trunc);
        out << "ngen-assetcache 1\n";
        for (const auto& [id, record] : records) {
            out << "asset\t" << id << '\t' << hashText(record.jobKey) << '\n';
            writeFile(out, "packer", record.packer);
            for (const auto& input : record.inputs) {
                writeFile(out, "input", input);
            }
            writeFile(out, "output", record.output);
        }
        if (!out) {
            return;
        }
    }
    fs::rename(temporary, file, ec);
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
