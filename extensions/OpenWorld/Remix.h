#pragma once

#include "OsmDoc.h"
#include "RemixSpec.h"
#include "Warp.h"

#include "Projection.h"
#include "Terrain.h"

#include <memory>
#include <string>
#include <vector>

/**
 * World Remix: real places composed onto one map. Each source's elevation, OSM data and land cover are read, scaled and
 * placed; regions are cut, moved, duplicated and deleted and pins warp the land between; seams are blended and eroded,
 * cut roads re-routed, rivers re-solved to drain and coastline re-merged. Everything goes through one map, so heights,
 * features and land cover stay consistent. Pure functions: no scene needed (the commands and node wrap them).
 */
namespace terrain
{
struct Dem;
}

namespace openworld
{
using terrain::Heightfield;

/** A source as read: heights (cm, unscaled) and land cover masks on its own local grid, and its OSM data. */
struct SourceData
{
    std::string id;
    terrain::Georef geo;
    Heightfield hf;
    OsmDoc osm;
    bool hasDem = false;
    /** The source's bathymetry grid, when it has one. */
    std::shared_ptr<const terrain::Dem> bathymetry;
    /** 1 where the elevation leaves the sea flat (low ground joined to the data's edge), on hf's grid; empty for none. */
    std::vector<float> flatSea;
    std::vector<std::string> notes;
};

/**
 * Reads a source's files (cached by path and modification time). Relative paths are under the workspace. landTolerance
 * decides what elevation counts as flat sea (bathymetry.landTolerance). Throws std::runtime_error.
 */
std::shared_ptr<const SourceData> loadSource(const Source& s, double landTolerance = 50.0);

/** A road or coastline end the remix cut, and what became of it. */
struct CutEnd
{
    int64_t node = 0;
    Vec2 at{0.0};
    std::string cls;
    bool joined = false;
    /** partner (the other side of the same cut), near (another end or road nearby), or empty while dangling. */
    std::string how;
};

struct RiverState
{
    int64_t way = 0;
    std::string name;
    Vec2 end{0.0};
    bool drains = true;
    /** Whether the river drained in the sources: one that never did (an inland basin) is not the remix's doing. */
    bool drainedBefore = true;
    /** cm of flow path added to reach the sea or the map's edge. */
    double extended = 0.0;
};

struct RemixResult
{
    /** The remixed map: heights cm, land cover masks, remixSeam (0 to 1 seam bands), remixHole (filled holes) and seabed (1 where the sea floor was laid). */
    Heightfield after;
    /** The base map on the same grid: sources placed and scaled, no regions or pins. */
    Heightfield before;
    /** The remixed OSM, positions in the output georeference. */
    OsmDoc osm;
    /** Output map (cm) to latitude and longitude; origin is the heightfield's centre, as terrainImportOsm(origin=) expects. */
    terrain::Georef geo;
    /** [latitude, longitude] of the heightfield's centre. */
    glm::dvec2 originLatLon{0.0};
    std::shared_ptr<Warp> warp;
    Vec2 lo{0.0}, hi{0.0};
    /** Each source's outline on the base map (empty for one that could not be read). */
    std::vector<Polygon> footprints;
    /** How far in from a source's edge it fades in, cm on the base map. */
    double feather = 0.0;

    std::vector<CutEnd> roadEnds;
    std::vector<CutEnd> coastEnds;
    std::vector<RiverState> rivers;
    /** Feature lines in map cm for the overlay: roads, rivers, coast; reconnections flagged. */
    struct Line
    {
        std::vector<Vec2> points;
        WayKind kind = WayKind::Other;
        bool added = false;
    };
    std::vector<Line> lines;

    nlohmann::json stats = nlohmann::json::object();
    std::vector<std::string> notes;
    /** Sources that could not be read; the map is built without them. */
    std::vector<std::string> errors;
};

struct RunOptions
{
    /** OSM features and their repairs; off for heights only. */
    bool features = true;
};

/** Runs the remix. Throws std::runtime_error when nothing can be built (no sources, or no grid). */
std::shared_ptr<RemixResult> runRemix(const RemixSpec& spec, const RunOptions& o = {});

/** The same spec (by JSON and file stamps) gives the cached result. */
std::shared_ptr<const RemixResult> cachedRemix(const RemixSpec& spec);

/**
 * Problems a remix leaves: rivers that no longer drain, road and coastline ends left dangling, regions whose targets
 * overlap, folds in the warp, and sources that could not be read. Returns {ok, counts, issues: [{kind, id, at, what}]}.
 */
nlohmann::json checkRemix(const RemixSpec& spec, const RemixResult& r);

/**
 * Moves an extract of a source's own OSM (terrain::extractOsm, real latitude and longitude) onto the remixed map's latitude
 * and longitude, every kind (roads, rivers, piers, quays, dams, land cover, buildings with holes and parts, places and
 * points of interest) through terrain::forEachPoint, the way the remix moved the ground: each element whole with the piece
 * holding most of its points (a cut or duplicated region's rigid motion, else the warp), so a building and the shop in it
 * move together. Elements on nothing (deleted, off the source) are dropped; returns how many. Throws std::runtime_error.
 */
size_t remixExtract(const RemixSpec& spec, const RemixResult& r, std::string_view source, terrain::OsmExtract& x);

/** Where a source's local point (cm) lands on the base map, and back. */
Vec2 placeOnBase(const Source& s, double horizontal, Vec2 local);
Vec2 baseToSource(const Source& s, double horizontal, Vec2 base);

/** The source's footprint on the base map. */
Polygon footprintOf(const Source& s, double horizontal, const Heightfield& hf);

/** Contour lines of heights at level, in map cm, inside [lo, hi]. */
std::vector<std::vector<Vec2>> contourLines(const Heightfield& h, double level, Vec2 lo, Vec2 hi);
} // namespace openworld
