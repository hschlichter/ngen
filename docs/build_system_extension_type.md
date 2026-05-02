```cpp
// build_extensions_example.cpp
//
// Reference sketch for a build graph where `build::Target` is the stable,
// language-agnostic node, and language-specific behavior is attached through
// typed extensions.
//
// This gives a C++ approximation of Rust-style trait extension:
//
//   build::Target          -> graph identity
//   build::cxx::Target     -> C++ capability attached to that identity
//   build::cxx::target(t)  -> accessor for that capability
//
// The important design choice is that C++-ness is not modeled through
// inheritance. A target can gain C++ behavior without changing its concrete
// base type.

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

using Path = std::string;

namespace build {

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

    template <typename Ext>
    auto has() const -> bool {
        return extensions_.contains(std::type_index(typeid(Ext)));
    }

    template <typename Ext>
    auto get() -> Ext& {
        auto it = extensions_.find(std::type_index(typeid(Ext)));
        if (it == extensions_.end()) {
            throw std::runtime_error("target extension not found");
        }

        return *static_cast<Ext*>(it->second.ptr.get());
    }

    template <typename Ext, typename... Args>
    auto add(Args&&... args) -> Ext& {
        auto key = std::type_index(typeid(Ext));

        // Extensions are unique per concrete extension type.
        // Calling add<T>() twice returns the existing extension instead of
        // replacing it, which makes accessors like cxx::target(t) idempotent.
        if (auto it = extensions_.find(key); it != extensions_.end()) {
            return *static_cast<Ext*>(it->second.ptr.get());
        }

        auto ext = std::make_unique<Ext>(std::forward<Args>(args)...);
        auto* raw = ext.release();

        // The extension map performs type erasure while preserving ownership.
        // In production code this would probably be wrapped in a small
        // ExtensionMap class to keep Target focused on graph behavior.
        extensions_.emplace(
            key,
            Extension{
                std::unique_ptr<void, void (*)(void*)>{
                    raw,
                    [](void* p) {
                        delete static_cast<Ext*>(p);
                    }
                }
            }
        );

        return *raw;
    }

    // Public here to keep the example compact.
    // A real implementation would likely expose read-only traversal and keep
    // mutation behind depend_on().
    std::vector<Target*> deps;

private:
    struct Extension {
        std::unique_ptr<void, void (*)(void*)> ptr;
    };

    std::string name_;
    std::unordered_map<std::type_index, Extension> extensions_;
};

struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::string exe_suffix;
};

struct Configuration {
    std::string name;
    Path out_dir = "_out";
};

} // namespace build

namespace build::cxx {

// Kind belongs to the C++ extension, not the base target.
// The base graph does not need to know what a program or static library is.
enum class Kind {
    Library,
    StaticLibrary,
    SharedLibrary,
    Program,
};

enum class OptLevel {
    O0,
    O1,
    O2,
    O3,
};

enum class Linkage {
    Static,
    Shared,
};

class Target {
public:
    explicit Target build::Target& owner)
        : owner_(&owner) {}

    auto owner() -> build::Target& {
        return *owner_;
    }

    auto kind() const -> Kind {
        return kind_;
    }

    auto set_kind(Kind kind) -> Target& {
        kind_ = kind;
        return *this;
    }

    // These fluent methods mutate only the C++ extension data.
    // Cross-target relationships still go through build::Target so the graph
    // remains language-agnostic.
    auto sources(std::vector<Path> paths) -> Target& {
        source_files.insert(source_files.end(), paths.begin(), paths.end());
        return *this;
    }

    auto std(std::string_view value) -> Target& {
        cpp_standard = std::string(value);
        return *this;
    }

    auto define(std::string value) -> Target& {
        defines.push_back(std::move(value));
        return *this;
    }

    auto include(Path path) -> Target& {
        includes.push_back(std::move(path));
        return *this;
    }

    auto public_include(Path path) -> Target& {
        public_includes.push_back(std::move(path));
        return *this;
    }

    auto warning_off(std::string_view warning) -> Target& {
        disabled_warnings.emplace_back(warning);
        return *this;
    }

    auto flag_raw(std::string flag) -> Target& {
        raw_compile_flags.push_back(std::move(flag));
        return *this;
    }

    auto optimize(OptLevel level) -> Target& {
        opt = level;
        return *this;
    }

    auto debug(bool enabled) -> Target& {
        debug_info = enabled;
        return *this;
    }

    auto pic(bool enabled) -> Target& {
        position_independent_code = enabled;
        return *this;
    }

    auto link(build::Target& other) -> Target& {
        // Linking a build target is both a C++ linker concern and a graph
        // dependency. Recording it in both places lets graph traversal stay
        // generic while C++ command generation can inspect link-specific data.
        owner_->depend_on(other);
        linked_targets.push_back(&other);
        return *this;
    }

    auto link(std::string_view system_lib) -> Target& {
        system_libs.emplace_back(system_lib);
        return *this;
    }

    auto link_raw(std::string flag) -> Target& {
        raw_link_flags.push_back(std::move(flag));
        return *this;
    }

    auto lib_search(Path path) -> Target& {
        lib_search_paths.push_back(std::move(path));
        return *this;
    }

    auto rpath(std::string path) -> Target& {
        rpaths.push_back(std::move(path));
        return *this;
    }

    // Public for the example.
    // In a full implementation these could remain public as declarative build
    // data, or become private once validation/invariants matter.
    std::vector<Path> source_files;
    std::vector<Path> includes;
    std::vector<Path> public_includes;

    std::vector<std::string> defines;
    std::vector<std::string> disabled_warnings;
    std::vector<std::string> raw_compile_flags;

    std::vector<build::Target*> linked_targets;
    std::vector<std::string> system_libs;
    std::vector<std::string> raw_link_flags;
    std::vector<Path> lib_search_paths;
    std::vector<std::string> rpaths;

    std::string cpp_standard = "c++17";
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    bool position_independent_code = false;

private:
    build::Target* owner_;

    // Defaulting to Library makes cxx::target(t) useful even before the final
    // artifact type is chosen.
    Kind kind_ = Kind::Library;
};

struct Toolchain {};

// These settings are intentionally separate from Target.
// They represent environment-level defaults that can be merged into targets
// during command generation.
struct PlatformSettings {
    Toolchain toolchain;
    std::vector<std::string> defines;
    std::vector<std::string> extra_compile_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;
};

struct ConfigurationSettings {
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    Linkage default_linkage = Linkage::Static;
    std::vector<std::string> defines;
    std::vector<std::string> extra_compile_flags;
    std::vector<std::string> extra_link_flags;
};

// This is the main extension accessor.
// It is deliberately idempotent: calling cxx::target(t) repeatedly returns the
// same attached extension.
auto target(build::Target& t) -> Target& {
    if (!t.has<Target>()) {
        return t.add<Target>(t);
    }

    return t.get<Target>();
}

// These helpers do not create different C++ types.
// They configure the same C++ extension with a different artifact kind.
auto library(build::Target& t) -> Target& {
    return target(t).set_kind(Kind::Library);
}

auto static_library(build::Target& t) -> Target& {
    return target(t).set_kind(Kind::StaticLibrary);
}

auto shared_library(build::Target& t) -> Target& {
    return target(t).set_kind(Kind::SharedLibrary);
}

auto program(build::Target& t) -> Target& {
    return target(t).set_kind(Kind::Program);
}

auto kind_name(Kind kind) -> std::string_view {
    switch (kind) {
        case Kind::Library: return "cxx.library";
        case Kind::StaticLibrary: return "cxx.static_library";
        case Kind::SharedLibrary: return "cxx.shared_library";
        case Kind::Program: return "cxx.program";
    }

    return "unknown";
}

} // namespace build::cxx

static void print_cxx_target(build::Target& base) {
    if (!base.has<build::cxx::Target>()) {
        std::cout << base.name() << " is not a C++ target\n";
        return;
    }

    auto& cxx = build::cxx::target(base);

    std::cout << base.name()
              << " [" << build::cxx::kind_name(cxx.kind()) << "]\n";

    for (const auto& source : cxx.source_files) {
        std::cout << "  source: " << source << "\n";
    }

    for (const auto& include : cxx.includes) {
        std::cout << "  include: " << include << "\n";
    }

    for (const auto& include : cxx.public_includes) {
        std::cout << "  public include: " << include << "\n";
    }

    for (const auto& define : cxx.defines) {
        std::cout << "  define: " << define << "\n";
    }

    for (const auto& lib : cxx.system_libs) {
        std::cout << "  system lib: " << lib << "\n";
    }

    for (auto* dep : base.deps) {
        std::cout << "  depends on: " << dep->name() << "\n";
    }

    std::cout << "\n";
}

int main() {
    build::Target core{"core"};
    build::Target app{"app"};

    build::cxx::static_library(core)
        .sources({"core.cpp"})
        .public_include("include")
        .std("c++20");

    build::cxx::program(app)
        .sources({"main.cpp"})
        .include("src")
        .define("USE_SDL")
        .link(core)
        .link("SDL2");

    // Later mutation uses the exact same accessor style as initial setup.
    // There is no separate assignment/init path to remember.
    build::cxx::target(core)
        .define("CORE_INTERNAL")
        .warning_off("unused-parameter");

    print_cxx_target(core);
    print_cxx_target(app);
}
```
