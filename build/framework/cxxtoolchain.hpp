#pragma once

#include "toolchain.hpp"
#include "toolchainhelpers.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace build {

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

    explicit CxxToolchain(Config config) : config_(std::move(config)) {}

    auto name() const -> std::string override { return config_.name; }
    auto default_std() const -> std::string { return config_.default_std; }

    auto compile_cxx(const CompileIntent& intent) const -> Command override {
        Command c{{config_.cxx, "-c", "-std=" + intent.std, opt_flag(intent.opt)}};
        if (intent.debug) {
            c.argv.push_back("-g");
        }
        if (intent.pic) {
            c.argv.push_back("-fPIC");
        }
        c.argv.push_back("-MMD");
        c.argv.push_back("-MF");
        c.argv.push_back(intent.object.string() + ".d");
        for (const auto& flag : config_.extra_cxx_flags) {
            c.argv.push_back(flag);
        }
        for (const auto& flag : intent.raw) {
            c.argv.push_back(flag);
        }
        for (const auto& define : intent.defines) {
            c.argv.push_back("-D" + define);
        }
        for (const auto& include : intent.includes) {
            c.argv.push_back("-I" + include.string());
        }
        for (const auto& warning : intent.warning_off) {
            c.argv.push_back("-Wno-" + warning);
        }
        c.argv.push_back("-o");
        c.argv.push_back(intent.object.string());
        c.argv.push_back(intent.source.string());
        return c;
    }

    auto archive(std::vector<Path> objects, Path output) const -> Command override {
        Command c{{config_.ar, "rcs", output.string()}};
        for (const auto& object : objects) {
            c.argv.push_back(object.string());
        }
        return c;
    }

    auto link_exe(const LinkIntent& intent) const -> Command override {
        Command c{{config_.linker.empty() ? config_.cxx : config_.linker}};
        for (const auto& object : intent.objects) {
            c.argv.push_back(object.string());
        }
        if (!intent.archives.empty()) {
            c.argv.push_back("-Wl,--start-group");
        }
        for (const auto& archive : intent.archives) {
            c.argv.push_back(archive.string());
        }
        if (!intent.archives.empty()) {
            c.argv.push_back("-Wl,--end-group");
        }
        for (const auto& shared : intent.shared_libs) {
            c.argv.push_back(shared.string());
        }
        c.argv.push_back("-o");
        c.argv.push_back(intent.output.string());
        for (const auto& dir : intent.lib_search) {
            c.argv.push_back("-L" + dir.string());
        }
        for (const auto& rpath : intent.rpaths) {
            c.argv.push_back("-Wl,-rpath," + rpath);
        }
        for (const auto& flag : config_.extra_link_flags) {
            c.argv.push_back(flag);
        }
        for (const auto& flag : intent.raw) {
            c.argv.push_back(flag);
        }
        for (const auto& lib : intent.external_libs) {
            c.argv.push_back(lib.starts_with("-l") ? lib : "-l" + lib);
        }
        return c;
    }

    auto link_shared(const LinkIntent& intent) const -> Command override {
        auto c = link_exe(intent);
        c.argv.insert(c.argv.begin() + 1, "-shared");
        return c;
    }

    auto dep_support(Path object) const -> std::optional<DepSupport> override { return DepSupport{object.string() + ".d", "gcc"}; }

    auto static_lib_name(std::string_view stem) const -> std::string override { return "lib" + std::string(stem) + ".a"; }
    auto shared_lib_name(std::string_view stem) const -> std::string override { return "lib" + std::string(stem) + ".so"; }
    auto exe_name(std::string_view stem, std::string_view platform_suffix) const -> std::string override {
        return std::string(stem) + std::string(platform_suffix);
    }

private:
    Config config_;
};

} // namespace build
