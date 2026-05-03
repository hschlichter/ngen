#pragma once

#include "alias.hpp"
#include "command.hpp"
#include "cxx/backendninja.hpp"
#include "cxx/configuration.hpp"
#include "cxx/objectfile.hpp"
#include "cxx/platform.hpp"
#include "cxx/target.hpp"
#include "cxx/toolchain.hpp"
#include "glob.hpp"
#include "path.hpp"
#include "project.hpp"
#include "target.hpp"
#include "tool.hpp"

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

inline auto join_command(const Command& command) -> std::string {
    std::string out;
    for (const auto& token : command.argv) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shell_quote(token);
    }
    return out;
}

inline auto ninja_escape_path(const Path& path) -> std::string {
    std::string out;
    for (char ch : path.string()) {
        if (ch == ' ' || ch == ':' || ch == '$') {
            out += '$';
        }
        out += ch;
    }
    return out;
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

inline auto append_all_str(std::vector<std::string>& out, const std::vector<std::string>& values) -> void {
    out.insert(out.end(), values.begin(), values.end());
}

inline auto resolve_alias(Target* target, const BuildVariant& variant) -> Target* {
    while (target) {
        auto* alias = target->extension<Alias>();
        if (!alias) {
            return target;
        }
        target = alias->resolve({{"platform", variant.platform->name()}, {"config", variant.config->name()}});
    }
    return target;
}

inline auto collect_public_includes(Target* target, const BuildVariant& variant, std::set<std::string>& seen) -> std::vector<Path> {
    target = resolve_alias(target, variant);
    if (!target || !seen.insert(target->name()).second) {
        return {};
    }

    std::vector<Path> out;
    if (auto* cxx_t = target->extension<cxx::Target>()) {
        out = cxx_t->public_includes_data;
        for (auto* dep : cxx_t->linked_targets_data) {
            append_unique(out, collect_public_includes(dep, variant, seen));
        }
    }
    for (auto* dep : target->deps) {
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    return out;
}

inline auto collect_includes(cxx::Target& target, const BuildVariant& variant) -> std::vector<Path> {
    std::vector<Path> out = target.includes_data;
    append_unique(out, target.public_includes_data);
    for (auto* dep : target.linked_targets_data) {
        std::set<std::string> seen;
        append_unique(out, collect_public_includes(dep, variant, seen));
    }
    return out;
}

inline auto
substitute(const std::vector<std::string>& argv_template, const std::vector<Path>& inputs, const std::vector<Path>& outputs, const Path& out_dir) -> Command {
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

inline auto object_path(const BuildVariant& variant, std::string_view target_name, const Path& source) -> Path {
    auto path = source.string();
    for (auto& ch : path) {
        if (ch == '\\') {
            ch = '/';
        } else if (ch == ':') {
            ch = '_';
        }
    }
    return variant.out_dir / "obj" / std::string(target_name) / (path + ".o");
}

class Emitter {
public:
    explicit Emitter(const Project& project) : project_(project) {}

    auto emit() -> std::expected<std::string, Error> {
        out_ << "ninja_required_version = 1.10\n\n";
        out_ << "builddir = _out/.ninja\n\n";
        out_ << "rule cxx\n  command = $cmd\n  depfile = $depfile\n  deps = gcc\n  description = CXX $out\n\n";
        out_ << "rule archive\n  command = $cmd\n  description = AR $out\n\n";
        out_ << "rule link_exe\n  command = $cmd\n  description = LINK $out\n\n";
        out_ << "rule link_shared\n  command = $cmd\n  description = LINK-SHARED $out\n\n";
        out_ << "rule tool\n  command = $cmd\n  description = TOOL $out\n\n";

        std::map<std::string, std::vector<std::string>> per_root_variants;

        for (auto* platform : project_.platforms()) {
            for (auto* config : project_.configs()) {
                BuildVariant variant{platform, config, Path("_out") / platform->name() / config->name()};
                ensure_dirs_.insert(variant.out_dir.string());
                outputs_.clear();

                for (auto* tgt : project_.build_all()) {
                    auto output = emit_target(tgt, variant);
                    if (!output) {
                        return std::unexpected(output.error());
                    }
                }

                for (auto* root : project_.roots()) {
                    auto cached = outputs_.find(root->name() + "|" + platform->name() + "|" + config->name());
                    if (cached == outputs_.end() || cached->second.empty()) {
                        continue;
                    }
                    auto name = root->name() + ":" + platform->name() + ":" + config->name();
                    per_root_variants[root->name()].push_back(name);
                    out_ << "build " << ninja_escape_path(name) << ": phony " << ninja_escape_path(cached->second) << "\n\n";
                }

                compile_commands_by_variant_[variant.out_dir.string()] = compile_commands_;
                compile_commands_.clear();
            }
        }

        for (auto* root : project_.roots()) {
            const auto& variants = per_root_variants[root->name()];
            if (!variants.empty()) {
                out_ << "build " << ninja_escape_path(root->name()) << ": phony " << ninja_escape_path(variants.front()) << "\n";
            }
        }

        for (auto* root : project_.roots()) {
            if (auto* tool = root->extension<Tool>(); tool && tool->is_global) {
                emit_global_tool(*tool);
            }
        }

        if (auto* def = project_.default_target()) {
            out_ << "default " << def->name() << "\n";
        }
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
        Target* target = resolve_alias(unresolved, variant);
        if (!target || !target->enabled_for(variant.platform->name(), variant.config->name())) {
            return Path{};
        }
        auto key = target->name() + "|" + variant.platform->name() + "|" + variant.config->name();
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
            if (output->empty()) {
                continue;
            }
            // ObjectFile children flow into the parent as direct inputs via
            // gather_object_outputs; keeping them out of order_only avoids
            // listing the same .o twice on the archive/link edge.
            auto* resolved = resolve_alias(dep, variant);
            if (resolved && resolved->has_extension<cxx::ObjectFile>()) {
                continue;
            }
            order_only.push_back(*output);
        }

        std::expected<Path, Error> output = Path{};
        if (auto* tool = target->extension<Tool>()) {
            output = emit_tool(*tool, variant, order_only);
        } else if (auto* obj = target->extension<cxx::ObjectFile>()) {
            output = emit_object_file(*obj, variant);
        } else if (auto* cxx_t = target->extension<cxx::Target>()) {
            output = emit_cxx(*cxx_t, variant, order_only);
        }
        if (!output) {
            visiting_.erase(key);
            return std::unexpected(output.error());
        }

        visiting_.erase(key);
        outputs_[key] = *output;
        return *output;
    }

    auto emit_cxx(cxx::Target& target, const BuildVariant& variant, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        if (target.kind() == cxx::Kind::Program) {
            return emit_cxx_program(target, variant, order_only);
        }
        bool shared = target.kind() == cxx::Kind::SharedLibrary;
        return emit_cxx_library(target, variant, shared);
    }

    auto emit_object_file(cxx::ObjectFile& obj, const BuildVariant& variant) -> std::expected<Path, Error> {
        const auto* platform_ext = cxx::find_platform(*variant.platform);
        if (!platform_ext) {
            return std::unexpected(Error{"platform " + variant.platform->name() + " has no cxx::Platform extension"});
        }
        auto* parent = obj.parent();
        if (!parent) {
            return std::unexpected(Error{"object " + obj.owner().name() + " has no parent cxx::Target"});
        }
        // Object inherits its parent's gating; if the parent is disabled for
        // this variant we emit nothing rather than producing a stray .o.
        if (!parent->owner().enabled_for(variant.platform->name(), variant.config->name())) {
            return Path{};
        }
        const auto* config_ext = cxx::find_configuration(*variant.config);
        const auto& tc = platform_ext->toolchain();
        if (tc.compiler().empty()) {
            return std::unexpected(Error{"platform " + variant.platform->name() + " has no compiler set"});
        }

        auto includes = collect_includes(*parent, variant);

        std::string std_value = !obj.std_data.empty() ? obj.std_data : (!parent->std_data.empty() ? parent->std_data : tc.default_std());
        if (std_value.empty()) {
            return std::unexpected(Error{"empty C++ standard for " + obj.owner().name()});
        }

        auto object = object_path(variant, parent->owner().name(), obj.source());
        ensure_dirs_.insert(object.parent_path().string());

        cxx::ninja::CompileInputs in;
        in.source = obj.source();
        in.object = object;
        in.std = std_value;

        in.defines = platform_ext->defines();
        if (config_ext) {
            append_all_str(in.defines, config_ext->defines());
        }
        append_all_str(in.defines, parent->defines_data);
        append_all_str(in.defines, obj.defines_data);

        in.includes = includes;
        in.warning_off = parent->warning_suppressions_data;
        append_all_str(in.warning_off, obj.warning_suppressions_data);

        in.compile_flags = platform_ext->compile_flags();
        if (config_ext) {
            append_all_str(in.compile_flags, config_ext->compile_flags());
        }
        append_all_str(in.compile_flags, parent->compile_flags_data);
        append_all_str(in.compile_flags, obj.compile_flags_data);

        auto command = cxx::ninja::compile_command(tc, in);
        out_ << "build " << ninja_escape_path(object) << ": cxx " << ninja_escape_path(obj.source()) << "\n";
        out_ << "  cmd = " << join_command(command) << "\n";
        out_ << "  depfile = " << object.string() << ".d\n\n";

        compile_commands_.push_back(compile_command_json(obj.source(), command));
        return object;
    }

    auto gather_object_outputs(cxx::Target& target, const BuildVariant& variant) -> std::expected<std::vector<Path>, Error> {
        std::vector<Path> objects;
        objects.reserve(target.objects_data.size());
        for (const auto& child : target.objects_data) {
            auto key = child->owner().name() + "|" + variant.platform->name() + "|" + variant.config->name();
            auto cached = outputs_.find(key);
            if (cached == outputs_.end() || cached->second.empty()) {
                return std::unexpected(Error{"object " + child->owner().name() + " not emitted before " + target.owner().name()});
            }
            objects.push_back(cached->second);
        }
        return objects;
    }

    auto emit_cxx_library(cxx::Target& target, const BuildVariant& variant, bool shared) -> std::expected<Path, Error> {
        auto objects = gather_object_outputs(target, variant);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        const auto* platform_ext = cxx::find_platform(*variant.platform);
        const auto& tc = platform_ext->toolchain();

        auto output_name = shared ? cxx::ninja::shared_lib_name(target.owner().name()) : cxx::ninja::static_lib_name(target.owner().name());
        auto output = variant.out_dir / "lib" / output_name;
        ensure_dirs_.insert(output.parent_path().string());

        Command command;
        if (shared) {
            cxx::ninja::LinkInputs link_inputs;
            link_inputs.objects = *objects;
            link_inputs.output = output;
            command = cxx::ninja::link_command(tc, link_inputs, true);
        } else {
            command = cxx::ninja::archive_command(tc, *objects, output);
        }

        out_ << "build " << ninja_escape_path(output) << ": " << (shared ? "link_shared" : "archive");
        for (const auto& object : *objects) {
            out_ << " " << ninja_escape_path(object);
        }
        out_ << "\n  cmd = " << join_command(command) << "\n\n";
        return output;
    }

    auto emit_cxx_program(cxx::Target& target, const BuildVariant& variant, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        auto objects = gather_object_outputs(target, variant);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        const auto* platform_ext = cxx::find_platform(*variant.platform);
        const auto* config_ext = cxx::find_configuration(*variant.config);
        const auto& tc = platform_ext->toolchain();

        auto output = variant.out_dir / cxx::ninja::exe_name(target.owner().name(), variant.platform->exe_suffix());
        ensure_dirs_.insert(output.parent_path().string());

        std::vector<Path> linked_outputs;
        for (auto* link : target.linked_targets_data) {
            auto* resolved = resolve_alias(link, variant);
            if (!resolved) {
                continue;
            }
            auto cached = outputs_.find(resolved->name() + "|" + variant.platform->name() + "|" + variant.config->name());
            if (cached != outputs_.end() && !cached->second.empty()) {
                linked_outputs.push_back(cached->second);
            }
        }

        cxx::ninja::LinkInputs in;
        in.objects = *objects;
        in.archives = linked_outputs;
        in.output = output;

        in.external_libs = platform_ext->system_libs();
        append_unique_str(in.external_libs, target.system_libs_data);

        in.lib_search = target.lib_search_dirs_data;
        in.rpaths = target.rpaths_data;

        in.link_flags = platform_ext->link_flags();
        if (config_ext) {
            append_all_str(in.link_flags, config_ext->link_flags());
        }
        append_all_str(in.link_flags, target.link_flags_data);

        auto command = cxx::ninja::link_command(tc, in, false);

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

            std::vector<Path> inputs_one;
            if (!input.empty()) {
                inputs_one.push_back(input);
            }
            Command command = substitute(target.argv_template, inputs_one, {output}, variant.out_dir);

            std::string cmd = join_command(command);
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

    const Project& project_;
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
    auto emit(const Project& project, Path output = "_out/build.ninja") const -> std::expected<void, Error> {
        detail::Emitter emitter(project);
        auto text = emitter.emit();
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
};

} // namespace build
