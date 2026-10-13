#include "worldcomposer.h"

#include "scenepack.h"
#include "xformops.h"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>

// How composition works here, after USD's prim indexing (pxr/usd/pcp):
//
// A composed prim's index is a tree of nodes. A node is a site, a prim path in a layer stack, that contributes opinions; the root node
// is the root layer stack at the prim's own path. A reference or payload adds a child node under the node whose spec authors it: the
// index of the target prim in its file's layer stack, mapped from the target's path to the authoring prim's. Opinions are read from the
// nodes in strength order, the tree depth first, a node before its children; within a node, its layer stack's layers strongest first.
// Siblings are ordered as USD orders them: references before payloads, an arc authored deeper in namespace before one inherited from an
// ancestor, then in authored order.
//
// A child prim's index is its parent's with every node's path extended by the child's name (a node without a spec there keeps only its
// own arcs), plus the arcs authored at the child's path. Paths a node authors (relationship targets) map to the composed namespace
// through the nodes above it; an internal reference maps paths outside its target unchanged, as in USD.
//
// An instance (instanceable, with arcs) shares a prototype with every instance of the same arcs: the prototype is composed once from the
// instance's index, and the instance's children are the prototype's. As in USD, the prototype keeps only the nodes an arc authored on the
// instance itself brings (with specs); every other node, the local layer stack and arcs from the instance's ancestors among them, is
// inert below the instance.

