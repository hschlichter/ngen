#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The scene pack types, separate from their reader and writer (scenepack.h) so that code built as C++20, the USD packer among it, can
// fill them: OpenUSD's headers don't compile as C++23, and the reader uses std::expected.

inline constexpr uint32_t scenePackFormatVersion = 4;
inline constexpr size_t scenePackHeaderSize = 32;

enum class ScenePackKind : uint32_t {
    Layer = 1,
    Mesh = 2,
    Material = 3,
};

// An authored value, kept as USD typed it: `typeName` is the USD value type ("matrix4d", "float3", "token[]", "asset", …). Numbers (bools,
// integers and every floating-point component, row by row for matrices) go in `numbers`, tokens and strings in `strings`. An asset
// path is already an asset id, resolved by the packer against the file that authors it; empty when it doesn't resolve.
struct PackValue {
    std::string typeName;
    std::vector<double> numbers;
    std::vector<std::string> strings;
};

struct PackAttribute {
    std::string name;
    PackValue value;
};

// A relationship's targets, as authored (paths in the file's namespace), and its metadata the engine reads (`bindMaterialAs`).
struct PackRelationship {
    std::string name;
    std::vector<std::string> targets;
    std::string bindMaterialAs;
};

enum class PlacementKind : uint32_t {
    Reference = 1,
    Payload = 2,
};

// A reference or payload a prim authors. `assetId` is the referenced file's id, empty for an internal reference (a prim in the same file);
// `primPath` is the target prim, empty for the file's default prim.
struct PackPlacement {
    PlacementKind kind = PlacementKind::Reference;
    std::string assetId;
    std::string primPath;
};

enum class PrimSpecifier : uint32_t {
    Def = 1,
    Over = 2,
    Class = 3,
};

// One prim spec of the file and the opinions on it the engine uses (src/asset/README.md lists them). A spec inside a variant is a prim spec
// of its own at its variant path ("/asset{shape=a}", "/asset{shape=a}Geom"), after the prim that authors the variant set.
struct PackPrim {
    std::string path;
    PrimSpecifier specifier = PrimSpecifier::Over;
    std::string typeName;
    std::vector<std::string> apiSchemas; // added or explicit, as authored
    int32_t active = -1;                 // -1 not authored, 0 false, 1 true
    int32_t instanceable = -1;
    std::string kind;
    std::vector<PackAttribute> attributes;
    std::vector<PackRelationship> relationships;
    std::vector<PackPlacement> placements;
    std::vector<std::string> variantSetNames;                           // the variant sets the spec adds (variantSetNames), as authored
    std::vector<std::pair<std::string, std::string>> variantSelections; // `variants`: a set's name and the variant selected, by name
    int64_t pointCount = -1;                                            // the number of `points` the spec authors (a mesh's, a point cloud's), -1 when it authors none
    std::string meshAsset;                                              // set when this spec defines geometry: "<file>#<path>"
    std::string materialAsset;                                          // set when this spec defines a material
};

struct PackWarning {
    std::string path; // the prim, or empty for the layer
    std::string text;
};

struct LayerPack {
    std::string upAxis;         // empty when not authored
    double metersPerUnit = 0.0; // 0 when not authored
    std::string defaultPrim;
    std::vector<std::string> sublayers; // asset ids, strongest first
    std::vector<PackPrim> prims;        // in the file's order, parents before children
    std::vector<PackWarning> warnings;
};

// The engine's vertex layout (types.h `Vertex`).
struct PackVertex {
    std::array<float, 3> position;
    std::array<float, 3> normal;
    std::array<float, 3> color;
    std::array<float, 2> texCoord;
};

// A range of the index buffer drawn with one material: a GeomSubset's faces, or the mesh's faces outside every subset (`subsetPath` empty).
// The material is the binding the engine resolves for the subset, or for the mesh.
struct PackSubmesh {
    uint32_t indexOffset = 0;
    uint32_t indexCount = 0;
    std::string subsetPath;
};

struct MeshPack {
    std::vector<PackVertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<PackSubmesh> submeshes;
    bool doubleSided = false;
    bool hasDisplayColor = false; // the colour a prim with no material shows (primvars:displayColor)
    std::array<float, 3> displayColor = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> boundsMin = {0.0f, 0.0f, 0.0f};
    std::array<float, 3> boundsMax = {0.0f, 0.0f, 0.0f};
};

// What the engine takes from a material's surface shader: the base colour, as a texture or a constant, and its cutout: drawn only where
// the opacity reaches opacityThreshold (0 is opaque; translucency isn't read). A texture whose file is connected to one of the material's
// own inputs (its interface) names that input: where the material is used, the input's composed value is the file, and the texture asset
// here is the input's own default (empty when it has none).
struct MaterialPack {
    bool hasBaseColor = false;
    std::array<float, 4> baseColor = {1.0f, 1.0f, 1.0f, 1.0f};
    std::string baseColorTexture;      // a texture asset id, empty when none
    std::string baseColorTextureInput; // the material input its file comes from ("color_texture" for inputs:color_texture), or empty
    float opacity = 1.0f;              // the constant, when there is no opacity texture
    std::string opacityTexture;        // a texture asset id, empty for none
    std::string opacityTextureInput;
    uint32_t opacityChannel = 0; // 0 r, 1 g, 2 b, 3 a
    bool opacityRaw = false;     // the texture is authored raw (sourceColorSpace): the sampler's sRGB decode is undone
    float opacityThreshold = 0.0f;
};
