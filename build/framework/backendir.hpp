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
#include "ir/json.hpp"
#include "ir/schema.hpp"
#include "ir/writer.hpp"
#include "path.hpp"
#include "project.hpp"
#include "target.hpp"
#include "tool.hpp"

#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace build {

namespace detail::ir {

inline auto bake_command(const Command& command) -> std::string {
    std::string out;
    for (const auto& token : command.argv) {
        if (!out.empty()) {
            out += ' ';
        }
        out += shell_quote(token);
    }
    return out;
}

inline auto paths_to_strings(const std::vector<Path>& paths) -> std::vector<std::string> {
    std::vector<std::string> out;
    out.reserve(paths.size());
    for (const auto& p : paths) {
        out.push_back(p.string());
    }
    return out;
}

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

inline auto json_escape(const std::string& in) -> std::string {
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

inline auto compile_command_entry(const Path& source, const Command& command) -> std::string {
    std::ostringstream json;
    json << "{\"directory\":\"" << json_escape(repo_root()) << "\",\"file\":\"" << json_escape(source.string()) << "\",\"command\":\""
         << json_escape(bake_command(command)) << "\"}";
    return json.str();
}

inline auto compile_commands_json(const std::vector<std::string>& commands) -> std::string {
    std::ostringstream json;
    json << "[\n";
    for (std::size_t i = 0; i < commands.size(); ++i) {
        json << "  " << commands[i];
        if (i + 1 < commands.size()) {
            json << ",";
        }
        json << "\n";
    }
    json << "]\n";
    return json.str();
}

class VariantEmitter {
public:
    VariantEmitter(const Project& project, BuildVariant variant) : project_(project), variant_(std::move(variant)) {}

    auto emit() -> std::expected<build::ir::IR, Error> {
        ir_.variant = variant_.platform->name() + "/" + variant_.config->name();
        ir_.project_root = repo_root();
        ir_.pools = build::ir::make_default_pools();
        ensure_dirs_.insert(variant_.out_dir.string());

        for (auto* tgt : project_.build_all()) {
            auto output = emit_target(tgt);
            if (!output) {
                return std::unexpected(output.error());
            }
        }

        for (auto* root : project_.roots()) {
            if (auto* tool = root->extension<Tool>(); tool && tool->is_global) {
                emit_global_tool(*tool);
            }
        }

        if (auto* def = project_.default_target()) {
            if (auto it = name_to_edge_.find(def->name()); it != name_to_edge_.end()) {
                ir_.default_targets.push_back(it->second);
            }
        }

        return std::move(ir_);
    }

    auto compile_commands() const -> const std::vector<std::string>& { return compile_commands_; }

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
    auto add_edge(build::ir::Edge edge) -> std::uint32_t {
        auto idx = static_cast<std::uint32_t>(ir_.edges.size());
        name_to_edge_[edge.name] = idx;
        ir_.edges.push_back(std::move(edge));
        return idx;
    }

    auto emit_target(Target* unresolved) -> std::expected<Path, Error> {
        assert(variant_.platform);
        assert(variant_.config);
        Target* target = resolve_alias(unresolved, variant_);
        if (!target || !target->enabled_for(variant_.platform->name(), variant_.config->name())) {
            return Path{};
        }
        if (auto it = primary_output_.find(target->name()); it != primary_output_.end()) {
            return it->second;
        }
        if (!visiting_.insert(target->name()).second) {
            return std::unexpected(Error{"cycle detected at target " + target->name()});
        }

        std::vector<Path> order_only;
        for (auto* dep : target->deps) {
            auto output = emit_target(dep);
            if (!output) {
                visiting_.erase(target->name());
                return std::unexpected(output.error());
            }
            if (output->empty()) {
                continue;
            }
            auto* resolved = resolve_alias(dep, variant_);
            if (resolved && resolved->has_extension<cxx::ObjectFile>()) {
                continue;
            }
            order_only.push_back(*output);
        }

        std::expected<Path, Error> output = Path{};
        if (auto* tool = target->extension<Tool>()) {
            output = emit_tool(*tool, order_only);
        } else if (auto* obj = target->extension<cxx::ObjectFile>()) {
            output = emit_object_file(*obj);
        } else if (auto* cxx_t = target->extension<cxx::Target>()) {
            output = emit_cxx(*cxx_t, order_only);
        }
        if (!output) {
            visiting_.erase(target->name());
            return std::unexpected(output.error());
        }

        visiting_.erase(target->name());
        primary_output_[target->name()] = *output;
        return *output;
    }

    auto emit_cxx(cxx::Target& target, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        if (target.kind() == cxx::Kind::Program) {
            return emit_cxx_program(target, order_only);
        }
        bool shared = target.kind() == cxx::Kind::SharedLibrary;
        return emit_cxx_library(target, shared);
    }

    auto emit_object_file(cxx::ObjectFile& obj) -> std::expected<Path, Error> {
        const auto* platform_ext = cxx::find_platform(*variant_.platform);
        if (!platform_ext) {
            return std::unexpected(Error{"platform " + variant_.platform->name() + " has no cxx::Platform extension"});
        }
        auto* parent = obj.parent();
        if (!parent) {
            return std::unexpected(Error{"object " + obj.owner().name() + " has no parent cxx::Target"});
        }
        if (!parent->owner().enabled_for(variant_.platform->name(), variant_.config->name())) {
            return Path{};
        }
        const auto* config_ext = cxx::find_configuration(*variant_.config);
        const auto& tc = platform_ext->toolchain();
        if (tc.compiler().empty()) {
            return std::unexpected(Error{"platform " + variant_.platform->name() + " has no compiler set"});
        }

        auto includes = collect_includes(*parent, variant_);

        std::string std_value = !obj.std_data.empty() ? obj.std_data : (!parent->std_data.empty() ? parent->std_data : tc.default_std());
        if (std_value.empty()) {
            return std::unexpected(Error{"empty C++ standard for " + obj.owner().name()});
        }

        auto object = object_path(variant_, parent->owner().name(), obj.source());
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

        build::ir::Edge edge;
        edge.name = obj.owner().name();
        edge.command = bake_command(command);
        edge.inputs = {obj.source().string()};
        edge.outputs = {object.string()};
        edge.depfile = object.string() + ".d";
        edge.description = "CXX " + object.string();
        edge.pool = build::ir::kPoolDefault;
        add_edge(std::move(edge));

        compile_commands_.push_back(compile_command_entry(obj.source(), command));
        return object;
    }

    auto gather_object_outputs(cxx::Target& target) const -> std::expected<std::vector<Path>, Error> {
        std::vector<Path> objects;
        objects.reserve(target.objects_data.size());
        for (const auto& child : target.objects_data) {
            auto it = primary_output_.find(child->owner().name());
            if (it == primary_output_.end() || it->second.empty()) {
                return std::unexpected(Error{"object " + child->owner().name() + " not emitted before " + target.owner().name()});
            }
            objects.push_back(it->second);
        }
        return objects;
    }

    auto emit_cxx_library(cxx::Target& target, bool shared) -> std::expected<Path, Error> {
        auto objects = gather_object_outputs(target);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        const auto* platform_ext = cxx::find_platform(*variant_.platform);
        const auto& tc = platform_ext->toolchain();

        auto output_name = shared ? cxx::ninja::shared_lib_name(target.owner().name()) : cxx::ninja::static_lib_name(target.owner().name());
        auto output = variant_.out_dir / "lib" / output_name;
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

        build::ir::Edge edge;
        edge.name = target.owner().name();
        edge.command = bake_command(command);
        edge.inputs = paths_to_strings(*objects);
        edge.outputs = {output.string()};
        edge.description = (shared ? std::string("LINK-SHARED ") : std::string("AR ")) + output.string();
        edge.pool = build::ir::kPoolDefault;
        add_edge(std::move(edge));

        return output;
    }

    auto emit_cxx_program(cxx::Target& target, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        auto objects = gather_object_outputs(target);
        if (!objects) {
            return std::unexpected(objects.error());
        }
        const auto* platform_ext = cxx::find_platform(*variant_.platform);
        const auto* config_ext = cxx::find_configuration(*variant_.config);
        const auto& tc = platform_ext->toolchain();

        auto output = variant_.out_dir / cxx::ninja::exe_name(target.owner().name(), variant_.platform->exe_suffix());
        ensure_dirs_.insert(output.parent_path().string());

        std::vector<Path> linked_outputs;
        for (auto* link : target.linked_targets_data) {
            auto* resolved = resolve_alias(link, variant_);
            if (!resolved) {
                continue;
            }
            auto it = primary_output_.find(resolved->name());
            if (it != primary_output_.end() && !it->second.empty()) {
                linked_outputs.push_back(it->second);
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

        build::ir::Edge edge;
        edge.name = target.owner().name();
        edge.command = bake_command(command);
        auto inputs = paths_to_strings(*objects);
        auto archives = paths_to_strings(linked_outputs);
        inputs.insert(inputs.end(), archives.begin(), archives.end());
        edge.inputs = std::move(inputs);
        edge.outputs = {output.string()};
        edge.order_only_deps = paths_to_strings(order_only);
        edge.description = "LINK " + output.string();
        edge.pool = build::ir::kPoolDefault;
        add_edge(std::move(edge));

        return output;
    }

    auto emit_tool(Tool& target, const std::vector<Path>& order_only) -> std::expected<Path, Error> {
        if (target.is_global) {
            return Path{};
        }

        std::vector<Path> outputs;
        if (target.output_for) {
            for (const auto& input : target.tool_inputs) {
                outputs.push_back(target.output_for(variant_, input));
            }
        } else {
            outputs = target.tool_outputs;
        }

        if (outputs.empty()) {
            auto stamp = variant_.out_dir / ("." + target.name() + ".stamp");
            ensure_dirs_.insert(stamp.parent_path().string());
            Command command = substitute(target.argv_template, target.tool_inputs, {}, variant_.out_dir);

            build::ir::Edge edge;
            edge.name = target.name();
            edge.command = bake_command(command);
            edge.inputs = paths_to_strings(target.tool_inputs);
            edge.outputs = {stamp.string()};
            edge.order_only_deps = paths_to_strings(order_only);
            edge.description = "TOOL " + stamp.string();
            // clean and similar tools mutate top-level state — keep them in the console pool.
            edge.pool = build::ir::kPoolConsole;
            add_edge(std::move(edge));
            return stamp;
        }

        std::vector<std::string> all_outputs;
        for (std::size_t i = 0; i < outputs.size(); ++i) {
            const auto& input = target.tool_inputs.empty() ? Path{} : target.tool_inputs[std::min(i, target.tool_inputs.size() - 1)];
            const auto& output = outputs[i];
            ensure_dirs_.insert(output.parent_path().string());

            std::vector<Path> inputs_one;
            if (!input.empty()) {
                inputs_one.push_back(input);
            }
            Command command = substitute(target.argv_template, inputs_one, {output}, variant_.out_dir);

            build::ir::Edge edge;
            edge.name = target.name() + "/" + output.string();
            edge.command = bake_command(command);
            edge.inputs = paths_to_strings(inputs_one);
            edge.outputs = {output.string()};
            edge.order_only_deps = paths_to_strings(order_only);
            edge.description = "TOOL " + output.string();
            edge.pool = build::ir::kPoolDefault;
            add_edge(std::move(edge));
            all_outputs.push_back(output.string());
        }

        auto phony = variant_.out_dir / ("." + target.name() + ".stamp");
        build::ir::Edge phony_edge;
        phony_edge.name = target.name();
        phony_edge.inputs = std::move(all_outputs);
        phony_edge.outputs = {phony.string()};
        phony_edge.description = "PHONY " + target.name();
        phony_edge.pool = build::ir::kPoolDefault;
        phony_edge.flags = build::ir::kEdgeFlagPhony;
        add_edge(std::move(phony_edge));
        return phony;
    }

    auto emit_global_tool(Tool& target) -> void {
        Command command = substitute(target.argv_template, target.tool_inputs, target.tool_outputs, Path{});
        build::ir::Edge edge;
        edge.name = target.name();
        edge.command = bake_command(command);
        edge.inputs = paths_to_strings(target.tool_inputs);
        edge.outputs = paths_to_strings(target.tool_outputs);
        // Mirror ninja's `build <name>: tool ...` convention: when a global tool produces no real
        // files, use its name as a virtual output so it is addressable as a build target.
        if (edge.outputs.empty()) {
            edge.outputs.push_back(target.name());
        }
        edge.description = "TOOL " + target.name();
        edge.pool = build::ir::kPoolConsole; // global tools (format/tidy) touch many files; serialize.
        add_edge(std::move(edge));
    }

    const Project& project_;
    BuildVariant variant_;
    build::ir::IR ir_;
    std::unordered_map<std::string, std::uint32_t> name_to_edge_;
    std::unordered_map<std::string, Path> primary_output_;
    std::set<std::string> visiting_;
    std::set<std::string> ensure_dirs_;
    std::vector<std::string> compile_commands_;
};

} // namespace detail::ir

class IrBackend {
public:
    auto emit(const Project& project, const Path& root_out_dir = Path("_out")) const -> std::expected<void, Error> {
        std::vector<std::string> merged_compile_commands;

        for (auto* platform : project.platforms()) {
            for (auto* config : project.configs()) {
                BuildVariant variant{platform, config, root_out_dir / platform->name() / config->name()};
                detail::ir::VariantEmitter emitter(project, variant);
                auto ir = emitter.emit();
                if (!ir) {
                    return std::unexpected(ir.error());
                }

                auto dirs = emitter.materialize_dirs();
                if (!dirs) {
                    return std::unexpected(dirs.error());
                }

                auto written = build::ir::write(*ir, variant.out_dir / "build.ngenir");
                if (!written) {
                    return std::unexpected(written.error());
                }

                auto cc_json = detail::ir::compile_commands_json(emitter.compile_commands());
                auto cc_written = write_if_changed(variant.out_dir / "compile_commands.json", cc_json);
                if (!cc_written) {
                    return std::unexpected(cc_written.error());
                }
                merged_compile_commands.insert(merged_compile_commands.end(), emitter.compile_commands().begin(), emitter.compile_commands().end());
            }
        }

        auto merged_json = detail::ir::compile_commands_json(merged_compile_commands);
        auto merged_written = write_if_changed(root_out_dir / "compile_commands.json", merged_json);
        if (!merged_written) {
            return std::unexpected(merged_written.error());
        }
        return {};
    }

    // Dump every variant's IR to stdout as JSON, separated by a newline. Used by --dump-graph for human inspection.
    auto dump(const Project& project, std::ostream& out) const -> std::expected<void, Error> {
        for (auto* platform : project.platforms()) {
            for (auto* config : project.configs()) {
                BuildVariant variant{platform, config, Path("_out") / platform->name() / config->name()};
                detail::ir::VariantEmitter emitter(project, variant);
                auto ir = emitter.emit();
                if (!ir) {
                    return std::unexpected(ir.error());
                }
                build::ir::dump_json(*ir, out);
                out << "\n";
            }
        }
        return {};
    }
};

} // namespace build
