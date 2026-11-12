#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <span>
#include <vector>

namespace lila::geo
{
/** Faces crossed by edge e's ring, in ring order: the quads on either side of e and on through the ring. */
LILA_SCENE_API std::vector<Index> faceLoop(const Mesh& mesh, Index e);

/**
 * The components from a to b inclusive, in order: the shorter way along a loop the two share (for edges, else a ring),
 * else the shortest path across the surface (by length for points and edges, by centroid distance for faces). Empty
 * when they are not connected.
 */
LILA_SCENE_API std::vector<Index> pointPathBetween(const Mesh& mesh, Index a, Index b);
LILA_SCENE_API std::vector<Index> edgePathBetween(const Mesh& mesh, Index a, Index b);
LILA_SCENE_API std::vector<Index> facePathBetween(const Mesh& mesh, Index a, Index b);

/**
 * Faces enclosed by the given edges, flood filled without crossing them (mesh boundaries are walls too). Per connected
 * piece of the mesh the edges split into regions: smaller keeps every region but the largest by area, otherwise only
 * the largest is kept. A piece the edges do not split contributes nothing. Sorted.
 */
LILA_SCENE_API std::vector<Index> containedFaces(const Mesh& mesh, std::span<const Index> edges, bool smaller = true);
} // namespace lila::geo