namespace {

auto childPath(const std::string& parent, const std::string& name) -> std::string {
    if (parent == "/") {
        return "/" + name;
    }
    return parent + "/" + name;
}

auto parentPath(const std::string& path) -> std::string {
    auto slash = path.rfind('/');
    if (slash == 0 || slash == std::string::npos) {
        return "/";
    }
    return path.substr(0, slash);
}

auto lastElement(const std::string& path) -> std::string {
    return path.substr(path.rfind('/') + 1);
}

auto elementCount(const std::string& path) -> int {
    if (path == "/") {
        return 0;
    }
    return (int) std::ranges::count(path, '/');
}

auto hasPrefix(const std::string& path, const std::string& prefix) -> bool {
    if (prefix == "/") {
        return true;
    }
    return path == prefix || (path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/');
}

auto replacePrefix(const std::string& path, const std::string& from, const std::string& to) -> std::string {
    if (path == from) {
        return to;
    }
    auto rest = from == "/" ? path.substr(1) : path.substr(from.size() + 1);
    return childPath(to, rest);
}

// Prim types that take transform operations (UsdGeomXformable and its subclasses).
const std::set<std::string> xformableTypes = {
    "Xform",
    "Mesh",
    "Cube",
    "Sphere",
    "Cylinder",
    "Cylinder_1",
    "Cone",
    "Capsule",
    "Capsule_1",
    "Plane",
    "Points",
    "BasisCurves",
    "NurbsCurves",
    "HermiteCurves",
    "NurbsPatch",
    "TetMesh",
    "PointInstancer",
    "Camera",
    "Volume",
    "SkelRoot",
    "Skeleton",
    "DistantLight",
    "SphereLight",
    "RectLight",
    "DiskLight",
    "CylinderLight",
    "DomeLight",
    "DomeLight_1",
    "PortalLight",
    "GeometryLight",
    "PluginLight",
};

// Prim types whose `points` USD counts (UsdGeomPointBased and its subclasses).
const std::set<std::string> pointBasedTypes = {"Mesh", "Points", "BasisCurves", "NurbsCurves", "HermiteCurves", "NurbsPatch", "TetMesh"};

const std::map<std::string, LightShape> lightTypes = {
    {"DistantLight", LightShape::Distant},
    {"SphereLight", LightShape::Sphere},
    {"RectLight", LightShape::Rect},
    {"DiskLight", LightShape::Disk},
    {"CylinderLight", LightShape::Cylinder},
    {"DomeLight", LightShape::Dome},
    {"DomeLight_1", LightShape::Dome},
};

struct LayerData {
    std::string id;
    LayerPack pack;
    std::unordered_map<std::string, uint32_t> primByPath;
    std::unordered_map<std::string, std::vector<std::string>> childNames; // by parent path, in the file's order; "/" holds the root prims

    auto prim(const std::string& path) const -> const PackPrim* {
        auto it = primByPath.find(path);
        if (it == primByPath.end()) {
            return nullptr;
        }
        return &pack.prims[it->second];
    }
};

struct LayerStack {
    std::string id;                       // the root layer's asset id
    std::vector<const LayerData*> layers; // strongest first
};

enum class ArcKind : uint8_t {
    Root = 0,
    Reference = 1,
    Payload = 2,
};

struct Node {
    const LayerStack* stack = nullptr;
    std::string path; // the site: a prim path in the stack's namespace
    ArcKind arc = ArcKind::Root;
    int namespaceDepth = 0; // path elements of the authoring node's path where the arc was authored
    int arcNumber = 0;      // the arc's position among those authored there
    // Maps this node's namespace to its parent's: paths under mapSource go under mapTarget; with rootIdentity (an internal arc) every
    // other path maps to itself.
    std::string mapSource;
    std::string mapTarget;
    bool rootIdentity = false;
    bool inert = false;  // contributes no opinions here or below: a node an instance's prototype doesn't keep
    bool direct = false; // the arc was authored at this prim's path, not inherited from an ancestor's index
    bool hasSpecs = false;
    std::vector<Node> children; // strongest first
};

auto strongerSibling(const Node& a, const Node& b) -> bool {
    if (a.arc != b.arc) {
        return a.arc < b.arc;
    }
    if (a.namespaceDepth != b.namespaceDepth) {
        return a.namespaceDepth > b.namespaceDepth;
    }
    return a.arcNumber < b.arcNumber;
}

struct PrimIndex {
    Node root;
    std::vector<std::string> payloadIds; // the payloads authored on the prim
    bool payloadsLoaded = false;
};

// A node that contributes opinions, and the nodes from it up to the root's child, to map its paths to the composed namespace.
struct Contribution {
    const Node* node = nullptr;
    std::vector<const Node*> chain;
};

auto collectContributions(const Node& node, std::vector<const Node*>& above, std::vector<Contribution>& out) -> void {
    if (node.hasSpecs && !node.inert) {
        Contribution contribution;
        contribution.node = &node;
        if (node.arc != ArcKind::Root) {
            contribution.chain.push_back(&node);
        }
        for (auto it = above.rbegin(); it != above.rend(); ++it) {
            if ((*it)->arc != ArcKind::Root) {
                contribution.chain.push_back(*it);
            }
        }
        out.push_back(std::move(contribution));
    }
    above.push_back(&node);
    for (const auto& child : node.children) {
        collectContributions(child, above, out);
    }
    above.pop_back();
}

auto contributions(const Node& root) -> std::vector<Contribution> {
    std::vector<Contribution> out;
    std::vector<const Node*> above;
    collectContributions(root, above, out);
    return out;
}

// A path a node's spec authors, in the composed namespace; nullopt when an arc on the way doesn't map it (outside a reference's target).
auto mapToComposed(const std::string& path, const Contribution& contribution) -> std::optional<std::string> {
    std::string mapped = path;
    for (const Node* node : contribution.chain) {
        if (hasPrefix(mapped, node->mapSource)) {
            mapped = replacePrefix(mapped, node->mapSource, node->mapTarget);
        } else if (!node->rootIdentity) {
            return std::nullopt;
        }
    }
    return mapped;
}

// A spec, and the contribution it comes through.
struct Opinion {
    const PackPrim* spec = nullptr;
    const Contribution* contribution = nullptr;
};

auto opinions(const std::vector<Contribution>& contributions) -> std::vector<Opinion> {
    std::vector<Opinion> out;
    for (const auto& contribution : contributions) {
        for (const LayerData* layer : contribution.node->stack->layers) {
            if (const PackPrim* spec = layer->prim(contribution.node->path)) {
                out.push_back({.spec = spec, .contribution = &contribution});
            }
        }
    }
    return out;
}

auto findAttribute(const std::vector<Opinion>& opinions, const std::string& name) -> const PackValue* {
    for (const auto& opinion : opinions) {
        for (const auto& attribute : opinion.spec->attributes) {
            if (attribute.name == name) {
                return &attribute.value;
            }
        }
    }
    return nullptr;
}

struct RelationshipOpinion {
    const PackRelationship* relationship = nullptr;
    const Contribution* contribution = nullptr;
};

auto findRelationship(const std::vector<Opinion>& opinions, const std::string& name) -> RelationshipOpinion {
    for (const auto& opinion : opinions) {
        for (const auto& relationship : opinion.spec->relationships) {
            if (relationship.name == name) {
                return {.relationship = &relationship, .contribution = opinion.contribution};
            }
        }
    }
    return {};
}

auto token(const PackValue* value) -> std::string {
    if (value == nullptr || value->strings.empty()) {
        return {};
    }
    return value->strings[0];
}

auto number(const PackValue* value, double fallback) -> double {
    if (value == nullptr || value->numbers.empty()) {
        return fallback;
    }
    return value->numbers[0];
}

auto color(const PackValue* value, glm::vec3 fallback) -> glm::vec3 {
    if (value == nullptr || value->numbers.size() < 3) {
        return fallback;
    }
    return {(float) value->numbers[0], (float) value->numbers[1], (float) value->numbers[2]};
}

// Where a composed prim's binding or heuristic points, to resolve once the content is composed.
struct PendingTarget {
    WorldContent* content = nullptr;
    EntityId entity = noEntity;
    std::string path;
    bool outside = false; // the path is the stage's, outside the prototype holding the entity
};

// The content being composed: the stage, or a prototype.
struct ContentContext {
    WorldContent* content = nullptr;
    std::string base; // the composed path of the content's root: "/" for the stage, the first instance's path for a prototype
    std::string key;  // the prototype's key; empty for the stage
};

auto relativePath(const ContentContext& context, const std::string& composedPath) -> std::string {
    if (context.base == "/") {
        return composedPath;
    }
    if (composedPath == context.base) {
        return "/";
    }
    return composedPath.substr(context.base.size());
}

auto layerData(const std::string& assetId, LayerPack pack) -> LayerData {
    LayerData data;
    data.id = assetId;
    data.pack = std::move(pack);
    for (uint32_t i = 0; i < data.pack.prims.size(); i++) {
        const auto& path = data.pack.prims[i].path;
        data.primByPath[path] = i;
        data.childNames[parentPath(path)].push_back(lastElement(path));
    }
    return data;
}

} // namespace

struct WorldComposer::Impl {
    std::string rootId;
    std::unordered_map<std::string, LayerData> layers;
    std::unordered_set<std::string> failed;
    std::unordered_map<std::string, uint32_t> lodLevels;
    // Levels set by hand on LOD roots inside prototypes: by prototype key, then the root's path in the prototype. A prototype loads
    // every level any of its instances wants.
    std::map<std::string, std::map<std::string, std::set<uint32_t>>> prototypeLevels;

