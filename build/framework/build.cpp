#include "build.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <regex>
#include <ranges>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace build {
namespace {

std::string shell_quote(const std::string& value) {
    if (value.empty()) {
        return "''";
    }
    bool simple = true;
    for (char ch : value) {
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_' && ch != '-' && ch != '.' && ch != '/' && ch != ':' && ch != '=' && ch != '$') {
            simple = false;
            break;
        }
    }
    if (simple) {
        return value;
    }
    std::string out = "'";
    for (char ch : value) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out += ch;
        }
    }
    out += "'";
    return out;
}

std::string join_command(const Command& command) {
    std::string out;
    for (const auto& token : command.argv) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shell_quote(token);
    }
    return out;
}

std::string ninja_escape_path(const Path& path) {
    std::string out;
    for (char ch : path.string()) {
        if (ch == ' ' || ch == ':' || ch == '$') {
            out += '$';
        }
        out += ch;
    }
    return out;
}

std::string opt_flag(OptLevel opt) {
    switch (opt) {
    case OptLevel::O0: return "-O0";
    case OptLevel::O1: return "-O1";
    case OptLevel::O2: return "-O2";
    case OptLevel::O3: return "-O3";
    }
    return "-O0";
}

std::vector<std::string> split_ws(const std::string& text) {
    std::istringstream in(text);
    std::vector<std::string> tokens;
    std::string token;
    while (in >> token) {
        tokens.push_back(token);
    }
    return tokens;
}

bool glob_match(std::string pattern, std::string text) {
    std::ranges::replace(pattern, '\\', '/');
    std::ranges::replace(text, '\\', '/');
    std::string re = "^";
    for (size_t i = 0; i < pattern.size(); ++i) {
        char ch = pattern[i];
        if (ch == '*') {
            if (i + 1 < pattern.size() && pattern[i + 1] == '*') {
                if (i + 2 < pattern.size() && pattern[i + 2] == '/') {
                    re += "(?:.*/)?";
                    i += 2;
                } else {
                    re += ".*";
                    ++i;
                }
            } else {
                re += "[^/]*";
            }
        } else if (ch == '?') {
            re += "[^/]";
        } else {
            if (std::string_view(R"(\.^$|()[]{}+)").find(ch) != std::string_view::npos) {
                re += '\\';
            }
            re += ch;
        }
    }
    re += "$";
    return std::regex_match(text, std::regex(re));
}

void append_unique(std::vector<Path>& out, const std::vector<Path>& values) {
    std::set<Path> existing(out.begin(), out.end());
    for (const auto& value : values) {
        if (!existing.contains(value)) {
            out.push_back(value);
            existing.insert(value);
        }
    }
}

void append_unique_str(std::vector<std::string>& out, const std::vector<std::string>& values) {
    std::set<std::string> existing(out.begin(), out.end());
    for (const auto& value : values) {
        if (!existing.contains(value)) {
            out.push_back(value);
            existing.insert(value);
        }
    }
}

Target* resolve_alias(Target* target, const BuildVariant& variant) {
    while (target && target->kind() == "alias") {
        auto* alias = static_cast<Alias*>(target);
        target = alias->resolve({{"platform", variant.platform->name}, {"config", variant.config->name}});
    }
    return target;
}

