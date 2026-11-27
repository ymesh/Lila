#pragma once

#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace terrain
{
/** A content hash of the grid, heights and masks: equal revisions mean the same terrain. */
std::string revision(const Heightfield& h);

/** Where a stamp applies, in the heightfield's own XZ. */
struct Footprint
{
    /** Closed rings, the last point not repeated; a sample inside any of them is inside. */
    std::vector<std::vector<glm::dvec2>> rings;
    /** Rings around the whole footprint, for retaining walls and piles: the rings themselves, or a mesh's border. */
    std::vector<std::vector<glm::dvec2>> outline;
    /** Lowest Y of the source geometry (level=object); NaN when it has none. */
    double lowestY;
    /** Open polylines, for line features: a path, or each curve (closed ones end where they start). */
    std::vector<std::vector<glm::dvec2>> lines;

    Footprint();
    bool empty() const { return rings.empty(); }
};

Footprint footprintFromPolygon(std::vector<glm::dvec2> ring);
/** Every face of the mesh seen from above, after toLocal; its border loops (or its hull, when closed) are the outline. */
Footprint footprintFromMesh(const lila::geo::Mesh& m, const glm::dmat4& toLocal);
/** Each curve as a ring, after toLocal. */
Footprint footprintFromCurves(const lila::gn::CurveSet& c, const glm::dmat4& toLocal);

enum class StampMode : uint8_t
{
    Flatten,
    Carve,
    Raise,
    Blend,
    Plinth,
    Terraced,
    Stilts,
};

enum class StampEdge : uint8_t
{
    Slope,
    Wall,
    Steps,
};

enum class StampLevel : uint8_t
{
    Mean,
    Min,
    Max,
    Median,
    Object,
    Value,
};

const std::vector<std::string>& stampModeNames();
const std::vector<std::string>& stampEdgeNames();
const std::vector<std::string>& stampLevelNames();

/**
 * One footprint stamp: a building pad (kind pad) or a plaza, park or yard (kind region). Lengths are cm in the
 * heightfield's own space; height is used when level is Value.
 */
struct Stamp
{
    std::string id;
    std::string kind = "pad";
    StampMode mode = StampMode::Flatten;
    StampEdge edge = StampEdge::Slope;
    StampLevel level = StampLevel::Mean;
    double height = 0.0;
    double offset = 0.0;
    /** Skirt width outside the footprint, blending back to the ground. */
    double falloff = 500.0;
    /** Steepest the skirt may fall (rise over run, local units); the falloff widens up to four times to keep to it. 0: as given. */
    double skirtGrade = 0.0;
    /** Blend mode: how far toward the target, 0 to 1. */
    double strength = 1.0;
    /** Regions: fall across the footprint, percent, along direction (downhill); the ground's own fall when direction is zero. */
    double grade = 0.0;
    glm::dvec2 direction{0.0};
    /** Terraced: number of steps (0 picks from stepHeight); Steps edge: riser height. */
    int steps = 0;
    double stepHeight = 150.0;
    /** Masks written with the stamp's coverage. */
    std::vector<std::string> masks{"cityFootprint"};
    double maskValue = 1.0;
    /** Height difference past which an edge is reported as a retaining wall. */
    double wallHeight = 50.0;
    /** The footprint: a ring, or the Object Info node of the object it follows. */
    std::vector<glm::dvec2> polygon;
    std::string object;

    /**
     * Line features (kind street, canal, moat, quay, levee, embankment, seawall, bridge, culvert, tunnel) run along path
     * (or the object's first curve), width wide. depth: canal and moat depth below the water, quay dredge depth, dredge
     * (a region kind) depth. rise: levee, embankment and seawall height above the ground. side: which side of the line a
     * quay, embankment or seawall holds land on (left or right of travel). maxGrade: street grade limit, percent.
     * clearance: bridge deck height over the ground below it.
     */
    std::vector<glm::dvec2> path;
    double width = 600.0;
    double depth = 200.0;
    double rise = 200.0;
    std::string side = "left";
    double maxGrade = 8.0;
    double clearance = 500.0;

    bool isLine() const;

    nlohmann::json toJson() const;
    /** Throws std::invalid_argument on unknown names. */
    static Stamp fromJson(const nlohmann::json& j);
};

struct StampResult
{
    /** Middle of the covered samples, where padHeight and the ground figures apply. */
    glm::dvec2 centroid{0.0};
    double padHeight = 0.0;
    /** The skirt width used: the stamp's falloff, or wider for its skirtGrade; 0 when not widened. */
    double falloff = 0.0;
    double groundMin = 0.0, groundMax = 0.0, groundMean = 0.0;
    /** Volumes moved, in cubic heightfield units. */
    double cut = 0.0, fill = 0.0;
    double area = 0.0;
    size_t samples = 0;
    /** Plinth: the base's depth below the pad at the lowest ground. */
    double plinthDepth = 0.0;
    struct WallPoint
    {
        glm::dvec2 at;
        double top, bottom;
    };
    /** Edges where pad and ground part by more than wallHeight and the skirt can't take it. */
    std::vector<std::vector<WallPoint>> walls;
    struct Pile
    {
        glm::dvec2 at;
        double top, ground;
    };
    std::vector<Pile> piles;
    /** Terraced: each step's level. */
    std::vector<double> levels;

    /** Line features: the graded centreline (x, surface height, z), its steepest grade (percent) and its length. */
    std::vector<glm::dvec3> centreline;
    double maxGrade = 0.0;
    double length = 0.0;
    /** Runs along the centreline (indices into it) the terrain was left alone under: bridges over hollows, tunnels through rises. */
    struct Span
    {
        size_t from = 0, to = 0;
        std::string kind;
    };
    std::vector<Span> spans;
    /** Where a street crosses a stream too small for a bridge. */
    std::vector<glm::dvec2> culverts;
    /** Canals and moats: their water level. */
    double waterLevel = 0.0;
};

/** Mask names a kind writes when the caller names none. */
std::vector<std::string> defaultMasks(const std::string& kind);
const std::vector<std::string>& lineKinds();

/** Every sample the footprint covers (with the cells its edges cross), over the whole grid. */
std::vector<uint8_t> footprintMask(const Heightfield& h, const Footprint& f);
/** Squared distances, in samples, from each sample of a w by d grid to the nearest set one (exact Euclidean). */
std::vector<float> squaredDistances(const std::vector<uint8_t>& set, int w, int d);

/** Applies a stamp to the heightfield, writing its masks; the result describes what it did. */
StampResult applyStamp(Heightfield& h, const Stamp& s, const Footprint& f);

/** What a Terrain Stamps node (by name) of a modifier did on its last evaluation, by stamp id. */
void publishStampResults(uint64_t modifier, const std::string& node, std::map<std::string, StampResult> results);
std::map<std::string, StampResult> stampResults(uint64_t modifier, const std::string& node);

enum class SculptOp : uint8_t
{
    Raise,
    Lower,
    Smooth,
    Flatten,
    Noise,
    Ramp,
    Terrace,
    Stamp,
    Erode,
    Uplift,
    Relax,
    Mask,
    Patch,
};

const std::vector<std::string>& sculptOpNames();

/** A sculpt stroke: dabs at points, or along a path every spacing. Lengths cm in the heightfield's space. */
struct SculptStroke
{
    SculptOp op = SculptOp::Raise;
    std::vector<glm::dvec2> points;
    bool path = false;
    double radius = 2000.0;
    /** Raise, lower, noise: height at the centre, cm. Smooth, flatten: 0 to 1. */
    double strength = 100.0;
    /** Between dabs along a path; 0 is a quarter of the radius. */
    double spacing = 0.0;
    /** Flatten: target height; NaN takes the mean under the stroke. */
    double height;
    double noiseSize = 1000.0;
    uint32_t seed = 0;
    /** Weight per point, from pen pressure (empty: 1 everywhere). */
    std::vector<float> pressure;
    /** A brush falloff shape name (smooth, bell, linear, ...); empty is the terrain falloff. */
    std::string falloff;
    /** Mirror the dabs across the heightfield's centre: x, z or xz; empty is off. */
    std::string mirror;
    /** The brushes past noise (ramp, terrace, stamp, erode, uplift, relax, mask): their settings, see Brushes.h. */
    nlohmann::json settings;

    SculptStroke();
    nlohmann::json toJson() const;
    static SculptStroke fromJson(const nlohmann::json& j);
};

void applyStroke(Heightfield& h, const SculptStroke& s);

/** What makes ground expensive to build a route across. cost = 1 + slope * deg / 10 + water * (wet) + sum mask * value. */
struct CostWeights
{
    double slope = 1.0;
    double water = 20.0;
    /** A sample is water when its flow mask passes this or its lakes mask is above zero. */
    double waterFlow = 0.75;
    std::map<std::string, double> masks;

    static CostWeights fromJson(const nlohmann::json& j);
};

std::vector<float> costField(const Heightfield& h, const CostWeights& w);

struct Route
{
    /** x, height, z in the heightfield's space. */
    std::vector<glm::dvec3> points;
    double length = 0.0;
    double cost = 0.0;
    /** Steepest grade along it, percent. */
    double maxGrade = 0.0;
    /** False when no path keeps within maxGrade and the steepest way was taken instead. */
    bool withinGrade = true;
};

/**
 * Cheapest path from a to b over the cost field, never steeper than maxGrade percent where any path allows (switchbacks
 * appear by themselves), on a grid of at most maxResolution samples a side.
 */
Route route(const Heightfield& h, glm::dvec2 a, glm::dvec2 b, const CostWeights& w, double maxGrade, int maxResolution = 1024);
} // namespace terrain