    // One compose's state.
    std::unordered_map<std::string, std::unique_ptr<LayerStack>> stacks;
    std::set<std::string> missing;
    std::unordered_map<std::string, Node> indexMemo;
    std::unordered_set<std::string> computing;
    std::vector<std::string> messages;
    std::unordered_map<std::string, int32_t> prototypeByKey;
    std::vector<std::unique_ptr<WorldContent>> prototypes;
    std::vector<std::string> prototypeKeys;
    std::vector<PendingTarget> pendingBindings;
    std::vector<PendingTarget> pendingHeuristics;
    std::map<std::pair<const WorldContent*, EntityId>, std::vector<float>> thresholds;

    World world;

    auto need(const std::string& id) -> void {
        if (!failed.contains(id)) {
            missing.insert(id);
        }
    }

    auto appendLayers(const std::string& id, LayerStack& stack, std::vector<std::string>& visiting) -> void {
        if (std::ranges::find(visiting, id) != visiting.end()) {
            messages.push_back(std::format("{}: sublayer cycle through {}", stack.id, id));
            return;
        }
        auto it = layers.find(id);
        if (it == layers.end()) {
            need(id);
            return;
        }
        stack.layers.push_back(&it->second);
        visiting.push_back(id);
        for (const auto& sublayer : it->second.pack.sublayers) {
            appendLayers(sublayer, stack, visiting);
        }
        visiting.pop_back();
    }

    // The layer stack rooted at a file; null while the file itself isn't held.
    auto stack(const std::string& id) -> const LayerStack* {
        if (auto it = stacks.find(id); it != stacks.end()) {
            return it->second.get();
        }
        if (!layers.contains(id)) {
            need(id);
            stacks[id] = nullptr;
            return nullptr;
        }
        auto built = std::make_unique<LayerStack>();
        built->id = id;
        std::vector<std::string> visiting;
        appendLayers(id, *built, visiting);
        auto* out = built.get();
        stacks[id] = std::move(built);
        return out;
    }

    static auto hasSpecsAt(const LayerStack& stack, const std::string& path) -> bool {
        return std::ranges::any_of(stack.layers, [&path](const LayerData* layer) { return layer->prim(path) != nullptr; });
    }

