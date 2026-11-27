#pragma once

#include "Erosion.h"
#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace terrain
{
/** What the water and site analysis treats as a river, lake or sea, and how big floods get. Lengths cm. */
struct AnalysisParams
{
    /** Drainage area, m2, past which a sample is a river. */
    double riverArea = 250000.0;
    /** Filled depth past which a depression is a lake. */
    double lakeDepth = 50.0;
    /** Sea level; NaN for no sea. Samples below it joined to the edge are sea. */
    double seaLevel;
    /** Flood height scale above drainage: frequent 1, rare 2, extreme 4 (times 2 m at a first-order stream). */
    double floodScale = 2.0;

    AnalysisParams();
    std::string key() const;
};

/** A run of samples along a river between a source, confluence, lake or the edge. Indices into the heightfield. */
struct Reach
{
    int id = 0;
    std::vector<uint32_t> cells;
    int order = 1;
    /** Reach this one flows into, -1 when it ends in a lake, the sea or off the edge. */
    int downstream = -1;
    std::vector<int> upstream;
    /** Where it ends: "confluence", "lake", "sea" or "edge"; and the lake id when a lake. */
    std::string mouth;
    int mouthLake = -1;
    /** Starts at a spring (no river above it) rather than a confluence or a lake outlet. */
    bool spring = false;
};

struct Lake
{
    int id = 0;
    std::vector<uint32_t> cells;
    double level = 0.0;
    double maxDepth = 0.0;
    bool sea = false;
    /** First sample past the lake its water leaves by; UINT32_MAX for the sea. */
    uint32_t outlet = UINT32_MAX;
    int outletReach = -1;
    std::vector<int> inlets;
};

/**
 * The analysis of one heightfield revision: drainage, rivers, lakes and the derived fields of the city contract (slope,
 * hand, floodRisk, buildability, ...). Fields are computed on first use and kept.
 */
class Analysis
{
public:
    Analysis(std::shared_ptr<const Heightfield> h, const AnalysisParams& p);

    const Heightfield& heightfield() const { return *mH; }
    const AnalysisParams& params() const { return mP; }
    const Drainage& drainage() const { return mD; }
    const std::vector<Reach>& reaches() const { return mReaches; }
    const std::vector<Lake>& lakes() const { return mLakes; }
    /** Per sample: the reach id it belongs to, or -1. */
    const std::vector<int32_t>& reachOf() const { return mReachOf; }
    /** Per sample: the lake (or sea) id, or -1. */
    const std::vector<int32_t>& lakeOf() const { return mLakeOf; }
    /** River discharge, m3/s, from drainage area. */
    double discharge(uint32_t cell) const;
    double areaM2(uint32_t cell) const;

    /** A field by name (see fieldNames) or any mask of the heightfield; empty when unknown. */
    const std::vector<float>& field(const std::string& name) const;
    static const std::vector<std::string>& fieldNames();

    /** Bilinear field value at a local XZ point. */
    double sample(const std::string& name, double x, double z) const;

private:
    std::vector<float> compute(const std::string& name) const;

    std::shared_ptr<const Heightfield> mH;
    AnalysisParams mP;
    Drainage mD;
    std::vector<Reach> mReaches;
    std::vector<Lake> mLakes;
    std::vector<int32_t> mReachOf, mLakeOf;
    mutable std::mutex mMutex;
    mutable std::map<std::string, std::shared_ptr<const std::vector<float>>> mFields;
};

/** The analysis of h, kept for the last few revisions. */
std::shared_ptr<const Analysis> analyse(std::shared_ptr<const Heightfield> h, const AnalysisParams& p = {});
/** Forgets the analyses kept for the last heightfields asked about (each holds its heightfield). */
void forgetAnalyses();

/** Contour lines of a grid at a level, chained into polylines (closed ones repeat their first point), in grid units. */
std::vector<std::vector<glm::dvec2>> contourLines(const std::vector<float>& grid, int nx, int nz, float level);

/** Outlines of the set samples, in the heightfield's XZ: outer rings and holes. */
std::vector<std::vector<glm::dvec2>> outlines(const Heightfield& h, const std::vector<uint8_t>& set);

/** Connected components (8-connected) of the set samples: a label per sample (-1 outside) and the count. */
int components(const std::vector<uint8_t>& set, int nx, int nz, std::vector<int32_t>& label);

/** Where results go: world positions from the heightfield's space, heights along the scene's up, lengths scaled. */
struct WorldMap
{
    glm::dmat4 m{1.0};
    glm::dvec3 up{0.0, 1.0, 0.0};
    double across = 1.0;
    double vertical = 1.0;

    nlohmann::json point(double x, double y, double z) const;
    double height(double x, double y, double z) const;
};

/**
 * Everything about the water as one JSON graph: rivers (reaches with per-point width, depth, bed and water level, flow
 * speed, discharge, banks and floodplains), lakes and sea (level, shoreline, depth, outlet, inlets, shore types,
 * harbour sites), springs, wetlands, fords, bridge sites, confluences, rapids and waterfalls. Lengths cm.
 */
nlohmann::json waterGraph(const Analysis& a, const WorldMap& w);

/** Polygons of contiguous buildable land with stats. Areas m2, slopes degrees. */
nlohmann::json buildableRegions(const Analysis& a, const WorldMap& w, double minAreaM2, double maxSlope, bool avoidFlood, double floodTolerance);

/** Scored points for a kind of site, best first, at least spacing (cm, the heightfield's units) apart. */
nlohmann::json sites(const Analysis& a, const WorldMap& w, const std::string& kind, int count, double spacing);
const std::vector<std::string>& siteKinds();

/** Where a ray from origin along dir first meets the surface; false when it misses. */
bool raycast(const Heightfield& h, glm::dvec3 origin, glm::dvec3 dir, glm::dvec3* hit, double maxDistance = 1e12);
} // namespace terrain
