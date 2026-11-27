#pragma once

#include "Materials.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace terrain
{
/**
 * One kind of ground cover: which items, how many per square metre where it fits fully, where it fits (terms times the
 * summed weights of the biomes and layers it names), and how each piece stands. Lengths cm.
 */
struct CoverRule
{
    std::string name;
    /** Item names (Object Info nodes of the ground cover group); each piece picks one. */
    std::vector<std::string> items;
    double density = 0.05;
    /** Radius kept clear around each piece at scale 1; 0 takes the item's own size across. */
    double footprint = 0.0;
    std::vector<Term> terms;
    std::vector<std::string> biomes;
    std::vector<std::string> layers;
    double scaleMin = 0.8;
    double scaleMax = 1.2;
    /** 0 stands up the scene's up axis, 1 follows the ground's normal. */
    double align = 0.0;
    /** Random turn about up, degrees either way. */
    double turn = 180.0;
    /** How far each piece sinks into the ground, cm at scale 1. */
    double sink = 0.0;
    /** Pieces of later rules keep clear of this one's footprints; off for grass that grows under trees. */
    bool solid = true;
    /** Below this fit nothing grows, however lucky the draw. */
    double minFit = 0.05;
    /** Each piece turns so its +Z faces downhill (roots, ferns hanging off a ledge) instead of a random turn. */
    bool faceDownhill = false;
};

/** Slope band presets by name: scree (below cliffs), ledgePlants (flat spots on and near cliffs), hangingRoots (cliff tops). */
const std::vector<std::string>& coverPresetNames();
/** A preset rule; throws std::runtime_error for an unknown name. */
CoverRule coverPreset(const std::string& name);

struct CoverSpec
{
    std::vector<CoverRule> rules;
    uint32_t seed = 1;
    /** Most pieces in all; past it every rule is thinned by the same factor (reported). */
    int maxCount = 200000;
    /** Multiplies every rule's density. */
    double densityScale = 1.0;

    nlohmann::json toJson() const;
    /** Throws std::runtime_error. */
    static CoverSpec fromJson(const nlohmann::json& j);
};

/** A piece, in the heightfield's own space. */
struct CoverPiece
{
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    double scale = 1.0;
    /** Radians about up. */
    double turn = 0.0;
    double radius = 0.0;
    float fit = 0.0f;
    float random = 0.0f;
    int rule = 0;
    /** Index into the rule's items. */
    int item = 0;
    /** The rule's grid cell it came from: with rule, an identity that holds while the seed and the rule's density do. */
    int cellX = 0;
    int cellZ = 0;
    /** Ground slope, degrees, and the horizontal downhill direction, in the heightfield's space. */
    double slope = 0.0;
    glm::dvec3 downhill{1.0, 0.0, 0.0};
};

struct CoverRuleStats
{
    size_t candidates = 0;
    size_t placed = 0;
    size_t blocked = 0;
    double meanFit = 0.0;
};

struct CoverResult
{
    std::vector<CoverPiece> pieces;
    std::vector<CoverRuleStats> rules;
    /** Fraction of the asked density kept to stay under maxCount; 1 when nothing was thinned. */
    double thinned = 1.0;
    std::vector<std::string> missing;
};

/**
 * Scatter over the heightfield, rule by rule: candidates on a jittered grid as dense as the rule asks, each kept with
 * probability equal to how well it fits there, then dropped when its footprint overlaps a solid piece already placed.
 * itemRadius gives each item's own radius at scale 1 (cm). Deterministic for the same inputs and seed.
 */
CoverResult scatterCover(const Heightfield& h, const CoverSpec& s, const std::vector<std::vector<double>>& itemRadius);

/**
 * The heightfield a terrain's output mesh was meshed from: a grid of points as Heightfield to Mesh lays them out (every
 * sample, or every step-th with the last row and column kept, which is resampled back to an even spacing), with its float
 * point attributes as masks. Empty when the mesh isn't such a grid.
 */
Heightfield heightfieldFromGridMesh(const lila::geo::Mesh& m);

/** Rocks on scree, trees by moisture, grass on the grass layer and debris below cliffs, with these item names. */
CoverSpec defaultCover(const Heightfield& h);
} // namespace terrain
