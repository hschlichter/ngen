#pragma once

#include "cxxtoolchain.hpp"
#include "flags.hpp"
#include "glob.hpp"
#include "graph.hpp"
#include "library.hpp"
#include "path.hpp"
#include "target.hpp"
#include "tool.hpp"
#include "toolchainhelpers.hpp"

#include <cassert>
#include <cstdlib>
#include <expected>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build {

struct ParsedTarget {
    std::string target;
    std::string platform;
    std::string config;
    std::string backend = "ninja";
};

inline auto parse_ninja_target(int argc, char** argv, std::string_view default_target) -> std::expected<ParsedTarget, Error> {
    ParsedTarget parsed;
    parsed.target = std::string(default_target);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto read_value = [&]() -> std::expected<std::string, Error> {
            if (i + 1 >= argc) {
                return std::unexpected(Error{"missing value for " + arg});
            }
            return std::string(argv[++i]);
        };
        if (arg == "--platform") {
            auto value = read_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            parsed.platform = *value;
        } else if (arg == "--config" || arg == "-c") {
            auto value = read_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            parsed.config = *value;
        } else if (arg == "--backend") {
            auto value = read_value();
            if (!value) {
                return std::unexpected(value.error());
            }
            parsed.backend = *value;
        } else {
            parsed.target = arg;
        }
    }
    return parsed;
}

