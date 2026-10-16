#include "usdpacker.h"

#include "assetid.h"
#include "primshapemesh.h"

#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/quatd.h>
#include <pxr/base/gf/quatf.h>
#include <pxr/base/gf/vec2d.h>
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4d.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/tf/stringUtils.h>
#include <pxr/base/tf/token.h>
#include <pxr/base/vt/array.h>
#include <pxr/base/vt/value.h>
#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/sdf/attributeSpec.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/sdf/primSpec.h>
#include <pxr/usd/sdf/relationshipSpec.h>
#include <pxr/usd/sdf/variantSetSpec.h>
#include <pxr/usd/sdf/variantSpec.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/cone.h>
#include <pxr/usd/usdGeom/cube.h>
#include <pxr/usd/usdGeom/cylinder.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdGeom/sphere.h>
#include <pxr/usd/usdGeom/subset.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usd/usdShade/connectableAPI.h>
#include <pxr/usd/usdShade/material.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>
#include <pxr/usd/usdShade/shader.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <optional>
#include <set>

PXR_NAMESPACE_USING_DIRECTIVE

namespace {

// Prim types whose spec defines geometry: a mesh asset is packed from it.
const std::set<std::string> geometryTypes = {"Mesh", "Cube", "Sphere", "Cylinder", "Cone"};

// Prim types whose attributes belong to a material asset, not to the layer pack.
const std::set<std::string> shadingTypes = {"Material", "Shader", "NodeGraph"};

// Geometry attributes: on a spec that doesn't define the prim (an over) they would override a mesh packed elsewhere, which isn't supported.
auto isGeometryAttribute(const std::string& name) -> bool {
    static const std::set<std::string> names = {"points",
                                                "faceVertexCounts",
                                                "faceVertexIndices",
                                                "normals",
                                                "orientation",
                                                "doubleSided",
                                                "subdivisionScheme",
                                                "holeIndices",
                                                "size",
                                                "radius",
                                                "height",
                                                "axis"};
    return names.contains(name) || name.starts_with("primvars:");
}

// The attributes the layer pack keeps (src/asset/README.md, "The USD packer").
auto isEngineAttribute(const std::string& name) -> bool {
    static const std::set<std::string> names = {"xformOpOrder", "visibility", "purpose", "extentsHint", "extent", "thresholds", "blendThresholds", "projectionMethod"};
    return names.contains(name) || name.starts_with("xformOp:") || name.starts_with("lod:") || name.starts_with("inputs:");
}

auto isEngineRelationship(const std::string& name) -> bool {
    return name == "material:binding" || name.starts_with("material:binding:") || name == "lod:heuristics" || name == "boundingVolume";
}

// An asset path as the file authors it, to an asset id: anchored to the file's folder, '\' read as a separator (assets authored on
// Windows write @textures\brick.png@). Empty when it doesn't resolve to a path below the server's directory.
auto resolveAssetId(const std::string& assetPath, const std::string& fileId) -> std::string {
    if (assetPath.empty()) {
        return {};
    }
    auto normalized = TfStringReplace(assetPath, "\\", "/");
    return assetIdForPath(std::filesystem::path(normalized), std::filesystem::path(fileId).parent_path());
}

auto warn(LayerPack& layer, const std::string& path, std::string text) -> void;

// A screen-size heuristic's authored extent that is empty or not finite measures nothing: the engine doesn't choose the heuristic (the root
// keeps its default level), so the author hears of it here.
auto warnEmptyHeuristicExtent(LayerPack& layer, const std::string& path, const SdfPrimSpecHandle& spec) -> void {
    auto attribute = spec->GetAttributeAtPath(spec->GetPath().AppendProperty(TfToken("extent")));
    if (!attribute || !attribute->HasDefaultValue() || !attribute->GetDefaultValue().IsHolding<VtArray<GfVec3f>>()) {
        return;
    }
    const auto& box = attribute->GetDefaultValue().UncheckedGet<VtArray<GfVec3f>>();
    bool valid = box.size() == 2;
    for (int axis = 0; valid && axis < 3; axis++) {
        valid = std::isfinite(box[0][axis]) && std::isfinite(box[1][axis]) && box[0][axis] <= box[1][axis] && box[1][axis] - box[0][axis] <= 1e30f;
    }
    if (!valid) {
        warn(layer, path, "a LOD screen-size heuristic's extent is empty or not finite; the engine keeps its root at its default level");
    }
}

auto warn(LayerPack& layer, const std::string& path, std::string text) -> void {
    layer.warnings.push_back({.path = path, .text = std::move(text)});
}

template <typename T>
auto numbers(const T& vec, size_t count) -> std::vector<double> {
    std::vector<double> out;
    for (size_t i = 0; i < count; i++) {
        out.push_back((double) vec[i]);
    }
    return out;
}

// A value as the layer pack keeps it; false for a type the engine doesn't read.
auto toPackValue(const VtValue& value, const std::string& typeName, const std::string& fileId, PackValue& out) -> bool {
    out.typeName = typeName;
    if (value.IsHolding<bool>()) {
        out.numbers = {value.UncheckedGet<bool>() ? 1.0 : 0.0};
        return true;
    }
    if (value.IsHolding<int>()) {
        out.numbers = {(double) value.UncheckedGet<int>()};
        return true;
    }
    if (value.IsHolding<float>()) {
        out.numbers = {(double) value.UncheckedGet<float>()};
        return true;
    }
    if (value.IsHolding<double>()) {
        out.numbers = {value.UncheckedGet<double>()};
        return true;
    }
    if (value.IsHolding<TfToken>()) {
        out.strings = {value.UncheckedGet<TfToken>().GetString()};
        return true;
    }
    if (value.IsHolding<std::string>()) {
        out.strings = {value.UncheckedGet<std::string>()};
        return true;
    }
    if (value.IsHolding<SdfAssetPath>()) {
        out.strings = {resolveAssetId(value.UncheckedGet<SdfAssetPath>().GetAssetPath(), fileId)};
        return true;
    }
    if (value.IsHolding<GfVec2f>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec2f>(), 2);
        return true;
    }
    if (value.IsHolding<GfVec2d>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec2d>(), 2);
        return true;
    }
    if (value.IsHolding<GfVec3f>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec3f>(), 3);
        return true;
    }
    if (value.IsHolding<GfVec3d>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec3d>(), 3);
        return true;
    }
    if (value.IsHolding<GfVec4f>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec4f>(), 4);
        return true;
    }
    if (value.IsHolding<GfVec4d>()) {
        out.numbers = numbers(value.UncheckedGet<GfVec4d>(), 4);
        return true;
    }
    // Quaternions as (real, i, j, k).
    if (value.IsHolding<GfQuatf>()) {
        const auto& q = value.UncheckedGet<GfQuatf>();
        out.numbers = {(double) q.GetReal(), (double) q.GetImaginary()[0], (double) q.GetImaginary()[1], (double) q.GetImaginary()[2]};
        return true;
    }
    if (value.IsHolding<GfQuatd>()) {
        const auto& q = value.UncheckedGet<GfQuatd>();
        out.numbers = {q.GetReal(), q.GetImaginary()[0], q.GetImaginary()[1], q.GetImaginary()[2]};
        return true;
    }
    // Matrices row by row.
    if (value.IsHolding<GfMatrix4d>()) {
        const auto& m = value.UncheckedGet<GfMatrix4d>();
        for (int row = 0; row < 4; row++) {
            for (int column = 0; column < 4; column++) {
                out.numbers.push_back(m[row][column]);
            }
        }
        return true;
    }
    if (value.IsHolding<VtArray<float>>()) {
        for (float f : value.UncheckedGet<VtArray<float>>()) {
            out.numbers.push_back((double) f);
        }
        return true;
    }
    if (value.IsHolding<VtArray<double>>()) {
        for (double d : value.UncheckedGet<VtArray<double>>()) {
            out.numbers.push_back(d);
        }
        return true;
    }
    if (value.IsHolding<VtArray<int>>()) {
        for (int i : value.UncheckedGet<VtArray<int>>()) {
            out.numbers.push_back((double) i);
        }
        return true;
    }
    if (value.IsHolding<VtArray<GfVec3f>>()) {
        for (const auto& v : value.UncheckedGet<VtArray<GfVec3f>>()) {
            out.numbers.push_back((double) v[0]);
            out.numbers.push_back((double) v[1]);
            out.numbers.push_back((double) v[2]);
        }
        return true;
    }
    if (value.IsHolding<VtArray<TfToken>>()) {
        for (const auto& t : value.UncheckedGet<VtArray<TfToken>>()) {
            out.strings.push_back(t.GetString());
        }
        return true;
    }
    return false;
}

