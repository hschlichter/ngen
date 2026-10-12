// ngen-packdump: prints a scene pack (src/asset/pack/scenepack.h) as JSON.
//
//   ngen-packdump [--vertices] <file>...       packs read from files
//   ngen-packdump [--vertices] --id <id>...    packed assets fetched from this variant's asset server (which packs them when needed)
//
// A layer pack prints whole; a mesh prints its counts, submeshes and bounds, and every vertex and index with --vertices; a material
// prints whole. Exit codes: 0 every pack printed, 1 one couldn't be read or fetched, 3 usage.

#include "assetclient.h"
#include "scenepack.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <print>
#include <string>
#include <vector>

namespace {

using Json = nlohmann::ordered_json;

auto valueJson(const PackValue& value) -> Json {
    Json out;
    out["type"] = value.typeName;
    if (!value.numbers.empty()) {
        out["numbers"] = value.numbers;
    }
    if (!value.strings.empty()) {
        out["strings"] = value.strings;
    }
    return out;
}

auto specifierName(PrimSpecifier specifier) -> const char* {
    switch (specifier) {
        case PrimSpecifier::Def:
            return "def";
        case PrimSpecifier::Over:
            return "over";
        case PrimSpecifier::Class:
            return "class";
    }
    return "over";
}

auto layerJson(const LayerPack& pack) -> Json {
    Json out;
    out["kind"] = "layer";
    out["upAxis"] = pack.upAxis;
    out["metersPerUnit"] = pack.metersPerUnit;
    out["defaultPrim"] = pack.defaultPrim;
    out["sublayers"] = pack.sublayers;
    auto prims = Json::array();
    for (const auto& prim : pack.prims) {
        Json p;
        p["path"] = prim.path;
        p["specifier"] = specifierName(prim.specifier);
        if (!prim.typeName.empty()) {
            p["type"] = prim.typeName;
        }
        if (!prim.apiSchemas.empty()) {
            p["apiSchemas"] = prim.apiSchemas;
        }
        if (prim.active >= 0) {
            p["active"] = prim.active == 1;
        }
        if (prim.instanceable >= 0) {
            p["instanceable"] = prim.instanceable == 1;
        }
        if (!prim.kind.empty()) {
            p["kind"] = prim.kind;
        }
        if (!prim.attributes.empty()) {
            Json attributes;
            for (const auto& attribute : prim.attributes) {
                attributes[attribute.name] = valueJson(attribute.value);
            }
            p["attributes"] = attributes;
        }
        if (!prim.relationships.empty()) {
            Json relationships;
            for (const auto& relationship : prim.relationships) {
                Json r;
                r["targets"] = relationship.targets;
                if (!relationship.bindMaterialAs.empty()) {
                    r["bindMaterialAs"] = relationship.bindMaterialAs;
                }
                relationships[relationship.name] = r;
            }
            p["relationships"] = relationships;
        }
        if (!prim.placements.empty()) {
            auto placements = Json::array();
            for (const auto& placement : prim.placements) {
                placements.push_back({
                    {"kind", placement.kind == PlacementKind::Payload ? "payload" : "reference"},
                    {"asset", placement.assetId},
                    {"prim", placement.primPath},
                });
            }
            p["placements"] = placements;
        }
        if (!prim.meshAsset.empty()) {
            p["mesh"] = prim.meshAsset;
        }
        if (!prim.materialAsset.empty()) {
            p["material"] = prim.materialAsset;
        }
        prims.push_back(p);
    }
    out["prims"] = prims;
    auto warnings = Json::array();
    for (const auto& warning : pack.warnings) {
        warnings.push_back({{"prim", warning.path}, {"text", warning.text}});
    }
    out["warnings"] = warnings;
    return out;
}

auto meshJson(const MeshPack& pack, bool vertices) -> Json {
    Json out;
    out["kind"] = "mesh";
    out["vertices"] = pack.vertices.size();
    out["indices"] = pack.indices.size();
    out["doubleSided"] = pack.doubleSided;
    if (pack.hasDisplayColor) {
        out["displayColor"] = pack.displayColor;
    }
    out["boundsMin"] = pack.boundsMin;
    out["boundsMax"] = pack.boundsMax;
    auto submeshes = Json::array();
    for (const auto& submesh : pack.submeshes) {
        submeshes.push_back({{"indexOffset", submesh.indexOffset}, {"indexCount", submesh.indexCount}, {"subset", submesh.subsetPath}});
    }
    out["submeshes"] = submeshes;
    if (vertices) {
        auto list = Json::array();
        for (const auto& vertex : pack.vertices) {
            list.push_back({{"position", vertex.position}, {"normal", vertex.normal}, {"color", vertex.color}, {"texCoord", vertex.texCoord}});
        }
        out["vertexData"] = list;
        out["indexData"] = pack.indices;
    }
    return out;
}

auto materialJson(const MaterialPack& pack) -> Json {
    Json out;
    out["kind"] = "material";
    if (pack.hasBaseColor) {
        out["baseColor"] = pack.baseColor;
    }
    out["baseColorTexture"] = pack.baseColorTexture;
    return out;
}

// One pack's JSON, or an error.
auto packJson(std::span<const std::byte> bytes, bool vertices) -> std::expected<Json, std::string> {
    auto kind = scenePackKind(bytes);
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (*kind == ScenePackKind::Layer) {
        auto pack = readLayerPack(bytes);
        if (!pack) {
            return std::unexpected(pack.error());
        }
        return layerJson(*pack);
    }
    if (*kind == ScenePackKind::Mesh) {
        auto pack = readMeshPack(bytes);
        if (!pack) {
            return std::unexpected(pack.error());
        }
        return meshJson(*pack, vertices);
    }
    auto pack = readMaterialPack(bytes);
    if (!pack) {
        return std::unexpected(pack.error());
    }
    return materialJson(*pack);
}

auto readFile(const std::string& path) -> std::optional<std::vector<std::byte>> {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) {
        return std::nullopt;
    }
    auto size = (size_t) in.tellg();
    in.seekg(0);
    std::vector<std::byte> bytes(size);
    in.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize) size);
    if (!in) {
        return std::nullopt;
    }
    return bytes;
}

} // namespace