    // The references and payloads a node's layer stack authors at its path, strongest layer first, each once.
    static auto authoredArcs(const LayerStack& stack, const std::string& path) -> std::vector<PackPlacement> {
        std::vector<PackPlacement> out;
        for (const LayerData* layer : stack.layers) {
            const PackPrim* spec = layer->prim(path);
            if (spec == nullptr) {
                continue;
            }
            for (const auto& placement : spec->placements) {
                bool seen = std::ranges::any_of(out, [&placement](const PackPlacement& other) {
                    return other.kind == placement.kind && other.assetId == placement.assetId && other.primPath == placement.primPath;
                });
                if (!seen) {
                    out.push_back(placement);
                }
            }
        }
        return out;
    }

    static auto extend(const Node& node, const std::string& name) -> std::optional<Node> {
        Node out;
        out.stack = node.stack;
        out.path = childPath(node.path, name);
        out.arc = node.arc;
        out.namespaceDepth = node.namespaceDepth;
        out.arcNumber = node.arcNumber;
        out.mapSource = node.mapSource;
        out.mapTarget = node.mapTarget;
        out.rootIdentity = node.rootIdentity;
        out.inert = node.inert;
        out.direct = false;
        out.hasSpecs = !node.inert && hasSpecsAt(*node.stack, out.path);
        for (const auto& child : node.children) {
            if (auto extended = extend(child, name)) {
                out.children.push_back(std::move(*extended));
            }
        }
        if (!out.hasSpecs && out.children.empty() && node.arc != ArcKind::Root) {
            return std::nullopt;
        }
        return out;
    }

    static auto preorder(Node& node, std::vector<Node*>& out) -> void {
        out.push_back(&node);
        for (auto& child : node.children) {
            preorder(child, out);
        }
    }

    // The arcs authored at the prim's path, each under the node whose spec authors it. Nodes are visited in reverse pre-order, so adding
    // children to one never moves a node still to be visited.
    auto addDirectArcs(PrimIndex& index, const std::string& composedPath, bool loadPayloads) -> void {
        std::vector<Node*> nodes;
        preorder(index.root, nodes);
        for (auto it = nodes.rbegin(); it != nodes.rend(); ++it) {
            Node* node = *it;
            if (!node->hasSpecs || node->inert) {
                continue;
            }
            int arcNumber = 0;
            for (const auto& placement : authoredArcs(*node->stack, node->path)) {
                arcNumber++;
                bool internal = placement.assetId.empty();
                ArcKind kind = placement.kind == PlacementKind::Payload ? ArcKind::Payload : ArcKind::Reference;
                if (kind == ArcKind::Payload) {
                    index.payloadIds.push_back(internal ? node->stack->id : placement.assetId);
                    if (!loadPayloads) {
                        continue;
                    }
                    index.payloadsLoaded = true;
                }
                const LayerStack* target = internal ? node->stack : stack(placement.assetId);
                if (target == nullptr || target->layers.empty()) {
                    continue;
                }
                std::string targetPath = placement.primPath;
                if (targetPath.empty()) {
                    const auto& defaultPrim = target->layers.front()->pack.defaultPrim;
                    if (defaultPrim.empty()) {
                        messages.push_back(std::format("{}: {} @{}@ names no prim, and the file has no default prim", composedPath, kind == ArcKind::Payload ? "payload" : "reference", target->id));
                        continue;
                    }
                    targetPath = "/" + defaultPrim;
                }
                if (!hasSpecsAt(*target, targetPath)) {
                    messages.push_back(std::format("{}: {} target {} isn't in @{}@", composedPath, kind == ArcKind::Payload ? "payload" : "reference", targetPath, target->id));
                    continue;
                }
                auto key = memoKey(*target, targetPath, loadPayloads);
                if (computing.contains(key)) {
                    messages.push_back(std::format("{}: arc cycle through @{}@<{}>", composedPath, target->id, targetPath));
                    continue;
                }
                Node arc = indexAt(*target, targetPath, loadPayloads);
                arc.arc = kind;
                arc.namespaceDepth = elementCount(node->path);
                arc.arcNumber = arcNumber;
                arc.mapSource = targetPath;
                arc.mapTarget = node->path;
                arc.rootIdentity = internal;
                arc.direct = true;
                auto position = std::ranges::find_if(node->children, [&arc](const Node& sibling) { return strongerSibling(arc, sibling); });
                node->children.insert(position, std::move(arc));
            }
        }
    }

    static auto memoKey(const LayerStack& stack, const std::string& path, bool loadPayloads) -> std::string {
        return std::format("{}\n{}\n{}", stack.id, path, loadPayloads ? 1 : 0);
    }

