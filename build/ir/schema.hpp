// ngen build IR — bespoke binary format that carries the build graph from the graph stage to the runner.
//
// The graph stage (`build/build.cpp` → `ir::Emitter`, defined in `emit.hpp`) walks a `Project` and produces one
// `IR` value per `(platform, config)` variant, then writes it via `writer.hpp` to
// `_out/<plat>/<cfg>/build.ngenir`. The runner (`build/run/main.cpp`) reads it back via `reader.hpp` and hands
// the value to `ngen::run::execute()`. This file owns only the in-memory types (`Edge`, `Pool`, `IR`) and the
// wire-format constants — no behavior. `writer.hpp` and `reader.hpp` must agree byte-for-byte with the offsets
// documented next to the constants below.
//
// All integers little-endian. No struct alignment is assumed; readers and writers do byte-level loads/stores.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace build::ir {

inline constexpr std::array<char, 4> kMagic = {'N', 'G', 'I', 'R'};
inline constexpr std::uint32_t kFormatVersion = 1;

// Pool indices baked into every IR. Higher pool indices are user-defined.
inline constexpr std::uint32_t kPoolDefault = 0; // depth 0 — capped by -j N at runtime.
inline constexpr std::uint32_t kPoolConsole = 1; // depth 1 — serialized, child inherits stdio.

// Edge flag bits.
inline constexpr std::uint32_t kEdgeFlagPhony = 1u << 0; // No command; pure ordering/alias node.

struct Pool {
    std::string name;
    std::uint32_t depth = 0;
};

struct Edge {
    std::string name;
    std::string command;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::vector<std::string> implicit_deps;
    std::vector<std::string> order_only_deps;
    std::uint32_t pool = kPoolDefault;
    std::string depfile;
    std::string description;
    std::uint32_t flags = 0;
};

struct IR {
    std::string variant;      // "linux-vulkan/debug"
    std::string project_root; // absolute path at emit time

    std::vector<Pool> pools;
    std::vector<Edge> edges;
    std::vector<std::uint32_t> default_targets; // indices into edges
};

inline auto make_default_pools() -> std::vector<Pool> {
    std::vector<Pool> pools;
    pools.push_back({.name = "default", .depth = 0});
    pools.push_back({.name = "console", .depth = 1});
    return pools;
}

// Wire-format constants. The header is a fixed-size prefix; pool/edge records
// are fixed-size too so a future reader can do O(1) indexed access after mmap.
//
// File layout:
//   [0 .. kHeaderSize)                Header           (fixed-size)
//   [pools_offset .. )                Pool[]           (kPoolRecordSize each)
//   [edges_offset .. )                Edge[]           (kEdgeRecordSize each)
//   [refs_offset .. )                 StringRef[]      (kStringRefSize each)
//   [default_targets_offset .. )      u32[]            (edge indices)
//   [string_table_offset .. )         raw UTF-8 bytes (no NUL terminators)
//
// A StringRef is { u32 offset_into_string_table, u32 length_in_bytes }.
// Edge list fields (inputs/outputs/implicit_deps/order_only_deps) point at
// contiguous spans inside the StringRef[] side-array; each edge stores
// (refs_offset, refs_count) for each list.

inline constexpr std::uint32_t kStringRefSize = 8; // u32 offset + u32 length

inline constexpr std::uint32_t kHeaderSize = 72;
//   offset  size  field
//   0       4     magic ("NGIR")
//   4       4     u32 format_version
//   8       8     u64 generated_at_ns
//   16      4     u32 string_table_offset
//   20      4     u32 string_table_size_bytes
//   24      4     u32 pools_offset
//   28      4     u32 pools_count
//   32      4     u32 edges_offset
//   36      4     u32 edges_count
//   40      4     u32 refs_offset                 (in bytes)
//   44      4     u32 refs_count                  (number of StringRefs)
//   48      4     u32 default_targets_offset
//   52      4     u32 default_targets_count
//   56      8     StringRef variant
//   64      8     StringRef project_root

inline constexpr std::uint32_t kPoolRecordSize = 16;
//   offset  size  field
//   0       8     StringRef name
//   8       4     u32 depth
//   12      4     u32 _reserved

inline constexpr std::uint32_t kEdgeRecordSize = 80;
//   offset  size  field
//   0       8     StringRef name
//   8       8     StringRef command
//   16      8     StringRef depfile
//   24      8     StringRef description
//   32      4     u32 inputs_refs_offset           (StringRef index into refs[])
//   36      4     u32 inputs_count
//   40      4     u32 outputs_refs_offset
//   44      4     u32 outputs_count
//   48      4     u32 implicit_deps_refs_offset
//   52      4     u32 implicit_deps_count
//   56      4     u32 order_only_deps_refs_offset
//   60      4     u32 order_only_deps_count
//   64      4     u32 pool
//   68      4     u32 flags
//   72      8     _reserved (pad to 80)

} // namespace build::ir
