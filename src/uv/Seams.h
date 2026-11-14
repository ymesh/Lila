#pragma once

#include "core/Export.h"

#include "uv/Chart.h"

#include <optional>
#include <span>
#include <vector>

namespace lila::uv
{
enum class SeamMethod : uint8_t
{
    Split,   // hard edges, cut each island open into a disk, split where it stretches, merge back where it does not
    OptCuts, // joint seam and distortion optimisation (Li et al. 2018): grow seams where they relieve the most stretch, zip back the rest
};

/**
 * A place seams hide from, such as where a first person camera sits or a character's face is seen from. A plane sees along
 * its normal instead, like an orthographic camera, onto whichever side the surface is on.
 */
struct SeamView
{
    glm::dvec3 position{0.0}; // object space
    /** How hard seams avoid what it sees; negative draws them towards it instead. */
    double weight = 1.0;
    /** Set for a plane through position: object space, unit length. */
    std::optional<glm::dvec3> planeNormal;
};

struct AutoSeamOptions
{
    SeamMethod method = SeamMethod::Split;
    /** Edges sharper than this (degrees) are always seams; islands never merge across them. */
    double hardAngle = 60.0;
    /** Edges marked "hard" are always seams too. */
    bool useHardEdges = false;
    /** Existing seams stay. */
    bool keepSeams = false;
    /** Islands whose flattened distortion (symmetric Dirichlet excess) is above this are split. */
    double maxDistortion = 0.08;
    /** Islands this small are never split for distortion. */
    int minSplitFaces = 8;
    /**
     * An island that can't flatten first tries darts: short cuts in from the points its surface caps over (a fingertip,
     * the tip of a nose), found as the singular points of its flow. It splits in two only when darts don't do. Shorter
     * seams and fewer islands, but notched islands pack a little looser.
     */
    bool darts = false;
    /** Prefer cuts where they are least visible: occluded, concave and downward facing. */
    bool visibility = true;
    /** Merge neighbouring islands back together while the result stays below maxDistortion. */
    bool merge = true;
    /** OptCuts: symmetric Dirichlet excess over all the islands together to get under. */
    double distortionBound = 0.05;
    /** OptCuts: price of seam length against distortion; higher keeps seams shorter and more hidden. */
    double seamWeight = 0.5;
    /** Edges these see, facing them and unoccluded, cost more to cut. */
    std::vector<SeamView> views;
    /** Per edge, its mirror image (kInvalid for none): each pair costs the same, so both halves of a symmetric mesh cut alike. */
    std::vector<Index> mirrorEdge;
};

struct AutoSeamResult
{
    /** Final islands as face lists, with the uvs of a quick flatten in charts. */
    std::vector<Chart> charts;
    Index seamEdges = 0;
};

/**
 * Seams for game assets: hard and sharp edges first, then each island is cut open into a disk along the
 * cheapest path (short, along creases, out of sight) and split where it cannot flatten without stretch.
 * Writes "seam" on the faces' edges (all live faces when empty).
 */
LILA_SCENE_API AutoSeamResult autoSeams(geo::Mesh& mesh, std::span<const Index> faces = {}, const AutoSeamOptions& opts = {});

/**
 * Adds the fewest seams that turn each island of the faces (from current seams) into a disk: closed parts
 * are split in two, tubes and holed islands are cut along the cheapest path. Returns the seams added.
 */
LILA_SCENE_API Index cutToDisks(geo::Mesh& mesh, std::span<const Index> faces = {});

/** Per edge capacity: the price of cutting each edge of the faces (its length, less where hidden, creased or concave). */
LILA_SCENE_API std::vector<double> seamCosts(const geo::Mesh& mesh, std::span<const Index> faces, bool visibility = true,
                                             std::span<const SeamView> views = {});

/** Per point capacity: how open each point is to the sky, 0 (enclosed) to 1. Ray traced, parallel. */
LILA_SCENE_API std::vector<float> pointVisibility(const geo::Mesh& mesh, int rays = 16);
} // namespace lila::uv