std::vector<Path> collect_public_includes(Target* target, const BuildVariant& variant, std::set<std::string>& seen) {
    target = resolve_alias(target, variant);
    if (!target || !seen.insert(target->name()).second) {
        return {};
    }

    std::vector<Path> out = target->public_includes;
    for (auto* dep : target->links) {
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    for (auto* dep : target->deps) {
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    return out;
}

std::vector<Path> collect_includes(Target& target, const BuildVariant& variant) {
    std::vector<Path> out = target.private_includes;
    append_unique(out, target.public_includes);
    for (auto* dep : target.links) {
        std::set<std::string> seen;
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    return out;
}

Path object_path(const BuildVariant& variant, const Target& target, const Path& source) {
    auto path = source.string();
    for (auto& ch : path) {
        if (ch == '/' || ch == '\\' || ch == ':') {
            ch = '_';
        }
    }
    return variant.out_dir / "obj" / target.name() / (path + ".o");
}

class Emitter {
public:
    explicit Emitter(const Graph& graph) : graph_(graph) {}

    std::string emit(Target& desired) {
        out_ << "ninja_required_version = 1.10\n\n";
        out_ << "builddir = _out/.ninja\n\n";
        out_ << "rule cxx\n  command = $cmd\n  depfile = $depfile\n  deps = gcc\n  description = CXX $out\n\n";
        out_ << "rule archive\n  command = $cmd\n  description = AR $out\n\n";
        out_ << "rule link_exe\n  command = $cmd\n  description = LINK $out\n\n";
        out_ << "rule link_shared\n  command = $cmd\n  description = LINK-SHARED $out\n\n";
        out_ << "rule tool\n  command = $cmd\n  description = TOOL $out\n\n";
        out_ << "rule clean_rule\n  command = rm -rf _out/linux-vulkan/debug _out/linux-vulkan/release _out/linux-vulkan/gamerelease shaders/*.spv\n  description = CLEAN\n\n";
        out_ << "rule format_rule\n  command = clang-format -i src/**/*.cpp src/**/*.h\n  description = FORMAT\n\n";
        out_ << "rule tidy_rule\n  command = clang-tidy ";
        for (const auto& file : glob({.include = "src/**/*.cpp", .exclude = ""})) {
            out_ << ninja_escape_path(file) << " ";
        }
        out_ << "-- -std=c++23\n  description = TIDY\n\n";

        std::vector<std::string> variant_targets;
        for (const auto& platform : graph_.platforms()) {
            for (const auto& config : graph_.configs()) {
                BuildVariant variant{&platform, &config, Path("_out") / platform.name / config.name};
                ensure_dirs_.insert(variant.out_dir.string());
                outputs_.clear();
                auto output = emit_target(&desired, variant);
                if (!output.empty()) {
                    auto name = desired.name() + ":" + platform.name + ":" + config.name;
                    variant_targets.push_back(name);
                    out_ << "build " << ninja_escape_path(name) << ": phony " << ninja_escape_path(output) << "\n\n";
                    compile_commands_by_variant_[variant.out_dir.string()] = compile_commands_;
                    compile_commands_.clear();
                }
            }
        }

        if (!variant_targets.empty()) {
            out_ << "build " << ninja_escape_path(desired.name()) << ": phony " << ninja_escape_path(variant_targets.front()) << "\n";
        }
        out_ << "build clean: clean_rule\n";
        out_ << "build format: format_rule\n";
        out_ << "build tidy: tidy_rule\n";
        out_ << "default " << desired.name() << "\n";
        return out_.str();
    }

    void write_compile_commands() const {
        std::vector<std::string> merged;
        for (const auto& [dir, commands] : compile_commands_by_variant_) {
            auto json = compile_commands_json(commands);
            write_if_changed(Path(dir) / "compile_commands.json", json);
            merged.insert(merged.end(), commands.begin(), commands.end());
        }
        write_if_changed("_out/compile_commands.json", compile_commands_json(merged));
    }

    void materialize_dirs() const {
        std::error_code ec;
        for (const auto& dir : ensure_dirs_) {
            std::filesystem::create_directories(dir, ec);
        }
    }

private:
    Path emit_target(Target* unresolved, const BuildVariant& variant) {
        Target* target = resolve_alias(unresolved, variant);
        if (!target || !target->enabled_for(variant.platform->name, variant.config->name)) {
            return {};
        }
        auto key = target->name() + "|" + variant.platform->name + "|" + variant.config->name;
        if (auto it = outputs_.find(key); it != outputs_.end()) {
            return it->second;
        }
        if (!visiting_.insert(key).second) {
            throw std::runtime_error("cycle detected at target " + target->name());
        }

        std::vector<Path> order_only;
        for (auto* dep : target->deps) {
            auto output = emit_target(dep, variant);
            if (!output.empty()) {
                order_only.push_back(output);
            }
        }

        std::vector<Path> linked_outputs;
        for (auto* link : target->links) {
            auto output = emit_target(link, variant);
            if (!output.empty()) {
                linked_outputs.push_back(output);
            }
        }

        Path output;
        if (target->kind() == "tool") {
            output = emit_tool(static_cast<Tool&>(*target), variant, order_only);
        } else if (target->kind() == "program") {
            output = emit_program(*target, variant, linked_outputs, order_only);
        } else if (target->kind() == "shared_library") {
            output = emit_library(*target, variant, true);
        } else {
            auto shared = false;
            if (target->kind() == "library") {
                auto* library = static_cast<Library*>(target);
                shared = library->forced_linkage.value_or(variant.config->default_linkage) == Linkage::Shared;
            }
            output = emit_library(*target, variant, shared);
        }

        visiting_.erase(key);
        outputs_[key] = output;
        return output;
    }

    std::vector<Path> emit_objects(Target& target, const BuildVariant& variant) {
        std::vector<Path> objects;
        const auto* toolchain = variant.platform->toolchain.get();
        auto includes = collect_includes(target, variant);

        for (const auto& source : target.sources) {
            auto object = object_path(variant, target, source);
            ensure_dirs_.insert(object.parent_path().string());
            CompileIntent intent;
            intent.source = source;
            intent.object = object;
            intent.std = target.cxx_std.empty() ? static_cast<const CxxToolchain*>(toolchain)->default_std() : target.cxx_std;
            if (intent.std.empty()) {
                throw std::runtime_error("empty C++ standard for " + target.name());
            }
            intent.opt = target.opt.value_or(variant.config->opt);
            intent.debug = target.debug_info.value_or(variant.config->debug_info);
            intent.pic = target.needs_pic;
            intent.defines = variant.platform->defines;
            append_unique_str(intent.defines, variant.config->defines);
            append_unique_str(intent.defines, target.defines);
            intent.includes = includes;
            intent.warning_off = target.warning_suppressions;
            intent.raw = variant.platform->extra_cxx_flags;
            append_unique_str(intent.raw, variant.config->extra_cxx_flags);
            append_unique_str(intent.raw, target.raw_compile_flags);

            auto command = toolchain->compile_cxx(intent);
            auto dep = toolchain->dep_support(object);
            out_ << "build " << ninja_escape_path(object) << ": cxx " << ninja_escape_path(source) << "\n";
            out_ << "  cmd = " << join_command(command) << "\n";
            out_ << "  depfile = " << (dep ? dep->depfile : object.string() + ".d") << "\n\n";

            compile_commands_.push_back(compile_command_json(source, command));
            objects.push_back(object);
        }
        return objects;
    }

    Path emit_library(Target& target, const BuildVariant& variant, bool shared) {
        auto objects = emit_objects(target, variant);
        auto output = variant.out_dir / "lib" / (shared ? variant.platform->toolchain->shared_lib_name(target.name()) : variant.platform->toolchain->static_lib_name(target.name()));
        ensure_dirs_.insert(output.parent_path().string());
        LinkIntent link_intent;
        link_intent.objects = objects;
        link_intent.output = output;
        Command command = shared ? variant.platform->toolchain->link_shared(link_intent)
                                 : variant.platform->toolchain->archive(objects, output);
        out_ << "build " << ninja_escape_path(output) << ": " << (shared ? "link_shared" : "archive");
        for (const auto& object : objects) {
            out_ << " " << ninja_escape_path(object);
        }
        out_ << "\n  cmd = " << join_command(command) << "\n\n";
        return output;
    }

    Path emit_program(Target& target, const BuildVariant& variant, const std::vector<Path>& linked_outputs, const std::vector<Path>& order_only) {
        auto objects = emit_objects(target, variant);
        auto output = variant.out_dir / variant.platform->toolchain->exe_name(target.name(), variant.platform->exe_suffix);
        ensure_dirs_.insert(output.parent_path().string());

        LinkIntent intent;
        intent.objects = objects;
        intent.output = output;
        intent.external_libs = variant.platform->system_libs;
        append_unique_str(intent.external_libs, target.system_libs);
        intent.lib_search = target.lib_search_dirs;
        intent.rpaths = target.rpaths;
        intent.raw = variant.platform->extra_link_flags;
        append_unique_str(intent.raw, variant.config->extra_link_flags);
        append_unique_str(intent.raw, target.raw_link_flags);
        for (const auto& linked : linked_outputs) {
            intent.archives.push_back(linked);
        }

        auto command = variant.platform->toolchain->link_exe(intent);
        out_ << "build " << ninja_escape_path(output) << ": link_exe";
        for (const auto& object : objects) {
            out_ << " " << ninja_escape_path(object);
        }
        for (const auto& linked : linked_outputs) {
            out_ << " " << ninja_escape_path(linked);
        }
        if (!order_only.empty()) {
            out_ << " ||";
            for (const auto& dep : order_only) {
                out_ << " " << ninja_escape_path(dep);
            }
        }
        out_ << "\n  cmd = " << join_command(command) << "\n\n";
        return output;
    }

    Path emit_tool(Tool& target, const BuildVariant& variant, const std::vector<Path>& order_only) {
        std::vector<Path> outputs;
        if (target.output_for) {
            for (const auto& input : target.tool_inputs) {
                outputs.push_back(target.output_for(variant, input));
            }
        } else {
            outputs = target.tool_outputs;
        }

        for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& input = target.tool_inputs.empty() ? Path{} : target.tool_inputs[std::min(i, target.tool_inputs.size() - 1)];
            const auto& output = outputs[i];
            ensure_dirs_.insert(output.parent_path().string());
            auto compatibility = Path("shaders") / output.filename();
            Command command;
            for (auto token : target.argv_template) {
                if (token == "$in") token = input.string();
                if (token == "$out") token = output.string();
                command.argv.push_back(token);
            }
            std::string cmd = join_command(command);
            if (target.name() == "shaders") {
                cmd += " && cp " + shell_quote(output.string()) + " " + shell_quote(compatibility.string());
            }
            out_ << "build " << ninja_escape_path(output);
            out_ << ": tool";
            if (!input.empty()) {
                out_ << " " << ninja_escape_path(input);
            }
            if (!order_only.empty()) {
                out_ << " ||";
                for (const auto& dep : order_only) {
                    out_ << " " << ninja_escape_path(dep);
                }
            }
            out_ << "\n  cmd = " << cmd << "\n\n";
        }
        auto phony = variant.out_dir / ("." + target.name() + ".stamp");
        out_ << "build " << ninja_escape_path(phony) << ": phony";
        for (const auto& output : outputs) {
            out_ << " " << ninja_escape_path(output);
        }
        out_ << "\n\n";
        return phony;
    }

    static std::string json_escape(const std::string& in) {
        std::string out;
        for (char ch : in) {
            if (ch == '\\' || ch == '"') {
                out += '\\';
            }
            if (ch == '\n') {
                out += "\\n";
            } else {
                out += ch;
            }
        }
        return out;
    }

    std::string compile_command_json(const Path& source, const Command& command) const {
        std::ostringstream json;
        json << "{\"directory\":\"" << json_escape(repo_root()) << "\",\"file\":\""
             << json_escape(source.string()) << "\",\"command\":\"" << json_escape(join_command(command)) << "\"}";
        return json.str();
    }

    static std::string compile_commands_json(const std::vector<std::string>& commands) {
        std::ostringstream json;
        json << "[\n";
        for (size_t i = 0; i < commands.size(); ++i) {
            json << "  " << commands[i];
            if (i + 1 < commands.size()) {
                json << ",";
            }
            json << "\n";
        }
        json << "]\n";
        return json.str();
    }

    const Graph& graph_;
    std::ostringstream out_;
    std::set<std::string> ensure_dirs_;
    std::unordered_map<std::string, Path> outputs_;
    std::set<std::string> visiting_;
    std::vector<std::string> compile_commands_;
    std::map<std::string, std::vector<std::string>> compile_commands_by_variant_;
};

} // namespace

Path operator/(const Path& lhs, const Path& rhs) {
    return lhs.value / rhs.value;
}

bool operator<(const Path& lhs, const Path& rhs) {
    return lhs.string() < rhs.string();
}

std::vector<Path> glob(GlobSpec spec) {
    std::vector<Path> out;
    auto root = std::filesystem::current_path();
    auto wildcard = spec.include.find_first_of("*?");
    std::filesystem::path search_root = root;
    if (wildcard != std::string::npos) {
        auto prefix = spec.include.substr(0, wildcard);
        auto slash = prefix.find_last_of('/');
        if (slash != std::string::npos) {
            search_root = root / prefix.substr(0, slash);
        }
    }
    if (!std::filesystem::exists(search_root)) {
        return out;
    }
    for (auto it = std::filesystem::recursive_directory_iterator(search_root); it != std::filesystem::recursive_directory_iterator(); ++it) {
        if (!it->is_regular_file()) {
            continue;
        }
        auto rel = std::filesystem::relative(it->path(), root).generic_string();
        if (glob_match(spec.include, rel) && (spec.exclude.empty() || !glob_match(spec.exclude, rel))) {
            out.emplace_back(rel);
        }
    }
    std::ranges::sort(out, [](const Path& lhs, const Path& rhs) {
        return lhs.string() < rhs.string();
    });
    return out;
}

std::vector<Path> concat(std::initializer_list<std::vector<Path>> lists) {
    std::vector<Path> out;
    for (const auto& list : lists) {
        out.insert(out.end(), list.begin(), list.end());
    }
    return out;
}

std::vector<std::string> concat_tokens(std::initializer_list<std::vector<std::string>> lists) {
    std::vector<std::string> out;
    for (const auto& list : lists) {
        out.insert(out.end(), list.begin(), list.end());
    }
    return out;
}

std::vector<std::string> concat_tokens(std::vector<std::string> a, std::vector<std::string> b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

std::vector<std::string> capture_tokens(std::initializer_list<std::string> argv) {
    std::string command;
    for (const auto& token : argv) {
        if (!command.empty()) {
            command += ' ';
        }
        command += shell_quote(token);
    }
    std::array<char, 256> buffer{};
    std::string output;
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
        return {};
    }
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) {
        output += buffer.data();
    }
    auto rc = pclose(pipe);
    if (rc != 0) {
        return {};
    }
    return split_ws(output);
}

