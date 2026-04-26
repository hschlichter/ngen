#pragma once

#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build {

struct Path {
    std::filesystem::path value;

    Path() = default;
    Path(const char* path) : value(path) {}
    Path(std::string path) : value(std::move(path)) {}
    Path(std::filesystem::path path) : value(std::move(path)) {}

    std::string string() const { return value.generic_string(); }
    Path filename() const { return value.filename(); }
    Path parent_path() const { return value.parent_path(); }
    bool empty() const { return value.empty(); }
};

Path operator/(const Path& lhs, const Path& rhs);
bool operator<(const Path& lhs, const Path& rhs);

struct Command {
    std::vector<std::string> argv;
};

enum class OptLevel { O0, O1, O2, O3 };
enum class Linkage { Static, Shared };

struct GlobSpec {
    std::string include;
    std::string exclude;
};

std::vector<Path> glob(GlobSpec spec);
std::vector<Path> concat(std::initializer_list<std::vector<Path>> lists);
std::vector<std::string> concat_tokens(std::initializer_list<std::vector<std::string>> lists);
std::vector<std::string> concat_tokens(std::vector<std::string> a, std::vector<std::string> b);
std::vector<std::string> capture_tokens(std::initializer_list<std::string> argv);
std::string repo_root();
bool write_if_changed(const Path& path, const std::string& text);

struct CompileIntent {
    Path source;
    Path object;
    std::string std;
    OptLevel opt = OptLevel::O0;
    bool debug = true;
    bool pic = true;
    std::vector<std::string> defines;
    std::vector<Path> includes;
    std::vector<std::string> warning_off;
    std::vector<std::string> raw;
};

struct LinkIntent {
    std::vector<Path> objects;
    std::vector<Path> archives;
    std::vector<Path> shared_libs;
    std::vector<std::string> external_libs;
    std::vector<Path> lib_search;
    std::vector<std::string> rpaths;
    std::vector<std::string> raw;
    Path output;
};

class Toolchain {
public:
    struct DepSupport {
        std::string depfile;
        std::string deps_format;
    };

    virtual ~Toolchain() = default;
    virtual std::string name() const = 0;
    virtual Command compile_cxx(const CompileIntent& intent) const = 0;
    virtual Command archive(std::vector<Path> objects, Path output) const = 0;
    virtual Command link_exe(const LinkIntent& intent) const = 0;
    virtual Command link_shared(const LinkIntent& intent) const = 0;
    virtual std::optional<DepSupport> dep_support(Path object) const = 0;
    virtual std::string static_lib_name(std::string_view stem) const = 0;
    virtual std::string shared_lib_name(std::string_view stem) const = 0;
    virtual std::string exe_name(std::string_view stem, std::string_view platform_suffix) const = 0;
};

class CxxToolchain final : public Toolchain {
public:
    struct Config {
        std::string name;
        std::string cxx;
        std::string ar;
        std::string linker;
        std::string default_std;
        std::vector<std::string> extra_cxx_flags;
        std::vector<std::string> extra_link_flags;
    };

    explicit CxxToolchain(Config config);
    std::string name() const override;
    Command compile_cxx(const CompileIntent& intent) const override;
    Command archive(std::vector<Path> objects, Path output) const override;
    Command link_exe(const LinkIntent& intent) const override;
    Command link_shared(const LinkIntent& intent) const override;
    std::optional<DepSupport> dep_support(Path object) const override;
    std::string static_lib_name(std::string_view stem) const override;
    std::string shared_lib_name(std::string_view stem) const override;
    std::string exe_name(std::string_view stem, std::string_view platform_suffix) const override;
    std::string default_std() const;

private:
    Config config_;
};

struct Platform {
    std::string name;
    std::string os;
    std::string graphics_api;
    std::unique_ptr<Toolchain> toolchain;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    std::vector<std::string> system_libs;
    std::string exe_suffix;
};

struct Configuration {
    std::string name;
    OptLevel opt = OptLevel::O0;
    bool debug_info = true;
    Linkage default_linkage = Linkage::Static;
    std::vector<std::string> defines;
    std::vector<std::string> extra_cxx_flags;
    std::vector<std::string> extra_link_flags;
    Path out_dir = "_out";
};

class Graph;

class Target {
public:
    explicit Target(std::string name);
    virtual ~Target() = default;

    const std::string& name() const;
    virtual std::string kind() const = 0;

