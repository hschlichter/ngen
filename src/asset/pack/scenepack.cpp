#include "scenepack.h"

#include <bit>
#include <cstring>
#include <format>
#include <unordered_map>

namespace {

static_assert(std::endian::native == std::endian::little, "scene packs are little-endian and read in place");
static_assert(sizeof(PackVertex) == 44, "PackVertex must match the engine's Vertex");

// Section ids, per kind.
enum class Section : uint32_t {
    LayerMetadata = 1,
    LayerSublayers = 2,
    LayerPrims = 3,
    LayerWarnings = 4,
    MeshInfo = 11,
    MeshVertices = 12,
    MeshIndices = 13,
    MeshSubmeshes = 14,
    MaterialInfo = 21,
};

// Builds one section's bytes, with strings as indices into a shared table.
class SectionWriter {
public:
    explicit SectionWriter(std::unordered_map<std::string, uint32_t>& strings, std::vector<std::string>& table) : stringIndex(strings), stringTable(table) {
    }

    auto u32(uint32_t value) -> void { put(&value, sizeof(value)); }
    auto i32(int32_t value) -> void { put(&value, sizeof(value)); }
    auto u64(uint64_t value) -> void { put(&value, sizeof(value)); }
    auto f32(float value) -> void { put(&value, sizeof(value)); }
    auto f64(double value) -> void { put(&value, sizeof(value)); }

    auto str(const std::string& value) -> void {
        auto it = stringIndex.find(value);
        if (it == stringIndex.end()) {
            it = stringIndex.emplace(value, (uint32_t) stringTable.size()).first;
            stringTable.push_back(value);
        }
        u32(it->second);
    }

    auto strings(const std::vector<std::string>& values) -> void {
        u64(values.size());
        for (const auto& value : values) {
            str(value);
        }
    }

    auto raw(const void* data, size_t size) -> void { put(data, size); }

    auto bytes() const -> const std::vector<std::byte>& { return out; }

private:
    auto put(const void* data, size_t size) -> void {
        const auto* p = static_cast<const std::byte*>(data);
        out.insert(out.end(), p, p + size);
    }

    std::unordered_map<std::string, uint32_t>& stringIndex;
    std::vector<std::string>& stringTable;
    std::vector<std::byte> out;
};

// Assembles a pack: header, string table, section table, sections.
class PackWriter {
public:
    auto section() -> SectionWriter { return SectionWriter(stringIndex, stringTable); }

    auto add(Section id, const SectionWriter& section) -> void { sections.push_back({id, section.bytes()}); }

    auto finish(ScenePackKind kind) const -> std::vector<std::byte> {
        std::vector<std::byte> out(scenePackHeaderSize);
        auto append = [&out](const void* data, size_t size) {
            const auto* p = static_cast<const std::byte*>(data);
            out.insert(out.end(), p, p + size);
        };
        uint64_t stringTableOffset = out.size();
        uint64_t count = stringTable.size();
        append(&count, sizeof(count));
        for (const auto& value : stringTable) {
            uint64_t length = value.size();
            append(&length, sizeof(length));
            append(value.data(), value.size());
        }
        uint64_t sectionTableOffset = out.size();
        uint64_t offset = sectionTableOffset + (sections.size() * 24);
        for (const auto& [id, bytes] : sections) {
            uint32_t sectionId = (uint32_t) id;
            uint32_t zero = 0;
            uint64_t size = bytes.size();
            append(&sectionId, sizeof(sectionId));
            append(&zero, sizeof(zero));
            append(&offset, sizeof(offset));
            append(&size, sizeof(size));
            offset += size;
        }
        for (const auto& [id, bytes] : sections) {
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        std::memcpy(out.data(), "NGPK", 4);
        uint32_t kindValue = (uint32_t) kind;
        uint32_t version = scenePackFormatVersion;
        uint32_t sectionCount = (uint32_t) sections.size();
        std::memcpy(out.data() + 4, &kindValue, 4);
        std::memcpy(out.data() + 8, &version, 4);
        std::memcpy(out.data() + 12, &sectionCount, 4);
        std::memcpy(out.data() + 16, &stringTableOffset, 8);
        std::memcpy(out.data() + 24, &sectionTableOffset, 8);
        return out;
    }

private:
    std::unordered_map<std::string, uint32_t> stringIndex;
    std::vector<std::string> stringTable;
    std::vector<std::pair<Section, std::vector<std::byte>>> sections;
};

auto writeValue(SectionWriter& out, const PackValue& value) -> void {
    out.str(value.typeName);
    out.u64(value.numbers.size());
    for (double number : value.numbers) {
        out.f64(number);
    }
    out.strings(value.strings);
}

// Reads a section's fields in order; any read past its end marks the whole read as failed.
class SectionReader {
public:
    SectionReader(std::span<const std::byte> bytes, const std::vector<std::string>& table) : data(bytes), stringTable(table) {
    }