std::string repo_root() {
    return std::filesystem::current_path().string();
}

bool write_if_changed(const Path& path, const std::string& text) {
    std::error_code ec;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path().value, ec);
    }
    {
        std::ifstream existing(path.string());
        std::ostringstream current;
        current << existing.rdbuf();
        if (existing && current.str() == text) {
            return false;
        }
    }
    std::ofstream out(path.string(), std::ios::binary);
    out << text;
    return true;
}

CxxToolchain::CxxToolchain(Config config) : config_(std::move(config)) {}
std::string CxxToolchain::name() const { return config_.name; }
std::string CxxToolchain::default_std() const { return config_.default_std; }

Command CxxToolchain::compile_cxx(const CompileIntent& intent) const {
    Command c{{config_.cxx, "-c", "-std=" + intent.std, opt_flag(intent.opt)}};
    if (intent.debug) c.argv.push_back("-g");
    if (intent.pic) c.argv.push_back("-fPIC");
    c.argv.push_back("-MMD");
    c.argv.push_back("-MF");
    c.argv.push_back(intent.object.string() + ".d");
    for (const auto& flag : config_.extra_cxx_flags) c.argv.push_back(flag);
    for (const auto& flag : intent.raw) c.argv.push_back(flag);
    for (const auto& define : intent.defines) c.argv.push_back("-D" + define);
    for (const auto& include : intent.includes) c.argv.push_back("-I" + include.string());
    for (const auto& warning : intent.warning_off) c.argv.push_back("-Wno-" + warning);
    c.argv.push_back("-o");
    c.argv.push_back(intent.object.string());
    c.argv.push_back(intent.source.string());
    return c;
}