    auto childIndex(const Node& parent, const std::string& name, const std::string& composedPath, bool loadPayloads) -> PrimIndex {
        PrimIndex index;
        index.root = *extend(parent, name);
        addDirectArcs(index, composedPath, loadPayloads);
        return index;
    }

    // A prim's index in a layer stack, as that stack composes it on its own: what an arc to it brings.
    auto indexAt(const LayerStack& stack, const std::string& path, bool loadPayloads) -> Node {
        auto key = memoKey(stack, path, loadPayloads);
        if (auto it = indexMemo.find(key); it != indexMemo.end()) {
            return it->second;
        }
        computing.insert(key);
        Node result;
        if (path == "/") {
            result.stack = &stack;
            result.path = "/";
            result.hasSpecs = true;
        } else {
            Node parent = indexAt(stack, parentPath(path), loadPayloads);
            result = std::move(childIndex(parent, lastElement(path), std::format("@{}@<{}>", stack.id, path), loadPayloads).root);
        }
        computing.erase(key);
        indexMemo[key] = result;
        return result;
    }

    // What a prototype keeps of an instance's index (Pcp_ChildNodeIsInstanceable): a node with specs in the subtree of an arc authored on the
    // instance, and everything below it. Every other node turns inert, its arcs kept for what they bring.
    static auto disableNonInstanceable(Node& node, bool directAbove) -> void {
        bool inDirectSubtree = directAbove || node.direct;
        if (inDirectSubtree && node.hasSpecs) {
            return;
        }
        node.inert = true;
        node.hasSpecs = false;
        for (auto& child : node.children) {
            disableNonInstanceable(child, inDirectSubtree);
        }
    }

    // The nodes a prototype keeps, as a key: instances with the same key share a prototype.
    static auto prototypeKey(const Node& node, bool loadPayloads) -> std::string {
        std::string key = loadPayloads ? "1" : "0";
        std::function<void(const Node&)> append = [&](const Node& n) {
            if (n.inert) {
                for (const auto& child : n.children) {
                    append(child);
                }
                return;
            }
            key += std::format("({} {} {}", (int) n.arc, n.stack->id, n.path);
            for (const auto& child : n.children) {
                append(child);
            }
            key += ")";
        };
        for (const auto& child : node.children) {
            append(child);
        }
        return key;
    }

    // The levels a LOD root loads: the one it wants, its coarsest, and those its instances want.
    auto loadedLevels(const ContentContext& context, EntityId id, const std::string& composedPath, uint32_t levelCount) -> std::set<uint32_t> {
        std::set<uint32_t> out;
        if (levelCount == 0) {
            return out;
        }
        const auto& root = context.content->lodRoots.at(id);
        uint32_t wanted = std::min(root.defaultIndex, levelCount - 1);
        if (context.key.empty()) {
            if (auto it = lodLevels.find(composedPath); it != lodLevels.end()) {
                wanted = std::min(it->second, levelCount - 1);
            }
        } else if (auto proto = prototypeLevels.find(context.key); proto != prototypeLevels.end()) {
            if (auto levels = proto->second.find(relativePath(context, composedPath)); levels != proto->second.end()) {
                for (uint32_t level : levels->second) {
                    out.insert(std::min(level, levelCount - 1));
                }
            }
        }
        out.insert(wanted);
        out.insert(levelCount - 1);
        return out;
    }

    static auto childNames(const Node& root) -> std::vector<std::string> {
        std::vector<std::string> names;
        std::unordered_set<std::string> seen;
        for (const auto& contribution : contributions(root)) {
            for (const LayerData* layer : contribution.node->stack->layers) {
                auto it = layer->childNames.find(contribution.node->path);
                if (it == layer->childNames.end()) {
                    continue;
                }
                for (const auto& name : it->second) {
                    if (seen.insert(name).second) {
                        names.push_back(name);
                    }
                }
            }
        }
        return names;
    }

    auto composeChildren(ContentContext& context, EntityId parent, const Node& parentRoot, const std::string& composedPath, bool loadPayloads) -> void {
        auto names = childNames(parentRoot);
        bool isLodRoot = context.content->lodRoots.contains(parent);
        std::set<uint32_t> levels;
        if (isLodRoot) {
            levels = loadedLevels(context, parent, composedPath, (uint32_t) names.size());
        }
        for (uint32_t i = 0; i < names.size(); i++) {
            bool childLoads = loadPayloads && (!isLodRoot || levels.contains(i));
            auto path = childPath(composedPath, names[i]);
            auto index = childIndex(parentRoot, names[i], path, childLoads);
            composePrim(context, parent, names[i], std::move(index), path, childLoads);
        }
        if (isLodRoot) {
            auto& root = context.content->lodRoots.at(parent);
            root.levels = context.content->entities[parent].children;
            if (!root.levels.empty()) {
                root.defaultIndex = std::min(root.defaultIndex, (uint32_t) root.levels.size() - 1);
            }
            root.loaded.assign(levels.begin(), levels.end());
        }
    }

