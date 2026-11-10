#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "geo/Ops.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <span>
#include <vector>

/**
 * Subdivision levels of a sculpt, as ZBrush keeps them: level 0 is the cage, and each level above is the Catmull-Clark
 * subdivision of the one below plus a detail offset per point, held in the local frame of that subdivided surface so
 * reshaping a lower level carries the finer detail along with it.
 */
namespace lila::sculpt
{
using geo::Index;

/** One level's topology and how it comes from the level below. */
struct LILA_SCENE_API SubdLevel
{
    /** Topology and attributes; its positions are only the ones it was built with. */
    geo::Mesh mesh;
    /** Per point of the level below: the point it became here. Empty on level 0. */
    std::vector<Index> parentPoint;
    /** The transpose of the stencil from the level below, for the least-squares step down. */
    geo::PointStencil down;
    /** Per point: the neighbour that fixes the tangent of its detail frame. */
    std::vector<Index> framePeer;
};

/** The levels of one sculpt. Immutable once built, so sessions share it; adding or deleting levels makes a new one. */
struct LILA_SCENE_API SubdStack
{
    geo::SubdivideOptions opts;
    /** [0] is the cage. */
    std::vector<std::shared_ptr<const SubdLevel>> levels;
    /** stencils[k] takes level k's points to level k + 1's, in the order subdivideStencils gives them. */
    std::vector<geo::PointStencil> stencils;

    int top() const { return int(levels.size()) - 1; }
};

/** A stack from the cage up through opts.levels levels. */
LILA_SCENE_API std::shared_ptr<const SubdStack> buildSubdStack(const geo::Mesh& cage, const geo::SubdivideOptions& opts);
/** s with one more level on top. */
LILA_SCENE_API std::shared_ptr<const SubdStack> addSubdLevel(const SubdStack& s);
/** s cut down to levels 0 to top. */
LILA_SCENE_API std::shared_ptr<const SubdStack> truncateSubdStack(const SubdStack& s, int top);

/** s with level `bottom` as its new level 0, the levels below it gone. */
LILA_SCENE_API std::shared_ptr<const SubdStack> dropSubdLevelsBelow(const SubdStack& s, int bottom);

/**
 * The cage whose Catmull-Clark subdivision has fine's topology, for rebuilding the levels below a mesh subdivided
 * elsewhere. Faces and UVs come from the fine corners at the cage's points. Null when fine isn't a subdivision: not all
 * quads, or no consistent labelling of its points as the cage's points, edge points and face points. coarseOf gets,
 * per fine point, its cage point (kInvalid for edge and face points).
 */
LILA_SCENE_API std::optional<geo::Mesh> reconstructCage(const geo::Mesh& fine, std::vector<Index>& coarseOf);

/**
 * Per point of a, the point of b at the same place: a and b are both one subdivision of the same cage, coarseA and
 * coarseB naming each fine point's cage point (kInvalid for edge and face points) in that cage's ids. Empty when they
 * don't line up.
 */
LILA_SCENE_API std::vector<Index> matchSubdivisions(const geo::Mesh& a, std::span<const Index> coarseA, const geo::Mesh& b,
                                                    std::span<const Index> coarseB);

/** Per point of level `level` (1 or more), its point on the level below, kInvalid for edge and face points. */
LILA_SCENE_API std::vector<Index> coarsePoints(const SubdLevel& level);

/** Per point layers carried between levels: polypaint, mask and freeze. Empty spans when a layer is unused. */
struct LevelLayers
{
    std::vector<glm::vec4> color;
    std::vector<float> mask;
    std::vector<float> freeze;
};

/** What a level above the one being sculpted adds: offsets in the local frame and layer differences. */
struct LevelDetail
{
    std::vector<glm::dvec3> offset;
    LevelLayers layers;
};

using DetailPtr = std::shared_ptr<const LevelDetail>;

/** Positions and layers one level up: subdivided, then the detail added (null detail adds nothing). */
LILA_SCENE_API void stepUp(const SubdStack& s, int to, const DetailPtr& detail, std::vector<glm::dvec3>& positions, LevelLayers& layers);

/**
 * Positions and layers one level down, from level `from`: the least-squares fit of the level below whose subdivision
 * best matches, and the detail that subdividing it back has to add to land exactly on the input.
 */
LILA_SCENE_API DetailPtr stepDown(const SubdStack& s, int from, std::vector<glm::dvec3>& positions, LevelLayers& layers);
} // namespace lila::sculpt