Command CxxToolchain::archive(std::vector<Path> objects, Path output) const {
    Command c{{config_.ar, "rcs", output.string()}};
    for (const auto& object : objects) c.argv.push_back(object.string());
    return c;
}

Command CxxToolchain::link_exe(const LinkIntent& intent) const {
    Command c{{config_.linker.empty() ? config_.cxx : config_.linker}};
    for (const auto& object : intent.objects) c.argv.push_back(object.string());
    if (!intent.archives.empty()) c.argv.push_back("-Wl,--start-group");
    for (const auto& archive : intent.archives) c.argv.push_back(archive.string());
    if (!intent.archives.empty()) c.argv.push_back("-Wl,--end-group");
    for (const auto& shared : intent.shared_libs) c.argv.push_back(shared.string());
    c.argv.push_back("-o");
    c.argv.push_back(intent.output.string());
    for (const auto& dir : intent.lib_search) c.argv.push_back("-L" + dir.string());
    for (const auto& rpath : intent.rpaths) c.argv.push_back("-Wl,-rpath," + rpath);
    for (const auto& flag : config_.extra_link_flags) c.argv.push_back(flag);
    for (const auto& flag : intent.raw) c.argv.push_back(flag);
    for (const auto& lib : intent.external_libs) c.argv.push_back(lib.starts_with("-l") ? lib : "-l" + lib);
    return c;
}

