#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "layout/Resolve.h"

#include <string>
#include <vector>

namespace lila::layout
{
struct LinesResult
{
    geo::Mesh mesh;
    std::vector<std::string> warnings;
    /** Edges Geometry output cut (before shaping), for tests and selection. */
    size_t cutEdges = 0;
};

/**
 * Renders the layout's Geometry and Sculpt lines into a copy of the mesh it was resolved on. Geometry lines first cut
 * edges along the profile's bends (beside the line at each break), so the groove keeps its shape on any density; then
 * every point within a groove moves in along its normal by the profile's depth. Sculpt lines only move points, for
 * meshes dense enough to show them.
 */
LILA_SCENE_API LinesResult applyLines(const Resolved& resolved);

/**
 * Cuts edges into `mesh` along a line beside the run, `offset` to its left (negative: right). Each end runs `reach` on
 * and then until it leaves its face, since a cut stopping inside a face is dropped; past the line it lies flat on the
 * surface. `original` is the mesh the layout was resolved on. Returns the edges made.
 */
LILA_SCENE_API size_t cutRun(geo::Mesh& mesh, const geo::Mesh& original, const geo::MeshBvh& originalBvh, const ResolvedRun& run, double offset,
                             double reach);

/** Moves every point of the mesh in along its normal by the depth of the grooves the masked outputs make there. */
LILA_SCENE_API void displace(const Resolved& resolved, geo::Mesh& mesh, uint32_t outputMask);

/** Material slot decal strips use; LilaLink fills it with M_LilaPanelLine. */
inline constexpr const char* kDecalMaterial = "LilaPanelLine";

/**
 * Strips along every Decal line: its width, lifted just off the surface, U along the line in widths and V across, on the
 * LilaPanelLine material slot, which LilaLink gives M_LilaPanelLine once that is built.
 */
LILA_SCENE_API geo::Mesh decalStrips(const Resolved& resolved);

/** How deep a point sits in the grooves the masked outputs make: 0 outside, else the depth to move it in. */
LILA_SCENE_API double grooveDepth(const Resolved& resolved, const glm::dvec3& p, uint32_t outputMask);
} // namespace lila::layout
