#pragma once

#include "../framework/glob.hpp"
#include "../framework/path.hpp"
#include "schema.hpp"

#include <cstdint>
#include <cstring>
#include <expected>
#include <fstream>
#include <ios>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace build::ir {

namespace detail {

inline auto get_u32(std::string_view buf, std::size_t off) -> std::uint32_t {
    std::uint32_t v = 0;
    v |= static_cast<std::uint8_t>(buf[off + 0]);
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[off + 1])) << 8;
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[off + 2])) << 16;
    v |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(buf[off + 3])) << 24;
    return v;
}

inline auto get_u64(std::string_view buf, std::size_t off) -> std::uint64_t {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(static_cast<std::uint8_t>(buf[off + i])) << (i * 8);
    }
    return v;
}

inline auto get_string(std::string_view buf, std::uint32_t string_table_offset, std::uint32_t ref_offset, std::uint32_t ref_length) -> std::string {
    if (ref_length == 0) {
        return {};
    }
    return std::string(buf.substr(string_table_offset + ref_offset, ref_length));
}

inline auto get_string_at(std::string_view buf, std::uint32_t string_table_offset, std::size_t header_off) -> std::string {
    auto off = get_u32(buf, header_off);
    auto len = get_u32(buf, header_off + 4);
    return get_string(buf, string_table_offset, off, len);
}

inline auto read_file(const Path& path) -> std::expected<std::string, Error> {
    std::ifstream in(path.string(), std::ios::binary);
    if (!in) {
        return std::unexpected(Error{"failed to open " + path.string()});
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        return std::unexpected(Error{"failed to read " + path.string()});
    }
    return ss.str();
}

} // namespace detail

inline auto read(const Path& path) -> std::expected<IR, Error> {
    using namespace detail;

    auto bytes_result = read_file(path);
    if (!bytes_result) {
        return std::unexpected(bytes_result.error());
    }
    auto bytes = std::move(*bytes_result);
    std::string_view buf{bytes};

    if (buf.size() < kHeaderSize) {
        return std::unexpected(Error{"IR file too small: " + path.string()});
    }
    if (buf[0] != kMagic[0] || buf[1] != kMagic[1] || buf[2] != kMagic[2] || buf[3] != kMagic[3]) {
        return std::unexpected(Error{"bad IR magic in " + path.string() + " (expected NGIR)"});
    }
    auto version = get_u32(buf, 4);
    if (version != kFormatVersion) {
        return std::unexpected(Error{"unsupported IR format version " + std::to_string(version) + " in " + path.string() + "; rerun the graph stage"});
    }

    auto string_table_offset = get_u32(buf, 16);
    auto string_table_size = get_u32(buf, 20);
    auto pools_offset = get_u32(buf, 24);
    auto pools_count = get_u32(buf, 28);
    auto edges_offset = get_u32(buf, 32);
    auto edges_count = get_u32(buf, 36);
    auto refs_offset = get_u32(buf, 40);
    auto refs_count = get_u32(buf, 44);
    auto default_targets_offset = get_u32(buf, 48);
    auto default_targets_count = get_u32(buf, 52);

    if (string_table_offset + string_table_size > buf.size()) {
        return std::unexpected(Error{"IR string table out of bounds"});
    }

    IR ir;
    ir.variant = get_string_at(buf, string_table_offset, 56);
    ir.project_root = get_string_at(buf, string_table_offset, 64);

    ir.pools.reserve(pools_count);
    for (std::uint32_t i = 0; i < pools_count; ++i) {
        auto base = pools_offset + i * kPoolRecordSize;
        Pool p;
        p.name = get_string_at(buf, string_table_offset, base);
        p.depth = get_u32(buf, base + 8);
        ir.pools.push_back(std::move(p));
    }

    auto read_string_list = [&](std::uint32_t list_off, std::uint32_t count) -> std::vector<std::string> {
        std::vector<std::string> out;
        out.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            auto ref_base = refs_offset + (list_off + i) * kStringRefSize;
            auto off = get_u32(buf, ref_base);
            auto len = get_u32(buf, ref_base + 4);
            out.push_back(get_string(buf, string_table_offset, off, len));
        }
        return out;
    };

    ir.edges.reserve(edges_count);
    for (std::uint32_t i = 0; i < edges_count; ++i) {
        auto base = edges_offset + i * kEdgeRecordSize;
        Edge e;
        e.name = get_string_at(buf, string_table_offset, base + 0);
        e.command = get_string_at(buf, string_table_offset, base + 8);
        e.depfile = get_string_at(buf, string_table_offset, base + 16);
        e.description = get_string_at(buf, string_table_offset, base + 24);

        auto inputs_off = get_u32(buf, base + 32);
        auto inputs_cnt = get_u32(buf, base + 36);
        auto outputs_off = get_u32(buf, base + 40);
        auto outputs_cnt = get_u32(buf, base + 44);
        auto implicit_off = get_u32(buf, base + 48);
        auto implicit_cnt = get_u32(buf, base + 52);
        auto order_only_off = get_u32(buf, base + 56);
        auto order_only_cnt = get_u32(buf, base + 60);

        e.inputs = read_string_list(inputs_off, inputs_cnt);
        e.outputs = read_string_list(outputs_off, outputs_cnt);
        e.implicit_deps = read_string_list(implicit_off, implicit_cnt);
        e.order_only_deps = read_string_list(order_only_off, order_only_cnt);

        e.pool = get_u32(buf, base + 64);
        e.flags = get_u32(buf, base + 68);
        ir.edges.push_back(std::move(e));
    }

    ir.default_targets.reserve(default_targets_count);
    for (std::uint32_t i = 0; i < default_targets_count; ++i) {
        ir.default_targets.push_back(get_u32(buf, default_targets_offset + i * 4));
    }

    (void)refs_count;
    return ir;
}

} // namespace build::ir
