#pragma once

#include "retopo/Internal.h"

namespace lila::retopo::detail
{
/** One triangle's corners in its chart of a seamless parametrization. */
using Chart = std::array<glm::dvec2, 3>;

/** What an extraction gives back: a polygon mesh plus, per point, the feature it came from. */
struct Extracted
{
    geo::Mesh mesh;
    std::vector<FeatureKind> feature; // per point
    std::vector<uint8_t> boundary;    // per point: came from a surface boundary
};

/**
 * The quad mesh straight from the integer grid of a seamless parametrization over s (after QEx, Ebke et al. 2013): its
 * points are the whole-number points of the charts (one per triangle of s), its edges trace the integer lines across
 * triangles and cuts, and its faces close round the grid cells. s needs buildTopology and detectFeatures.
 */
LILA_SCENE_API Extracted extractIntegerGrid(const Surface& s, const std::vector<Chart>& chart);
} // namespace lila::retopo::detail
