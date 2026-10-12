#pragma once

#include "scenepacktypes.h"

#include <cstddef>
#include <expected>
#include <span>
#include <string>
#include <vector>

// Scene packs, as ngen-packer-usd writes them and the engine reads them: the opinions one USD file authors (a layer pack), and the
// meshes and materials it defines (one asset each, id "<file>#<prim path>"). Little-endian; every count and offset is 64-bit.
//
//   offset  size  field
//   0       4     magic "NGPK"
//   4       4     u32 kind (ScenePackKind)
//   8       4     u32 format version (scenePackFormatVersion)
//   12      4     u32 section count
//   16      8     u64 string table offset
//   24      8     u64 section table offset
//
// The string table is a u64 count, then each string as a u64 length and its bytes; sections name strings by u32 index. The section table
// is one entry per section: u32 section id, u32 zero, u64 offset, u64 size. A section is a sequence of fields in the order the writer puts
// them (scenepack.cpp). See src/asset/README.md.

auto writeLayerPack(const LayerPack& pack) -> std::vector<std::byte>;
auto writeMeshPack(const MeshPack& pack) -> std::vector<std::byte>;
auto writeMaterialPack(const MaterialPack& pack) -> std::vector<std::byte>;

// The kind of a scene pack, or an error when the header isn't one.
auto scenePackKind(std::span<const std::byte> bytes) -> std::expected<ScenePackKind, std::string>;

auto readLayerPack(std::span<const std::byte> bytes) -> std::expected<LayerPack, std::string>;
auto readMeshPack(std::span<const std::byte> bytes) -> std::expected<MeshPack, std::string>;
auto readMaterialPack(std::span<const std::byte> bytes) -> std::expected<MaterialPack, std::string>;