auto toPlacement(PlacementKind kind, const std::string& assetPath, const SdfPath& primPath, const std::string& fileId, const std::string& specPath, LayerPack& layer)
    -> PackPlacement {
    PackPlacement placement;
    placement.kind = kind;
    placement.primPath = primPath.IsEmpty() ? std::string() : primPath.GetString();
    if (!assetPath.empty()) {
        placement.assetId = resolveAssetId(assetPath, fileId);
        if (placement.assetId.empty()) {
            warn(layer, specPath, std::format("{} @{}@ doesn't resolve to a file below the asset server's directory", kind == PlacementKind::Payload ? "payload" : "reference", assetPath));
        }
    }
    return placement;
}

// One prim spec's opinions into the layer pack, then its children's, then its variants' (each variant's prim spec and what is under it, at
// their variant paths).
auto packSpec(const SdfPrimSpecHandle& spec, const std::string& fileId, LayerPack& layer) -> void {
    PackPrim prim;
    prim.path = spec->GetPath().GetString();
    switch (spec->GetSpecifier()) {
        case SdfSpecifierDef:
            prim.specifier = PrimSpecifier::Def;
            break;
        case SdfSpecifierClass:
            prim.specifier = PrimSpecifier::Class;
            break;
        default:
            prim.specifier = PrimSpecifier::Over;
            break;
    }
    prim.typeName = spec->GetTypeName().GetString();
    if (spec->HasInfo(TfToken("apiSchemas"))) {
        auto schemas = spec->GetInfo(TfToken("apiSchemas"));
        if (schemas.IsHolding<SdfTokenListOp>()) {
            for (const auto& token : schemas.UncheckedGet<SdfTokenListOp>().GetAppliedItems()) {
                prim.apiSchemas.push_back(token.GetString());
            }
        }
    }
    if (spec->HasActive()) {
        prim.active = spec->GetActive() ? 1 : 0;
    }
    if (spec->HasInstanceable()) {
        prim.instanceable = spec->GetInstanceable() ? 1 : 0;
    }
    if (spec->HasKind()) {
        prim.kind = spec->GetKind().GetString();
    }

    bool shading = shadingTypes.contains(prim.typeName);
    bool definesGeometry = spec->GetSpecifier() == SdfSpecifierDef && geometryTypes.contains(prim.typeName);
    size_t timeSampled = 0;
    std::vector<std::string> overriddenGeometry;
    for (const auto& attribute : spec->GetAttributes()) {
        auto name = attribute->GetName();
        if (!definesGeometry && spec->GetSpecifier() != SdfSpecifierDef && isGeometryAttribute(name)) {
            overriddenGeometry.push_back(name);
            continue;
        }
        if (name == "points" && attribute->HasDefaultValue()) {
            prim.pointCount = (int64_t) attribute->GetDefaultValue().GetArraySize();
        }
        if (shading || !isEngineAttribute(name)) {
            continue;
        }
        if (!attribute->HasDefaultValue()) {
            if (attribute->HasInfo(TfToken("timeSamples"))) {
                timeSampled++;
            }
            continue;
        }
        PackAttribute packed;
        packed.name = name;
        if (!toPackValue(attribute->GetDefaultValue(), attribute->GetTypeName().GetAsToken().GetString(), fileId, packed.value)) {
            warn(layer, prim.path, std::format("{} has a value of type {}, which the engine doesn't read", name, attribute->GetTypeName().GetAsToken().GetString()));
            continue;
        }
        prim.attributes.push_back(std::move(packed));
    }
    if (!overriddenGeometry.empty()) {
        warn(layer, prim.path, std::format("an over authors geometry ({}); ignored: a mesh is packed from the file that defines it", TfStringJoin(overriddenGeometry, ", ")));
    }
    if (timeSampled > 0) {
        warn(layer, prim.path, std::format("{} attributes have time samples only; the engine reads default values", timeSampled));
    }
    if (prim.typeName == "LODScreenSizeHeuristic") {
        warnEmptyHeuristicExtent(layer, prim.path, spec);
    }

    for (const auto& relationship : spec->GetRelationships()) {
        auto name = relationship->GetName();
        if (!isEngineRelationship(name)) {
            continue;
        }
        PackRelationship packed;
        packed.name = name;
        for (const auto& target : relationship->GetTargetPathList().GetAppliedItems()) {
            packed.targets.push_back(target.MakeAbsolutePath(spec->GetPath()).GetString());
        }
        if (relationship->HasInfo(TfToken("bindMaterialAs"))) {
            auto strength = relationship->GetInfo(TfToken("bindMaterialAs"));
            if (strength.IsHolding<TfToken>()) {
                packed.bindMaterialAs = strength.UncheckedGet<TfToken>().GetString();
            }
        }
        if (name.starts_with("material:binding:collection")) {
            warn(layer, prim.path, std::format("{} is a collection-based binding, which the engine doesn't read", name));
            continue;
        }
        prim.relationships.push_back(std::move(packed));
    }

    for (const auto& reference : spec->GetReferenceList().GetAppliedItems()) {
        if (!reference.GetLayerOffset().IsIdentity()) {
            warn(layer, prim.path, "a reference has a layer offset; ignored");
        }
        prim.placements.push_back(toPlacement(PlacementKind::Reference, reference.GetAssetPath(), reference.GetPrimPath(), fileId, prim.path, layer));
    }
    for (const auto& payload : spec->GetPayloadList().GetAppliedItems()) {
        if (!payload.GetLayerOffset().IsIdentity()) {
            warn(layer, prim.path, "a payload has a layer offset; ignored");
        }
        prim.placements.push_back(toPlacement(PlacementKind::Payload, payload.GetAssetPath(), payload.GetPrimPath(), fileId, prim.path, layer));
    }
    if (!spec->GetInheritPathList().GetAppliedItems().empty()) {
        warn(layer, prim.path, "inherits, which the engine doesn't compose");
    }
    if (!spec->GetSpecializesList().GetAppliedItems().empty()) {
        warn(layer, prim.path, "specializes, which the engine doesn't compose");
    }
    for (const auto& name : spec->GetVariantSetNameList().GetAppliedItems()) {
        prim.variantSetNames.push_back(name);
    }
    for (const auto& [set, variant] : spec->GetVariantSelections()) {
        prim.variantSelections.emplace_back(set, variant);
    }
    if (spec->HasField(TfToken("relocates"))) {
        warn(layer, prim.path, "relocates, which the engine doesn't compose");
    }

    if (definesGeometry) {
        prim.meshAsset = fileId + "#" + prim.path;
    }
    if (spec->GetSpecifier() == SdfSpecifierDef && prim.typeName == "Material") {
        prim.materialAsset = fileId + "#" + prim.path;
    }
    layer.prims.push_back(std::move(prim));

    for (const auto& child : spec->GetNameChildren()) {
        packSpec(child, fileId, layer);
    }
    for (const auto& entry : spec->GetVariantSets()) {
        SdfVariantSetSpecHandle set = entry.second;
        for (const auto& variant : set->GetVariantList()) {
            if (variant->GetPrimSpec()) {
                packSpec(variant->GetPrimSpec(), fileId, layer);
            }
        }
    }
}

