#pragma once

#include "GenCommon.h"

#include <optional>

/**
 * Roofing: sheets, planks or tiles laid in rows over roof faces, the way a roofer lays them: from the eave up, each row
 * overlapping the one below, each piece overlapping its neighbour, ridge caps along ridges. Then age: missing pieces,
 * lifted corners, a sagging middle, and jitter from neat stylized rows (neatness 1) to a messy roof (neatness 0).
 *
 * Pieces are fitted by their bounds: Y is out of the roof, the longer horizontal side runs down the slope. Rows step by
 * the shortest piece and columns by each piece's width, so a chunky toon tile gives fewer, bigger rows than a realistic one.
 */
namespace weathering::roof
{
using gen::Bounds;
using gen::Index;
using gen::Mesh;

struct Options
{
    double overlap = 0.15;     // of a piece's length, down the slope
    double sideOverlap = 0.08; // of a piece's width, across
    double overhang = 5.0;     // cm past the eave
    double scale = 1.0;        // every piece
    double damage = 0.3;       // 0 to 1: drives missing, lifted and how far corners lift
    std::optional<double> missing; // fraction of pieces left out; default damage * 0.25
    std::optional<double> lifted;  // fraction with a lifted corner; default damage * 0.35
    double liftAngle = 0.0;    // degrees; 0 is 6 + 20 * damage
    double sag = 0.0;          // cm the middle of each face sits below its edges
    double neatness = 0.3;     // 0 messy, 1 regular rows
    double stagger = 0.5;      // of a width, alternate rows shift (neat rows)
    bool ridge = true;         // caps along ridges
    double ridgeOverlap = 0.1;
    double maxSlope = 75.0;    // faces picked automatically: at most this steep, degrees
    uint32_t seed = 1;
};

struct Input
{
    const Mesh* host = nullptr;   // world space
    std::vector<Index> faces;     // empty: every face within maxSlope of up
    std::vector<Bounds> cover;    // the pieces laid in rows
    std::vector<double> coverWeights;
    std::vector<Bounds> caps;     // ridge caps; none: no caps
    std::vector<double> capWeights;
};

/** One planar part of the roof: its frame and extents (a across, v up the slope). */
struct Region
{
    glm::dvec3 origin{0.0}, across{1, 0, 0}, up{0, 0, -1}, normal{0, 1, 0};
    double a0 = 0, a1 = 0, v0 = 0, v1 = 0;
    std::vector<std::vector<glm::dvec2>> faces; // each face's outline in (a, v)
    bool inside(const glm::dvec2& p) const;
};

struct Placed
{
    glm::dmat4 xf{1.0};
    int slot = 0; // 0 cover, 1 ridge cap
    int piece = 0;
    int region = 0;
    int row = 0;
    bool lifted = false;
    float random = 0.0f;
    /** Footprint corners in the region's (a, v), before lifting: for coverage checks. */
    std::array<glm::dvec2, 4> footprint{};
};

struct Result
{
    std::vector<Region> regions;
    std::vector<Placed> placed;
    int candidates = 0; // cover pieces before missing ones were left out
    int missing = 0;
    int lifted = 0;
    int rows = 0;
    int caps = 0;
};

/** Planar regions of the faces (or of every upward face), grouped by shared edges and normal. */
std::vector<Region> regionsOf(const Mesh& host, std::span<const Index> faces, double maxSlope, std::vector<std::vector<Index>>* regionFaces = nullptr);

Result layout(const Input& in, const Options& o);
} // namespace weathering::roof
