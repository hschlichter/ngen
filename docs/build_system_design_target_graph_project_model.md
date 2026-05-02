# Build System Design: Target Graph + Project Model

## Overview

**Model**
- Targets form the dependency graph
- Project defines entry targets + build context
- Extensions attach build behavior

**Separation**
- Structure: `Target`
- Context: `Project`
- Behavior: extensions (e.g. `cxx`)

---

## Core Concepts

### Target
- Identity + dependencies (`deps`)
- No platform/config/build logic
- Graph is distributed across targets

### Extensions
- Attach language/tool behavior to a target
- May add data and edges (e.g. `link`)
- Multiple extensions per target

### Project
- Owns platforms/configs
- Registers **entry targets** (what users can build)
- Chooses a default target
- Discovers the graph by traversal from entry targets

---

## Example

```cpp
// build_project_example.cpp
//
// This demonstrates the model:
//
// - Targets form a dependency graph themselves
// - Project owns entry targets: the targets users can explicitly build
// - Dependencies are discovered by walking the graph from those entry targets
// - Extensions, such as cxx, attach build behavior to targets
//
// Key idea:
//   Targets define structure
//   Project defines build context + entry points
//   Extensions define how targets are built

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace build {

// ---------------------------------------------
// Core Target: graph node
// ---------------------------------------------
//
// Target is intentionally small. It represents identity and graph structure,
// not language-specific build behavior.
//
// The dependency graph is distributed across targets through deps.
// There is no central graph object that owns all edges.
class Target {
public:
    explicit Target(std::string name)
        : name_(std::move(name)) {}

    auto name() const -> const std::string& {
        return name_;
    }

    auto depend_on(Target& other) -> Target& {
        deps.push_back(&other);
        return *this;
    }

    // Public here to keep the example focused.
    // A real implementation may keep this private and expose traversal helpers.
    std::vector<Target*> deps;

private:
    std::string name_;
};

struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::string exe_suffix;
};

struct Configuration {
    std::string name;
    std::string out_dir = "_out";
};

// ---------------------------------------------
// Project: build context + entry points
// ---------------------------------------------
//
// Project is not a registry of every target in the dependency graph.
// It only stores public entry targets: the targets a user can ask to build.
//
// Dependencies are discovered by walking from those roots.
class Project {
public:
    auto target(Target& t) -> void {
        roots_.push_back(&t);
    }

    auto default_target(Target& t) -> void {
        default_ = &t;
        target(t);
    }

    auto add_platform(Platform platform) -> void {
        platforms_.push_back(std::move(platform));
    }

    auto add_config(Configuration config) -> void {
        configs_.push_back(std::move(config));
    }

    auto find(std::string_view name) const -> Target* {
        for (auto* t : roots_) {
            if (t->name() == name) {
                return t;
            }
        }

        return nullptr;
    }

    auto build(std::string_view name) const -> std::vector<Target*> {
        auto* root = find(name);
        if (!root) {
            throw std::runtime_error("unknown target");
        }

        return reachable_from(*root);
    }

    auto build_all() const -> std::vector<Target*> {
        std::vector<Target*> result;
        std::unordered_set<Target*> seen;

        for (auto* root : roots_) {
            visit(root, seen, result);
        }

        return result;
    }

    auto default_build() const -> std::vector<Target*> {
        if (!default_) {
            throw std::runtime_error("no default target");
        }

        return reachable_from(*default_);
    }

    auto platforms() const -> const std::vector<Platform>& {
        return platforms_;
    }

    auto configs() const -> const std::vector<Configuration>& {
        return configs_;
    }

private:
    static void visit(
        Target* t,
        std::unordered_set<Target*>& seen,
        std::vector<Target*>& out)
    {
        if (!t || seen.contains(t)) {
            return;
        }

        seen.insert(t);

        for (auto* dep : t->deps) {
            visit(dep, seen, out);
        }

        // Post-order gives build order: dependencies before dependents.
        out.push_back(t);
    }

    static auto reachable_from(Target& root) -> std::vector<Target*> {
        std::vector<Target*> result;
        std::unordered_set<Target*> seen;
        visit(&root, seen, result);
        return result;
    }

private:
    std::vector<Target*> roots_; // public build entry points
    Target* default_ = nullptr;

    std::vector<Platform> platforms_;
    std::vector<Configuration> configs_;
};

} // namespace build

// ---------------------------------------------
// C++ extension
// ---------------------------------------------
//
// The extension *owns* the base Target for creation-time ergonomics.
// It exposes the C++ API and implicitly converts to build::Target& so it can
// be used wherever the base type is required (e.g. Project::target).
namespace build::cxx {

enum class Kind {
    StaticLibrary,
    SharedLibrary,
    Program,
};

class Target {
public:
    explicit Target(std::string name, Kind kind)
        : base_(std::make_unique<build::Target>(std::move(name))), kind_(kind) {}

    // Implicit view as the base graph node
    operator build::Target&() { return *base_; }
    operator const build::Target&() const { return *base_; }

    auto owner() -> build::Target& { return *base_; }

    auto kind() const -> Kind { return kind_; }

    auto sources(std::vector<std::string> s) -> Target& {
        sources_ = std::move(s);
        return *this;
    }

    auto include(std::string path) -> Target& {
        includes_.push_back(std::move(path));
        return *this;
    }

    auto public_include(std::string path) -> Target& {
        public_includes_.push_back(std::move(path));
        return *this;
    }

    auto define(std::string value) -> Target& {
        defines_.push_back(std::move(value));
        return *this;
    }

    auto std(std::string value) -> Target& {
        cpp_standard_ = std::move(value);
        return *this;
    }

    // Link to another C++ target
    auto link(Target& other) -> Target& {
        base_->depend_on(other.owner());
        target_links_.push_back(&other.owner());
        return *this;
    }

    // Link to a raw base target (rare)
    auto link(build::Target& other) -> Target& {
        base_->depend_on(other);
        target_links_.push_back(&other);
        return *this;
    }

    auto link(std::string system_library) -> Target& {
        system_libraries_.push_back(std::move(system_library));
        return *this;
    }

    auto describe() const -> void {
        std::cout << base_->name() << " [" << kind_name(kind_) << "]
";

        for (const auto& source : sources_) {
            std::cout << "  source: " << source << "
";
        }

        for (const auto& include : includes_) {
            std::cout << "  include: " << include << "
";
        }

        for (const auto& include : public_includes_) {
            std::cout << "  public include: " << include << "
";
        }

        for (const auto& define : defines_) {
            std::cout << "  define: " << define << "
";
        }

        for (const auto& lib : system_libraries_) {
            std::cout << "  system library: " << lib << "
";
        }
    }

private:
    static auto kind_name(Kind kind) -> std::string_view {
        switch (kind) {
            case Kind::StaticLibrary: return "cxx.static_library";
            case Kind::SharedLibrary: return "cxx.shared_library";
            case Kind::Program: return "cxx.program";
        }
        return "unknown";
    }

private:
    std::unique_ptr<build::Target> base_;
    Kind kind_;

    std::vector<std::string> sources_;
    std::vector<std::string> includes_;
    std::vector<std::string> public_includes_;
    std::vector<std::string> defines_;
    std::vector<build::Target*> target_links_;
    std::vector<std::string> system_libraries_;
    std::string cpp_standard_ = "c++17";
};

inline auto static_library(std::string name) -> Target {
    return Target{std::move(name), Kind::StaticLibrary};
}

inline auto shared_library(std::string name) -> Target {
    return Target{std::move(name), Kind::SharedLibrary};
}

inline auto program(std::string name) -> Target {
    return Target{std::move(name), Kind::Program};
}

} // namespace build::cxx

// ---------------------------------------------
// Example usage
// ---------------------------------------------
int main() {
    using namespace build;

    auto logger =
        cxx::static_library("logger")
            .sources({"logger.cpp"})
            .public_include("include")
            .std("c++20");

    auto engine =
        cxx::program("engine")
            .sources({"engine.cpp"})
            .include("src")
            .define("ENGINE_BUILD")
            .link(logger)
            .link("SDL2");

    auto editor =
        cxx::program("editor")
            .sources({"editor.cpp"})
            .link(logger);

    // Later mutation can continue chaining on the same handle
    logger.define("LOGGER_VERBOSE");

    Project p;

    p.add_platform({
        .name = "linux-vulkan",
        .os = "linux",
        .graphics_api = "vulkan",
        .exe_suffix = "",
    });

    p.add_config({
        .name = "debug",
        .out_dir = "_out/debug",
    });

    // Project targets are the public build entry points.
    // logger is not registered because it is only a dependency.
    p.target(engine);
    p.target(editor);
    p.default_target(engine);

    auto print = [](const char* label, const std::vector<Target*>& targets) {
        std::cout << label << ":
";
        for (auto* t : targets) {
            std::cout << "  " << t->name() << "
";
        }
        std::cout << "
";
    };

    print("build", p.default_build());
    print("build engine", p.build("engine"));
    print("build editor", p.build("editor"));
    print("build all", p.build_all());
}
```

> Note: the C++ extension side table in this example is intentionally simple. A production implementation should store extensions directly on `build::Target`, as shown earlier with an `ExtensionMap`, to avoid lifetime and address-stability problems when targets are moved.

---

## Build Behavior

- `build` → default target
- `build <name>` → that entry target + deps
- `build all` → all entry targets (deps deduped)

Traversal is post-order (deps first), producing a valid build order.

---

## Design Notes

- No global registry: targets exist if referenced
- No incomplete nodes: targets are defined before use
- Entry targets ≠ all targets (dependencies are implicit)

---

## Mental Model

```
Targets     → graph
Project     → what to build
Extensions  → how to build
```

