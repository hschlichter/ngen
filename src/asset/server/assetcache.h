#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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
// A job's secondary outputs ("<id>#<fragment>") are kept in its record, each with its packed file: they share the job's packer and
// inputs, and are up to date when those are and their own file is.
//
// Stored as text in .ngen-assets/<platform>/<config>/.ngen-assetcache, rewritten through a temporary file and a rename at most once a
// second while records change, by a thread of its own, and by flush(). A record lost to a crash in between only means that asset is
// packed again. Thread-safe.
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
        std::vector<std::pair<std::string, File>> secondaries; // id and packed file
    };

    AssetCache() = default;
    ~AssetCache();
    AssetCache(const AssetCache&) = delete;
    auto operator=(const AssetCache&) -> AssetCache& = delete;

    // Reads the cache file and starts saving changes to it.
    auto load(const std::filesystem::path& file) -> void;
    // Saves now when anything changed since the last save; the server calls it when it stops.
    auto flush() -> void;

    // How many assets have a record, secondary outputs included.
    auto size() -> size_t;

    // Every job's record, by its primary asset id, sorted.
    auto entries() -> std::vector<std::pair<std::string, Record>>;

    // The packed file's hash (the asset's version) when the asset is up to date for this job; nullopt otherwise.
    auto upToDate(const std::string& id, uint64_t jobKey) -> std::optional<uint64_t>;

    // Records a finished job; the cache is saved within a second. `inputs` are the source and the depfile's files; `secondaries` the job's
    // secondary outputs (id and file), kept in the job's record; they replace those an earlier run of the job wrote. Returns the packed file's hash, or nullopt when a file can't be read.
    auto record(const std::string& id,
                uint64_t jobKey,
                const std::string& packer,
                const std::vector<std::string>& inputs,
                const std::string& output,
                const std::vector<std::pair<std::string, std::string>>& secondaries = {}) -> std::optional<uint64_t>;

    // The packed file recorded for an asset; nullopt when it has no record.
    auto outputPath(const std::string& id) -> std::optional<std::string>;

    // The files an asset was packed from: its job's packer and inputs. Empty when it has no record.
    auto filesOf(const std::string& id) -> std::vector<std::string>;

private:
    // The record holding an asset (its own, or its job's for a secondary output) and the asset's packed file; null when none.
    auto find(const std::string& id) -> std::pair<const Record*, const File*>;
    auto save() -> void;
    auto saveLoop() -> void;

    std::mutex mutex;
    std::filesystem::path file;
    std::unordered_map<std::string, Record> records;          // by primary id
    std::unordered_map<std::string, std::string> secondaryOf; // secondary id to its job's primary id
    bool dirty = false;
    bool stopping = false;
    std::condition_variable changed;
    std::mutex saveMutex; // one save at a time
    std::thread saver;
};

// The dependencies in a Make-format depfile (every path after the first colon); empty when it can't be read.
auto parseDepfile(const std::string& path) -> std::vector<std::string>;
