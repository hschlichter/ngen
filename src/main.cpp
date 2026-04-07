#include "camera.h"
#include "debugdraw.h"
#include "material.h"
#include "mesh.h"
#include "renderer.h"
#include "renderworld.h"
#include "rhidebugui.h"
#include "rhidevicevulkan.h"
#include "scenequery.h"
#include "usdrenderextractor.h"
#include "usdscene.h"

#include <imgui.h>

#include <SDL3/SDL.h>

#include <functional>
#include <print>
#include <unordered_set>

auto main(int argc, char* argv[]) -> int {
    USDScene usdScene;
    USDRenderExtractor usdExtractor;
    SceneQuerySystem sceneQuery;
    MeshLibrary meshLib;
    MaterialLibrary matLib;
    RenderWorld renderWorld;
    PrimHandle selectedPrim;

    if (argc >= 2) {
        if (!usdScene.open(argv[1])) {
            std::println(stderr, "Failed to open USD scene: {}", argv[1]);
            return 1;
        }
        usdScene.updateAssetBindings(meshLib, matLib);
        usdExtractor.extract(usdScene, meshLib, renderWorld);
        sceneQuery.rebuild(usdScene, meshLib);
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
    auto sceneChanged = false;
    auto showSceneWindow = false;
    auto showAABBs = false;
    auto showSelectedAABB = true;
    auto requestQuit = false;
    std::string pendingOpenPath;

    auto openScene = [&](const char* path) {
        if (usdScene.isOpen()) {
            usdScene.close();
        }
        meshLib = {};
        matLib = {};
        renderWorld.clear();
        selectedPrim = {};

        if (!usdScene.open(path)) {
            std::println(stderr, "Failed to open: {}", path);
            return;
        }
        usdScene.updateAssetBindings(meshLib, matLib);
        usdExtractor.extract(usdScene, meshLib, renderWorld);
        sceneQuery.rebuild(usdScene, meshLib);
        renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
        showSceneWindow = true;
    };
    std::unordered_set<uint32_t> selectedAncestors;

    std::function<void(PrimHandle)> drawSceneNode;
    drawSceneNode = [&](PrimHandle h) {
        const auto* rec = usdScene.getPrimRecord(h);
        if (!rec) {
            return;
        }

        bool hasChildren = static_cast<bool>(usdScene.firstChild(h));
        bool isSelected = (h == selectedPrim);

        ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;
        if (isSelected) {
            flags |= ImGuiTreeNodeFlags_Selected;
        }
        if (!hasChildren) {
            flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
        }

        if (selectedAncestors.contains(h.index)) {
            ImGui::SetNextItemOpen(true);
        }

        const char* tag = "";
        if (rec->flags & PrimFlagRenderable) {
            tag = " [mesh]";
        } else if (rec->flags & PrimFlagLight) {
            tag = " [light]";
        }

        ImGui::PushID(h.index);
        bool open = ImGui::TreeNodeEx(rec->name.c_str(), flags, "%s%s", rec->name.c_str(), tag);

        if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
            selectedPrim = isSelected ? PrimHandle{} : h;
        }

        if (open && hasChildren) {
            auto child = usdScene.firstChild(h);
            while (child) {
                drawSceneNode(child);
                child = usdScene.nextSibling(child);
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    };

    renderer.debugui()->setDrawCallback([&] {
        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open...")) {
                    static const SDL_DialogFileFilter filters[] = {
                        {"USD Files", "usd;usda;usdc;usdz"},
                        {"All Files", "*"},
                    };
                    SDL_ShowOpenFileDialog(
                        [](void* userdata, const char* const* filelist, int) {
                            if (filelist && filelist[0]) {
                                *static_cast<std::string*>(userdata) = filelist[0];
                            }
                        },
                        &pendingOpenPath,
                        window,
                        filters,
                        2,
                        nullptr,
                        false);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit")) {
                    requestQuit = true;
                }
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Windows")) {
                ImGui::MenuItem("Scene", nullptr, &showSceneWindow, usdScene.isOpen());
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Debug")) {
                ImGui::MenuItem("Show AABBs", nullptr, &showAABBs);
                ImGui::MenuItem("Show Selected AABB", nullptr, &showSelectedAABB);
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        if (!showSceneWindow || !usdScene.isOpen()) {
            return;
        }

        if (ImGui::Begin("USD Scene", &showSceneWindow)) {
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
                    if (layer.readOnly) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("[read-only]");
                    }
                    ImGui::PopID();
                }

                ImGui::Spacing();
                static char newLayerPath[256] = "new_layer.usda";
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Add").x - ImGui::GetStyle().ItemSpacing.x -
                                        ImGui::GetStyle().FramePadding.x * 2);
                ImGui::InputText("##newlayer", newLayerPath, sizeof(newLayerPath));
                ImGui::SameLine();
                if (ImGui::Button("Add")) {
                    auto h = usdScene.addSubLayer(newLayerPath);
                    if (h) {
                        usdScene.setEditTarget(h);
                        sceneChanged = true;
                    }
                }

                float buttonWidth = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
                if (ImGui::Button("Save All", {buttonWidth, 0})) {
                    usdScene.saveAllDirty();
                }
                ImGui::SameLine();
                if (ImGui::Button("Clear Session", {buttonWidth, 0})) {
                    usdScene.clearSessionLayer();
                }
            }

            // Stats
            ImGui::Text("Mesh instances: %zu", renderWorld.meshInstances.size());

            // Selected prim info
            if (selectedPrim) {
                const auto* rec = usdScene.getPrimRecord(selectedPrim);
                if (rec) {
                    ImGui::Separator();
                    ImGui::Text("Selected: %s", rec->path.c_str());
                }
            }

            // Build ancestor set for auto-expanding to selected prim
            selectedAncestors.clear();
            if (selectedPrim) {
                auto cur = selectedPrim;
                while (cur) {
                    const auto* r = usdScene.getPrimRecord(cur);
                    if (!r || !r->parent) {
                        break;
                    }
                    cur = r->parent;
                    selectedAncestors.insert(cur.index);
                }
            }

            // Scene graph
            if (ImGui::CollapsingHeader("Scene Graph", ImGuiTreeNodeFlags_DefaultOpen)) {
                for (const auto& prim : usdScene.allPrims()) {
                    if (!prim.parent) {
                        drawSceneNode(prim.handle);
                    }
                }
            }

            // Properties
            if (selectedPrim) {
                const auto* rec = usdScene.getPrimRecord(selectedPrim);
                if (rec && ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
                    // Prim info
                    if (ImGui::TreeNodeEx("Prim Info", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Text("Path: %s", rec->path.c_str());
                        ImGui::Text("Name: %s", rec->name.c_str());

                        std::string typeStr;
                        if (rec->flags & PrimFlagRenderable) {
                            typeStr += "Mesh ";
                        }
                        if (rec->flags & PrimFlagLight) {
                            typeStr += "Light ";
                        }
                        if (rec->flags & PrimFlagCamera) {
                            typeStr += "Camera ";
                        }
                        if (rec->flags & PrimFlagXformable) {
                            typeStr += "Xform ";
                        }
                        if (typeStr.empty()) {
                            typeStr = "None";
                        }
                        ImGui::Text("Flags: %s", typeStr.c_str());
                        ImGui::Text("Active: %s", rec->active ? "yes" : "no");
                        ImGui::Text("Loaded: %s", rec->loaded ? "yes" : "no");
                        ImGui::TreePop();
                    }

                    // Transform
                    const auto* xf = usdScene.getTransform(selectedPrim);
                    if (xf && ImGui::TreeNodeEx("Transform", ImGuiTreeNodeFlags_DefaultOpen)) {
                        // World (read-only)
                        auto worldPos = glm::vec3(xf->world[3]);
                        ImGui::BeginDisabled();
                        ImGui::DragFloat3("World Pos", &worldPos.x);
                        ImGui::EndDisabled();

                        ImGui::Separator();

                        // Local (editable)
                        auto local = xf->local;
                        bool changed = false;
                        changed |= ImGui::DragFloat3("Position", &local.position.x, 0.1f);
                        auto euler = glm::degrees(glm::eulerAngles(local.rotation));
                        if (ImGui::DragFloat3("Rotation", &euler.x, 0.5f)) {
                            local.rotation = glm::quat(glm::radians(euler));
                            changed = true;
                        }
                        changed |= ImGui::DragFloat3("Scale", &local.scale.x, 0.01f);
                        if (changed) {
                            usdScene.setTransform(selectedPrim, local);
                            sceneChanged = true;
                        }
                        ImGui::TreePop();
                    }

                    // Visibility
                    if (ImGui::TreeNodeEx("Visibility", ImGuiTreeNodeFlags_DefaultOpen)) {
                        bool visible = rec->visible;
                        if (ImGui::Checkbox("Visible", &visible)) {
                            usdScene.setVisibility(selectedPrim, visible);
                            sceneChanged = true;
                        }
                        ImGui::TreePop();
                    }

                    // Bounds
                    const auto* bc = sceneQuery.bounds().get(selectedPrim);
                    if (bc && bc->valid && ImGui::TreeNodeEx("Bounds", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::Text("Local min: %.2f, %.2f, %.2f", bc->localBounds.min.x, bc->localBounds.min.y, bc->localBounds.min.z);
                        ImGui::Text("Local max: %.2f, %.2f, %.2f", bc->localBounds.max.x, bc->localBounds.max.y, bc->localBounds.max.z);
                        ImGui::Text("World min: %.2f, %.2f, %.2f", bc->worldBounds.min.x, bc->worldBounds.min.y, bc->worldBounds.min.z);
                        ImGui::Text("World max: %.2f, %.2f, %.2f", bc->worldBounds.max.x, bc->worldBounds.max.y, bc->worldBounds.max.z);
                        ImGui::TreePop();
                    }

                    // Material
                    const auto* binding = usdScene.getAssetBinding(selectedPrim);
                    if (binding && binding->material) {
                        const auto* mat = matLib.get(binding->material);
                        if (mat && ImGui::TreeNodeEx("Material", ImGuiTreeNodeFlags_DefaultOpen)) {
                            ImGui::ColorEdit4("Base Color", (float*) &mat->baseColorFactor, ImGuiColorEditFlags_NoInputs);
                            if (mat->texWidth > 0) {
                                ImGui::Text("Texture: %dx%d", mat->texWidth, mat->texHeight);
                            } else {
                                ImGui::TextDisabled("No texture");
                            }
                            ImGui::TreePop();
                        }
                    }
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
    while (!quit && !requestQuit) {
        auto nowTicks = SDL_GetTicksNS();
        auto dt = (float) (nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;

        if (!pendingOpenPath.empty()) {
            openScene(pendingOpenPath.c_str());
            pendingOpenPath.clear();
        }

        if (usdScene.isOpen()) {
            usdScene.endFrame();
        }

        if (sceneChanged && usdScene.isOpen()) {
            usdExtractor.extract(usdScene, meshLib, renderWorld);
            renderer.uploadRenderWorld(renderWorld, meshLib, matLib);
            sceneQuery.rebuild(usdScene, meshLib);
            sceneChanged = false;
        }

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
                    selectedPrim = hit.prim;
                } else {
                    selectedPrim = {};
                }
            }
        }

        const auto* keys = SDL_GetKeyboardState(nullptr);
        cam.update(keys, dt);

        debugDraw.newFrame();
        if (showAABBs) {
            for (const auto& inst : renderWorld.meshInstances) {
                if (inst.worldBounds.valid()) {
                    debugDraw.box(inst.worldBounds, {0.0f, 1.0f, 0.0f, 1.0f});
                }
            }
        }
        if (selectedPrim && showSelectedAABB) {
            std::function<void(PrimHandle)> highlightSubtree = [&](PrimHandle h) {
                const auto* bc = sceneQuery.bounds().get(h);
                if (bc && bc->worldBounds.valid()) {
                    debugDraw.box(bc->worldBounds, {1.0f, 0.0f, 0.6f, 1.0f});
                }
                auto child = usdScene.firstChild(h);
                while (child) {
                    highlightSubtree(child);
                    child = usdScene.nextSibling(child);
                }
            };
            highlightSubtree(selectedPrim);
        }

        renderer.render(cam, window, debugDraw.data());
    }

    renderer.destroy();
    rhiDevice.destroy();
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
