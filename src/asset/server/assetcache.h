#pragma once

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

// What each packed asset was packed from, so the asset server knows when it is up to date.
//
// Per asset id the cache records the job key (rule name, version, parameters and packer), and the size,
// modification time and content hash of the packer binary, of every input (the source and every file in the
// packer's depfile) and of the packed file. An asset is up to date when its record's job key matches and every
// recorded file still hashes the same. A file whose size and modification time are unchanged is taken as
// unchanged without re-hashing it.
//
// Stored as text in <out_dir>/assets/.ngen-assetcache, rewritten through a temporary file and a rename. Thread-safe.
class AssetCache {
public:
    struct File {
        std::string path;
        int64_t size = 0;
        int64_t mtimeNs = 0;
        uint64_t hash = 0;
    };

    struct Record {
        uint64_t jobKey = 0;
        File packer;
        std::vector<File> inputs;
        File output;
    };

    auto load(const std::filesystem::path& file) -> void;

    // How many assets have a record.
    auto size() -> size_t;

    // The packed file's hash (the asset's version) when the asset is up to date for this job; nullopt otherwise.
    auto upToDate(const std::string& id, uint64_t jobKey) -> std::optional<uint64_t>;

    // Records a finished job and saves the cache. `inputs` are the source and the depfile's files. Returns the
    // packed file's hash, or nullopt when a file can't be read.
    auto record(const std::string& id, uint64_t jobKey, const std::string& packer, const std::vector<std::string>& inputs, const std::string& output)
        -> std::optional<uint64_t>;

private:
    auto save() -> void;

    std::mutex mutex;
    std::filesystem::path file;
    std::unordered_map<std::string, Record> records;
};

// The dependencies in a Make-format depfile (every path after the first colon); empty when it can't be read.
auto parseDepfile(const std::string& path) -> std::vector<std::string>;
