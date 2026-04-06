#include "camera.h"
#include "material.h"
#include "mesh.h"
#include "renderer.h"
#include "renderworld.h"
#include "rhidebugui.h"
#include "rhidevicevulkan.h"
#include "sceneloader.h"
#include "types.h"
#include "usdscene.h"
#include "usdrenderextractor.h"

#include <imgui.h>

#include <SDL3/SDL.h>

#include <print>

static auto buildRenderWorldFromScene(const Scene& scene, MeshLibrary& meshLib, MaterialLibrary& matLib) -> RenderWorld {
    RenderWorld world;
    for (const auto& md : scene.meshes) {
        auto meshHandle = meshLib.add({
            .vertices = md.vertices,
            .indices = md.indices,
        });
        auto matHandle = matLib.add({
            .texWidth = md.texWidth,
            .texHeight = md.texHeight,
            .texPixels = md.texPixels,
        });
        world.meshInstances.push_back({
            .mesh = meshHandle,
            .material = matHandle,
            .worldTransform = md.transform,
        });
    }
    return world;
}

auto main(int argc, char* argv[]) -> int {
    if (argc < 2) {
        std::println(stderr, "Usage: {} <model.gltf>", argv[0]);
        return 1;
    }

    std::string_view filepath = argv[1];
    bool isUsd = filepath.ends_with(".usda") || filepath.ends_with(".usd") || filepath.ends_with(".usdc") ||
                 filepath.ends_with(".usdz");

    USDScene usdScene;
    USDRenderExtractor usdExtractor;
    MeshLibrary meshLib;
    MaterialLibrary matLib;
    RenderWorld renderWorld;

    if (isUsd) {
        if (!usdScene.open(argv[1])) {
            std::println(stderr, "Failed to open USD scene");
            return 1;
        }
        usdScene.updateAssetBindings(meshLib, matLib);
        usdExtractor.extract(usdScene, renderWorld);
    } else {
        auto scene = loadGltf(argv[1]);
        if (scene.meshes.empty()) {
            std::println(stderr, "Failed to load model");
            return 1;
        }
        renderWorld = buildRenderWorldFromScene(scene, meshLib, matLib);
    }

    // SDL init and window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "SDL_Init failed: {}", SDL_GetError());
        return 1;
    }

    auto windowProps = SDL_CreateProperties();
    SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ngen");
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 2560);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 1440);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, 1);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, 1);
    auto* window = SDL_CreateWindowWithProperties(windowProps);
    if (window == nullptr) {
        std::println(stderr, "SDL_CreateWindowWithProperties failed: {}", SDL_GetError());
        return 1;
    }
    SDL_DestroyProperties(windowProps);

    // RHI device
    RhiDeviceVulkan rhiDevice;
    if (!rhiDevice.init(window)) {
        return 1;
    }

    // Renderer
    Renderer renderer;
    if (!renderer.init(&rhiDevice, window)) {
        return 1;
    }
    renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
    renderer.debugui()->setDrawCallback([] { ImGui::ShowDemoWindow(); });

    // Camera
    auto cam = Camera{
        .position = glm::vec3(2.0f, 1.5f, 2.0f),
        .yaw = -135.0f,
        .pitch = -20.0f,
        .speed = 5.0f,
        .mouseSensitivity = 0.15f,
    };
    auto mouseCapture = false;

    // Main loop
    auto lastTicks = SDL_GetTicksNS();
    auto quit = false;
    while (!quit) {
        auto nowTicks = SDL_GetTicksNS();
        auto dt = (float) (nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            auto uiCaptured = renderer.debugui()->processEvent(&ev);

            if (ev.type == SDL_EVENT_QUIT) {
                std::println("Quitting");
                quit = true;
            }

            if (uiCaptured) {
                continue;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = true;
            }
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT) {
                mouseCapture = false;
            }
            if (ev.type == SDL_EVENT_MOUSE_MOTION && mouseCapture) {
                cam.handleMouseMotion(ev.motion.xrel, ev.motion.yrel);
            }
        }

        const auto* keys = SDL_GetKeyboardState(nullptr);
        cam.update(keys, dt);

        renderer.render(cam, window);
    }

    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
