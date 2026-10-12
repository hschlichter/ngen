#pragma once

#include "scenepacktypes.h"

#include <glm/glm.hpp>

#include <functional>
#include <string>
#include <vector>

// A prim's local transform from its xformOpOrder and the op attributes it names, as UsdGeomXformable computes it: the ops in order, the
// last applied first; `!invert!` ops inverted; a leading `!resetXformStack!` sets `resetsXformStack`. An op the order names but nothing
// authors is skipped, and so is an op type USD doesn't define.
//
// The matrix is glm's, column vectors; its elements m[i][j] are USD's GfMatrix4d [i][j] (row vectors), so a translation is m[3].
auto evaluateXformOps(const std::vector<std::string>& order, const std::function<const PackValue*(const std::string&)>& attribute, bool& resetsXformStack)
    -> glm::dmat4;
