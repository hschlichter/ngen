// Persistent dirty-detection state for the runner.
//
// One binary file per build variant at `_out/<plat>/<cfg>/.ngen-buildlog`, plus `_out/.system/.ngen-buildlog`
// for the build-system self-build. The format is a small bespoke layout — magic `NGBL`, format version, then
// a flat list of length-prefixed entries:
//
//     edge_name (length-prefixed string)
//     command_hash (u64)
//     last_run_ns (u64)
//     inputs[]              : TrackedFile records (path + StatTuple + content_hash)
//     outputs[]             : TrackedFile records
//     discovered_headers[]  : TrackedFile records (from the last successful depfile parse)
//
// `BuildLog::load` reads from disk if present; missing or version-mismatched files are tolerated (the log
// just starts empty so the next build is a clean one). `BuildLog::save` writes to a `.tmp` file and renames —
// atomic, so an interrupted build keeps the previous good log on disk.
//
// `refresh(TrackedFile&, prev, present)` is the read-modify-write helper paired with the stat fast-path: if
// the on-disk `(size, mtime, ctime)` matches the prior entry, reuse the cached `content_hash`; otherwise
// re-hash. Used both for pre-run dirty checks (`execute.hpp::compute_dirty`) and post-run log updates.
//
// `find(name)` and `upsert(name, entry)` are the operations the dirty-detection code uses. Entries are keyed
// by edge name; edge names are unique within a single variant's IR.

#pragma once

#include "../framework/glob.hpp"
#include "../framework/path.hpp"
#include "hash.hpp"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ngen::run {

struct TrackedFile {
    std::string path;
    StatTuple stat;
    std::uint64_t content_hash = 0;
};

struct LogEntry {
    std::uint64_t command_hash = 0;
    std::uint64_t last_run_ns = 0;
    std::vector<TrackedFile> inputs;
    std::vector<TrackedFile> outputs;
    std::vector<TrackedFile> discovered_headers;
};

namespace detail {

inline constexpr char kLogMagic[4] = {'N', 'G', 'B', 'L'};
inline constexpr std::uint32_t kLogFormatVersion = 1;

inline auto put_u32(std::string& out, std::uint32_t v) -> void {
    char b[4];
    b[0] = static_cast<char>(v & 0xff);
    b[1] = static_cast<char>((v >> 8) & 0xff);
    b[2] = static_cast<char>((v >> 16) & 0xff);
    b[3] = static_cast<char>((v >> 24) & 0xff);
    out.append(b, 4);
}

inline auto put_u64(std::string& out, std::uint64_t v) -> void {
    char b[8];
    for (int i = 0; i < 8; ++i) {
        b[i] = static_cast<char>((v >> (i * 8)) & 0xff);
    }
    out.append(b, 8);
}

inline auto put_string(std::string& out, std::string_view s) -> void {
    put_u32(out, static_cast<std::uint32_t>(s.size()));
    out.append(s);
}

inline auto put_tracked(std::string& out, const TrackedFile& t) -> void {
    put_string(out, t.path);
    put_u64(out, t.stat.size);
    put_u64(out, t.stat.mtime_ns);
    put_u64(out, t.stat.ctime_ns);
    put_u64(out, t.content_hash);
}

inline auto get_u32(std::string_view buf, std::size_t& pos) -> std::expected<std::uint32_t, build::Error> {
    if (pos + 4 > buf.size()) {
        return std::unexpected(build::Error{"build log truncated"});
    }
    std::uint32_t v = 0;
    v |= static_cast<std::uint8_t>(buf[pos + 0]);
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[pos + 1])) << 8;
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[pos + 2])) << 16;
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[pos + 3])) << 24;
    pos += 4;
    return v;
}

inline auto get_u64(std::string_view buf, std::size_t& pos) -> std::expected<std::uint64_t, build::Error> {
    if (pos + 8 > buf.size()) {
        return std::unexpected(build::Error{"build log truncated"});
    }
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[pos + i])) << (i * 8);
    }
    pos += 8;
    return v;
}

inline auto get_string(std::string_view buf, std::size_t& pos) -> std::expected<std::string, build::Error> {
    auto len = get_u32(buf, pos);
    if (!len) {
        return std::unexpected(len.error());
    }
    if (pos + *len > buf.size()) {
        return std::unexpected(build::Error{"build log string truncated"});
    }
    std::string s(buf.substr(pos, *len));
    pos += *len;
    return s;
}

inline auto get_tracked(std::string_view buf, std::size_t& pos) -> std::expected<TrackedFile, build::Error> {
    TrackedFile t;
    auto path = get_string(buf, pos);
    if (!path) {
        return std::unexpected(path.error());
    }
    t.path = std::move(*path);
    auto sz = get_u64(buf, pos);
    if (!sz) {
        return std::unexpected(sz.error());
    }
    t.stat.size = *sz;
    auto mt = get_u64(buf, pos);
    if (!mt) {
        return std::unexpected(mt.error());
    }
    t.stat.mtime_ns = *mt;
    auto ct = get_u64(buf, pos);
    if (!ct) {
        return std::unexpected(ct.error());
    }
    t.stat.ctime_ns = *ct;
    auto h = get_u64(buf, pos);
    if (!h) {
        return std::unexpected(h.error());
    }
    t.content_hash = *h;
    return t;
}

} // namespace detail

class BuildLog {
public:
    auto find(const std::string& edge_name) const -> const LogEntry* {
        auto it = entries_.find(edge_name);
        return it == entries_.end() ? nullptr : &it->second;
    }

