#pragma once

struct Scene;
struct Camera;
struct SDL_Window;

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    virtual ~Renderer() = default;

    virtual int init(SDL_Window* window) = 0;
    virtual void uploadScene(const Scene& scene) = 0;
    virtual void render(const Camera& camera, SDL_Window* window) = 0;
    virtual void destroy() = 0;
};
