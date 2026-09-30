#pragma once

#include <cstddef>
#include <cstdint>
#include <format>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// FNV-1a 64: the content hash the asset server records and uses as an asset's version.

inline constexpr uint64_t fnv1a64Seed = 14695981039346656037ull;

inline auto fnv1a64(std::span<const std::byte> bytes, uint64_t hash = fnv1a64Seed) -> uint64_t {
    for (auto byte : bytes) {
        hash ^= (uint64_t) byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

inline auto fnv1a64(std::string_view text, uint64_t hash = fnv1a64Seed) -> uint64_t {
    return fnv1a64(std::as_bytes(std::span(text.data(), text.size())), hash);
}

// The hash of a file's contents; nullopt when it can't be read.
inline auto hashFile(const std::string& path) -> std::optional<uint64_t> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    uint64_t hash = fnv1a64Seed;
    char buffer[64 * 1024];
    while (in) {
        in.read(buffer, sizeof(buffer));
        auto count = (size_t) in.gcount();
        hash = fnv1a64(std::as_bytes(std::span(buffer, count)), hash);
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return hash;
}

// A hash as 16 lowercase hex digits: how versions travel in messages.
inline auto hashText(uint64_t hash) -> std::string {
    return std::format("{:016x}", hash);
}