// The file-local stage a prim spec's mesh or material is read from: for a spec inside variants, one with those variants selected (a session
// layer selects them on the prims that own the sets), by the selections; the spec's path on it has the selections stripped.
struct LocalStages {
    SdfLayerRefPtr local;
    std::map<std::string, UsdStageRefPtr> bySelections;

    auto stageFor(const SdfPath& specPath) -> UsdStageRefPtr {
        std::vector<std::pair<SdfPath, std::pair<std::string, std::string>>> selections;
        for (SdfPath at = specPath; !at.IsEmpty() && at != SdfPath::AbsoluteRootPath(); at = at.GetParentPath()) {
            if (at.IsPrimVariantSelectionPath()) {
                selections.emplace_back(at.GetParentPath().StripAllVariantSelections(), at.GetVariantSelection());
            }
        }
        std::string key;
        for (const auto& [owner, selection] : selections) {
            key += std::format("{}{{{}={}}};", owner.GetString(), selection.first, selection.second);
        }
        if (auto it = bySelections.find(key); it != bySelections.end()) {
            return it->second;
        }
        auto session = SdfLayer::CreateAnonymous("selections.usda");
        for (const auto& [owner, selection] : selections) {
            auto over = SdfCreatePrimInLayer(session, owner);
            over->SetVariantSelection(selection.first, selection.second);
        }
        auto stage = selections.empty() ? UsdStage::Open(local, UsdStage::LoadAll) : UsdStage::Open(local, session, UsdStage::LoadAll);
        bySelections[key] = stage;
        return stage;
    }
};

