#pragma once

#include "retopo/Internal.h"

namespace lila::retopo::detail
{
/**
 * Near-equilateral triangles with edges about target[v] long (per vertex of s, carried along as it is remeshed), for the
 * cross field and the quad cover to work on. s needs buildTopology, buildGeometry and detectFeatures. Its feature edges
 * and borders stay edge chains (their points slide along lines), its corners and pinned points stay where they are, and
 * the rest lies on bvh's surface. Line points slide along creases, or border on a border. The result's hard set holds every feature and border edge. outTarget gets the edge
 * length per result vertex.
 */
LILA_SCENE_API Surface remeshIsotropic(const Surface& s, const std::vector<double>& target, const geo::MeshBvh& bvh, const FeatureLines& creases,
                                       const FeatureLines& border,
                                       std::vector<double>* outTarget = nullptr, int iterations = 6);
} // namespace lila::retopo::detail
