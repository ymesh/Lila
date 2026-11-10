#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <vector>

/**
 * Quad patches: fills a closed boundary loop with an all-quad grid, following the pattern-based quadrangulation of
 * N-sided patches (Takayama, Panozzo and Sorkine-Hornung 2014) for 2 to 6 sides.
 */
namespace lila::retopo
{
using geo::Index;

/** A closed loop of boundary points split into sides at its corners. */
struct PatchBoundary
{
    /** In order around the loop, not repeated at the end. */
    std::vector<glm::dvec3> points;
    /** Indices into points where one side ends and the next begins, ascending. Two to six of them. */
    std::vector<int> corners;
};

struct PatchOptions
{
    /** Concentric loops of quads kept along the whole boundary before the pattern starts; negative picks the fewest that fit. */
    int padding = -1;
    /** Which of the valid patterns and parameter choices to use, wrapping past the last; see PatchResult::variants. */
    int variant = 0;
    /**
     * Where the patch should put its irregular points, like a drag inside it: the variant whose poles lie nearest this
     * point wins (overriding variant), and the interior leans toward it.
     */
    std::optional<glm::dvec3> pull;
    /**
     * For a cap: the direction the patch bulges along. The interior is carried along it onto the surface (a fingertip's
     * dome) before relaxing, rather than to the nearest surface point, which on a tube is its side.
     */
    std::optional<glm::dvec3> lift;
    /** Tangential relax passes on the surface once the interior is placed. */
    int relaxIterations = 10;
};

struct PatchResult
{
    /** The first points.size() entries are the boundary points, unchanged and in order; interior points follow. */
    std::vector<glm::dvec3> positions;
    /** Quads, wound the same way as the boundary loop runs. */
    std::vector<std::vector<Index>> faces;
    /** Irregular interior points (valence other than 4), as indices into positions. */
    std::vector<Index> poles;
    /** How many distinct variants this boundary has, for cycling. */
    int variants = 0;
    /** The variant used. */
    int variant = 0;
    std::string error;
    bool ok() const { return error.empty(); }
};

/**
 * Sides' edge counts, from the boundary's corners. An all-quad fill needs the total to be even; patchSidesFillable says
 * whether the pattern set can fill a given set of counts at all.
 */
LILA_SCENE_API std::vector<int> patchSideCounts(const PatchBoundary& boundary);
LILA_SCENE_API bool patchSidesFillable(const std::vector<int>& sideCounts);

/**
 * Fills the boundary with quads. The interior is placed by a harmonic solve against the fixed boundary, then pulled onto
 * the surface and relaxed on it when surface is given (its mesh in the same space as the points). Boundaries of more
 * than six sides, or counts no pattern fits, come back with an error.
 */
LILA_SCENE_API PatchResult fillPatch(const PatchBoundary& boundary, const PatchOptions& opts = {}, const geo::MeshBvh* surface = nullptr);

/**
 * Corners for a closed loop: points where it turns sharper than minTurnDegrees, the sharpest first, two to six of them.
 * With fewer sharp turns than wanted it picks points spread evenly by arc length, so the result always has `wanted`
 * corners when the loop has that many points.
 */
LILA_SCENE_API std::vector<int> patchCorners(const std::vector<glm::dvec3>& loop, int wanted = 4, double minTurnDegrees = 35.0);
} // namespace lila::retopo