// Every prim spec of a layer, variant contents included.
auto forEachSpec(const SdfPrimSpecHandle& spec, const std::function<void(const SdfPrimSpecHandle&)>& visit) -> void {
    visit(spec);
    for (const auto& child : spec->GetNameChildren()) {
        forEachSpec(child, visit);
    }
    for (const auto& entry : spec->GetVariantSets()) {
        SdfVariantSetSpecHandle set = entry.second;
        for (const auto& variant : set->GetVariantList()) {
            if (variant->GetPrimSpec()) {
                forEachSpec(variant->GetPrimSpec(), visit);
            }
        }
    }
}

// A copy of the file with every arc to another file removed: sublayers, and references and payloads naming a file.
auto fileLocalLayer(const SdfLayerRefPtr& layer) -> SdfLayerRefPtr {
    auto copy = SdfLayer::CreateAnonymous("filelocal.usda");
    copy->TransferContent(layer);
    copy->SetSubLayerPaths({});
    for (const auto& root : copy->GetRootPrims()) {
        forEachSpec(root, [](const SdfPrimSpecHandle& spec) {
            for (const auto& reference : spec->GetReferenceList().GetAppliedItems()) {
                if (!reference.GetAssetPath().empty()) {
                    spec->GetReferenceList().RemoveItemEdits(reference);
                }
            }
            for (const auto& payload : spec->GetPayloadList().GetAppliedItems()) {
                if (!payload.GetAssetPath().empty()) {
                    spec->GetPayloadList().RemoveItemEdits(payload);
                }
            }
        });
    }
    return copy;
}

// ---- Meshes: what USDScene's extractMesh did, on the file-local stage.

// A primvar with its index table applied, and its interpolation. Indexed faceVarying primvars (NewSponza's st and normals) read raw would
// scramble.
template <typename T>
auto readPrimvarFlattened(const UsdGeomPrimvarsAPI& api, const TfToken& name, VtArray<T>& values, TfToken& interpolation) -> bool {
    auto primvar = api.GetPrimvar(name);
    if (!primvar || !primvar.HasValue()) {
        return false;
    }
    interpolation = primvar.GetInterpolation();
    return primvar.ComputeFlattened(&values, UsdTimeCode::Default()) && !values.empty();
}

