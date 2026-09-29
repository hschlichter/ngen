// build::ir::write — serialize an IR to a binary file.
//
// Two-pass serializer: first interns every string into a deduplicated table and builds the `StringRef`
// side-array referenced by edge list fields; then assembles the byte buffer (header → pools → edges → refs →
// default targets → string table) and hands it to `write_if_changed`. The result mmap-reads cleanly through
// `reader.hpp`.
//
// Layout is fixed in `schema.hpp` — the writer and reader share `kHeaderSize`, `kPoolRecordSize`,
// `kEdgeRecordSize` so neither side has to compute offsets at run time.
//
// Errors propagate as `std::expected<void, build::Error>` (no exceptions). Called from `ir::Emitter::emit` once
// per variant; not used by the runner.

#pragma once

#include "../framework/glob.hpp"
#include "../framework/path.hpp"
#include "schema.hpp"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build::ir {

namespace detail {

inline auto put_u32(std::string& out, std::uint32_t v) -> void {
    char bytes[4];
    bytes[0] = static_cast<char>(v & 0xff);
    bytes[1] = static_cast<char>((v >> 8) & 0xff);
    bytes[2] = static_cast<char>((v >> 16) & 0xff);
    bytes[3] = static_cast<char>((v >> 24) & 0xff);
    out.append(bytes, 4);
}

inline auto put_u64(std::string& out, std::uint64_t v) -> void {
    char bytes[8];
    for (int i = 0; i < 8; ++i) {
        bytes[i] = static_cast<char>((v >> (i * 8)) & 0xff);
    }
    out.append(bytes, 8);
}

class StringTable {
public:
    auto intern(std::string_view s) -> std::pair<std::uint32_t, std::uint32_t> {
        if (s.empty()) {
            return {0, 0};
        }
        std::string key(s);
        if (auto it = offsets_.find(key); it != offsets_.end()) {
            return {it->second, static_cast<std::uint32_t>(s.size())};
        }
        auto offset = static_cast<std::uint32_t>(bytes_.size());
        bytes_.append(s);
        offsets_.emplace(std::move(key), offset);
        return {offset, static_cast<std::uint32_t>(s.size())};
    }

    auto bytes() const -> const std::string& { return bytes_; }

private:
    std::unordered_map<std::string, std::uint32_t> offsets_;
    std::string bytes_;
};

} // namespace detail