    auto upsert(std::string edge_name, LogEntry entry) -> void { entries_.insert_or_assign(std::move(edge_name), std::move(entry)); }

    auto entries() const -> const std::unordered_map<std::string, LogEntry>& { return entries_; }

    auto load(const build::Path& path) -> std::expected<void, build::Error> {
        entries_.clear();
        std::ifstream in(path.string(), std::ios::binary);
        if (!in) {
            // Missing log is not an error — first build.
            return {};
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        auto buf = ss.str();
        if (buf.size() < 8) {
            return std::unexpected(build::Error{"build log too small: " + path.string()});
        }
        if (buf[0] != detail::kLogMagic[0] || buf[1] != detail::kLogMagic[1] || buf[2] != detail::kLogMagic[2] || buf[3] != detail::kLogMagic[3]) {
            // Unrecognized log — treat as missing. Plan §7: missing log = clean build.
            entries_.clear();
            return {};
        }
        std::size_t pos = 4;
        auto version = detail::get_u32(buf, pos);
        if (!version) {
            return std::unexpected(version.error());
        }
        if (*version != detail::kLogFormatVersion) {
            entries_.clear();
            return {};
        }
        auto num_entries = detail::get_u32(buf, pos);
        if (!num_entries) {
            return std::unexpected(num_entries.error());
        }
        for (std::uint32_t i = 0; i < *num_entries; ++i) {
            auto name = detail::get_string(buf, pos);
            if (!name) {
                return std::unexpected(name.error());
            }
            LogEntry e;
            auto cmd_hash = detail::get_u64(buf, pos);
            if (!cmd_hash) {
                return std::unexpected(cmd_hash.error());
            }
            e.command_hash = *cmd_hash;
            auto last_run = detail::get_u64(buf, pos);
            if (!last_run) {
                return std::unexpected(last_run.error());
            }
            e.last_run_ns = *last_run;
            auto read_list = [&](std::vector<TrackedFile>& out) -> std::expected<void, build::Error> {
                auto count = detail::get_u32(buf, pos);
                if (!count) {
                    return std::unexpected(count.error());
                }
                out.reserve(*count);
                for (std::uint32_t k = 0; k < *count; ++k) {
                    auto t = detail::get_tracked(buf, pos);
                    if (!t) {
                        return std::unexpected(t.error());
                    }
                    out.push_back(std::move(*t));
                }
                return {};
            };
            if (auto r = read_list(e.inputs); !r) {
                return std::unexpected(r.error());
            }
            if (auto r = read_list(e.outputs); !r) {
                return std::unexpected(r.error());
            }
            if (auto r = read_list(e.discovered_headers); !r) {
                return std::unexpected(r.error());
            }
            entries_.emplace(std::move(*name), std::move(e));
        }
        return {};
    }

    auto save(const build::Path& path) const -> std::expected<void, build::Error> {
        std::string buf;
        buf.append(detail::kLogMagic, 4);
        detail::put_u32(buf, detail::kLogFormatVersion);
        detail::put_u32(buf, static_cast<std::uint32_t>(entries_.size()));
        for (const auto& [name, entry] : entries_) {
            detail::put_string(buf, name);
            detail::put_u64(buf, entry.command_hash);
            detail::put_u64(buf, entry.last_run_ns);
            auto write_list = [&](const std::vector<TrackedFile>& list) {
                detail::put_u32(buf, static_cast<std::uint32_t>(list.size()));
                for (const auto& t : list) {
                    detail::put_tracked(buf, t);
                }
            };
            write_list(entry.inputs);
            write_list(entry.outputs);
            write_list(entry.discovered_headers);
        }
        auto tmp_path = build::Path(path.string() + ".tmp");
        std::error_code ec;
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path().value, ec);
            if (ec) {
                return std::unexpected(build::Error{"failed to create directory " + path.parent_path().string() + ": " + ec.message()});
            }
        }
        {
            std::ofstream out(tmp_path.string(), std::ios::binary | std::ios::trunc);
            if (!out) {
                return std::unexpected(build::Error{"failed to open " + tmp_path.string() + " for writing"});
            }
            out.write(buf.data(), static_cast<std::streamsize>(buf.size()));
            if (!out) {
                return std::unexpected(build::Error{"failed to write " + tmp_path.string()});
            }
        }
        std::filesystem::rename(tmp_path.value, path.value, ec);
        if (ec) {
            return std::unexpected(build::Error{"rename " + tmp_path.string() + " -> " + path.string() + " failed: " + ec.message()});
        }
        return {};
    }

private:
    std::unordered_map<std::string, LogEntry> entries_;
};

// Refresh content_hash + stat on a single tracked file using the mtime fast-path:
// if the on-disk (size,mtime,ctime) tuple matches what we last recorded, reuse the
// stored content_hash. Otherwise re-hash. Sets `present` to false if the file is
// missing.
inline auto refresh(TrackedFile& t, const TrackedFile* previous, bool& present) -> std::expected<void, build::Error> {
    auto stat = stat_file(t.path);
    if (!stat) {
        present = false;
        return {};
    }
    present = true;
    t.stat = *stat;
    if (previous && stat_matches(previous->stat, *stat)) {
        t.content_hash = previous->content_hash;
        return {};
    }
    auto hash = hash_file(t.path);
    if (!hash) {
        return std::unexpected(hash.error());
    }
    t.content_hash = *hash;
    return {};
}

} // namespace ngen::run