// The index into a flattened primvar for a triangle corner, per the primvar's interpolation.
auto primvarIndex(const TfToken& interpolation, int pointIndex, int cornerIndex, int faceIndex) -> int {
    if (interpolation == UsdGeomTokens->constant) {
        return 0;
    }
    if (interpolation == UsdGeomTokens->uniform) {
        return faceIndex;
    }
    if (interpolation == UsdGeomTokens->faceVarying) {
        return cornerIndex;
    }
    return pointIndex;
}

auto toPackVertex(const Vertex& vertex) -> PackVertex {
    return {.position = vertex.position, .normal = vertex.normal, .color = vertex.color, .texCoord = vertex.texCoord};
}

auto shapeMesh(const UsdPrim& prim, MeshPack& mesh) -> bool {
    MeshDesc desc;
    if (auto cube = UsdGeomCube(prim)) {
        double size = 2.0;
        cube.GetSizeAttr().Get(&size, UsdTimeCode::Default());
        desc = tessellateCube(size);
    } else if (auto sphere = UsdGeomSphere(prim)) {
        double radius = 1.0;
        sphere.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
        desc = tessellateSphere(radius);
    } else if (auto cylinder = UsdGeomCylinder(prim)) {
        double radius = 1.0;
        double height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        cylinder.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
        cylinder.GetHeightAttr().Get(&height, UsdTimeCode::Default());
        cylinder.GetAxisAttr().Get(&axis, UsdTimeCode::Default());
        desc = tessellateCylinder(radius, height, axis.GetText());
    } else if (auto cone = UsdGeomCone(prim)) {
        double radius = 1.0;
        double height = 2.0;
        TfToken axis = UsdGeomTokens->z;
        cone.GetRadiusAttr().Get(&radius, UsdTimeCode::Default());
        cone.GetHeightAttr().Get(&height, UsdTimeCode::Default());
        cone.GetAxisAttr().Get(&axis, UsdTimeCode::Default());
        desc = tessellateCone(radius, height, axis.GetText());
    } else {
        return false;
    }
    for (const auto& vertex : desc.vertices) {
        mesh.vertices.push_back(toPackVertex(vertex));
    }
    mesh.indices = desc.indices;
    if (!mesh.vertices.empty()) {
        mesh.submeshes.push_back({.indexOffset = 0, .indexCount = (uint32_t) mesh.indices.size(), .subsetPath = {}});
    }
    return true;
}

