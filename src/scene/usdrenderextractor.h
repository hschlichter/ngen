#pragma once

class USDScene;
class MeshLibrary;
struct RenderWorld;

class USDRenderExtractor {
public:
    void extract(const USDScene& scene, const MeshLibrary& meshLib, RenderWorld& out);
};