auto main(int argc, char** argv) -> int {
    bool vertices = false;
    bool fromServer = false;
    std::vector<std::string> names;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--vertices") {
            vertices = true;
        } else if (arg == "--id") {
            fromServer = true;
        } else if (arg.starts_with("--")) {
            std::println(stderr, "usage: ngen-packdump [--vertices] [--id] <file or id>...");
            return 3;
        } else {
            names.push_back(arg);
        }
    }
    if (names.empty()) {
        std::println(stderr, "usage: ngen-packdump [--vertices] [--id] <file or id>...");
        return 3;
    }

    AssetClient client;
    if (fromServer) {
        // The variant is the folder the binary is in, _out/<platform>/<config>/, as for ngen-view.
        std::error_code ec;
        auto bin = std::filesystem::canonical("/proc/self/exe", ec).parent_path();
        auto variant = bin.parent_path().filename().string() + "/" + bin.filename().string();
        if (auto connected = client.connect(variant); !connected) {
            std::println(stderr, "ngen-packdump: {}", connected.error());
            return 1;
        }
        client.request(names);
        client.wait(names);
    }

    int exitCode = 0;
    for (const auto& name : names) {
        std::optional<std::vector<std::byte>> bytes;
        if (fromServer) {
            if (auto asset = client.take(name)) {
                bytes = std::move(asset->bytes);
            } else {
                std::println(stderr, "ngen-packdump: {} failed", name);
                for (const auto& line : client.errors(name)) {
                    std::println(stderr, "  {}", line);
                }
                exitCode = 1;
                continue;
            }
        } else {
            bytes = readFile(name);
            if (!bytes) {
                std::println(stderr, "ngen-packdump: cannot read {}", name);
                exitCode = 1;
                continue;
            }
        }
        auto json = packJson(*bytes, vertices);
        if (!json) {
            std::println(stderr, "ngen-packdump: {}: {}", name, json.error());
            exitCode = 1;
            continue;
        }
        Json out;
        out["name"] = name;
        out.update(*json);
        std::println("{}", out.dump(2));
    }
    return exitCode;
}