auto polygonMesh(const UsdPrim& prim, MeshPack& mesh) -> bool {
    UsdGeomMesh geomMesh(prim);
    if (!geomMesh) {
        return false;
    }
    VtArray<GfVec3f> points;
    geomMesh.GetPointsAttr().Get(&points, UsdTimeCode::Default());
    if (points.empty()) {
        return false;
    }
    VtArray<int> faceVertexCounts;
    VtArray<int> faceVertexIndices;
    geomMesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts, UsdTimeCode::Default());
    geomMesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices, UsdTimeCode::Default());

    UsdGeomPrimvarsAPI primvars(prim);
    // Normals: primvars:normals first (how NewSponza and most exports author them), then the schema's normals, else computed below.
    VtArray<GfVec3f> normals;
    TfToken normalsInterpolation;
    bool hasNormals = readPrimvarFlattened(primvars, TfToken("normals"), normals, normalsInterpolation);
    if (!hasNormals && geomMesh.GetNormalsAttr().Get(&normals, UsdTimeCode::Default()) && !normals.empty()) {
        normalsInterpolation = geomMesh.GetNormalsInterpolation();
        hasNormals = true;
    }
    VtArray<GfVec2f> uvs;
    TfToken uvInterpolation;
    bool hasUv = false;
    for (const auto* name : {"st", "st0", "st1", "UVMap"}) {
        if (readPrimvarFlattened(primvars, TfToken(name), uvs, uvInterpolation)) {
            hasUv = true;
            break;
        }
    }

    // rightHanded (the default) is counter-clockwise front faces, what the pipelines cull against; leftHanded meshes swap two corners per
    // triangle. With no authored normals a subdivision surface (USD's default scheme) gets smooth normals, area weighted; only an explicit
    // subdivisionScheme "none" keeps flat faces.
    TfToken orientation;
    geomMesh.GetOrientationAttr().Get(&orientation, UsdTimeCode::Default());
    bool leftHanded = orientation == UsdGeomTokens->leftHanded;
    geomMesh.GetDoubleSidedAttr().Get(&mesh.doubleSided, UsdTimeCode::Default());

    std::vector<std::array<float, 3>> smoothNormals;
    if (!hasNormals) {
        TfToken scheme;
        geomMesh.GetSubdivisionSchemeAttr().Get(&scheme, UsdTimeCode::Default());
        if (scheme != UsdGeomTokens->none) {
            smoothNormals.assign(points.size(), {0.0f, 0.0f, 0.0f});
            size_t corner = 0;
            for (size_t f = 0; f < faceVertexCounts.size(); f++) {
                int count = faceVertexCounts[f];
                for (int t = 0; t < count - 2; t++) {
                    int i0 = faceVertexIndices[corner];
                    int i1 = faceVertexIndices[corner + t + 1];
                    int i2 = faceVertexIndices[corner + t + 2];
                    if (leftHanded) {
                        std::swap(i1, i2);
                    }
                    GfVec3f n = GfCross(points[i1] - points[i0], points[i2] - points[i0]);
                    for (int i : {i0, i1, i2}) {
                        smoothNormals[i][0] += n[0];
                        smoothNormals[i][1] += n[1];
                        smoothNormals[i][2] += n[2];
                    }
                }
                corner += count;
            }
            for (auto& n : smoothNormals) {
                float length = std::sqrt((n[0] * n[0]) + (n[1] * n[1]) + (n[2] * n[2]));
                if (length > 0.0f) {
                    n = {n[0] / length, n[1] / length, n[2] / length};
                }
            }
        }
    }

    // Triangulate as a fan, one vertex per corner in face order, remembering where each face's vertices land so the index buffer can be
    // grouped by GeomSubset afterwards.
    std::vector<uint32_t> faceStart(faceVertexCounts.size(), 0);
    std::vector<uint32_t> faceCount(faceVertexCounts.size(), 0);
    uint32_t corner = 0;
    for (size_t f = 0; f < faceVertexCounts.size(); f++) {
        int count = faceVertexCounts[f];
        faceStart[f] = (uint32_t) mesh.vertices.size();
        for (int t = 0; t < count - 2; t++) {
            int indices[3] = {faceVertexIndices[corner], faceVertexIndices[corner + t + 1], faceVertexIndices[corner + t + 2]};
            int corners[3] = {(int) corner, (int) (corner + t + 1), (int) (corner + t + 2)};
            if (leftHanded) {
                std::swap(indices[1], indices[2]);
                std::swap(corners[1], corners[2]);
            }
            std::array<float, 3> faceNormal = {0.0f, 1.0f, 0.0f};
            if (!hasNormals && smoothNormals.empty()) {
                GfVec3f n = GfCross(points[indices[1]] - points[indices[0]], points[indices[2]] - points[indices[0]]);
                float length = n.GetLength();
                if (length > 0.0f) {
                    faceNormal = {n[0] / length, n[1] / length, n[2] / length};
                }
            }
            for (int v = 0; v < 3; v++) {
                PackVertex vertex = {};
                const auto& p = points[indices[v]];
                vertex.position = {p[0], p[1], p[2]};
                if (hasNormals) {
                    int ni = primvarIndex(normalsInterpolation, indices[v], corners[v], (int) f);
                    if (ni < (int) normals.size()) {
                        vertex.normal = {normals[ni][0], normals[ni][1], normals[ni][2]};
                    }
                } else if (!smoothNormals.empty()) {
                    vertex.normal = smoothNormals[indices[v]];
                } else {
                    vertex.normal = faceNormal;
                }
                if (hasUv) {
                    int ui = primvarIndex(uvInterpolation, indices[v], corners[v], (int) f);
                    if (ui < (int) uvs.size()) {
                        vertex.texCoord = {uvs[ui][0], 1.0f - uvs[ui][1]};
                    }
                }
                // The base colour comes from the material, so vertices stay white and don't tint it.
                vertex.color = {1.0f, 1.0f, 1.0f};
                mesh.vertices.push_back(vertex);
            }
        }
        faceCount[f] = (uint32_t) mesh.vertices.size() - faceStart[f];
        corner += count;
    }
    if (mesh.vertices.empty()) {
        return false;
    }

    // The index buffer grouped so each GeomSubset bound to a material is one contiguous range; faces outside every subset are the last.
    auto appendFace = [&](uint32_t f) {
        for (uint32_t j = 0; j < faceCount[f]; j++) {
            mesh.indices.push_back(faceStart[f] + j);
        }
    };
    auto subsets = UsdShadeMaterialBindingAPI(prim).GetMaterialBindSubsets();
    std::vector<bool> assigned(faceCount.size(), false);
    for (const auto& subset : subsets) {
        VtArray<int> subsetFaces;
        subset.GetIndicesAttr().Get(&subsetFaces, UsdTimeCode::Default());
        PackSubmesh submesh;
        submesh.indexOffset = (uint32_t) mesh.indices.size();
        submesh.subsetPath = subset.GetPath().GetString();
        for (int f : subsetFaces) {
            if (f < 0 || f >= (int) faceCount.size()) {
                continue;
            }
            assigned[f] = true;
            appendFace((uint32_t) f);
        }
        submesh.indexCount = (uint32_t) mesh.indices.size() - submesh.indexOffset;
        if (submesh.indexCount > 0) {
            mesh.submeshes.push_back(submesh);
        }
    }
    PackSubmesh rest;
    rest.indexOffset = (uint32_t) mesh.indices.size();
    for (uint32_t f = 0; f < faceCount.size(); f++) {
        if (!assigned[f]) {
            appendFace(f);
        }
    }
    rest.indexCount = (uint32_t) mesh.indices.size() - rest.indexOffset;
    if (rest.indexCount > 0) {
        mesh.submeshes.push_back(rest);
    }
    return true;
}

