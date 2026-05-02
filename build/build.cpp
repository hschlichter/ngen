#include "framework/backendninja.hpp"
#include "framework/cxx/configuration.hpp"
#include "framework/cxx/platform.hpp"
#include "framework/cxx/target.hpp"
#include "framework/glob.hpp"
#include "framework/project.hpp"
#include "framework/tool.hpp"

#include <filesystem>
#include <iostream>

using namespace build;

namespace {

auto add_usd_linkage(cxx::Target& target) -> void {
    target.lib_search("external/openusd_build/lib")
        .rpath((std::filesystem::current_path() / "external/openusd_build/lib").string())
        .link_flag("-lusd_usd")
        .link_flag("-lusd_usdGeom")
        .link_flag("-lusd_usdShade")
        .link_flag("-lusd_usdLux")
        .link_flag("-lusd_sdf")
        .link_flag("-lusd_pcp")
        .link_flag("-lusd_tf")
        .link_flag("-lusd_vt")
        .link_flag("-lusd_gf")
        .link_flag("-lusd_ar")
        .link_flag("-lusd_arch")
        .link_flag("-lusd_plug")
        .link_flag("-lusd_js")
        .link_flag("-lusd_work")
        .link_flag("-lusd_trace")
        .link_flag("-lusd_ts")
        .link_flag("-lusd_pegtl")
        .link_flag("-lusd_kind");
}

} // namespace

