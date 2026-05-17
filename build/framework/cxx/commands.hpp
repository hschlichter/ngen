#pragma once

#include "../command.hpp"
#include "../path.hpp"
#include "configuration.hpp"
#include "platform.hpp"
#include "target.hpp"
#include "toolchain.hpp"

#include <string>
#include <string_view>
#include <vector>

// Language-agnostic builders that turn cxx toolchain + per-edge inputs into a
// fully-formed `Command` ready to be baked into IR. Backends call these to get
// the argv list; the rest of the file is plain data marshalling.

namespace build::cxx::cmd {

struct CompileInputs {
    Path source;
    Path object;
    std::string std;
    std::vector<std::string> defines;
    std::vector<Path> includes;
    std::vector<std::string> warning_off;
    std::vector<std::string> compile_flags;
};

struct LinkInputs {
    std::vector<Path> objects;
    std::vector<Path> archives;
    std::vector<std::string> external_libs;
    std::vector<Path> lib_search;
    std::vector<std::string> rpaths;
    std::vector<std::string> link_flags;
    Path output;
};

inline auto compile_command(const Toolchain& tc, const CompileInputs& in) -> Command {
    Command c{{tc.compiler(), "-c", "-std=" + in.std}};
    c.argv.emplace_back("-MMD");
    c.argv.emplace_back("-MF");
    c.argv.push_back(in.object.string() + ".d");
    for (const auto& flag : in.compile_flags) {
        c.argv.push_back(flag);
    }
    for (const auto& d : in.defines) {
        c.argv.push_back("-D" + d);
    }
    for (const auto& i : in.includes) {
        c.argv.push_back("-I" + i.string());
    }
    for (const auto& w : in.warning_off) {
        c.argv.push_back("-Wno-" + w);
    }
    c.argv.emplace_back("-o");
    c.argv.push_back(in.object.string());
    c.argv.push_back(in.source.string());
    return c;
}

inline auto archive_command(const Toolchain& tc, std::vector<Path> objects, Path output) -> Command {
    Command c{{tc.archiver(), "rcs", output.string()}};
    for (const auto& o : objects) {
        c.argv.push_back(o.string());
    }
    return c;
}

inline auto link_command(const Toolchain& tc, const LinkInputs& in, bool shared) -> Command {
    Command c{{tc.linker().empty() ? tc.compiler() : tc.linker()}};
    if (shared) {
        c.argv.emplace_back("-shared");
    }
    for (const auto& o : in.objects) {
        c.argv.push_back(o.string());
    }
    if (!in.archives.empty()) {
        c.argv.emplace_back("-Wl,--start-group");
    }
    for (const auto& a : in.archives) {
        c.argv.push_back(a.string());
    }
    if (!in.archives.empty()) {
        c.argv.emplace_back("-Wl,--end-group");
    }
    c.argv.emplace_back("-o");
    c.argv.push_back(in.output.string());
    for (const auto& dir : in.lib_search) {
        c.argv.push_back("-L" + dir.string());
    }
    for (const auto& r : in.rpaths) {
        c.argv.push_back("-Wl,-rpath," + r);
    }
    for (const auto& flag : in.link_flags) {
        c.argv.push_back(flag);
    }
    for (const auto& lib : in.external_libs) {
        c.argv.push_back(lib.starts_with("-l") ? lib : "-l" + lib);
    }
    return c;
}

inline auto static_lib_name(std::string_view stem) -> std::string {
    return "lib" + std::string(stem) + ".a";
}

inline auto shared_lib_name(std::string_view stem) -> std::string {
    return "lib" + std::string(stem) + ".so";
}

inline auto exe_name(std::string_view stem, std::string_view platform_suffix) -> std::string {
    return std::string(stem) + std::string(platform_suffix);
}

} // namespace build::cxx::cmd