Command CxxToolchain::link_shared(const LinkIntent& intent) const {
    auto c = link_exe(intent);
    c.argv.insert(c.argv.begin() + 1, "-shared");
    return c;
}

std::optional<Toolchain::DepSupport> CxxToolchain::dep_support(Path object) const {
    return DepSupport{object.string() + ".d", "gcc"};
}

std::string CxxToolchain::static_lib_name(std::string_view stem) const { return "lib" + std::string(stem) + ".a"; }
std::string CxxToolchain::shared_lib_name(std::string_view stem) const { return "lib" + std::string(stem) + ".so"; }
std::string CxxToolchain::exe_name(std::string_view stem, std::string_view platform_suffix) const { return std::string(stem) + std::string(platform_suffix); }

Target::Target(std::string name) : name_(std::move(name)) {}
const std::string& Target::name() const { return name_; }
Target& Target::cxx(std::vector<Path> s) { sources = std::move(s); return *this; }
Target& Target::cxx(std::initializer_list<Path> s) { sources.assign(s.begin(), s.end()); return *this; }
Target& Target::std(std::string_view version) { cxx_std = std::string(version); return *this; }
Target& Target::define(std::string macro) { defines.push_back(std::move(macro)); return *this; }
Target& Target::include(Path dir) { private_includes.push_back(std::move(dir)); return *this; }
Target& Target::include(std::vector<Path> dirs) { private_includes.insert(private_includes.end(), dirs.begin(), dirs.end()); return *this; }
Target& Target::include(std::initializer_list<Path> dirs) { private_includes.insert(private_includes.end(), dirs.begin(), dirs.end()); return *this; }
Target& Target::public_include(Path dir) { public_includes.push_back(std::move(dir)); return *this; }
Target& Target::public_include(std::vector<Path> dirs) { public_includes.insert(public_includes.end(), dirs.begin(), dirs.end()); return *this; }
Target& Target::public_include(std::initializer_list<Path> dirs) { public_includes.insert(public_includes.end(), dirs.begin(), dirs.end()); return *this; }
Target& Target::warning_off(std::string_view name) { warning_suppressions.emplace_back(name); return *this; }
Target& Target::flag_raw(std::string token) { raw_compile_flags.push_back(std::move(token)); return *this; }
Target& Target::flags_raw(std::vector<std::string> tokens) { raw_compile_flags.insert(raw_compile_flags.end(), tokens.begin(), tokens.end()); return *this; }
Target& Target::optimize(OptLevel level) { opt = level; return *this; }
Target& Target::debug(bool enabled) { debug_info = enabled; return *this; }
Target& Target::pic(bool enabled) { needs_pic = enabled; return *this; }
Target& Target::depend_on(Target& other) { deps.push_back(&other); return *this; }
Target& Target::link(Target& other) { links.push_back(&other); return *this; }
Target& Target::link(std::string_view system_lib) { system_libs.emplace_back(system_lib); return *this; }
Target& Target::link_raw(std::string token) { raw_link_flags.push_back(std::move(token)); return *this; }
Target& Target::link_raw_many(std::vector<std::string> tokens) { raw_link_flags.insert(raw_link_flags.end(), tokens.begin(), tokens.end()); return *this; }
Target& Target::lib_search(Path dir) { lib_search_dirs.push_back(std::move(dir)); return *this; }
Target& Target::rpath(std::string path) { rpaths.push_back(std::move(path)); return *this; }
Target& Target::only_in(std::initializer_list<std::string_view> names) { for (auto n : names) only_configs_.emplace(n); return *this; }
Target& Target::except_in(std::initializer_list<std::string_view> names) { for (auto n : names) except_configs_.emplace(n); return *this; }
Target& Target::only_on(std::initializer_list<std::string_view> names) { for (auto n : names) only_platforms_.emplace(n); return *this; }
Target& Target::except_on(std::initializer_list<std::string_view> names) { for (auto n : names) except_platforms_.emplace(n); return *this; }