auto main() -> int {
    auto sdl3_cflags = capture_tokens({"pkg-config", "--cflags", "sdl3"});
    auto sdl3_libs = capture_tokens({"pkg-config", "--libs", "sdl3"});

    auto obs =
        cxx::static_library("obs")
            .sources(glob({.include = "src/obs/**/*.cpp"}))
            .public_include({
                "src/obs",
                "external/concurrentqueue",
            });

    auto rhi = cxx::static_library("rhi").sources(glob({.include = "src/rhi/*.cpp"})).public_include({"src/rhi"}).include({"external/imgui"});

    auto rhivulkan =
        cxx::static_library("rhivulkan")
            .sources(glob({.include = "src/rhi/vulkan/**/*.cpp"}))
            .public_include({"src/rhi/vulkan"})
            .include({
                "src",
                "external/imgui",
                "external/imgui/backends",
            })
            .only_on({"linux-vulkan"})
            .link(rhi);

    Alias rhi_backend("rhi-backend");
    rhi_backend.select("platform", "linux-vulkan", rhivulkan.owner());

    auto renderer =
        cxx::static_library("renderer")
            .sources(glob({.include = "src/renderer/**/*.cpp"}))
            .public_include({
                "src/renderer",
                "src/renderer/passes",
            })
            .include({
                "src",
                "src/rhi",
                "src/rhi/vulkan",
                "src/scene",
                "src/obs",
            })
            .link(obs)
            .link(rhi)
            .link(rhi_backend);

    auto scene =
        cxx::static_library("scene")
            .sources(glob({.include = "src/scene/*.cpp", .exclude = "src/scene/usd*.cpp"}))
            .public_include({
                "src",
                "src/scene",
            })
            .include({
                "src/ui",
                "src/renderer",
                "src/obs",
            });

    auto sceneusd =
        cxx::static_library("sceneusd")
            .std("c++20")
            .sources(glob({.include = "src/scene/usd*.cpp"}))
            .public_include({
                "src",
                "src/scene",
            })
            .include({
                "src/obs",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/ui",
                "external/openusd_build/include",
                "external/glm",
                "external/cgltf",
                "external/stb",
                "external/imgui",
                "external/imgui/backends",
                "external/concurrentqueue",
            })
            .warning_off("deprecated-declarations");

    auto imgui =
        cxx::static_library("imgui")
            .sources({
                "external/imgui/imgui.cpp",
                "external/imgui/imgui_draw.cpp",
                "external/imgui/imgui_tables.cpp",
                "external/imgui/imgui_widgets.cpp",
                "external/imgui/imgui_demo.cpp",
                "external/imgui/backends/imgui_impl_vulkan.cpp",
                "external/imgui/backends/imgui_impl_sdl3.cpp",
            })
            .public_include({
                "external/imgui",
                "external/imgui/backends",
            });

    auto ui =
        cxx::static_library("ui")
            .sources(glob({.include = "src/ui/**/*.cpp"}))
            .public_include({"src/ui"})
            .include({
                "src",
                "src/obs",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "external/imgui",
            })
            .link(renderer)
            .link(scene)
            .link(sceneusd)
            .link(imgui);

    Tool shaders("shaders");
    shaders.command({"glslc", "$in", "-o", "$out"})
        .for_each(
            concat({
                glob({.include = "shaders/*.vert"}),
                glob({.include = "shaders/*.frag"}),
            }),
            [](const BuildVariant& variant, const Path& source) { return variant.out_dir / "shaders" / (source.filename().string() + ".spv"); });

    Tool clean("clean");
    clean.command({"rm", "-rf", "$out_dir"});

    Tool format("format");
    format.global()
        .inputs(concat({
            glob({.include = "src/**/*.cpp"}),
            glob({.include = "src/**/*.h"}),
            glob({.include = "build/**/*.cpp"}),
            glob({.include = "build/**/*.hpp"}),
        }))
        .command({"clang-format", "-i", "$in"});

    Tool tidy("tidy");
    tidy.global()
        .inputs(concat({
            glob({.include = "src/**/*.cpp"}),
            glob({.include = "build/**/*.cpp"}),
        }))
        .command({"clang-tidy", "$in", "--", "-std=c++23", "-Ibuild/framework"});

    auto view =
        cxx::program("ngen-view")
            .sources({
                "src/main.cpp",
                "src/camera.cpp",
                "src/debugdraw.cpp",
                "src/jobsystem.cpp",
            })
            .include({
                "src",
                "src/obs",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "src/ui",
                "external/glm",
                "external/cgltf",
                "external/stb",
                "external/imgui",
                "external/imgui/backends",
                "external/concurrentqueue",
            })
            .link(obs)
            .link(rhi)
            .link(rhivulkan)
            .link(renderer)
            .link(scene)
            .link(sceneusd)
            .link(ui)
            .link(imgui)
            .link_flags(sdl3_libs)
            .depend_on(shaders);
    add_usd_linkage(view);

    Project p;

    auto& linux_vulkan = p.platform("linux-vulkan").os("linux").graphics_api("vulkan").exe_suffix("");

    cxx::platform(linux_vulkan)
        .compile_flag("-fPIC")
        .compile_flag("-Wall")
        .define("NGEN_PLATFORM_LINUX")
        .define("NGEN_GFX_VULKAN")
        .define("GLM_FORCE_RADIANS")
        .define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
        .system_lib("vulkan")
        .system_lib("m")
        .toolchain()
        .compiler("clang++")
        .archiver("ar")
        .default_std("c++23");

    for (const auto& flag : sdl3_cflags) {
        cxx::platform(linux_vulkan).compile_flag(flag);
    }

    cxx::configuration(p.config("debug").out_dir("_out")).compile_flag("-O0").compile_flag("-g").define("DEBUG=1");

    cxx::configuration(p.config("release").out_dir("_out")).compile_flag("-O2").compile_flag("-g").define("NDEBUG");

    cxx::configuration(p.config("gamerelease").out_dir("_out"))
        .compile_flag("-O3")
        .compile_flag("-fvisibility=hidden")
        .link_flag("-flto")
        .link_flag("-Wl,-s")
        .link_flag("-Wl,--gc-sections")
        .define("NDEBUG")
        .define("SHIPPING=1");

    p.target(view);
    p.target(clean);
    p.target(format);
    p.target(tidy);
    p.default_target(view);

    auto emitted = NinjaBackend{}.emit(p);
    if (!emitted) {
        std::cerr << emitted.error().message << "\n";
        return 1;
    }
    return 0;
}