    Target& cxx(std::vector<Path> sources);
    Target& cxx(std::initializer_list<Path> sources);
    Target& std(std::string_view version);
    Target& define(std::string macro);
    Target& include(Path dir);
    Target& include(std::vector<Path> dirs);
    Target& include(std::initializer_list<Path> dirs);
    Target& public_include(Path dir);
    Target& public_include(std::vector<Path> dirs);
    Target& public_include(std::initializer_list<Path> dirs);
    Target& warning_off(std::string_view name);
    Target& flag_raw(std::string token);
    Target& flags_raw(std::vector<std::string> tokens);
    Target& optimize(OptLevel level);
    Target& debug(bool enabled = true);
    Target& pic(bool enabled = true);
    Target& depend_on(Target& other);
    Target& link(Target& other);
    Target& link(std::string_view system_lib);
    Target& link_raw(std::string token);
    Target& link_raw_many(std::vector<std::string> tokens);
    Target& lib_search(Path dir);
    Target& rpath(std::string path);
    Target& only_in(std::initializer_list<std::string_view> config_names);
    Target& except_in(std::initializer_list<std::string_view> config_names);
    Target& only_on(std::initializer_list<std::string_view> platform_names);
    Target& except_on(std::initializer_list<std::string_view> platform_names);

    bool enabled_for(std::string_view platform, std::string_view config) const;

    std::vector<Path> sources;
    std::string cxx_std;
    std::vector<std::string> defines;
    std::vector<Path> private_includes;
    std::vector<Path> public_includes;
    std::vector<std::string> warning_suppressions;
    std::vector<std::string> raw_compile_flags;
    std::optional<OptLevel> opt;
    std::optional<bool> debug_info;
    bool needs_pic = true;
    std::vector<Target*> deps;
    std::vector<Target*> links;
    std::vector<std::string> system_libs;
    std::vector<std::string> raw_link_flags;
    std::vector<Path> lib_search_dirs;
    std::vector<std::string> rpaths;

private:
    std::string name_;
    std::set<std::string> only_configs_;
    std::set<std::string> except_configs_;
    std::set<std::string> only_platforms_;
    std::set<std::string> except_platforms_;
};

class Program final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "program"; }
};

class StaticLibrary final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "static_library"; }
};

class SharedLibrary final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "shared_library"; }
};

class Library final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "library"; }
    Library& linkage(Linkage linkage);
    std::optional<Linkage> forced_linkage;
};

class Alias final : public Target {
public:
    using Target::Target;
    std::string kind() const override { return "alias"; }
    Alias& to(Target& target);
    Alias& select(std::string_view key, std::string_view value, Target& target);
    Alias& fallback(Target& target);
    Target* resolve(const std::map<std::string, std::string>& context) const;

private:
    Target* fallback_ = nullptr;
    std::vector<std::tuple<std::string, std::string, Target*>> selections_;
};

struct BuildVariant {
    const Platform* platform = nullptr;
    const Configuration* config = nullptr;
    Path out_dir;
};

class Tool final : public Target {
public:
    using OutputFor = std::function<Path(const BuildVariant&, const Path&)>;

    using Target::Target;
    std::string kind() const override { return "tool"; }
    Tool& command(std::vector<std::string> argv_template);
    Tool& inputs(std::vector<Path> input_paths);
    Tool& outputs(std::vector<Path> output_paths);
    Tool& for_each(std::vector<Path> input_paths, OutputFor output_for);

    std::vector<std::string> argv_template;
    std::vector<Path> tool_inputs;
    std::vector<Path> tool_outputs;
    OutputFor output_for;
};

class Graph {
public:
    template<class T, class... Args>
    T& add(std::string name, Args&&... args) {
        auto target = std::make_unique<T>(std::move(name), std::forward<Args>(args)...);
        T& ref = *target;
        targets_.push_back(std::move(target));
        return ref;
    }

    Target* find(std::string_view name) const;
    void addPlatform(Platform platform);
    void addConfig(Configuration config);
    void setDefault(Target& target);

    const std::vector<std::unique_ptr<Target>>& targets() const { return targets_; }
    const std::vector<Platform>& platforms() const { return platforms_; }
    const std::vector<Configuration>& configs() const { return configs_; }
    Target* default_target() const { return default_; }

private:
    std::vector<std::unique_ptr<Target>> targets_;
    std::vector<Platform> platforms_;
    std::vector<Configuration> configs_;
    Target* default_ = nullptr;
};

struct ParsedTarget {
    std::string target;
    std::string platform;
    std::string config;
    std::string backend = "ninja";
};

ParsedTarget parse_ninja_target(int argc, char** argv, std::string_view default_target);

class NinjaBackend {
public:
    bool build(const Graph& graph, Target& desired, const ParsedTarget& parsed) const;
    bool emit(const Graph& graph, Target& desired, Path output = "_out/build.ninja") const;
};

} // namespace build
