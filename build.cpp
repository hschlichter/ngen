#include "src/build/framework/cxx/configuration.hpp"
#include "src/build/framework/cxx/platform.hpp"
#include "src/build/framework/cxx/target.hpp"
#include "src/build/framework/glob.hpp"
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

    auto profile =
        cxx::static_library("profile")
            .sources(glob({.include = "src/profile/**/*.cpp"}))
            .public_include({"src/profile"})
            .include({"src/rhi"});

    // RPC core: frames, JSON-RPC, TCP on loopback, discovery. The standard library plus header-only
    // nlohmann/json, so programs outside the engine, such as the asset server, use it too (src/rpc/README.md).
    auto rpccore =
        cxx::static_library("rpccore")
            .sources(glob({.include = "src/rpc/core/*.cpp"}))
            .public_include({
                "src/rpc/core",
                "external/json/single_include",
            });

    // Trace (src/trace/README.md): structured events in an always-on ring per process, streamed to subscribers over
    // RPC. Every process that emits events links it, the asset server included.
    auto traceLib =
        cxx::static_library("trace")
            .sources(glob({.include = "src/trace/*.cpp"}))
            .public_include({"src/trace"})
            .link(rpccore);

    // RPC engine layer: method registry with parameter schemas, responders, the endpoint that
    // dispatches calls onto the main thread.
    auto rpc =
        cxx::static_library("rpc")
            .sources(glob({.include = "src/rpc/*.cpp"}))
            .public_include({"src/rpc"})
            .link(rpccore)
            .link(traceLib);

    // The engine side of the asset server's stream (src/asset/README.md).
    auto assetClient =
        cxx::static_library("assetclient")
            .sources({"src/asset/assetclient.cpp"})
            .public_include({"src/asset"})
            .link(rpccore);

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

    // What packers share (the packer contract, the packed texture format, the mip filter). The renderer uses the mip
    // filter and the packed texture format too.
    auto packer =
        cxx::static_library("packer")
            .sources({
                "src/asset/pack/packer.cpp",
                "src/asset/pack/mipchain.cpp",
                "src/asset/pack/packedtexture.cpp",
            })
            .public_include({"src/asset/pack"});

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
                "src/trace",
                "src/profile",
                "external/imgui",
                "external/stb",
                "external/glm",
            })
            .link(traceLib)
            .link(profile)
            .link(assetClient)
            .link(packer)
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
                "src/trace",
                "src/profile",
                "external/glm",
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
                "src/trace",
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
            .warning_off("deprecated-declarations")
            // The asset resolver uses AssetClient, which needs C++23. It includes only usd/ar, usd/sdf and base headers, which
            // compile as C++23; the rest of OpenUSD's headers don't (usd/usd/schemaRegistry.h).
            .for_source("src/scene/usdassetresolver.cpp", [](cxx::ObjectFile& file) { file.std("c++23"); })
            .link(assetClient)
            .link(packer)
            .link(profile);

    // The resolver's plugin metadata: USD only uses a resolver whose type a registered plugin declares.
    auto usdPlugins =
        tool("usdplugins")
            .for_each({"src/scene/usdplugins/plugInfo.json"},
                      [](const BuildVariant& variant, const Path& source) -> Path { return variant.out_dir / "usdplugins" / source.filename(); })
            .command({"cp", "$in", "$out"});

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
                "external/imgui/backends/imgui_impl_sdlrenderer3.cpp",
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
                "src/trace",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "src/profile",
                "src/session",
                "external/imgui",
                "external/glm",
            })
            .link(renderer)
            .link(profile)
            .link(session)
            .link(scene)
            .link(sceneusd)
            .link(imgui);

    // Assets (src/asset/README.md): one packer program per asset type, and ngen-asset-server, which packs on
    // request with the rules in the root pack.cpp and streams the results. Nothing here lists assets.
    auto packerShader = cxx::program("ngen-packer-shader").sources({"src/apps/packershader.cpp"}).link(packer);
    auto packerCopy = cxx::program("ngen-packer-copy").sources({"src/apps/packercopy.cpp"}).link(packer);
    auto packerTexture = cxx::program("ngen-packer-texture").sources({"src/apps/packertexture.cpp"}).include({"external/stb"}).link(packer);
    auto assetServer =
        cxx::program("ngen-asset-server")
            .sources({
                "src/apps/assetserver.cpp",
                "pack.cpp",
            })
            .sources(glob({.include = "src/asset/server/*.cpp"}))
            .include({
                "src/asset",
                "src/asset/pack",
                "src/asset/server",
            })
            .link(traceLib)
            .link(rpccore)
            .depend_on(packerShader)
            .depend_on(packerCopy)
            .depend_on(packerTexture);

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
                "src/trace",
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
            .link(traceLib)
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
            .link(assetClient)
            .link(packer)
            .link_flags(sdl3_libs)
            .depend_on(assetServer)
            .depend_on(usdPlugins)
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

    // ngen-introspect (src/introspect/README.md): sees into and calls the running processes, as a window drawn with
    // SDL's renderer and Dear ImGui, or from the command line. An RPC client only; nothing of the engine.
    auto introspectLib =
        cxx::static_library("introspect")
            .sources(glob({.include = "src/introspect/*.cpp"}))
            .public_include({"src/introspect"})
            .link(rpccore)
            .link(imgui);
    auto introspectTool =
        cxx::program("ngen-introspect")
            .sources({"src/apps/introspect.cpp"})
            .include({"external/stb"})
            .link(introspectLib)
            .link(rpccore)
            .link(imgui)
            .link_flags(sdl3_libs);

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
    p.target(introspectTool);
    p.target(packerShader);
    p.target(packerCopy);
    p.target(packerTexture);
    p.target(assetServer);
    p.target(usdPlugins);
    p.target(format);
    p.target(tidy);
    p.default_target(view);

    return ir::main(argc, argv, p);
}
