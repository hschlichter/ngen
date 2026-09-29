#include "src/build/framework/cxx/configuration.hpp"
#include "src/build/framework/cxx/platform.hpp"
#include "src/build/framework/cxx/target.hpp"
#include "src/build/framework/glob.hpp"
#include "src/build/framework/packrule.hpp"
#include "src/build/framework/phony.hpp"
#include "src/build/framework/project.hpp"
#include "src/build/framework/tool.hpp"
#include "src/build/ir/main.hpp"

#include <filesystem>
#include <string>

using namespace build;

auto main(int argc, char** argv) -> int {
    auto format =
        tool("format")
            .global()
            .inputs(concat({
                glob({.include = "src/**/*.cpp"}),
                glob({.include = "src/**/*.h", .exclude = "src/build/ir/xxhash.h"}), // vendored
                glob({.include = "src/**/*.hpp"}),
            }))
            .command({"clang-format", "-i", "$in"});

    auto tidy =
        tool("tidy")
            .global()
            .inputs(concat({
                glob({.include = "src/**/*.cpp"}),
            }))
            .command({"clang-tidy", "$in", "--", "-std=c++23", "-Isrc/build/framework"});

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
            .define("DEBUG=1")
            .define("NGEN_INTROSPECTION=1"); // debugging and introspection tooling (RenderDoc, ...); not in gamerelease

    auto release =
        cxx::configuration("release")
            .out_dir("_out")
            .compile_flag("-O2")
            .compile_flag("-g")
            .compile_flag("-fno-omit-frame-pointer")
            .define("NDEBUG")
            .define("NGEN_INTROSPECTION=1");

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

    auto profile =
        cxx::static_library("profile")
            .sources(glob({.include = "src/profile/**/*.cpp"}))
            .public_include({"src/profile"})
            .include({"src/rhi"});

    // RPC core: frames, JSON-RPC, TCP on loopback, discovery. The standard library plus header-only
    // nlohmann/json, so the build server can use it too (src/rpc/README.md).
    auto rpccore =
        cxx::static_library("rpccore")
            .sources(glob({.include = "src/rpc/core/*.cpp"}))
            .public_include({
                "src/rpc/core",
                "external/json/single_include",
            });

    // RPC engine layer: method registry with parameter schemas, responders, the endpoint that
    // dispatches calls onto the main thread.
    auto rpc =
        cxx::static_library("rpc")
            .sources(glob({.include = "src/rpc/*.cpp"}))
            .public_include({"src/rpc"})
            .link(rpccore)
            .link(obs);

    // Session commands: verbs shared by CLI flags, scripts and the camera window.
    auto session =
        cxx::static_library("session")
            .sources(glob({.include = "src/session/**/*.cpp"}))
            .public_include({"src/session"});

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
                "src/profile",
                "external/imgui",
                "external/stb",
            })
            .link(obs)
            .link(profile)
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
                "src/profile",
            })
            .link(profile);

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
                "src/profile",
                "src/session",
                "external/imgui",
            })
            .link(renderer)
            .link(profile)
            .link(session)
            .link(scene)
            .link(sceneusd)
            .link(imgui);

    // Packing (src/pack/README.md): one packer program per asset type, rules saying which assets each packs,
    // and pack targets listing assets to pack. A packed asset is written to <out_dir>/packs/<asset id>.
    auto packLib = cxx::static_library("pack").sources(glob({.include = "src/pack/*.cpp"})).public_include({"src/pack"});
    auto packerShader = cxx::program("ngen-packer-shader").sources({"src/apps/packershader.cpp"}).link(packLib);

    // Shader parameters follow the configuration the way compiler flags do: debug keeps source-level
    // debug info and no optimisation, release optimises and keeps debug info, gamerelease optimises only.
    auto shaderRule =
        pack_rule("shader")
            .match({"shaders/*.vert", "shaders/*.frag", "shaders/*.comp", "shaders/*.geom"})
            .packer(packerShader)
            .param("optimize", per_config({{"debug", "0"}, {"release", "1"}, {"gamerelease", "1"}}))
            .param("debug_info", per_config({{"debug", "1"}, {"release", "1"}, {"gamerelease", "0"}}))
            .version(1);

    // The core pack: the assets ngen-view loads at start-up.
    auto corePack =
        build::pack("core")
            .assets(concat({
                glob({.include = "shaders/*.vert"}),
                glob({.include = "shaders/*.frag"}),
                glob({.include = "shaders/*.comp"}),
                glob({.include = "shaders/*.geom"}),
            }));

    auto view =
        cxx::program("ngen-view")
            .sources({
                "src/apps/view.cpp",
                "src/camera.cpp",
                "src/debugdraw.cpp",
                "src/jobsystem.cpp",
                "src/imguibackendvulkan.cpp",
                "src/renderdoccapture.cpp",
                "src/view/viewcommands.cpp",
                "src/view/viewdumps.cpp",
            })
            .include({
                "src",
                "src/view",
                "src/obs",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "src/ui",
                "src/profile",
                "src/session",
                "external/glm",
                "external/cgltf",
                "external/stb",
                "external/imgui",
                "external/imgui/backends",
                "external/concurrentqueue",
                "external/renderdoc", // renderdoc_app.h only; the library is dlopen'd at runtime
            })
            .link(obs)
            .link(profile)
            .link(session)
            .link(rhivulkan)
            .link(renderer)
            .link(scene)
            .link(sceneusd)
            .link(ui)
            .link(imgui)
            .link(rpc)
            .link(rpccore)
            .link_flags(sdl3_libs)
            .depend_on(corePack)
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

    // ngen-rpc: command-line RPC client (list, describe, call) for agents and humans.
    auto rpcTool = cxx::program("ngen-rpc").sources({"src/apps/rpc.cpp"}).link(rpccore);

    // ngen-cli: one front door to the tools of the set variant (src/apps/cli.cpp). Standard library only.
    auto cli = cxx::program("ngen-cli").sources({"src/apps/cli.cpp"});

    // RHI examples: one program per feature, reaching only into src/rhi/. See src/rhi/README.md, "Examples".
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
    auto exampleCompute = rhiExample("compute");
    auto exampleTimestamps = rhiExample("timestamps");
    auto exampleGpuZones = rhiExample("gpuzones");
    auto exampleBindless = rhiExample("bindless");
    auto exampleIndirect = rhiExample("indirect");

    // One name for the whole RHI example set, so the sweep cannot run stale binaries.
    auto examples = phony("examples")
                        .depend_on(exampleTriangle)
                        .depend_on(exampleQuad)
                        .depend_on(exampleTexture)
                        .depend_on(exampleUniforms)
                        .depend_on(exampleDepth)
                        .depend_on(exampleRenderTarget)
                        .depend_on(examplePushConstants)
                        .depend_on(exampleBlend)
                        .depend_on(exampleLines)
                        .depend_on(exampleMipCube)
                        .depend_on(exampleCompute)
                        .depend_on(exampleTimestamps)
                        .depend_on(exampleGpuZones)
                        .depend_on(exampleBindless)
                        .depend_on(exampleIndirect);

    p.target(view);
    p.target(examples);
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
    p.target(exampleCompute);
    p.target(exampleTimestamps);
    p.target(exampleGpuZones);
    p.target(exampleBindless);
    p.target(exampleIndirect);
    p.target(cli);
    p.target(rpcTool);
    p.pack_rule(shaderRule);
    p.target(packerShader);
    p.target(corePack);
    p.target(format);
    p.target(tidy);
    p.default_target(view);

    return ir::main(argc, argv, p);
}
