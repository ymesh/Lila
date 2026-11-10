#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lila::geo
{
class SymmetryMap;

/** How each column of the bridge runs from the source border to the target border. */
enum class BridgeCurve : uint8_t
{
    /** Straight lines between paired points. */
    Linear,
    /** A cubic leaving each border square to it in the plane of its faces (Maya Blend, Blender Blend Surface). */
    Blend,
    /** The section carried along BridgeOptions::path from one border's centre to the other's (Maya Curve). */
    Path,
    /**
     * Fits the faces behind each border: a straight line or circular arc when the edge flow on both sides lies on one,
     * else a curve matching their slope and bend. A deleted strip of a cylinder, sphere or torus comes back exactly.
     */
    Auto,
};

/** How several selected loops connect (Blender's Connect Loops). */
enum class BridgeConnect : uint8_t
{
    /** Each loop with its nearest facing partner. */
    Pairs,
    /** One run through every loop in order of distance, like a tube through rings. */
    Chain,
    /** Chain, with the last loop joined back to the first. */
    ChainClosed,
};

/** Falloff of the profile bulge across the bridge (Blender's Profile Shape). */
enum class BridgeProfile : uint8_t
{
    Smooth,
    Sphere,
    Root,
    Sharp,
    Linear,
    InverseSquare,
    Constant,
};

/** How a bridge between borders of different edge counts makes up the difference. */
enum class BridgeTransition : uint8_t
{
    /** 3-to-1 quad patterns spread over the rows; one triangle where the difference is odd. */
    Quads,
    /** Triangle fans in the middle row. */
    Triangles,
};

struct BridgeOptions
{
    /** Extra rows of faces between the borders; -1 picks them from the gap, the edges around it and the open ends. */
    int divisions = 0;
    BridgeCurve curve = BridgeCurve::Linear;
    /** 0 straightens every curve type to Linear, 1 is the full curve. */
    double smoothness = 1.0;
    /** Handle length leaving the source and entering the target, as a multiple of the gap (Blend and Auto). */
    double tensionSource = 1.0;
    double tensionTarget = 1.0;
    /** Steps the target's pairing round a closed loop (Maya Bridge Offset, Blender Twist). */
    int offset = 0;
    /** Reads that side the other way round, crossing the bridge (Maya Direction). */
    bool reverseSource = false;
    bool reverseTarget = false;
    /** Turns the middle rows about the bridge's axis, growing from 0 at the source to this at the target, degrees. */
    double twist = 0.0;
    /** Scale of the middle rows about their centre: 1 leaves them, 0.5 pinches to half. */
    double taper = 1.0;
    /** Where across the bridge the taper is strongest, 0 at the source, 1 at the target. */
    double bias = 0.5;
    /** Optional ramp of taper strength across the bridge, values evenly spaced from source to target. */
    std::vector<double> taperCurve;
    /** Bulge (positive) or pinch (negative) of the middle rows, as a fraction of the gap (Blender Profile Factor). */
    double profileFactor = 0.0;
    BridgeProfile profileShape = BridgeProfile::Smooth;
    /** The curve Path follows, in the mesh's space; its ends map onto the two borders' centres. */
    std::vector<glm::dvec3> path;
    BridgeTransition transition = BridgeTransition::Quads;
    /**
     * Open bridges: rows meeting the border that joins the two sides' ends land on its points and weld; rows with no
     * point split that border. Off leaves the end rows wherever the curve puts them.
     */
    bool weldEnds = true;
    /** How far a row may be from a border point and still weld to it, as a fraction of the row spacing. */
    double weldTolerance = 0.35;
    /** Smoothing passes over the new inner points. */
    int relax = 0;
    /** New edges sharper than this are hard; negative leaves edge hardness alone. */
    double smoothingAngle = -1.0;
    /** Collapses the two borders into one at mergeFactor instead of bridging (0 onto the source, 1 onto the target). */
    bool merge = false;
    double mergeFactor = 0.5;
    BridgeConnect connect = BridgeConnect::Pairs;
    /** Mirror symmetry: a border on one side never pairs with one on the other, so each half bridges on its own. */
    const SymmetryMap* symmetry = nullptr;
    /** Points that must pair: the source's and the target's start, then any further (source, target) pairs. */
    std::vector<std::pair<Index, Index>> pairs;
};

struct BridgeResult
{
    std::vector<Index> faces;
    /** Rows of faces the bridges ended up with (the most of any pair). */
    int rows = 0;
    /** Bridge rows that landed on existing border points at the open ends. */
    int welded = 0;
    /** Rows that split an end border because no point was near. */
    int split = 0;
    /** End border points no row reached; they stay as extra corners of the end faces. */
    int unwelded = 0;
    int triangles = 0;
    int pairsBridged = 0;
    std::vector<std::string> warnings;
};

/**
 * Bridges the selected edges: border (or wire) chains pair up per opts.connect and each pair is joined with a strip of
 * faces. Borders of different lengths pair by arc length. Returns the new faces and what happened at the ends.
 */
LILA_SCENE_API BridgeResult bridge(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts = {});

/** Deletes the faces and bridges the holes they leave (Maya's face bridge). */
LILA_SCENE_API BridgeResult bridgeFaces(Mesh& mesh, std::span<const Index> faces, const BridgeOptions& opts = {});

/** Joins exactly two border chains, read from edges, with no pairing search beyond them. Returns the new faces. */
LILA_SCENE_API std::vector<Index> bridgeEdgeLoops(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts = {});

/**
 * Splits a selection of edges into the pairs of border chains to bridge. Edges off the border are ignored. Whole chains
 * pair with their nearest facing partner, preferring the same length; a chain left over (one loop round a slot, or a U)
 * is cut at its corners and its two longest opposite sides pair, leaving the short ends that join them.
 */
LILA_SCENE_API std::vector<std::vector<Index>> planBridges(const Mesh& mesh, std::span<const Index> edges);

/** Bridges every pair planBridges finds. Returns the new faces. */
LILA_SCENE_API std::vector<Index> bridgeEdges(Mesh& mesh, std::span<const Index> edges, const BridgeOptions& opts = {});
} // namespace lila::geo