    auto pendingTarget(const ContentContext& context, EntityId id, const RelationshipOpinion& opinion) -> std::optional<PendingTarget> {
        if (opinion.relationship == nullptr || opinion.relationship->targets.empty()) {
            return std::nullopt;
        }
        auto mapped = mapToComposed(opinion.relationship->targets[0], *opinion.contribution);
        if (!mapped) {
            return std::nullopt;
        }
        PendingTarget target;
        target.content = context.content;
        target.entity = id;
        if (context.key.empty() || hasPrefix(*mapped, context.base)) {
            target.path = relativePath(context, *mapped);
        } else {
            target.path = *mapped;
            target.outside = true;
        }
        return target;
    }

    auto composePrim(ContentContext& context, EntityId parent, const std::string& name, PrimIndex index, const std::string& composedPath, bool loadPayloads) -> void {
        auto contributing = contributions(index.root);
        auto specs = opinions(contributing);

        Entity entity;
        entity.name = name;
        entity.parent = parent;
        std::vector<std::string> apiSchemas;
        int32_t active = -1;
        int32_t instanceable = -1;
        for (const auto& opinion : specs) {
            const PackPrim& spec = *opinion.spec;
            if (!entity.defined && spec.specifier != PrimSpecifier::Over) {
                entity.defined = true;
                entity.abstract = spec.specifier == PrimSpecifier::Class;
            }
            if (entity.typeName.empty()) {
                entity.typeName = spec.typeName;
            }
            if (active < 0) {
                active = spec.active;
            }
            if (instanceable < 0) {
                instanceable = spec.instanceable;
            }
            for (const auto& schema : spec.apiSchemas) {
                if (std::ranges::find(apiSchemas, schema) == apiSchemas.end()) {
                    apiSchemas.push_back(schema);
                }
            }
            if (entity.pointCount < 0 && spec.pointCount >= 0) {
                entity.pointCount = spec.pointCount;
            }
            if (entity.meshAsset.empty()) {
                entity.meshAsset = spec.meshAsset;
            }
            if (entity.materialAsset.empty()) {
                entity.materialAsset = spec.materialAsset;
            }
        }
        entity.active = active != 0;
        if (!pointBasedTypes.contains(entity.typeName)) {
            entity.pointCount = -1;
        } else if (entity.pointCount < 0) {
            entity.pointCount = 0;
        }

        auto attribute = [&specs](const std::string& attributeName) -> const PackValue* {
            return findAttribute(specs, attributeName);
        };
        entity.xformable = xformableTypes.contains(entity.typeName);
        if (entity.xformable) {
            if (const PackValue* order = attribute("xformOpOrder")) {
                entity.localTransform = evaluateXformOps(order->strings, attribute, entity.resetsXformStack);
            }
        }
        entity.invisible = token(attribute("visibility")) == "invisible";
        auto purpose = token(attribute("purpose"));
        if (purpose == "default") {
            entity.purpose = Purpose::Default;
        } else if (purpose == "render") {
            entity.purpose = Purpose::Render;
        } else if (purpose == "proxy") {
            entity.purpose = Purpose::Proxy;
        } else if (purpose == "guide") {
            entity.purpose = Purpose::Guide;
        }

        auto& content = *context.content;
        auto id = (EntityId) content.entities.size();
        auto binding = findRelationship(specs, "material:binding");
        if (auto target = pendingTarget(context, id, binding)) {
            entity.binding = MaterialBinding{.targetPath = target->path, .materialAsset = {}, .strongerThanDescendants = binding.relationship->bindMaterialAs == "strongerThanDescendants"};
            pendingBindings.push_back(*target);
        }
        content.entities.push_back(std::move(entity));
        content.entities[parent].children.push_back(id);
        content.byPath[relativePath(context, composedPath)] = id;

        const auto& typeName = content.entities[id].typeName;
        if (auto light = lightTypes.find(typeName); light != lightTypes.end()) {
            Light l;
            l.shape = light->second;
            l.color = color(attribute("inputs:color"), l.color);
            l.intensity = (float) number(attribute("inputs:intensity"), l.intensity);
            l.exposure = (float) number(attribute("inputs:exposure"), l.exposure);
            l.angle = (float) number(attribute("inputs:angle"), l.angle);
            l.shadowEnable = number(attribute("inputs:shadow:enable"), 1.0) != 0.0;
            l.shadowColor = color(attribute("inputs:shadow:color"), l.shadowColor);
            content.lights[id] = l;
        }
        if (!index.payloadIds.empty()) {
            PayloadPlacement placement;
            placement.assetIds = index.payloadIds;
            placement.loaded = index.payloadsLoaded;
            if (const PackValue* hint = attribute("extentsHint"); hint != nullptr && hint->numbers.size() >= 6) {
                const auto& n = hint->numbers;
                placement.extentsHint = std::array<glm::vec3, 2>{glm::vec3((float) n[0], (float) n[1], (float) n[2]), glm::vec3((float) n[3], (float) n[4], (float) n[5])};
            }
            content.payloads[id] = std::move(placement);
        }
        if (const PackValue* values = attribute("thresholds")) {
            std::vector<float> list;
            for (double v : values->numbers) {
                list.push_back((float) v);
            }
            thresholds[{&content, id}] = std::move(list);
        }
        bool isLodRoot = std::ranges::find(apiSchemas, "LODRootAPI") != apiSchemas.end();
        if (isLodRoot) {
            LodRoot root;
            root.defaultIndex = (uint32_t) std::max(0.0, number(attribute("lod:default:index"), 0.0));
            content.lodRoots[id] = std::move(root);
            if (auto heuristic = pendingTarget(context, id, findRelationship(specs, "lod:heuristics"))) {
                pendingHeuristics.push_back(*heuristic);
            }
        }

        if (!content.entities[id].active) {
            return;
        }
        if (instanceable == 1) {
            // An instance only when a node is left for its prototype, as USD's Pcp_PrimIndexIsInstanceable.
            Node kept = index.root;
            disableNonInstanceable(kept, false);
            if (keepsAnyNode(kept)) {
                if (isLodRoot) {
                    messages.push_back(std::format("{}: a LOD root that is an instance; its levels aren't selected", composedPath));
                }
                content.entities[id].prototype = prototype(kept, composedPath, loadPayloads);
                return;
            }
        }
        composeChildren(context, id, index.root, composedPath, loadPayloads);
    }

