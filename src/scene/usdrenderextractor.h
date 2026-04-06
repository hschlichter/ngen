#pragma once

class USDScene;
struct RenderWorld;

class USDRenderExtractor {
public:
    void extract(const USDScene& scene, RenderWorld& out);
};
