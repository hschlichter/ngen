// ngen-introspect: sees into the running processes (ngen-view, ngen-asset-server, …) found through .ngen-discovery/
// in the working directory, and calls them. See src/introspect/README.md.
//
//   ngen-introspect [--select=TARGET/RECORD] [--frames=N] [--screenshot=PATH]
//       the window; --select shows a record once its process is there, --frames stops after N frames, --screenshot
//       writes the last frame as a PNG
//   ngen-introspect list | get | describe | call …     the command line, for agents and scripts (no window)

#include "introspectcommands.h"
#include "introspectsession.h"
#include "introspectwindow.h"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include <charconv>
#include <expected>
#include <print>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct WindowOptions {
    int frames = 0; // 0 = until the window is closed
    std::string screenshot;
    std::string selectTarget;
    std::string selectRecord;
};

auto parseWindowOptions(const std::vector<std::string>& args) -> std::expected<WindowOptions, std::string> {
    WindowOptions options;
    for (const auto& arg : args) {
        std::string_view text = arg;
        if (text.starts_with("--frames=")) {
            auto value = text.substr(std::string_view("--frames=").size());
            auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), options.frames);
            if (ec != std::errc() || ptr != value.data() + value.size() || options.frames < 1) {
                return std::unexpected("--frames needs a positive number");
            }
        } else if (text.starts_with("--select=")) {
            auto value = text.substr(std::string_view("--select=").size());
            auto slash = value.find('/');
            if (slash == std::string_view::npos || slash == 0 || slash + 1 == value.size()) {
                return std::unexpected("--select needs <target>/<record>, such as view/culling");
            }
            options.selectTarget = std::string(value.substr(0, slash));
            options.selectRecord = std::string(value.substr(slash + 1));
        } else if (text.starts_with("--screenshot=")) {
            options.screenshot = std::string(text.substr(std::string_view("--screenshot=").size()));
        } else {
            return std::unexpected("unknown argument '" + arg + "'");
        }
    }
    return options;
}

// The renderer's current frame as an RGBA PNG.
auto writeScreenshot(SDL_Renderer* renderer, const std::string& path) -> bool {
    auto* surface = SDL_RenderReadPixels(renderer, nullptr);
    if (surface == nullptr) {
        return false;
    }
    auto* rgba = SDL_ConvertSurface(surface, SDL_PIXELFORMAT_ABGR8888);
    SDL_DestroySurface(surface);
    if (rgba == nullptr) {
        return false;
    }
    bool written = stbi_write_png(path.c_str(), rgba->w, rgba->h, 4, rgba->pixels, rgba->pitch) != 0;
    SDL_DestroySurface(rgba);
    return written;
}

auto runWindow(const WindowOptions& options) -> int {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::println(stderr, "ngen-introspect: SDL_Init: {}", SDL_GetError());
        return 1;
    }
    auto* window = SDL_CreateWindow("ngen-introspect", 1280, 800, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::println(stderr, "ngen-introspect: SDL_CreateWindow: {}", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    auto* renderer = SDL_CreateRenderer(window, nullptr);
    if (renderer == nullptr) {
        std::println(stderr, "ngen-introspect: SDL_CreateRenderer: {}", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    auto& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // The layout is fixed; nothing to remember between runs, and no imgui.ini in the working directory.
    io.IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    int status = 0;
    {
        IntrospectSession session;
        IntrospectWindow introspectWindow(session);
        if (!options.selectTarget.empty()) {
            introspectWindow.selectWhenAvailable(options.selectTarget, options.selectRecord);
        }
        bool quit = false;
        int frame = 0;
        while (!quit) {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) {
                    quit = true;
                }
                if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window)) {
                    quit = true;
                }
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            introspectWindow.draw();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 20, 20, 24, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            frame++;
            bool last = options.frames > 0 && frame >= options.frames;
            if (last && !options.screenshot.empty()) {
                if (writeScreenshot(renderer, options.screenshot)) {
                    std::println("Screenshot written: {}", options.screenshot);
                } else {
                    std::println(stderr, "ngen-introspect: cannot write {}: {}", options.screenshot, SDL_GetError());
                    status = 1;
                }
            }
            SDL_RenderPresent(renderer);
            if (last) {
                quit = true;
            }
        }
    }

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return status;
}

} // namespace

auto main(int argc, char** argv) -> int {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && !args[0].starts_with("--")) {
        return runIntrospectCommand(args);
    }
    auto options = parseWindowOptions(args);
    if (!options) {
        std::println(stderr, "ngen-introspect: {}", options.error());
        return 3;
    }
    return runWindow(*options);
}