    static auto keepsAnyNode(const Node& node) -> bool {
        if (!node.inert) {
            return true;
        }
        return std::ranges::any_of(node.children, [](const Node& child) { return keepsAnyNode(child); });
    }

    // The prototype of an instance's kept nodes: found by their key, or composed now.
    auto prototype(const Node& kept, const std::string& composedPath, bool loadPayloads) -> int32_t {
        auto key = prototypeKey(kept, loadPayloads);
        if (auto it = prototypeByKey.find(key); it != prototypeByKey.end()) {
            return it->second;
        }
        auto id = (int32_t) prototypes.size();
        prototypeByKey[key] = id;
        prototypes.push_back(std::make_unique<WorldContent>());
        prototypeKeys.push_back(key);
        auto* content = prototypes.back().get();
        content->sourcePath = composedPath;
        Entity root;
        root.defined = true;
        content->entities.push_back(root);
        content->byPath["/"] = 0;
        ContentContext context{.content = content, .base = composedPath, .key = key};
        composeChildren(context, 0, kept, composedPath, loadPayloads);
        return id;
    }

    auto resolveTargets() -> void {
        for (const auto& pending : pendingBindings) {
            const WorldContent& lookIn = pending.outside ? world.stage : *pending.content;
            auto it = lookIn.byPath.find(pending.path);
            if (it == lookIn.byPath.end()) {
                messages.push_back(std::format("{}{}: material binding names {}, which isn't there", pending.content->sourcePath == "/" ? "" : pending.content->sourcePath + " ", pending.content->path(pending.entity), pending.path));
                continue;
            }
            pending.content->entities[pending.entity].binding->materialAsset = lookIn.entities[it->second].materialAsset;
        }
        for (const auto& pending : pendingHeuristics) {
            if (pending.outside) {
                continue;
            }
            auto it = pending.content->byPath.find(pending.path);
            if (it == pending.content->byPath.end()) {
                continue;
            }
            auto& root = pending.content->lodRoots.at(pending.entity);
            root.heuristic = it->second;
            if (auto values = thresholds.find({pending.content, it->second}); values != thresholds.end()) {
                root.thresholds = values->second;
            }
        }
    }

