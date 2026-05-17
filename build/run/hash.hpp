#pragma once

#define XXH_INLINE_ALL
#include "../ir/xxhash.h"

#include "../framework/glob.hpp"
#include "../framework/path.hpp"

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <expected>
#include <string>
#include <string_view>
#include <sys/stat.h>

namespace ngen::run {

// File stat snapshot used by the mtime fast-path.
struct StatTuple {
    std::uint64_t size = 0;
    std::uint64_t mtime_ns = 0;
    std::uint64_t ctime_ns = 0;
};

inline auto stat_file(const std::string& path) -> std::expected<StatTuple, build::Error> {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) {
        return std::unexpected(build::Error{"stat failed for " + path + ": " + std::strerror(errno)});
    }
    StatTuple t;
    t.size = static_cast<std::uint64_t>(st.st_size);
    t.mtime_ns = static_cast<std::uint64_t>(st.st_mtim.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(st.st_mtim.tv_nsec);
    t.ctime_ns = static_cast<std::uint64_t>(st.st_ctim.tv_sec) * 1'000'000'000ull + static_cast<std::uint64_t>(st.st_ctim.tv_nsec);
    return t;
}

inline auto stat_matches(const StatTuple& a, const StatTuple& b) -> bool {
    return a.size == b.size && a.mtime_ns == b.mtime_ns && a.ctime_ns == b.ctime_ns;
}

inline auto hash_string(std::string_view s) -> std::uint64_t {
    return XXH3_64bits(s.data(), s.size());
}

inline auto hash_file(const std::string& path) -> std::expected<std::uint64_t, build::Error> {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        return std::unexpected(build::Error{"open failed for " + path + ": " + std::strerror(errno)});
    }
    XXH3_state_t* state = XXH3_createState();
    XXH3_64bits_reset(state);
    std::array<unsigned char, 64 * 1024> buf{};
    while (auto n = std::fread(buf.data(), 1, buf.size(), f)) {
        XXH3_64bits_update(state, buf.data(), n);
    }
    bool err = std::ferror(f);
    std::fclose(f);
    if (err) {
        XXH3_freeState(state);
        return std::unexpected(build::Error{"read failed for " + path});
    }
    auto h = static_cast<std::uint64_t>(XXH3_64bits_digest(state));
    XXH3_freeState(state);
    return h;
}

} // namespace ngen::run