auto packMesh(const UsdPrim& prim, MeshPack& mesh) -> bool {
    if (!shapeMesh(prim, mesh) && !polygonMesh(prim, mesh)) {
        return false;
    }
    VtArray<GfVec3f> displayColors;
    if (UsdGeomGprim(prim).GetDisplayColorAttr().Get(&displayColors) && !displayColors.empty()) {
        mesh.hasDisplayColor = true;
        mesh.displayColor = {displayColors[0][0], displayColors[0][1], displayColors[0][2]};
    }
    mesh.boundsMin = mesh.vertices.front().position;
    mesh.boundsMax = mesh.vertices.front().position;
    for (const auto& vertex : mesh.vertices) {
        for (int axis = 0; axis < 3; axis++) {
            mesh.boundsMin[axis] = std::min(mesh.boundsMin[axis], vertex.position[axis]);
            mesh.boundsMax[axis] = std::max(mesh.boundsMax[axis], vertex.position[axis]);
        }
    }
    return true;
}

// ---- Materials: the surface shader's diffuse colour, a texture or a constant, as USDScene's extractMaterial read it.

// The shader a connection ends at, through node graphs, and the name of the output it reads.
auto resolveConnectedOutput(UsdShadeConnectableAPI source, TfToken sourceName) -> std::pair<UsdShadeShader, TfToken> {
    for (int guard = 0; guard < 16 && source.GetPrim(); guard++) {
        if (source.GetPrim().IsA<UsdShadeShader>()) {
            return {UsdShadeShader(source.GetPrim()), sourceName};
        }
        auto output = source.GetOutput(sourceName);
        if (!output) {
            break;
        }
        UsdShadeConnectableAPI next;
        TfToken nextName;
        UsdShadeAttributeType nextType;
        if (!output.GetConnectedSource(&next, &nextName, &nextType)) {
            break;
        }
        source = next;
        sourceName = nextName;
    }
    return {UsdShadeShader(), TfToken()};
}

// The texture an input is connected to, through node graphs: its asset id (empty when it doesn't resolve), the output it reads, and its
// sourceColorSpace. Nullopt when the input isn't connected to a texture.
struct ConnectedTexture {
    std::string assetId;
    std::string output;
    std::string colorSpace;
};

auto connectedTexture(const UsdShadeInput& input, const UsdPrim& material, const std::string& fileId, LayerPack& layer) -> std::optional<ConnectedTexture> {
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType;
    if (!input.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return std::nullopt;
    }
    auto [textureShader, outputName] = resolveConnectedOutput(source, sourceName);
    if (!textureShader) {
        return std::nullopt;
    }
    auto file = textureShader.GetInput(TfToken("file"));
    SdfAssetPath assetPath;
    if (!file || !file.Get(&assetPath, UsdTimeCode::Default())) {
        return std::nullopt;
    }
    ConnectedTexture texture;
    texture.assetId = resolveAssetId(assetPath.GetAssetPath(), fileId);
    if (texture.assetId.empty() && !assetPath.GetAssetPath().empty()) {
        warn(layer, material.GetPath().GetString(), std::format("texture @{}@ doesn't resolve to a file below the asset server's directory", assetPath.GetAssetPath()));
    }
    texture.output = outputName.GetString();
    TfToken colorSpace;
    if (auto space = textureShader.GetInput(TfToken("sourceColorSpace")); space && space.Get(&colorSpace, UsdTimeCode::Default())) {
        texture.colorSpace = colorSpace.GetString();
    }
    return texture;
}