    // The content and entity at a full path, stepping into prototypes at instances.
    auto locate(const std::string& path) const -> std::optional<std::pair<int32_t, EntityId>> {
        const WorldContent* content = &world.stage;
        int32_t contentIndex = -1;
        EntityId id = 0;
        size_t start = 1;
        while (start <= path.size() && path != "/") {
            auto end = path.find('/', start);
            auto name = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
            const auto& entity = content->entities[id];
            if (entity.prototype >= 0) {
                contentIndex = entity.prototype;
                content = &world.prototypes[(size_t) contentIndex];
                id = 0;
            }
            auto& children = content->entities[id].children;
            auto child = std::ranges::find_if(children, [&](EntityId c) { return content->entities[c].name == name; });
            if (child == children.end()) {
                return std::nullopt;
            }
            id = *child;
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return std::pair{contentIndex, id};
    }

    // Levels set by hand on roots inside prototypes, for the next compose; true when they add a level a prototype doesn't load yet.
    auto updatePrototypeLevels() -> bool {
        bool changed = false;
        for (const auto& [path, level] : lodLevels) {
            auto located = locate(path);
            if (!located || located->first < 0) {
                continue;
            }
            const auto& content = world.prototypes[(size_t) located->first];
            const auto& key = prototypeKeys[(size_t) located->first];
            if (prototypeLevels[key][content.path(located->second)].insert(level).second) {
                changed = true;
            }
        }
        return changed;
    }

    auto composeOnce() -> void {
        stacks.clear();
        missing.clear();
        indexMemo.clear();
        computing.clear();
        messages.clear();
        prototypeByKey.clear();
        prototypes.clear();
        prototypeKeys.clear();
        pendingBindings.clear();
        pendingHeuristics.clear();
        thresholds.clear();
        world = World();
        world.lodLevels = lodLevels;
        world.stage.sourcePath = "/";
        Entity root;
        root.defined = true;
        world.stage.entities.push_back(root);
        world.stage.byPath["/"] = 0;

        const LayerStack* rootStack = stack(rootId);
        if (rootStack == nullptr) {
            return;
        }
        for (const LayerData* layer : rootStack->layers) {
            world.layerStack.push_back(layer->id);
        }
        const auto& rootPack = rootStack->layers.front()->pack;
        if (!rootPack.upAxis.empty()) {
            world.upAxis = rootPack.upAxis;
        }
        if (rootPack.metersPerUnit > 0.0) {
            world.metersPerUnit = rootPack.metersPerUnit;
        }

        Node pseudoRoot;
        pseudoRoot.stack = rootStack;
        pseudoRoot.path = "/";
        pseudoRoot.hasSpecs = true;
        ContentContext context{.content = &world.stage, .base = "/", .key = {}};
        composeChildren(context, 0, pseudoRoot, "/", true);
        resolveTargets();
        for (auto& content : prototypes) {
            world.prototypes.push_back(std::move(*content));
        }
        prototypes.clear();
    }
};

WorldComposer::WorldComposer(std::string rootAssetId)
    : m_impl(std::make_unique<Impl>()) {
    m_impl->rootId = std::move(rootAssetId);
}

WorldComposer::~WorldComposer() = default;

auto WorldComposer::addPack(const std::string& assetId, std::span<const std::byte> bytes) -> std::optional<std::string> {
    auto pack = readLayerPack(bytes);
    if (!pack) {
        m_impl->failed.insert(assetId);
        return std::format("{}: {}", assetId, pack.error());
    }
    m_impl->layers.insert_or_assign(assetId, layerData(assetId, std::move(*pack)));
    return std::nullopt;
}

auto WorldComposer::markFailed(const std::string& assetId) -> void {
    m_impl->failed.insert(assetId);
}

auto WorldComposer::compose() -> std::vector<std::string> {
    // A level set inside a prototype is only found once the prototype is composed: compose again while that adds levels to load.
    m_impl->composeOnce();
    for (int pass = 0; pass < 8 && m_impl->updatePrototypeLevels(); pass++) {
        m_impl->composeOnce();
    }
    return {m_impl->missing.begin(), m_impl->missing.end()};
}

auto WorldComposer::setLodLevel(const std::string& rootPath, uint32_t level) -> void {
    m_impl->lodLevels[rootPath] = level;
}

auto WorldComposer::world() const -> const World& {
    return m_impl->world;
}

auto WorldComposer::messages() const -> const std::vector<std::string>& {
    return m_impl->messages;
}
