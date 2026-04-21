#pragma once

#include "mesh.h"

// Tessellation helpers for the USD shape schemas (UsdGeomCube/Sphere/Cylinder/Cone).
// The renderer only knows how to draw indexed triangle meshes; USD shape schemas are
// procedural. The extractor calls these on first encounter of such a prim to produce a
// MeshDesc that flows through MeshLibrary exactly like an authored UsdGeomMesh would.
//
// `axis` for cylinder/cone is a USD token — "X", "Y", or "Z" — matching the schema
// attribute. Values outside that set fall back to "Z".

struct ShapeColor {
    float r = 1.0f, g = 1.0f, b = 1.0f;
};

MeshDesc tessellateCube(double size, ShapeColor color = {});
MeshDesc tessellateSphere(double radius, ShapeColor color = {}, int latitudeSegments = 16, int longitudeSegments = 32);
MeshDesc tessellateCylinder(double radius, double height, const char* axis, ShapeColor color = {});
MeshDesc tessellateCone(double radius, double height, const char* axis, ShapeColor color = {});
