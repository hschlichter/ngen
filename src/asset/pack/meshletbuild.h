#pragma once

#include "scenepacktypes.h"

// What the mesh packers do to a mesh after building it, with meshoptimizer: weld its vertices, then split each submesh into meshlets. Both
// are deterministic, so a repack gives the same bytes.

// Merges bitwise-identical vertices and renumbers the indices; index order and submesh ranges stay as they are.
auto weldMesh(MeshPack& mesh) -> void;

// Orders each submesh's triangles for the vertex cache and splits it into meshlets (at most meshletMaxVertices vertices and
// meshletMaxTriangles triangles) with a bounding sphere and normal cone each; fills the submeshes' meshlet ranges.
auto buildMeshlets(MeshPack& mesh) -> void;
