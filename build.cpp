#include "build/framework/cxx/configuration.hpp"
#include "build/framework/cxx/platform.hpp"
#include "build/framework/cxx/target.hpp"
#include "build/framework/glob.hpp"
#include "build/framework/project.hpp"
#include "build/framework/tool.hpp"
#include "build/ir/main.hpp"

#include <filesystem>
#include <string>

using namespace build;

auto main(int argc, char** argv) -> int {
    auto format =
        tool("format")
            .global()
            .inputs(concat({
                glob({.include = "src/**/*.cpp"}),
                glob({.include = "src/**/*.h"}),
                glob({.include = "build/**/*.cpp"}),
                glob({.include = "build/**/*.hpp"}),
            }))
            .command({"clang-format", "-i", "$in"});

    auto tidy =
        tool("tidy")
            .global()
            .inputs(concat({
                glob({.include = "src/**/*.cpp"}),
                glob({.include = "build/**/*.cpp"}),
            }))
            .command({"clang-tidy", "$in", "--", "-std=c++23", "-Ibuild/framework"});

    auto sdl3_cflags = capture_tokens({"pkg-config", "--cflags", "sdl3"});
    auto sdl3_libs = capture_tokens({"pkg-config", "--libs", "sdl3"});

    auto clang = cxx::toolchain()
        .compiler("clang++")
        .archiver("ar")
        .default_std("c++23");

    auto linux_vulkan =
        cxx::platform("linux-vulkan")
            .os("linux")
            .graphics_api("vulkan")
            .exe_suffix("")
            .toolchain(clang)
            .compile_flag("-fPIC")
            .compile_flag("-Wall")
            .compile_flags(sdl3_cflags)
            .define("NGEN_PLATFORM_LINUX")
            .define("NGEN_GFX_VULKAN")
            .define("GLM_FORCE_RADIANS")
            .define("GLM_FORCE_DEPTH_ZERO_TO_ONE")
            .system_lib("vulkan")
            .system_lib("m");

    auto debug =
        cxx::configuration("debug")
            .out_dir("_out")
            .compile_flag("-O0")
            .compile_flag("-g")
            .define("DEBUG=1");

    auto release =
        cxx::configuration("release")
            .out_dir("_out")
            .compile_flag("-O2")
            .compile_flag("-g")
            .compile_flag("-fno-omit-frame-pointer")
            .define("NDEBUG");

    auto gamerelease =
        cxx::configuration("gamerelease")
            .out_dir("_out")
            .compile_flag("-O3")
            .compile_flag("-fvisibility=hidden")
            .link_flag("-flto")
            .link_flag("-Wl,-s")
            .link_flag("-Wl,--gc-sections")
            .define("NDEBUG")
            .define("SHIPPING=1");

    Project p;
    p.platform(linux_vulkan);
    p.config(debug);
    p.config(release);
    p.config(gamerelease);

    auto obs =
        cxx::static_library("obs")
            .sources(glob({.include = "src/obs/**/*.cpp"}))
            .public_include({
                "src/obs",
                "external/concurrentqueue",
            });

    // src/rhi is header-only: the backend-agnostic interface. Consumers add the
    // include path directly; only backends are libraries.
    auto rhivulkan =
        cxx::static_library("rhivulkan")
            .sources(glob({.include = "src/rhi/vulkan/**/*.cpp"}))
            .public_include({
                "src/rhi",
                "src/rhi/vulkan",
            })
            .include({
                "src",
            })
            .only_on({"linux-vulkan"});

    auto rhi_backend = alias("rhi-backend").select("platform", "linux-vulkan", rhivulkan.owner());

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
                "external/imgui",
            })
            .link(obs)
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

    auto shaders =
        tool("shaders")
            .command({"glslc", "$in", "-o", "$out"})
            .for_each(
                concat({
                    glob({.include = "shaders/*.vert"}),
                    glob({.include = "shaders/*.frag"}),
                }),
                [](const BuildVariant& variant, const Path& source) -> Path { return variant.out_dir / "shaders" / (source.filename().string() + ".spv"); });

    auto view =
        cxx::program("ngen-view")
            .sources({
                "src/main.cpp",
                "src/camera.cpp",
                "src/debugdraw.cpp",
                "src/jobsystem.cpp",
                "src/imguibackendvulkan.cpp",
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
            .link(rhivulkan)
            .link(renderer)
            .link(scene)
            .link(sceneusd)
            .link(ui)
            .link(imgui)
            .link_flags(sdl3_libs)
            .depend_on(shaders)
            .lib_search("external/openusd_build/lib")
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

    // RHI examples: one program per feature, reaching only into src/rhi/. See docs/plan_rhi_examples.md.
    auto rhiExample = [&](const std::string& name) {
        return cxx::program("ngen-example-" + name)
            .sources({"src/rhi/examples/" + name + ".cpp"})
            .include({
                "src/rhi",
                "src/rhi/vulkan",
                "src/rhi/examples",
                "external/stb", // screenshot PNG writer; the one third-party header examples may use
            })
            .link(rhi_backend)
            .link("shaderc_shared")
            .link_flags(sdl3_libs);
    };
    auto exampleTriangle = rhiExample("triangle");
    auto exampleQuad = rhiExample("quad");
    auto exampleTexture = rhiExample("texture");
    auto exampleUniforms = rhiExample("uniforms");
    auto exampleDepth = rhiExample("depth");
    auto exampleRenderTarget = rhiExample("rendertarget");
    auto examplePushConstants = rhiExample("pushconstants");
    auto exampleBlend = rhiExample("blend");
    auto exampleLines = rhiExample("lines");
    auto exampleMipCube = rhiExample("mipcube");

    p.target(view);
    p.target(exampleTriangle);
    p.target(exampleQuad);
    p.target(exampleTexture);
    p.target(exampleUniforms);
    p.target(exampleDepth);
    p.target(exampleRenderTarget);
    p.target(examplePushConstants);
    p.target(exampleBlend);
    p.target(exampleLines);
    p.target(exampleMipCube);
    p.target(format);
    p.target(tidy);
    p.default_target(view);

    return ir::main(argc, argv, p);
}
