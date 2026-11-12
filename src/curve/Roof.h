#pragma once

#include "core/Export.h"
#include "curve/Planar.h"
#include "geo/Mesh.h"

#include <span>
#include <string>

/** Roofs from building footprints, and insets and centre lines of planar regions, all on the straight skeleton. */
namespace lila::curve
{
struct RoofOptions
{
    /** Slope of every roof plane, degrees from horizontal. */
    double pitch = 30.0;
    /** Positive: the ridge's height above the footprint instead of the pitch, cm. */
    double height = 0.0;
    /** How far the eaves reach past the footprint, cm. */
    double overhang = 0.0;
    /** Footprint edges that end in a vertical gable instead of a hip (edge i runs from point i to i + 1, holes after the outline). */
    std::vector<int> gables;
    /** Also gable every end: an edge between two convex corners that is shorter than both its neighbours (a rectangle's short sides). */
    bool gableEnds = false;
    /** Roof slab thickness, cm; 0 is a single surface. */
    double thickness = 0.0;
    /** cm per UV unit along each roof plane. */
    double uvScale = 100.0;
    glm::dvec3 normal{0.0};
    double tolerance = 0.01;
};

struct RoofResult
{
    geo::Mesh mesh;
    std::vector<std::string> warnings;
};

/**
 * A roof over closed footprint curves (inner loops are courtyards): the straight skeleton lifted at the pitch, a plane per
 * footprint edge, hips and valleys where they meet and ridges where they level off. The footprint's plane is the wall
 * top; the eaves drop below it by the overhang. Faces carry uv0 laid along each plane's slope and "part" (0 roof, 1 gable,
 * 2 underside, 3 fascia).
 */
LILA_SCENE_API RoofResult roof(const Splines& footprint, const RoofOptions& o);

struct SkeletonCurvesResult
{
    Splines curves;
    std::vector<std::string> warnings;
};

/**
 * The region closed curves bound (holes by containment) inset along the straight skeleton: corners stay mitred, parts
 * narrower than twice the distance vanish and loops pinched apart become separate curves. steps above 1 gives that many
 * evenly spaced insets out to distance. Poly curves out.
 */
LILA_SCENE_API SkeletonCurvesResult insetCurves(const Splines& curves, double distance, int steps, const glm::dvec3& normal, double tolerance);

/** The region's centre lines: the straight skeleton's arcs between interior nodes, as open poly curves. */
LILA_SCENE_API SkeletonCurvesResult centerLines(const Splines& curves, const glm::dvec3& normal, double tolerance);

/** Every arc of the region's straight skeleton as a two-point poly curve, each end raised along the normal by its offset time times rise. */
LILA_SCENE_API SkeletonCurvesResult skeletonArcs(const Splines& curves, double rise, const glm::dvec3& normal, double tolerance);

struct RegionInsetOptions
{
    /** Inward distance in the faces' plane, cm. */
    double distance = 1.0;
    /** Push of the inner part along the normal, cm (the band ramps to it). */
    double depth = 0.0;
    /** Faces of a region may bend this many degrees from its plane and still count as flat. */
    double planarAngle = 1.0;
};

struct RegionInsetResult
{
    /** The faces inside the band, and the band itself. */
    std::vector<geo::Index> inner, band;
    std::vector<std::string> warnings;
};

/**
 * Insets each flat, connected region of the faces as one along the straight skeleton: the band follows the region's outline
 * (and holes) with mitred corners, where the inset meets itself the band closes in a ridge, and parts it pinches off
 * become separate inner faces. The region's own faces are replaced; the inside becomes one face per part (quad-filled when
 * the part has holes). UVs carry on from the region's mapping.
 */
LILA_SCENE_API RegionInsetResult insetRegion(geo::Mesh& mesh, std::span<const geo::Index> faces, const RegionInsetOptions& o);
} // namespace lila::curve
