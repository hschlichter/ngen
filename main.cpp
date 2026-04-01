#include "types.h"
#include "sceneloader.h"
#include "camera.h"
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
#include "renderervulkan.h"
#endif

#include <SDL3/SDL.h>

#include <cstdio>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <model.gltf>\n", argv[0]);
        return 1;
    }

    Scene scene = loadGltf(argv[1]);
    if (scene.meshes.empty()) {
        fprintf(stderr, "Failed to load model\n");
        return 1;
    }

    // SDL init and window
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_PropertiesID windowProps = SDL_CreateProperties();
    SDL_SetStringProperty(windowProps, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "ngen");
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 1920);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 1080);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_VULKAN_BOOLEAN, 1);
    SDL_SetNumberProperty(windowProps, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, 1);
    SDL_Window* window = SDL_CreateWindowWithProperties(windowProps);
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindowWithProperties failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_DestroyProperties(windowProps);

    // Renderer
#if defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
    Renderer* renderer = new RendererVulkan();
#else
    #error "No renderer backend for this platform"
#endif
    if (renderer->init(window)) return 1;
    renderer->uploadScene(scene);

    // Camera
    Camera cam = {
        .position = glm::vec3(2.0f, 1.5f, 2.0f),
        .yaw = -135.0f,
        .pitch = -20.0f,
        .speed = 1.0f,
        .mouseSensitivity = 0.15f,
    };
    bool mouseCapture = false;

    // Main loop
    uint64_t lastTicks = SDL_GetTicksNS();
    bool quit = false;
    while (!quit) {
        uint64_t nowTicks = SDL_GetTicksNS();
        float dt = (float)(nowTicks - lastTicks) / 1.0e9f;
        lastTicks = nowTicks;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                printf("Quitting\n");
                quit = true;
            }

            if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && ev.button.button == SDL_BUTTON_RIGHT)
                mouseCapture = true;
            if (ev.type == SDL_EVENT_MOUSE_BUTTON_UP && ev.button.button == SDL_BUTTON_RIGHT)
                mouseCapture = false;

            if (ev.type == SDL_EVENT_MOUSE_MOTION && mouseCapture) {
                cam.handleMouseMotion(ev.motion.xrel, ev.motion.yrel);
            }
        }

        const bool* keys = SDL_GetKeyboardState(NULL);
        cam.update(keys, dt);
        renderer->render(cam, window);
    }

    renderer->destroy();
    delete renderer;
    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}