    auto u32() -> uint32_t { return get<uint32_t>(); }
    auto i32() -> int32_t { return get<int32_t>(); }
    auto u64() -> uint64_t { return get<uint64_t>(); }
    auto f32() -> float { return get<float>(); }
    auto f64() -> double { return get<double>(); }

    auto str() -> std::string {
        auto index = u32();
        if (index >= stringTable.size()) {
            failed = true;
            return {};
        }
        return stringTable[index];
    }

    auto strings() -> std::vector<std::string> {
        auto count = count64(4);
        std::vector<std::string> values;
        values.reserve(count);
        for (uint64_t i = 0; i < count; i++) {
            values.push_back(str());
        }
        return values;
    }

    // A count read from the section, checked against the bytes left at `elementSize` bytes per element.
    auto count64(size_t elementSize) -> uint64_t {
        auto count = u64();
        if (elementSize > 0 && count > (data.size() - position) / elementSize) {
            failed = true;
            return 0;
        }
        return count;
    }

    auto raw(void* out, size_t size) -> void {
        if (size > data.size() - position) {
            failed = true;
            return;
        }
        std::memcpy(out, data.data() + position, size);
        position += size;
    }

    auto ok() const -> bool { return !failed; }

private:
    template <typename T>
    auto get() -> T {
        T value{};
        raw(&value, sizeof(T));
        return value;
    }

    std::span<const std::byte> data;
    const std::vector<std::string>& stringTable;
    size_t position = 0;
    bool failed = false;
};

auto readValue(SectionReader& in) -> PackValue {
    PackValue value;
    value.typeName = in.str();
    auto count = in.count64(8);
    value.numbers.reserve(count);
    for (uint64_t i = 0; i < count; i++) {
        value.numbers.push_back(in.f64());
    }
    value.strings = in.strings();
    return value;
}

// A pack opened for reading: its string table and where each section is.
struct PackReader {
    std::span<const std::byte> bytes;
    std::vector<std::string> strings;
    std::unordered_map<uint32_t, std::span<const std::byte>> sections;