namespace detail {

inline auto append_unique(std::vector<Path>& out, const std::vector<Path>& values) -> void {
    std::set<Path> existing(out.begin(), out.end());
    for (const auto& value : values) {
        if (!existing.contains(value)) {
            out.push_back(value);
            existing.insert(value);
        }
    }
}

inline auto append_unique_str(std::vector<std::string>& out, const std::vector<std::string>& values) -> void {
    std::set<std::string> existing(out.begin(), out.end());
    for (const auto& value : values) {
        if (!existing.contains(value)) {
            out.push_back(value);
            existing.insert(value);
        }
    }
}

inline auto resolve_alias(Target* target, const BuildVariant& variant) -> Target* {
    while (target && target->kind() == "alias") {
        auto* alias = static_cast<Alias*>(target);
        target = alias->resolve({{"platform", variant.platform->name}, {"config", variant.config->name}});
    }
    return target;
}

inline auto collect_public_includes(Target* target, const BuildVariant& variant, std::set<std::string>& seen) -> std::vector<Path> {
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

inline auto collect_includes(Target& target, const BuildVariant& variant) -> std::vector<Path> {
    std::vector<Path> out = target.private_includes;
    append_unique(out, target.public_includes);
    for (auto* dep : target.links) {
        std::set<std::string> seen;
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    return out;
}

inline auto substitute(const std::vector<std::string>& argv_template, const std::vector<Path>& inputs, const std::vector<Path>& outputs, const Path& out_dir)
    -> Command {
    Command cmd;
    for (const auto& token : argv_template) {
        if (token == "$in") {
            for (const auto& p : inputs) {
                cmd.argv.push_back(p.string());
            }
        } else if (token == "$out") {
            for (const auto& p : outputs) {
                cmd.argv.push_back(p.string());
            }
        } else if (token == "$out_dir") {
            cmd.argv.push_back(out_dir.string());
        } else {
            cmd.argv.push_back(token);
        }
    }
    return cmd;
}

inline auto object_path(const BuildVariant& variant, const Target& target, const Path& source) -> Path {
    auto path = source.string();
    for (auto& ch : path) {
        if (ch == '\\') {
            ch = '/';
        } else if (ch == ':') {
            ch = '_';
        }
    }
    return variant.out_dir / "obj" / target.name() / (path + ".o");
}

class Emitter {
public:
    explicit Emitter(const Graph& graph) : graph_(graph) {}

    auto emit(Target& desired) -> std::expected<std::string, Error> {
        out_ << "ninja_required_version = 1.10\n\n";
        out_ << "builddir = _out/.ninja\n\n";
        out_ << "rule cxx\n  command = $cmd\n  depfile = $depfile\n  deps = gcc\n  description = CXX $out\n\n";
        out_ << "rule archive\n  command = $cmd\n  description = AR $out\n\n";
        out_ << "rule link_exe\n  command = $cmd\n  description = LINK $out\n\n";
        out_ << "rule link_shared\n  command = $cmd\n  description = LINK-SHARED $out\n\n";
        out_ << "rule tool\n  command = $cmd\n  description = TOOL $out\n\n";

        std::vector<std::string> variant_targets;
        for (const auto& platform : graph_.platforms()) {
            for (const auto& config : graph_.configs()) {
                BuildVariant variant{&platform, &config, Path("_out") / platform.name / config.name};
                ensure_dirs_.insert(variant.out_dir.string());
                outputs_.clear();

                std::optional<Path> desired_output;
                for (const auto& tgt : graph_.targets()) {
                    auto output = emit_target(tgt.get(), variant);
                    if (!output) {
                        return std::unexpected(output.error());
                    }
                    if (tgt.get() == &desired) {
                        desired_output = *output;
                    }
                }

                if (desired_output && !desired_output->empty()) {
                    auto name = desired.name() + ":" + platform.name + ":" + config.name;
                    variant_targets.push_back(name);
                    out_ << "build " << ninja_escape_path(name) << ": phony " << ninja_escape_path(*desired_output) << "\n\n";
                }
                compile_commands_by_variant_[variant.out_dir.string()] = compile_commands_;
                compile_commands_.clear();
            }
        }

        if (!variant_targets.empty()) {
            out_ << "build " << ninja_escape_path(desired.name()) << ": phony " << ninja_escape_path(variant_targets.front()) << "\n";
        }

        for (const auto& tgt : graph_.targets()) {
            if (auto* tool = dynamic_cast<Tool*>(tgt.get()); tool && tool->is_global) {
                emit_global_tool(*tool);
            }
        }

        out_ << "default " << desired.name() << "\n";
        return out_.str();
    }

    auto write_compile_commands() const -> std::expected<void, Error> {
        std::vector<std::string> merged;
        for (const auto& [dir, commands] : compile_commands_by_variant_) {
            auto json = compile_commands_json(commands);
            auto written = write_if_changed(Path(dir) / "compile_commands.json", json);
            if (!written) {
                return std::unexpected(written.error());
            }
            merged.insert(merged.end(), commands.begin(), commands.end());
        }
        auto written = write_if_changed("_out/compile_commands.json", compile_commands_json(merged));
        if (!written) {
            return std::unexpected(written.error());
        }
        return {};
    }

    auto materialize_dirs() const -> std::expected<void, Error> {
        std::error_code ec;
        for (const auto& dir : ensure_dirs_) {
            std::filesystem::create_directories(dir, ec);
            if (ec) {
                return std::unexpected(Error{"failed to create directory " + dir + ": " + ec.message()});
            }
        }
        return {};
    }

private:
    auto emit_target(Target* unresolved, const BuildVariant& variant) -> std::expected<Path, Error> {
        assert(variant.platform);
        assert(variant.config);
        assert(variant.platform->toolchain);
        Target* target = resolve_alias(unresolved, variant);
        if (!target || !target->enabled_for(variant.platform->name, variant.config->name)) {
            return Path{};
        }
        auto key = target->name() + "|" + variant.platform->name + "|" + variant.config->name;
        if (auto it = outputs_.find(key); it != outputs_.end()) {
            return it->second;
        }
        if (!visiting_.insert(key).second) {
            return std::unexpected(Error{"cycle detected at target " + target->name()});
        }

        std::vector<Path> order_only;
        for (auto* dep : target->deps) {
            auto output = emit_target(dep, variant);
            if (!output) {
                visiting_.erase(key);
                return std::unexpected(output.error());
            }
            if (!output->empty()) {
                order_only.push_back(*output);
            }
        }

        std::vector<Path> linked_outputs;
        for (auto* link : target->links) {
            auto output = emit_target(link, variant);
            if (!output) {
                visiting_.erase(key);
                return std::unexpected(output.error());
            }
            if (!output->empty()) {
                linked_outputs.push_back(*output);
            }
        }

        std::expected<Path, Error> output = Path{};
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
        if (!output) {
            visiting_.erase(key);
            return std::unexpected(output.error());
        }

        visiting_.erase(key);
        outputs_[key] = *output;
        return *output;
    }

    auto emit_objects(Target& target, const BuildVariant& variant) -> std::expected<std::vector<Path>, Error> {
        assert(variant.platform);
        assert(variant.config);
        assert(variant.platform->toolchain);
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
                return std::unexpected(Error{"empty C++ standard for " + target.name()});
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

    auto emit_library(Target& target, const BuildVariant& variant, bool shared) -> std::expected<Path, Error> {
        auto objects = emit_objects(target, variant);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        auto output = variant.out_dir / "lib" /
                      (shared ? variant.platform->toolchain->shared_lib_name(target.name()) : variant.platform->toolchain->static_lib_name(target.name()));
        ensure_dirs_.insert(output.parent_path().string());
        LinkIntent link_intent;
        link_intent.objects = *objects;
        link_intent.output = output;
        Command command = shared ? variant.platform->toolchain->link_shared(link_intent) : variant.platform->toolchain->archive(*objects, output);
        out_ << "build " << ninja_escape_path(output) << ": " << (shared ? "link_shared" : "archive");
        for (const auto& object : *objects) {
            out_ << " " << ninja_escape_path(object);
        }
        out_ << "\n  cmd = " << join_command(command) << "\n\n";
        return output;
    }

    auto emit_program(Target& target, const BuildVariant& variant, const std::vector<Path>& linked_outputs, const std::vector<Path>& order_only)
        -> std::expected<Path, Error> {
        auto objects = emit_objects(target, variant);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        auto output = variant.out_dir / variant.platform->toolchain->exe_name(target.name(), variant.platform->exe_suffix);
        ensure_dirs_.insert(output.parent_path().string());

        LinkIntent intent;
        intent.objects = *objects;
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
        for (const auto& object : *objects) {
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

    auto emit_tool(Tool& target, const BuildVariant& variant, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        if (target.is_global) {
            return Path{};
        }

        std::vector<Path> outputs;
        if (target.output_for) {
            for (const auto& input : target.tool_inputs) {
                outputs.push_back(target.output_for(variant, input));
            }
        } else {
            outputs = target.tool_outputs;
        }

        if (outputs.empty()) {
            auto stamp = variant.out_dir / ("." + target.name() + ".stamp");
            ensure_dirs_.insert(stamp.parent_path().string());
            Command command = substitute(target.argv_template, target.tool_inputs, {}, variant.out_dir);
            out_ << "build " << ninja_escape_path(stamp) << ": tool";
            for (const auto& input : target.tool_inputs) {
                out_ << " " << ninja_escape_path(input);
            }
            if (!order_only.empty()) {
                out_ << " ||";
                for (const auto& dep : order_only) {
                    out_ << " " << ninja_escape_path(dep);
                }
            }
            out_ << "\n  cmd = " << join_command(command) << "\n\n";
            return stamp;
        }

        for (size_t i = 0; i < outputs.size(); ++i) {
            const auto& input = target.tool_inputs.empty() ? Path{} : target.tool_inputs[std::min(i, target.tool_inputs.size() - 1)];
            const auto& output = outputs[i];
            ensure_dirs_.insert(output.parent_path().string());
            auto compatibility = Path("shaders") / output.filename();

            std::vector<Path> inputs_one;
            if (!input.empty()) {
                inputs_one.push_back(input);
            }
            Command command = substitute(target.argv_template, inputs_one, {output}, variant.out_dir);

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

    auto emit_global_tool(Tool& target) -> void {
        Command command = substitute(target.argv_template, target.tool_inputs, target.tool_outputs, Path{});
        out_ << "build " << ninja_escape_path(target.name()) << ": tool";
        for (const auto& input : target.tool_inputs) {
            out_ << " " << ninja_escape_path(input);
        }
        out_ << "\n  cmd = " << join_command(command) << "\n\n";
    }

    static auto json_escape(const std::string& in) -> std::string {
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

    auto compile_command_json(const Path& source, const Command& command) const -> std::string {
        std::ostringstream json;
        json << "{\"directory\":\"" << json_escape(repo_root()) << "\",\"file\":\"" << json_escape(source.string()) << "\",\"command\":\""
             << json_escape(join_command(command)) << "\"}";
        return json.str();
    }

    static auto compile_commands_json(const std::vector<std::string>& commands) -> std::string {
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

} // namespace detail

class NinjaBackend {
public:
    auto emit(const Graph& graph, Target& desired, Path output = "_out/build.ninja") const -> std::expected<void, Error> {
        detail::Emitter emitter(graph);
        auto text = emitter.emit(desired);
        if (!text) {
            return std::unexpected(text.error());
        }
        auto written = write_if_changed(output, *text);
        if (!written) {
            return std::unexpected(written.error());
        }
        auto compile_commands = emitter.write_compile_commands();
        if (!compile_commands) {
            return std::unexpected(compile_commands.error());
        }
        auto dirs = emitter.materialize_dirs();
        if (!dirs) {
            return std::unexpected(dirs.error());
        }
        return {};
    }

    auto build(const Graph& graph, Target& desired, const ParsedTarget& parsed) const -> std::expected<void, Error> {
        if (parsed.backend != "ninja") {
            return std::unexpected(Error{"unsupported backend: " + parsed.backend});
        }
        auto emitted = emit(graph, desired);
        if (!emitted) {
            return std::unexpected(emitted.error());
        }

        std::string ninja_target = parsed.target.empty() ? desired.name() : parsed.target;
        if (!parsed.platform.empty() || !parsed.config.empty()) {
            auto platform = parsed.platform.empty() ? graph.platforms().front().name : parsed.platform;
            auto config = parsed.config.empty() ? graph.configs().front().name : parsed.config;
            ninja_target += ":" + platform + ":" + config;
        }
        auto command = "ninja -f _out/build.ninja " + shell_quote(ninja_target);
        if (std::system(command.c_str()) != 0) {
            return std::unexpected(Error{"command failed: " + command});
        }
        return {};
    }
};

} // namespace build
