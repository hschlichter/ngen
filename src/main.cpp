#include "camera.h"
#include "debugdraw.h"
#include "material.h"
#include "mesh.h"
#include "renderer.h"
#include "renderworld.h"
#include "rhidebugui.h"
#include "rhidevicevulkan.h"
#include "sceneloader.h"
#include "scenequery.h"
#include "types.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

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
            .worldBounds = meshLib.bounds(meshHandle).transformed(md.transform),
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
    bool isUsd = filepath.ends_with(".usda") || filepath.ends_with(".usd") || filepath.ends_with(".usdc") || filepath.ends_with(".usdz");

    USDScene usdScene;
    USDRenderExtractor usdExtractor;
    SceneQuerySystem sceneQuery;
    MeshLibrary meshLib;
    MaterialLibrary matLib;
    RenderWorld renderWorld;
    std::string pickedPrimPath;
    AABB pickedBounds;

    if (isUsd) {
        if (!usdScene.open(argv[1])) {
            std::println(stderr, "Failed to open USD scene");
            return 1;
        }
        usdScene.updateAssetBindings(meshLib, matLib);
        usdExtractor.extract(usdScene, meshLib, renderWorld);
        sceneQuery.rebuild(usdScene, meshLib);
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
    renderer.debugui()->setDrawCallback([&] {
        if (!usdScene.isOpen()) {
            return;
        }

        if (ImGui::Begin("USD Scene")) {
            // Layer stack
            if (ImGui::CollapsingHeader("Layers", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (const auto& layer : usdScene.layers()) {
                    auto isCurrent = (layer.handle == usdScene.currentEditTarget());
                    auto isSession = (layer.handle == usdScene.sessionLayer());

                    ImGui::PushID(layer.handle.index);
                    if (ImGui::Selectable(layer.displayName.c_str(), isCurrent)) {
                        usdScene.setEditTarget(layer.handle);
                    }
                    ImGui::SameLine();
                    if (layer.dirty) {
                        ImGui::TextColored({1, 0.7f, 0, 1}, "(dirty)");
                    }
                    if (isSession) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[session]");
                    }
                    ImGui::PopID();
                }

                if (ImGui::Button("Clear Session")) {
                    usdScene.clearSessionLayer();
                }
                ImGui::SameLine();
                if (ImGui::Button("Save All")) {
                    usdScene.saveAllDirty();
                }
            }

            // Stats
            ImGui::Text("Mesh instances: %zu", renderWorld.meshInstances.size());

            // Picked prim
            if (!pickedPrimPath.empty()) {
                ImGui::Separator();
                ImGui::Text("Picked: %s", pickedPrimPath.c_str());
            }

            // Prim list
            if (ImGui::CollapsingHeader("Prims")) {
                for (const auto& prim : usdScene.allPrims()) {
                    const char* tag = "";
                    if (prim.flags & PrimFlagRenderable) {
                        tag = " [mesh]";
                    } else if (prim.flags & PrimFlagLight) {
                        tag = " [light]";
                    } else if (prim.flags & PrimFlagXformable) {
                        tag = " [xform]";
                    }

                    ImGui::Text("%s%s", prim.name.c_str(), tag);
                }
            }
        }
        ImGui::End();
    });

    DebugDraw debugDraw;

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

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_LEFT && usdScene.isOpen()) {
                int winW = 0, winH = 0;
                SDL_GetWindowSizeInPixels(window, &winW, &winH);
                float mx = ev.button.x;
                float my = ev.button.y;

                float ndcX = (2.0f * mx / (float) winW) - 1.0f;
                float ndcY = (2.0f * my / (float) winH) - 1.0f;

                auto aspect = (float) winW / (float) winH;
                auto proj = glm::perspective(glm::radians(45.0f), aspect, 0.1f, 3000.0f);
                proj[1][1] *= -1.0f;
                auto invVP = glm::inverse(proj * cam.viewMatrix());

                auto nearPt = invVP * glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
                auto farPt = invVP * glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
                nearPt /= nearPt.w;
                farPt /= farPt.w;

                Ray ray = {
                    .origin = glm::vec3(nearPt),
                    .direction = glm::normalize(glm::vec3(farPt - nearPt)),
                };

                RaycastHit hit;
                if (sceneQuery.raycast(ray, 3000.0f, hit)) {
                    auto* rec = usdScene.getPrimRecord(hit.prim);
                    pickedPrimPath = rec ? rec->path : "";
                    auto* bc = sceneQuery.bounds().get(hit.prim);
                    pickedBounds = bc ? bc->worldBounds : AABB{};
                } else {
                    pickedPrimPath.clear();
                    pickedBounds = {};
                }
            }
        }

        const auto* keys = SDL_GetKeyboardState(nullptr);
        cam.update(keys, dt);

        debugDraw.newFrame();
        for (const auto& inst : renderWorld.meshInstances) {
            if (inst.worldBounds.valid()) {
                debugDraw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
            }
        }
        if (pickedBounds.valid()) {
            debugDraw.box(pickedBounds, {1.0f, 0.0f, 0.6f, 1.0f});
        }

        renderer.render(cam, window, debugDraw.data());
    }

    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
