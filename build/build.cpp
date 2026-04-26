#include "framework/backend.hpp"
#include "framework/backendninja.hpp"
#include "framework/configuration.hpp"
#include "framework/cxxtoolchain.hpp"
#include "framework/flags.hpp"
#include "framework/glob.hpp"
#include "framework/graph.hpp"
#include "framework/library.hpp"
#include "framework/path.hpp"
#include "framework/platform.hpp"
#include "framework/program.hpp"
#include "framework/staticlibrary.hpp"
#include "framework/target.hpp"
#include "framework/tool.hpp"

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

using namespace build;

namespace {

std::string absolute_openusd_lib_path() {
    return (std::filesystem::current_path() / "external/openusd_build/lib").string();
}

void add_usd_linkage(Target& target) {
    target
        .lib_search("external/openusd_build/lib")
        .rpath(absolute_openusd_lib_path())
        .link_raw("-lusd_usd")
        .link_raw("-lusd_usdGeom")
        .link_raw("-lusd_usdShade")
        .link_raw("-lusd_usdLux")
        .link_raw("-lusd_sdf")
        .link_raw("-lusd_pcp")
        .link_raw("-lusd_tf")
        .link_raw("-lusd_vt")
        .link_raw("-lusd_gf")
        .link_raw("-lusd_ar")
        .link_raw("-lusd_arch")
        .link_raw("-lusd_plug")
        .link_raw("-lusd_js")
        .link_raw("-lusd_work")
        .link_raw("-lusd_trace")
        .link_raw("-lusd_ts")
        .link_raw("-lusd_pegtl")
        .link_raw("-lusd_kind");
}

} // namespace