bool Target::enabled_for(std::string_view platform, std::string_view config) const {
    if (!only_platforms_.empty() && !only_platforms_.contains(std::string(platform))) return false;
    if (except_platforms_.contains(std::string(platform))) return false;
    if (!only_configs_.empty() && !only_configs_.contains(std::string(config))) return false;
    if (except_configs_.contains(std::string(config))) return false;
    return true;
}

Library& Library::linkage(Linkage linkage) { forced_linkage = linkage; return *this; }
Alias& Alias::to(Target& target) { fallback_ = &target; return *this; }
Alias& Alias::select(std::string_view key, std::string_view value, Target& target) { selections_.emplace_back(key, value, &target); return *this; }
Alias& Alias::fallback(Target& target) { fallback_ = &target; return *this; }
Target* Alias::resolve(const std::map<std::string, std::string>& context) const {
    for (const auto& [key, value, target] : selections_) {
        if (auto it = context.find(key); it != context.end() && it->second == value) {
            return target;
        }
    }
    return fallback_;
}

Tool& Tool::command(std::vector<std::string> argv) { argv_template = std::move(argv); return *this; }
Tool& Tool::inputs(std::vector<Path> paths) { tool_inputs = std::move(paths); return *this; }
Tool& Tool::outputs(std::vector<Path> paths) { tool_outputs = std::move(paths); return *this; }
Tool& Tool::for_each(std::vector<Path> paths, OutputFor fn) { tool_inputs = std::move(paths); output_for = std::move(fn); return *this; }

