#pragma once

#include "core/Export.h"

#include <glm/glm.hpp>

#include <array>
#include <utility>
#include <vector>

namespace lila::curve
{
struct CdtResult
{
    /** Counter-clockwise triangles as indices into the points given. */
    std::vector<std::array<int, 3>> triangles;
    /** Segments that could not be kept, such as ones crossing another. */
    int lostSegments = 0;
};

/**
 * Constrained Delaunay triangulation of points with segments (index pairs) kept as edges. With insideOnly, only
 * triangles an odd number of segment crossings in from the outside are kept, so closed loops bound regions and loops
 * inside them are holes; otherwise the whole convex hull is filled. Repeated points share the first one's index.
 */
LILA_SCENE_API CdtResult constrainedDelaunay(const std::vector<glm::dvec2>& points, const std::vector<std::pair<int, int>>& segments, bool insideOnly);
} // namespace lila::curve