int main(int argc, char** argv) {
    Graph g;

    auto sdl3_cflags = capture_tokens({"pkg-config", "--cflags", "sdl3"});
    auto sdl3_libs = capture_tokens({"pkg-config", "--libs", "sdl3"});
    auto files = [](std::string include) {
        return GlobSpec{.include = std::move(include), .exclude = ""};
    };

    auto cxx = std::make_unique<CxxToolchain>(CxxToolchain::Config{
        .name = "clang",
        .cxx = "clang++",
        .ar = "ar",
        .linker = "",
        .default_std = "c++23",
        .extra_cxx_flags = {},
        .extra_link_flags = {},
    });

    g.addPlatform(Platform{
            .name = "linux-vulkan",
            .os = "linux",
            .graphics_api = "vulkan",
            .toolchain = std::move(cxx),
            .defines = {
                "NGEN_PLATFORM_LINUX",
                "NGEN_GFX_VULKAN",
                "GLM_FORCE_RADIANS",
                "GLM_FORCE_DEPTH_ZERO_TO_ONE",
            },
            .extra_cxx_flags = concat_tokens({{"-fPIC", "-Wall"}, sdl3_cflags}),
            .extra_link_flags = {},
            .system_libs = {"vulkan", "m"},
            .exe_suffix = "",
    });

    g.addConfig(Configuration{
            .name = "debug",
            .opt = OptLevel::O0,
            .debug_info = true,
            .default_linkage = Linkage::Static,
            .defines = {"DEBUG=1"},
            .extra_cxx_flags = {},
            .extra_link_flags = {},
    });
    g.addConfig(Configuration{
            .name = "release",
            .opt = OptLevel::O2,
            .debug_info = true,
            .default_linkage = Linkage::Static,
            .defines = {"NDEBUG"},
            .extra_cxx_flags = {},
            .extra_link_flags = {},
    });
    g.addConfig(Configuration{
            .name = "gamerelease",
            .opt = OptLevel::O3,
            .debug_info = false,
            .default_linkage = Linkage::Static,
            .defines = {"NDEBUG", "SHIPPING=1"},
            .extra_cxx_flags = {"-fvisibility=hidden"},
            .extra_link_flags = {"-flto", "-Wl,-s", "-Wl,--gc-sections"},
    });

    auto& obs = g.add<Library>("obs");
    obs.cxx(glob(files("src/obs/**/*.cpp")))
        .public_include({"src/obs", "external/concurrentqueue"});

    auto& rhi = g.add<Library>("rhi");
    rhi.cxx(glob(files("src/rhi/*.cpp")))
        .public_include({"src/rhi"})
        .include({"external/imgui"});

    auto& rhivulkan = g.add<Library>("rhivulkan");
    rhivulkan.cxx(glob(files("src/rhi/vulkan/**/*.cpp")))
        .public_include({"src/rhi/vulkan"})
        .include({"src", "external/imgui", "external/imgui/backends"})
        .only_on({"linux-vulkan"})
        .link(rhi);

    auto& rhi_backend = g.add<Alias>("rhi-backend");
    rhi_backend.select("platform", "linux-vulkan", rhivulkan);

    auto& renderer = g.add<Library>("renderer");
    renderer.cxx(glob(files("src/renderer/**/*.cpp")))
        .public_include({"src/renderer", "src/renderer/passes"})
        .include({"src", "src/rhi", "src/rhi/vulkan", "src/scene", "src/obs"})
        .link(obs)
        .link(rhi)
        .link(rhi_backend);

    auto& scene = g.add<Library>("scene");
    scene.cxx(glob({.include = "src/scene/*.cpp", .exclude = "src/scene/usd*.cpp"}))
        .public_include({"src", "src/scene"})
        .include({"src/ui", "src/renderer", "src/obs"});

    auto& sceneusd = g.add<StaticLibrary>("sceneusd");
    sceneusd.std("c++20")
        .cxx(glob(files("src/scene/usd*.cpp")))
        .public_include({"src", "src/scene"})
        .include({
            "src/obs", "src/rhi", "src/rhi/vulkan", "src/renderer",
            "src/renderer/passes", "src/ui", "external/openusd_build/include",
            "external/glm", "external/cgltf", "external/stb", "external/imgui",
            "external/imgui/backends", "external/concurrentqueue"
        })
        .warning_off("deprecated-declarations");

    auto& imgui = g.add<StaticLibrary>("imgui");
    imgui.cxx({
        "external/imgui/imgui.cpp",
        "external/imgui/imgui_draw.cpp",
        "external/imgui/imgui_tables.cpp",
        "external/imgui/imgui_widgets.cpp",
        "external/imgui/imgui_demo.cpp",
        "external/imgui/backends/imgui_impl_vulkan.cpp",
        "external/imgui/backends/imgui_impl_sdl3.cpp",
    }).public_include({"external/imgui", "external/imgui/backends"});

    auto& ui = g.add<Library>("ui");
    ui.cxx(glob(files("src/ui/**/*.cpp")))
        .public_include({"src/ui"})
        .include({"src", "src/obs", "src/rhi", "src/rhi/vulkan", "src/renderer", "src/renderer/passes", "src/scene", "external/imgui"})
        .link(renderer)
        .link(scene)
        .link(sceneusd)
        .link(imgui);

    auto& shaders = g.add<Tool>("shaders");
    shaders.command({"glslc", "$in", "-o", "$out"})
        .for_each(concat({
            glob(files("shaders/*.vert")),
            glob(files("shaders/*.frag")),
        }), [](const BuildVariant& variant, const Path& source) {
            return variant.out_dir / "shaders" / (source.filename().string() + ".spv");
        });

    auto& view = g.add<Program>("ngen-view");
    view.cxx({
        "src/main.cpp",
        "src/camera.cpp",
        "src/debugdraw.cpp",
        "src/jobsystem.cpp",
    }).include({
        "src", "src/obs", "src/rhi", "src/rhi/vulkan", "src/renderer",
        "src/renderer/passes", "src/scene", "src/ui", "external/glm",
        "external/cgltf", "external/stb", "external/imgui",
        "external/imgui/backends", "external/concurrentqueue"
    }).link(obs)
      .link(rhi)
      .link(rhivulkan)
      .link(renderer)
      .link(scene)
      .link(sceneusd)
      .link(ui)
      .link(imgui)
      .link_raw_many(sdl3_libs)
      .depend_on(shaders);
    add_usd_linkage(view);

    g.setDefault(view);

    auto parsed = parse_ninja_target(argc, argv, view.name());
    if (!parsed) {
        std::cerr << parsed.error().message << "\n";
        return 1;
    }

    Target* desired = g.find(parsed->target);
    if (!desired || parsed->target == "clean" || parsed->target == "format" || parsed->target == "tidy") {
        desired = &view;
    }

    auto emitted = NinjaBackend{}.emit(g, *desired);
    if (!emitted) {
        std::cerr << emitted.error().message << "\n";
        return 1;
    }
    return 0;
}