Target* Graph::find(std::string_view name) const {
    for (const auto& target : targets_) {
        if (target->name() == name) {
            return target.get();
        }
    }
    return nullptr;
}

void Graph::addPlatform(Platform platform) { platforms_.push_back(std::move(platform)); }
void Graph::addConfig(Configuration config) { configs_.push_back(std::move(config)); }
void Graph::setDefault(Target& target) { default_ = &target; }

ParsedTarget parse_ninja_target(int argc, char** argv, std::string_view default_target) {
    ParsedTarget parsed;
    parsed.target = std::string(default_target);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto read_value = [&](std::string& out) {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + arg);
            out = argv[++i];
        };
        if (arg == "--platform") read_value(parsed.platform);
        else if (arg == "--config" || arg == "-c") read_value(parsed.config);
        else if (arg == "--backend") read_value(parsed.backend);
        else parsed.target = arg;
    }
    return parsed;
}

bool NinjaBackend::emit(const Graph& graph, Target& desired, Path output) const {
    Emitter emitter(graph);
    auto text = emitter.emit(desired);
    write_if_changed(output, text);
    emitter.write_compile_commands();
    emitter.materialize_dirs();
    return true;
}

bool NinjaBackend::build(const Graph& graph, Target& desired, const ParsedTarget& parsed) const {
    if (parsed.backend != "ninja") {
        std::cerr << "unsupported backend: " << parsed.backend << "\n";
        return false;
    }
    if (!emit(graph, desired)) {
        return false;
    }

    std::string ninja_target = parsed.target.empty() ? desired.name() : parsed.target;
    if (!parsed.platform.empty() || !parsed.config.empty()) {
        auto platform = parsed.platform.empty() ? graph.platforms().front().name : parsed.platform;
        auto config = parsed.config.empty() ? graph.configs().front().name : parsed.config;
        ninja_target += ":" + platform + ":" + config;
    }
    auto command = "ninja -f _out/build.ninja " + shell_quote(ninja_target);
    return std::system(command.c_str()) == 0;
}

} // namespace build
