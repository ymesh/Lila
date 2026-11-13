#pragma once

#include "MasonryBuilding.h"
#include "MasonryCore.h"

#include <nlohmann/json.hpp>

#include <span>
#include <string>
#include <vector>

namespace masonry
{
/** A ruin brush stroke: stones near the line fall (strength above 0) or are held up (below 0). */
struct RuinStroke
{
    std::vector<glm::dvec3> points;
    double radius = 50.0;  // cm
    double strength = 1.0; // -1 to 1
};

/** Strokes from JSON [{points: [[x, y, z], ...], radius, strength}]. Throws std::runtime_error. */
std::vector<RuinStroke> strokesFromJson(const nlohmann::json& list);

/** How a structure is ruined. Defaults leave it standing. */
struct RuinParams
{
    double left = 1.0;       // 0 to 1: share of the stone volume left standing, met to within a few percent
    double collapse = 0.0;   // deg about Y, 0 along +X: the side the structure fell toward, where it stands lowest
    double slope = 0.5;      // 0 to 1: how much lower it stands toward the collapse side
    double roughness = 0.35; // 0 to 1: how ragged the broken top is
    double scale = 0.0;      // cm, feature size of the ragged top; 0 is ten courses
    int gaps = 0;            // breaches cut down toward the ground
    double survivors = 0.1;  // 0 to 1: share of stones just above the break that hang on
    double loose = 0.4;      // 0 to 1: share of top stones tipped or slid
    double tip = 40.0;       // deg, the most a loose stone tips
    double rubble = 0.6;     // 0 to 1: share of fallen stones lying at the foot; the rest are gone
    bool settle = false;     // drop the rubble with the rigid-body solver instead of only heaping it
    uint32_t seed = 1;
    std::vector<std::string> regions; // only stones of these regions (or regions under them) ruin; empty is all
    std::vector<RuinStroke> strokes;  // painted damage, on top of the break
    bool structural = false;          // also drop what the rigid-body solver can't hold up (arches without keystones, overhangs)
    bool active() const { return left < 1.0 - 1e-9 || gaps > 0 || !strokes.empty() || structural; }
};

struct RuinStats
{
    int removed = 0;     // stones that fell (rubble or gone)
    int survivors = 0;   // stones above the break kept
    int loose = 0;
    int unsupported = 0; // stones kept by the break but with nothing under them, so they fell too
    int rubble = 0;
    int slabPieces = 0;
    int fallenPieces = 0;
    int painted = 0;    // stones the brush strokes took
    int unstable = 0;   // stones the structural pass found could not stand
    double left = 1.0;   // share of stone volume standing after the ruin
    bool settled = false;
    nlohmann::json toJson() const;
};

/**
 * Ruins any set of stones (generated or placed by hand): stones above a ragged break (lower toward the collapse side,
 * cut by gaps) fall, a few survive, stones left with nothing under them fall, top stones tip about their bottom edge,
 * and a share of the fallen land in heaps at the foot (role Rubble, damage 1). Kept stones near the break get damage.
 * Deterministic for the same stones and params. Stones with grounded unset everywhere are grounded by height. Only
 * eligible stones (all when empty) fall, tip or count toward left; the rest stand and hold others up. kept gets the
 * source index of every stone left, rubble included, in order.
 */
RuinStats ruinStones(std::vector<Stone>& stones, const RuinParams& p, double pitch = 0.0, std::span<const char> eligible = {},
                     std::vector<int>* kept = nullptr);

/** How old the stones look. */
struct AgeParams
{
    double age = 0.0;       // 0 to 1
    double moss = 0.5;      // 0 to 1, scaled by age
    double wetHeight = 0.0; // cm above the ground that stays damp; 0 is a fifth of the structure's height
    uint32_t seed = 1;
    double plaster = 0.0;          // 0 to 1: how much of a render coat is left on the outer faces
    double plasterThickness = 0.0; // cm; 0 is a third of a stone height
    double ivy = 0.0;              // 0 to 1: how much ivy climbs from the ground
    double ivyHeight = 0.7;        // 0 to 1: how far up the structure it reaches
    double leaf = 0.0;             // cm, leaf size; 0 is two thirds of a typical stone
};

/** The render coat left on the outer faces, a tile per stone, peeled most where high, damaged and exposed; slot MasonryPlaster. */
geo::Mesh plasterMesh(std::span<const Stone> stones, std::span<const float> height, const AgeParams& p, double joint);

/** Ivy strands climbing the outer faces from the ground, stopping where the stones do; leaf cards and stems, slot MasonryIvy. */
geo::Mesh ivyMesh(std::span<const Stone> stones, const AgeParams& p);

/** Fills every stone's age, wet and moss (damage comes from the ruin): wet near the ground and on tops, moss where wet and exposed. */
void ageStones(std::vector<Stone>& stones, const AgeParams& p);

/** Lays the plan out, ruins and ages it, then builds the core from the stones left and makes the meshes. */
Built realizePlan(const Plan& plan, const RuinParams& ruin, const AgeParams& age, bool lods, RuinStats* stats = nullptr);

/** Stones whose bottom is not on the ground or resting on another stone (within tolerance cm). What masonryCheck counts. */
int countFloating(std::span<const Stone> stones, double tolerance, std::vector<int>* which = nullptr);

/** What the ruin left lying: rubble, and slab pieces that fell. */
inline bool isRubble(const Stone& s) { return s.role == Role::Rubble || (s.role == Role::Slab && s.loose); }

/** Rubble lying more than height (cm) above the ground under it: heaps stacked into columns. What masonryCheck counts. */
int countStacked(std::span<const Stone> stones, double height, std::vector<int>* which = nullptr, double* highest = nullptr);
} // namespace masonry
