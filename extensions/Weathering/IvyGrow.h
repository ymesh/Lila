#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

/**
 * The ivy grower: strands climb a surface from seed points or along painted strokes, branch, hang over ledges and droop
 * from overhangs, then carry leaves and flowers from the user's pieces. The branching itself is the geometry-nodes Space
 * Colonization node run over attractors scattered on the surface; this file decides where the attractors go, keeps the
 * strands on the surface, shapes their curves and builds the mesh.
 *
 * Everything is in one space (the caller's, normally world space, Y up) and deterministic for a seed.
 */
namespace weathering::ivy
{
enum class Profile
{
    Round,
    Ribbon,
    Custom,
};

enum class LeafOrient
{
    Surface, // tips up and out from the surface, splayed to the sides
    Up,      // tips straight up
    Stem,    // tips along the stem
    Random,  // any way round in the surface's plane
};

struct Params
{
    uint32_t seed = 1;
    /** Longest path from a seed along the stems, hanging strands included, cm. */
    double length = 300.0;
    /** 0 sparse single strands to 1 dense mats of side shoots. */
    double branchiness = 0.5;
    /** 0 spreads every way (moss, roots) to 1 heads straight up. */
    double climb = 0.7;
    /** 0 none to 1 long strands hanging over ledges and drooping from overhangs. */
    double gravity = 0.3;
    /** Farthest a clinging stem's centre line sits from the surface, cm. */
    double adhesion = 2.0;
    /** 0 natural wander to 1 smooth stylized arcs. */
    double smoothness = 0.15;
    /** Stem radius at the base, cm; tips taper to a third of it. */
    double stemRadius = 0.6;
    Profile profile = Profile::Round;
    /** Custom: a closed outline in the stem's cross-section, x across the surface and y out of it, about unit size. */
    std::vector<glm::dvec2> profilePoints;
    /** Sides of a round stem. */
    int sides = 5;
    /** Leaves per metre of stem. */
    double leafDensity = 30.0;
    /** Longest side of a leaf, cm. */
    double leafSize = 8.0;
    /** 0 every leaf the same size to 1 from a fifth to nearly double. */
    double leafSizeJitter = 0.35;
    LeafOrient leafOrient = LeafOrient::Surface;
    /** Flowers per metre of stem. */
    double flowerDensity = 0.0;
    double flowerSize = 4.0;
    /** Step along a strand, cm; 0 picks one from the length and leaf size. */
    double step = 0.0;
};

/**
 * A piece mesh in its own space, laid out like a Foliage leaf card: along +Z from its stem end, face toward +Y. It is
 * fitted by its bounds (longest side to the leaf size, stem end at the origin), so any mesh at any scale works.
 * Material slots it carries are kept; faces without one get the role's slot.
 */
struct Piece
{
    lila::geo::Mesh mesh;
    double weight = 1.0;
};

struct Strand
{
    std::vector<glm::dvec3> points;
    /** Surface normal under each point (for hanging points, the direction they face). */
    std::vector<glm::dvec3> normals;
    std::vector<double> radius;
    /** Path length from the seed, cm. */
    std::vector<double> growth;
    /** 1 where the point hangs free of the surface. */
    std::vector<uint8_t> hang;
    int parent = -1;
};

struct Result
{
    std::vector<Strand> strands;
    lila::geo::Mesh mesh;
    size_t leaves = 0;
    size_t flowers = 0;
    size_t attractors = 0;
    std::vector<std::string> warnings;
};

/** Pieces for the leaves, flowers and stem segments; empty stems sweep the profile instead. */
struct Pieces
{
    std::vector<Piece> leaves;
    std::vector<Piece> flowers;
    std::vector<Piece> stems;
};

/** Material slots the mesh uses for faces that bring none of their own. */
inline constexpr const char* kStemSlot = "IvyStem";
inline constexpr const char* kLeafSlot = "IvyLeaf";
inline constexpr const char* kFlowerSlot = "IvyFlower";
/** The colour set the wind reads: R sway weight, G per-leaf phase, B 1 on leaves and flowers. */
inline constexpr const char* kWindSet = "wind";

/** Grows from seed points and along strokes (polylines on or near the surface) over a surface mesh. */
Result grow(const lila::geo::Mesh& surface, std::span<const glm::dvec3> seeds, std::span<const std::vector<glm::dvec3>> strokes, const Params& p,
            const Pieces& pieces);

/**
 * Seed points where the surface meets the ground: on steep faces within a band above the lowest point of the surface
 * (or above groundY when given), spread along that contact.
 */
std::vector<glm::dvec3> groundSeeds(const lila::geo::Mesh& surface, int count, uint32_t seed, const double* groundY = nullptr);

/** Seed points where a per-point weight (a painted mask, 0 to 1) is high: picked in proportion to it. */
std::vector<glm::dvec3> maskSeeds(const lila::geo::Mesh& surface, std::span<const float> pointWeight, int count, uint32_t seed);

/** Mean turning angle per cm along the clinging parts of the strands: lower is smoother. */
double meanCurvature(const Result& r);

/** The built-in leaf shapes, used by the placeholders and by the Ivy geometry node when no leaves are given. */
lila::geo::Mesh leafCard(bool toon, int variant);
lila::geo::Mesh flowerCard(bool toon);
lila::geo::Mesh mossTuft(bool toon);
} // namespace weathering::ivy