inline auto serialize(const IR& ir) -> std::string {
    using namespace detail;

    StringTable strings;

    auto variant_ref = strings.intern(ir.variant);
    auto project_root_ref = strings.intern(ir.project_root);

    struct PoolWire {
        std::uint32_t name_off;
        std::uint32_t name_len;
        std::uint32_t depth;
    };
    std::vector<PoolWire> pool_wires;
    pool_wires.reserve(ir.pools.size());
    for (const auto& p : ir.pools) {
        auto [off, len] = strings.intern(p.name);
        pool_wires.push_back({off, len, p.depth});
    }

    struct Ref {
        std::uint32_t offset;
        std::uint32_t length;
    };
    std::vector<Ref> refs;

    auto append_list = [&](const std::vector<std::string>& list) -> std::pair<std::uint32_t, std::uint32_t> {
        auto start = static_cast<std::uint32_t>(refs.size());
        for (const auto& s : list) {
            auto [o, l] = strings.intern(s);
            refs.push_back({o, l});
        }
        return {start, static_cast<std::uint32_t>(list.size())};
    };

    struct EdgeWire {
        std::uint32_t name_off, name_len;
        std::uint32_t cmd_off, cmd_len;
        std::uint32_t depfile_off, depfile_len;
        std::uint32_t desc_off, desc_len;
        std::uint32_t inputs_off, inputs_count;
        std::uint32_t outputs_off, outputs_count;
        std::uint32_t implicit_off, implicit_count;
        std::uint32_t order_only_off, order_only_count;
        std::uint32_t pool;
        std::uint32_t flags;
    };
    std::vector<EdgeWire> edge_wires;
    edge_wires.reserve(ir.edges.size());
    for (const auto& e : ir.edges) {
        EdgeWire w{};
        std::tie(w.name_off, w.name_len) = strings.intern(e.name);
        std::tie(w.cmd_off, w.cmd_len) = strings.intern(e.command);
        std::tie(w.depfile_off, w.depfile_len) = strings.intern(e.depfile);
        std::tie(w.desc_off, w.desc_len) = strings.intern(e.description);
        std::tie(w.inputs_off, w.inputs_count) = append_list(e.inputs);
        std::tie(w.outputs_off, w.outputs_count) = append_list(e.outputs);
        std::tie(w.implicit_off, w.implicit_count) = append_list(e.implicit_deps);
        std::tie(w.order_only_off, w.order_only_count) = append_list(e.order_only_deps);
        w.pool = e.pool;
        w.flags = e.flags;
        edge_wires.push_back(w);
    }

    auto pools_offset = kHeaderSize;
    auto pools_size = static_cast<std::uint32_t>(pool_wires.size()) * kPoolRecordSize;
    auto edges_offset = pools_offset + pools_size;
    auto edges_size = static_cast<std::uint32_t>(edge_wires.size()) * kEdgeRecordSize;
    auto refs_offset = edges_offset + edges_size;
    auto refs_size = static_cast<std::uint32_t>(refs.size()) * kStringRefSize;
    auto default_targets_offset = refs_offset + refs_size;
    auto default_targets_size = static_cast<std::uint32_t>(ir.default_targets.size()) * 4;
    auto string_table_offset = default_targets_offset + default_targets_size;
    auto string_table_size = static_cast<std::uint32_t>(strings.bytes().size());

    std::string buf;
    buf.reserve(string_table_offset + string_table_size);

    buf.append(kMagic.data(), 4);
    put_u32(buf, kFormatVersion);
    auto now_ns = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    put_u64(buf, now_ns);
    put_u32(buf, string_table_offset);
    put_u32(buf, string_table_size);
    put_u32(buf, pools_offset);
    put_u32(buf, static_cast<std::uint32_t>(pool_wires.size()));
    put_u32(buf, edges_offset);
    put_u32(buf, static_cast<std::uint32_t>(edge_wires.size()));
    put_u32(buf, refs_offset);
    put_u32(buf, static_cast<std::uint32_t>(refs.size()));
    put_u32(buf, default_targets_offset);
    put_u32(buf, static_cast<std::uint32_t>(ir.default_targets.size()));
    put_u32(buf, variant_ref.first);
    put_u32(buf, variant_ref.second);
    put_u32(buf, project_root_ref.first);
    put_u32(buf, project_root_ref.second);
    assert(buf.size() == kHeaderSize);

    for (const auto& p : pool_wires) {
        put_u32(buf, p.name_off);
        put_u32(buf, p.name_len);
        put_u32(buf, p.depth);
        put_u32(buf, 0);
    }

    for (const auto& e : edge_wires) {
        put_u32(buf, e.name_off);
        put_u32(buf, e.name_len);
        put_u32(buf, e.cmd_off);
        put_u32(buf, e.cmd_len);
        put_u32(buf, e.depfile_off);
        put_u32(buf, e.depfile_len);
        put_u32(buf, e.desc_off);
        put_u32(buf, e.desc_len);
        put_u32(buf, e.inputs_off);
        put_u32(buf, e.inputs_count);
        put_u32(buf, e.outputs_off);
        put_u32(buf, e.outputs_count);
        put_u32(buf, e.implicit_off);
        put_u32(buf, e.implicit_count);
        put_u32(buf, e.order_only_off);
        put_u32(buf, e.order_only_count);
        put_u32(buf, e.pool);
        put_u32(buf, e.flags);
        put_u32(buf, 0);
        put_u32(buf, 0);
    }

    for (const auto& r : refs) {
        put_u32(buf, r.offset);
        put_u32(buf, r.length);
    }

    for (auto idx : ir.default_targets) {
        put_u32(buf, idx);
    }

    buf.append(strings.bytes());
    return buf;
}

inline auto write(const IR& ir, const Path& output) -> std::expected<void, Error> {
    auto bytes = serialize(ir);
    return write_if_changed(output, bytes);
}

} // namespace build::ir
