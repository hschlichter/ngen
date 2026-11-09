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
            .define("NGEN_INTROSPECTION=1") // debugging and introspection tooling (RenderDoc, ...); not in gamerelease
            // Tracy's client (src/profile/README.md): started by the program that wants it (profile::startTracy), listens on loopback,
            // records only while a Tracy server is connected.
            .define("TRACY_ENABLE")
            .define("TRACY_ON_DEMAND")
            .define("TRACY_ONLY_LOCALHOST")
            .define("TRACY_MANUAL_LIFETIME")
            .define("TRACY_DELAYED_INIT");

    auto release =
        cxx::configuration("release")
            .out_dir("_out")
            .compile_flag("-O2")
            .compile_flag("-g")
            .compile_flag("-fno-omit-frame-pointer")
            .define("NDEBUG")
            .define("NGEN_INTROSPECTION=1")
            .define("TRACY_ENABLE")
            .define("TRACY_ON_DEMAND")
            .define("TRACY_ONLY_LOCALHOST")
            .define("TRACY_MANUAL_LIFETIME")
            .define("TRACY_DELAYED_INIT");

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

    // Configuration (src/config/README.md): the engine's settings, typed values declared in code by the system they configure. Standard
    // library only, beside profile and trace.
    auto config =
        cxx::static_library("config")
            .sources(glob({.include = "src/config/*.cpp"}))
            .public_include({"src/config"});

    // Tracy's client, vendored at the version of the Tracy tools it talks to. Compiles to nothing in a configuration without TRACY_ENABLE
    // (gamerelease); programs that link profile link it too.
    auto tracy =
        cxx::static_library("tracy")
            .sources({"external/tracy/public/TracyClient.cpp"})
            .public_include({"external/tracy/public"})
            // Vendored.
            .warning_off("unused-variable")
            .warning_off("unused-function");

    auto profile =
        cxx::static_library("profile")
            .sources(glob({.include = "src/profile/*.cpp"}))
            .public_include({"src/profile"})
            .include({"src/rhi"})
            .link(tracy)
            .link(config);

    // RPC core: frames, JSON-RPC, TCP on loopback, discovery. The standard library plus header-only
    // nlohmann/json, so programs outside the engine, such as the asset server, use it too (src/rpc/README.md).
    auto rpccore =
        cxx::static_library("rpccore")
            .sources(glob({.include = "src/rpc/core/*.cpp"}))
            .public_include({
                "src/rpc/core",
                "external/json/single_include",
            })
            .link(profile);

    // Trace (src/trace/README.md): structured events in an always-on ring per process, streamed to subscribers over
    // RPC. Every process that emits events links it, the asset server included.
    auto traceLib =
        cxx::static_library("trace")
            .sources(glob({.include = "src/trace/*.cpp"}))
            .public_include({"src/trace"})
            .link(rpccore)
            .link(profile);

    // The profiler's records and dump (src/profile/README.md) for any process with an RPC endpoint: ngen-view and the asset server.
    auto profileRpc =
        cxx::static_library("profilerpc")
            .sources(glob({.include = "src/profile/rpc/*.cpp"}))
            .public_include({"src/profile/rpc"})
            .link(rpccore)
            .link(profile);

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
                "src/asset/pack/scenepack.cpp",
            })
            .public_include({"src/asset/pack"})
            .link(profile);

    // The runtime scene (src/world/README.md): the World, composed from scene packs. No OpenUSD.
    auto worldLib =
        cxx::static_library("world")
            .sources(glob({.include = "src/world/*.cpp"}))
            .public_include({
                "src/world",
                "external/glm",
            })
            .link(packer)
            .link(profile);

    auto renderer =
        cxx::static_library("renderer")
            .sources(glob({.include = "src/renderer/**/*.cpp"}))
            .public_include({
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "src",
                "external/glm",
            })
            .include({
                "src",
                "src/rhi",
                "src/rhi/vulkan",
                "src/scene",
                "src/trace",
                "src/profile",
                "external/stb",
                "external/glm",
            })
            .link(traceLib)
            .link(profile)
            .link(config)
            .link(assetClient)
            .link(packer)
            .link(rhi_backend);

    // The renderer's instrumentation (src/rendertools/README.md): the frame graph's debug snapshot and previews, the render debug snapshot,
    // the frame debugger, GPU counters, the texture inspector and dumps, screenshots. A render extension; no ImGui.
    auto rendertools =
        cxx::static_library("rendertools")
            .sources(glob({.include = "src/rendertools/*.cpp"}))
            .public_include({"src/rendertools"})
            .include({
                "src/trace",
                "src/profile",
                "src/rhi",
                "src/rhi/vulkan",
                "external/stb",
            })
            .link(renderer)
            .link(traceLib)
            .link(profile);

    // The view's runtime (src/view/README.md): the scene composed from the packs and placed for the renderer, picking, the camera modes, and
    // the frame's engine steps the main loop calls (ViewFrame) with the hooks the tools and the UI join it by.
    auto viewLib =
        cxx::static_library("view")
            .sources({
                "src/view/viewscene.cpp",
                "src/view/placement.cpp",
                "src/view/viewpicking.cpp",
                "src/view/ground.cpp",
                "src/view/camera.cpp",
                "src/view/cameraflight.cpp",
                "src/view/assetgoto.cpp",
                "src/view/viewframe.cpp",
            })
            .public_include({"src/view"})
            .include({
                "src/trace",
                "src/profile",
                "src/rhi",
                "src/rhi/vulkan",
                "external/json/single_include",
            })
            .link(renderer)
            .link(worldLib)
            .link(assetClient)
            .link(packer)
            .link(config)
            .link(traceLib)
            .link(profile);

    // The view's commands (src/view/README.md): every engine method, script verb and record, by domain, and the dumps that answer frames later.
    // What an agent reaches without the tools or the UI.
    auto viewCommands =
        cxx::static_library("viewcommands")
            .sources({
                "src/view/viewcommands.cpp",
                "src/view/cameracommands.cpp",
                "src/view/scenecommands.cpp",
                "src/view/lodcommands.cpp",
                "src/view/configcommands.cpp",
                "src/view/displaycommands.cpp",
                "src/view/profilecommands.cpp",
                "src/view/viewdumps.cpp",
            })
            .include({
                "src",
                "src/trace",
                "src/profile",
                "src/rhi",
                "src/rhi/vulkan",
                "external/json/single_include",
            })
            .link(viewLib)
            .link(rendertools)
            .link(renderer)
            .link(rpc)
            .link(rpccore)
            .link(session)
            .link(assetClient)
            .link(config)
            .link(traceLib)
            .link(profileRpc)
            .link(profile);

    // The tools (src/tools/README.md): the LOD and Streaming tools and the debug camera, their data built from the view's scene. No UI.
    auto toolsLib =
        cxx::static_library("tools")
            .sources(glob({.include = "src/tools/*.cpp"}))
            .public_include({"src/tools"})
            .include({
                "src/trace",
                "src/profile",
                "src/rhi",
                "src/rhi/vulkan",
            })
            .link(viewCommands)
            .link(viewLib)
            .link(renderer)
            .link(config);

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
            })
            // Vendored: the Vulkan backend sets a flag it reads only when it loads Vulkan's functions itself.
            .for_source("external/imgui/backends/imgui_impl_vulkan.cpp", [](cxx::ObjectFile& file) { file.warning_off("unused-but-set-global"); });

    auto ui =
        cxx::static_library("ui")
            .sources(glob({.include = "src/ui/**/*.cpp"}))
            .public_include({"src/ui"})
            .include({
                "src",
                "src/asset",
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
                "external/json/single_include",
            })
            .link(toolsLib)
            .link(rendertools)
            .link(renderer)
            .link(profile)
            .link(config)
            .link(session)
            .link(imgui);

    // Assets (src/asset/README.md): one packer program per asset type, and ngen-asset-server, which packs on
    // request with the rules in the root pack.cpp and streams the results. Nothing here lists assets.
    auto packerShader = cxx::program("ngen-packer-shader").sources({"src/apps/packershader.cpp"}).link(packer).link(profile).link(tracy).link(config);
    auto packerCopy = cxx::program("ngen-packer-copy").sources({"src/apps/packercopy.cpp"}).link(packer).link(profile).link(tracy).link(config);
    auto packerTexture = cxx::program("ngen-packer-texture").sources({"src/apps/packertexture.cpp"}).include({"external/stb"}).link(packer).link(profile).link(tracy).link(config);
    auto packerExr = cxx::program("ngen-packer-exr").sources({"src/apps/packerexr.cpp"}).include({"external/stb", "external/tinyexr"}).link(packer).link(profile).link(tracy).link(config);
    // ngen-packdump: prints a scene pack, from a file or fetched from the asset server, as JSON.
    auto packDump = cxx::program("ngen-packdump")
                        .sources({"src/apps/packdump.cpp"})
                        .include({"src/asset", "src/asset/pack", "external/json/single_include"})
                        .link(packer)
                        .link(assetClient)
                        .link(rpccore)
                        .link(profile)
                        .link(tracy)
                        .link(config);
    // ngen-pack: has the asset server pack everything a scene can reach, so a view opens it from a full cache.
    auto scenePack = cxx::program("ngen-pack").sources({"src/apps/pack.cpp"}).include({"src/asset", "src/asset/pack"}).link(packer).link(assetClient).link(rpccore).link(profile).link(tracy).link(config);
    // meshoptimizer: welding and meshlets for the USD packer's meshes.
    auto meshoptimizer = cxx::static_library("meshoptimizer").sources(glob({.include = "external/meshoptimizer/src/*.cpp"})).public_include({"external/meshoptimizer/src"});
    // The USD packer (src/asset/README.md): one USD file to a layer pack plus its meshes and materials. It links OpenUSD; the USD half
    // builds as C++20, as OpenUSD's headers require.
    auto packerUsd =
        cxx::program("ngen-packer-usd")
            .sources({
                "src/apps/packerusd.cpp",
                "src/asset/pack/usd/usdpacker.cpp",
                "src/asset/pack/usd/primshapemesh.cpp",
                "src/asset/pack/meshletbuild.cpp",
            })
            .include({
                "src",
                "src/asset",
                "src/asset/pack",
                "src/asset/pack/usd",
                "src/scene",
                "external/openusd_build/include",
                "external/glm",
            })
            .link(meshoptimizer)
            .warning_off("deprecated-declarations")
            .for_source("src/asset/pack/usd/usdpacker.cpp", [](cxx::ObjectFile& file) { file.std("c++20"); })
            .link(packer)
            .link(profile)
            .link(tracy)
            .link(config)
            .lib_search("external/openusd_build/lib")
            .rpath((std::filesystem::current_path() / "external/openusd_build/lib").string())
            .link_flag("-lusd_usd")
            .link_flag("-lusd_usdGeom")
            .link_flag("-lusd_usdShade")
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
            .link_flag("-lusd_kind")
            .link_flag("-ltbb");
    // ngen-worldcheck and ngen-usdcheck (src/world/README.md): a scene's fingerprint as the World composes it from packs, and as
    // OpenUSD composes it, to compare. ngen-usdcheck links OpenUSD and builds as C++20, as OpenUSD's headers require.
    auto worldCheck = cxx::program("ngen-worldcheck").sources({"src/apps/worldcheck.cpp"}).include({"src/asset"}).link(worldLib).link(packer).link(assetClient).link(rpccore).link(profile).link(tracy).link(config);
    auto usdCheck =
        cxx::program("ngen-usdcheck")
            .sources({"src/apps/usdcheck.cpp"})
            .include({
                "src/world",
                "external/openusd_build/include",
            })
            .warning_off("deprecated-declarations")
            .for_source("src/apps/usdcheck.cpp", [](cxx::ObjectFile& file) { file.std("c++20"); })
            .lib_search("external/openusd_build/lib")
            .rpath((std::filesystem::current_path() / "external/openusd_build/lib").string())
            .link_flag("-lusd_usd")
            .link_flag("-lusd_usdGeom")
            .link_flag("-lusd_usdLod")
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
            .link_flag("-lusd_kind")
            .link_flag("-ltbb");
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
            .link(profileRpc)
            .link(rpccore)
            .link(packer)
            .link(profile)
            .link(tracy)
            .link(config)
            .depend_on(packerShader)
            .depend_on(packerCopy)
            .depend_on(packerTexture)
            .depend_on(packerExr)
            .depend_on(packerUsd);

    auto view =
        cxx::program("ngen-view")
            .sources({
                "src/apps/view.cpp",
                "src/jobsystem.cpp",
                "src/imguibackendvulkan.cpp",
                "src/renderdoccapture.cpp",
            })
            .include({
                "src",
                "src/view",
                "src/tools",
                "src/trace",
                "src/rhi",
                "src/rhi/vulkan",
                "src/renderer",
                "src/renderer/passes",
                "src/scene",
                "src/ui",
                "src/profile",
                "src/session",
                "src/world",
                "src/asset/pack",
                "external/glm",
                "external/stb",
                "external/imgui",
                "external/imgui/backends",
                "external/renderdoc", // renderdoc_app.h only; the library is dlopen'd at runtime
            })
            .link(traceLib)
            .link(profileRpc)
            .link(profile)
            .link(tracy)
            .link(config)
            .link(session)
            .link(rhivulkan)
            .link(renderer)
            .link(toolsLib)
            .link(viewCommands)
            .link(viewLib)
            .link(rendertools)
            .link(ui)
            .link(imgui)
            .link(rpc)
            .link(rpccore)
            .link(assetClient)
            .link(worldLib)
            .link(packer)
            .link_flags(sdl3_libs)
            .depend_on(assetServer)
            .depend_on(scenePack);

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
            .link(profile)
            .link(tracy)
            .link(config)
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
    auto exampleMeshShader = rhiExample("meshshader");
    auto exampleMipChain = rhiExample("mipchain");

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
                        .depend_on(exampleIndirect)
                        .depend_on(exampleMeshShader)
                        .depend_on(exampleMipChain);

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
    p.target(exampleMeshShader);
    p.target(exampleMipChain);
    p.target(cli);
    p.target(introspectTool);
    p.target(packerShader);
    p.target(packerCopy);
    p.target(packerTexture);
    p.target(packerExr);
    p.target(packerUsd);
    p.target(packDump);
    p.target(scenePack);
    p.target(worldCheck);
    p.target(usdCheck);
    p.target(assetServer);
    p.target(format);
    p.target(tidy);

    return ir::main(argc, argv, p);
}