    auto section(Section id) const -> std::span<const std::byte> {
        auto it = sections.find((uint32_t) id);
        return it == sections.end() ? std::span<const std::byte>() : it->second;
    }
};

auto readU64At(std::span<const std::byte> bytes, size_t offset) -> uint64_t {
    uint64_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

auto readU32At(std::span<const std::byte> bytes, size_t offset) -> uint32_t {
    uint32_t value = 0;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

auto openPack(std::span<const std::byte> bytes, ScenePackKind expected) -> std::expected<PackReader, std::string> {
    auto kind = scenePackKind(bytes);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind != expected) {
        return std::unexpected(std::format("a scene pack of kind {}, not {}", (uint32_t) *kind, (uint32_t) expected));
    }
    PackReader reader;
    reader.bytes = bytes;
    auto sectionCount = readU32At(bytes, 12);
    auto stringTableOffset = readU64At(bytes, 16);
    auto sectionTableOffset = readU64At(bytes, 24);
    if (stringTableOffset > bytes.size() || sectionTableOffset > bytes.size() || sectionCount > (bytes.size() - sectionTableOffset) / 24) {
        return std::unexpected("the string or section table lies outside the pack");
    }
    // The string table.
    size_t position = stringTableOffset;
    if (position + 8 > bytes.size()) {
        return std::unexpected("the string table is cut short");
    }
    auto count = readU64At(bytes, position);
    position += 8;
    if (count > (bytes.size() - position) / 8) {
        return std::unexpected("the string table is cut short");
    }
    reader.strings.reserve(count);
    for (uint64_t i = 0; i < count; i++) {
        if (position + 8 > bytes.size()) {
            return std::unexpected("the string table is cut short");
        }
        auto length = readU64At(bytes, position);
        position += 8;
        if (length > bytes.size() - position) {
            return std::unexpected("the string table is cut short");
        }
        reader.strings.emplace_back(reinterpret_cast<const char*>(bytes.data() + position), length);
        position += length;
    }
    // The section table.
    for (uint32_t i = 0; i < sectionCount; i++) {
        size_t entry = sectionTableOffset + ((size_t) i * 24);
        auto id = readU32At(bytes, entry);
        auto offset = readU64At(bytes, entry + 8);
        auto size = readU64At(bytes, entry + 16);
        if (offset > bytes.size() || size > bytes.size() - offset) {
            return std::unexpected(std::format("section {} lies outside the pack", id));
        }
        reader.sections[id] = bytes.subspan(offset, size);
    }
    return reader;
}

} // namespace

auto scenePackKind(std::span<const std::byte> bytes) -> std::expected<ScenePackKind, std::string> {
    if (bytes.size() < scenePackHeaderSize || std::memcmp(bytes.data(), "NGPK", 4) != 0) {
        return std::unexpected("not a scene pack (no NGPK header)");
    }
    auto version = readU32At(bytes, 8);
    if (version != scenePackFormatVersion) {
        return std::unexpected(std::format("scene pack format version {}, this reader reads {}", version, scenePackFormatVersion));
    }
    auto kind = readU32At(bytes, 4);
    if (kind < (uint32_t) ScenePackKind::Layer || kind > (uint32_t) ScenePackKind::Material) {
        return std::unexpected(std::format("unknown scene pack kind {}", kind));
    }
    return (ScenePackKind) kind;
}

auto writeLayerPack(const LayerPack& pack) -> std::vector<std::byte> {
    PackWriter writer;

    auto metadata = writer.section();
    metadata.str(pack.upAxis);
    metadata.f64(pack.metersPerUnit);
    metadata.str(pack.defaultPrim);
    writer.add(Section::LayerMetadata, metadata);

    auto sublayers = writer.section();
    sublayers.strings(pack.sublayers);
    writer.add(Section::LayerSublayers, sublayers);

    auto prims = writer.section();
    prims.u64(pack.prims.size());
    for (const auto& prim : pack.prims) {
        prims.str(prim.path);
        prims.u32((uint32_t) prim.specifier);
        prims.str(prim.typeName);
        prims.strings(prim.apiSchemas);
        prims.i32(prim.active);
        prims.i32(prim.instanceable);
        prims.str(prim.kind);
        prims.u64(prim.attributes.size());
        for (const auto& attribute : prim.attributes) {
            prims.str(attribute.name);
            writeValue(prims, attribute.value);
        }
        prims.u64(prim.relationships.size());
        for (const auto& relationship : prim.relationships) {
            prims.str(relationship.name);
            prims.strings(relationship.targets);
            prims.str(relationship.bindMaterialAs);
        }
        prims.u64(prim.placements.size());
        for (const auto& placement : prim.placements) {
            prims.u32((uint32_t) placement.kind);
            prims.str(placement.assetId);
            prims.str(placement.primPath);
        }
        prims.str(prim.meshAsset);
        prims.str(prim.materialAsset);
    }
    writer.add(Section::LayerPrims, prims);

    auto warnings = writer.section();
    warnings.u64(pack.warnings.size());
    for (const auto& warning : pack.warnings) {
        warnings.str(warning.path);
        warnings.str(warning.text);
    }
    writer.add(Section::LayerWarnings, warnings);

    return writer.finish(ScenePackKind::Layer);
}

auto writeMeshPack(const MeshPack& pack) -> std::vector<std::byte> {
    PackWriter writer;

    auto info = writer.section();
    info.u32(pack.doubleSided ? 1 : 0);
    info.u32(pack.hasDisplayColor ? 1 : 0);
    for (float c : pack.displayColor) {
        info.f32(c);
    }
    for (float c : pack.boundsMin) {
        info.f32(c);
    }
    for (float c : pack.boundsMax) {
        info.f32(c);
    }
    writer.add(Section::MeshInfo, info);

    auto vertices = writer.section();
    vertices.u64(pack.vertices.size());
    vertices.raw(pack.vertices.data(), pack.vertices.size() * sizeof(PackVertex));
    writer.add(Section::MeshVertices, vertices);

    auto indices = writer.section();
    indices.u64(pack.indices.size());
    indices.raw(pack.indices.data(), pack.indices.size() * sizeof(uint32_t));
    writer.add(Section::MeshIndices, indices);

    auto submeshes = writer.section();
    submeshes.u64(pack.submeshes.size());
    for (const auto& submesh : pack.submeshes) {
        submeshes.u32(submesh.indexOffset);
        submeshes.u32(submesh.indexCount);
        submeshes.str(submesh.subsetPath);
    }
    writer.add(Section::MeshSubmeshes, submeshes);

    return writer.finish(ScenePackKind::Mesh);
}

auto writeMaterialPack(const MaterialPack& pack) -> std::vector<std::byte> {
    PackWriter writer;
    auto info = writer.section();
    info.u32(pack.hasBaseColor ? 1 : 0);
    for (float c : pack.baseColor) {
        info.f32(c);
    }
    info.str(pack.baseColorTexture);
    writer.add(Section::MaterialInfo, info);
    return writer.finish(ScenePackKind::Material);
}

auto readLayerPack(std::span<const std::byte> bytes) -> std::expected<LayerPack, std::string> {
    auto reader = openPack(bytes, ScenePackKind::Layer);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    LayerPack pack;

    SectionReader metadata(reader->section(Section::LayerMetadata), reader->strings);
    pack.upAxis = metadata.str();
    pack.metersPerUnit = metadata.f64();
    pack.defaultPrim = metadata.str();

    SectionReader sublayers(reader->section(Section::LayerSublayers), reader->strings);
    pack.sublayers = sublayers.strings();

    SectionReader prims(reader->section(Section::LayerPrims), reader->strings);
    auto primCount = prims.count64(8);
    pack.prims.reserve(primCount);
    for (uint64_t i = 0; i < primCount && prims.ok(); i++) {
        PackPrim prim;
        prim.path = prims.str();
        prim.specifier = (PrimSpecifier) prims.u32();
        prim.typeName = prims.str();
        prim.apiSchemas = prims.strings();
        prim.active = prims.i32();
        prim.instanceable = prims.i32();
        prim.kind = prims.str();
        auto attributeCount = prims.count64(4);
        for (uint64_t a = 0; a < attributeCount && prims.ok(); a++) {
            PackAttribute attribute;
            attribute.name = prims.str();
            attribute.value = readValue(prims);
            prim.attributes.push_back(std::move(attribute));
        }
        auto relationshipCount = prims.count64(4);
        for (uint64_t r = 0; r < relationshipCount && prims.ok(); r++) {
            PackRelationship relationship;
            relationship.name = prims.str();
            relationship.targets = prims.strings();
            relationship.bindMaterialAs = prims.str();
            prim.relationships.push_back(std::move(relationship));
        }
        auto placementCount = prims.count64(12);
        for (uint64_t p = 0; p < placementCount && prims.ok(); p++) {
            PackPlacement placement;
            placement.kind = (PlacementKind) prims.u32();
            placement.assetId = prims.str();
            placement.primPath = prims.str();
            prim.placements.push_back(std::move(placement));
        }
        prim.meshAsset = prims.str();
        prim.materialAsset = prims.str();
        pack.prims.push_back(std::move(prim));
    }

    SectionReader warnings(reader->section(Section::LayerWarnings), reader->strings);
    auto warningCount = warnings.count64(8);
    for (uint64_t i = 0; i < warningCount && warnings.ok(); i++) {
        PackWarning warning;
        warning.path = warnings.str();
        warning.text = warnings.str();
        pack.warnings.push_back(std::move(warning));
    }

    if (!metadata.ok() || !sublayers.ok() || !prims.ok() || !warnings.ok()) {
        return std::unexpected("a layer pack section is cut short or names a string that isn't there");
    }
    return pack;
}

auto readMeshPack(std::span<const std::byte> bytes) -> std::expected<MeshPack, std::string> {
    auto reader = openPack(bytes, ScenePackKind::Mesh);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    MeshPack pack;

    SectionReader info(reader->section(Section::MeshInfo), reader->strings);
    pack.doubleSided = info.u32() != 0;
    pack.hasDisplayColor = info.u32() != 0;
    for (auto& c : pack.displayColor) {
        c = info.f32();
    }
    for (auto& c : pack.boundsMin) {
        c = info.f32();
    }
    for (auto& c : pack.boundsMax) {
        c = info.f32();
    }

    SectionReader vertices(reader->section(Section::MeshVertices), reader->strings);
    auto vertexCount = vertices.count64(sizeof(PackVertex));
    pack.vertices.resize(vertexCount);
    vertices.raw(pack.vertices.data(), vertexCount * sizeof(PackVertex));

    SectionReader indices(reader->section(Section::MeshIndices), reader->strings);
    auto indexCount = indices.count64(sizeof(uint32_t));
    pack.indices.resize(indexCount);
    indices.raw(pack.indices.data(), indexCount * sizeof(uint32_t));

    SectionReader submeshes(reader->section(Section::MeshSubmeshes), reader->strings);
    auto submeshCount = submeshes.count64(12);
    for (uint64_t i = 0; i < submeshCount && submeshes.ok(); i++) {
        PackSubmesh submesh;
        submesh.indexOffset = submeshes.u32();
        submesh.indexCount = submeshes.u32();
        submesh.subsetPath = submeshes.str();
        pack.submeshes.push_back(std::move(submesh));
    }

    if (!info.ok() || !vertices.ok() || !indices.ok() || !submeshes.ok()) {
        return std::unexpected("a mesh pack section is cut short or names a string that isn't there");
    }
    for (const auto& submesh : pack.submeshes) {
        if (submesh.indexOffset > pack.indices.size() || submesh.indexCount > pack.indices.size() - submesh.indexOffset) {
            return std::unexpected("a submesh lies outside the index buffer");
        }
    }
    for (uint32_t index : pack.indices) {
        if (index >= pack.vertices.size()) {
            return std::unexpected("an index points past the vertices");
        }
    }
    return pack;
}

auto readMaterialPack(std::span<const std::byte> bytes) -> std::expected<MaterialPack, std::string> {
    auto reader = openPack(bytes, ScenePackKind::Material);
    if (!reader) {
        return std::unexpected(reader.error());
    }
    MaterialPack pack;
    SectionReader info(reader->section(Section::MaterialInfo), reader->strings);
    pack.hasBaseColor = info.u32() != 0;
    for (auto& c : pack.baseColor) {
        c = info.f32();
    }
    pack.baseColorTexture = info.str();
    if (!info.ok()) {
        return std::unexpected("the material pack's section is cut short or names a string that isn't there");
    }
    return pack;
}