auto packMaterial(const UsdPrim& prim, const std::string& fileId, MaterialPack& material, LayerPack& layer) -> bool {
    UsdShadeMaterial shadeMaterial(prim);
    if (!shadeMaterial) {
        return false;
    }
    auto surface = shadeMaterial.GetSurfaceOutput();
    UsdShadeConnectableAPI source;
    TfToken sourceName;
    UsdShadeAttributeType sourceType;
    if (!surface || !surface.GetConnectedSource(&source, &sourceName, &sourceType)) {
        return true;
    }
    UsdShadeShader shader(source.GetPrim());
    if (!shader) {
        return true;
    }

    if (auto input = shader.GetInput(TfToken("diffuseColor"))) {
        if (auto texture = connectedTexture(input, prim, fileId, layer)) {
            material.baseColorTexture = texture->assetId;
        }
        GfVec3f color;
        if (input.Get(&color, UsdTimeCode::Default())) {
            material.hasBaseColor = true;
            material.baseColor = {color[0], color[1], color[2], 1.0f};
        }
    }

    // Cutout: the opacity, a constant or a texture's channel, cut against opacityThreshold. A threshold of 0 with an opacity below 1 is
    // translucency, which the engine doesn't draw: reported, and drawn opaque.
    float threshold = 0.0f;
    if (auto input = shader.GetInput(TfToken("opacityThreshold"))) {
        input.Get(&threshold, UsdTimeCode::Default());
    }
    std::optional<ConnectedTexture> opacityTexture;
    float opacity = 1.0f;
    if (auto input = shader.GetInput(TfToken("opacity"))) {
        opacityTexture = connectedTexture(input, prim, fileId, layer);
        input.Get(&opacity, UsdTimeCode::Default());
    }
    if (threshold > 0.0f) {
        material.opacityThreshold = threshold;
        material.opacity = opacity;
        if (opacityTexture) {
            material.opacityTexture = opacityTexture->assetId;
            const auto& output = opacityTexture->output;
            material.opacityChannel = output == "g" ? 1 : (output == "b" ? 2 : (output == "a" ? 3 : 0));
            material.opacityRaw = opacityTexture->colorSpace == "raw";
        }
    } else if (opacityTexture || opacity < 1.0f) {
        warn(layer, prim.GetPath().GetString(), "translucent (opacity below 1 with opacityThreshold 0), which the engine draws opaque");
    }
    return true;
}

} // namespace

auto packUsdFile(const std::string& assetId, const std::string& sourcePath) -> UsdPackResult {
    UsdPackResult result;
    auto layer = SdfLayer::FindOrOpen(sourcePath);
    if (!layer) {
        result.error = std::format("cannot open {} as a USD layer", sourcePath);
        return result;
    }
    auto& pack = result.layer;
    auto pseudoRoot = layer->GetPseudoRoot();
    if (pseudoRoot->HasInfo(TfToken("upAxis"))) {
        auto upAxis = pseudoRoot->GetInfo(TfToken("upAxis"));
        if (upAxis.IsHolding<TfToken>()) {
            pack.upAxis = upAxis.UncheckedGet<TfToken>().GetString();
        }
    }
    if (pseudoRoot->HasInfo(TfToken("metersPerUnit"))) {
        auto meters = pseudoRoot->GetInfo(TfToken("metersPerUnit"));
        if (meters.IsHolding<double>()) {
            pack.metersPerUnit = meters.UncheckedGet<double>();
        }
    }
    pack.defaultPrim = layer->GetDefaultPrim().GetString();
    for (const std::string sublayer : layer->GetSubLayerPaths()) {
        auto id = resolveAssetId(sublayer, assetId);
        if (id.empty()) {
            warn(pack, {}, std::format("sublayer @{}@ doesn't resolve to a file below the asset server's directory", sublayer));
            continue;
        }
        pack.sublayers.push_back(id);
    }
    for (const auto& root : layer->GetRootPrims()) {
        packSpec(root, assetId, pack);
    }

    // Meshes and materials, from the file alone.
    bool definesAssets = std::ranges::any_of(pack.prims, [](const PackPrim& prim) { return !prim.meshAsset.empty() || !prim.materialAsset.empty(); });
    if (!definesAssets) {
        return result;
    }
    LocalStages stages{.local = fileLocalLayer(layer), .bySelections = {}};
    if (!stages.stageFor(SdfPath::AbsoluteRootPath())) {
        result.error = std::format("cannot compose {} on its own", sourcePath);
        return result;
    }
    for (const auto& prim : pack.prims) {
        auto specPath = SdfPath(prim.path);
        auto stage = stages.stageFor(specPath);
        auto composedPath = specPath.StripAllVariantSelections();
        if (!prim.meshAsset.empty()) {
            MeshPack mesh;
            auto usdPrim = stage ? stage->GetPrimAtPath(composedPath) : UsdPrim();
            if (usdPrim && packMesh(usdPrim, mesh)) {
                result.meshes.emplace_back(prim.path, std::move(mesh));
            } else {
                warn(pack, prim.path, "defines no geometry the engine can draw");
            }
        }
        if (!prim.materialAsset.empty()) {
            MaterialPack material;
            auto usdPrim = stage ? stage->GetPrimAtPath(composedPath) : UsdPrim();
            if (usdPrim && packMaterial(usdPrim, assetId, material, pack)) {
                result.materials.emplace_back(prim.path, std::move(material));
            }
        }
    }
    // A prim whose geometry or material didn't pack names no asset.
    for (auto& prim : pack.prims) {
        if (!prim.meshAsset.empty() && std::ranges::none_of(result.meshes, [&prim](const auto& mesh) { return mesh.first == prim.path; })) {
            prim.meshAsset.clear();
        }
        if (!prim.materialAsset.empty() && std::ranges::none_of(result.materials, [&prim](const auto& material) { return material.first == prim.path; })) {
            prim.materialAsset.clear();
        }
    }
    return result;
}
