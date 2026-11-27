#pragma once

// Street network generators past the built-in patterns: tensor fields, eras and districts, fortifications, imported
// and example-based streets, contour streets and multi-level (3D) networks. All of it reads Options::generators.

#include "SettlementCore.h"

#include <lila/Names.h>

#include <functional>
#include <limits>
#include <optional>

namespace settlement
{
// ---------------------------------------------------------------------------------------------------------------------
// What generators read: Options::generators as JSON, checked by parseGenerators.

/** One basis field of a tensor field. Kinds: grid, radial, boundary, height. */
struct TensorBasis
{
    std::string kind = "grid";
    double angle = 0.0;  // degrees, grid
    Vec2 center{0.0};    // radial, and grid or radial with decay
    bool hasCenter = false;
    std::vector<std::vector<Vec2>> lines; // boundary: the curves the field follows
    std::string follow;  // boundary: "outline" or "water" instead of lines
    double weight = 1.0;
    double decay = 0.0;  // cm: weight falls off as exp(-(d / decay)^2) from the centre or lines; 0 never
};

/** A raster of weights over XZ, 0 to 1, row-major from origin (painted influence). */
struct WeightMask
{
    Vec2 origin{0.0};
    double cell = 100.0;
    int w = 0, h = 0;
    std::vector<float> values;
    double at(Vec2 p) const;
};

/** A painted stroke or region: inside radius of the stroke (or inside the closed region), the field turns to angle or follows the stroke. */
struct TensorPaint
{
    std::vector<Vec2> pts;
    bool closed = false;
    double radius = 3000.0;
    std::optional<double> angle; // degrees; none follows the stroke
    double weight = 4.0;
};

struct TensorSpec
{
    std::vector<TensorBasis> fields; // empty: grid at the profile's angle, radial round the centre, the outline and water
    std::vector<TensorPaint> paint;
    std::optional<WeightMask> mask; // multiplies the painted influence
    double majorSpacing = 0.0;      // cm between major streets; 0 from the profile's grid
    double minorSpacing = 0.0;      // cm between minor streets; 0 from the profile's grid
    double step = 0.0;              // cm per tracing step; 0 from the spacing
    bool minor = true;
};

/** A wall around the core: along defensible ground or a given polygon, gates where main roads cross, towers by spacing. */
struct WallSpec
{
    bool enabled = false;
    double radius = 0.0;          // fraction of the settlement radius; 0 for the profile's core radius
    std::vector<Vec2> polygon;    // given line, closed; empty to find one
    bool contour = true;          // follow high ground and breaks of slope on a ground that is not flat
    double thickness = 300.0;
    double height = 900.0;
    double towerSpacing = 4000.0;
    bool starFort = false;        // bastions instead of a plain circuit
    int bastions = 0;             // 0: 5 for a star fort
    double moat = 0.0;            // width, cm; 0 none
    double moatGap = 1200.0;      // between the wall and the moat's near bank
};

/** A straight avenue cut through what is there (a baroque axis): older streets running along it are removed. */
struct AxisSpec
{
    std::vector<Vec2> pts;   // two points, or empty for gate to gate through the centre
    int count = 1;           // without pts: this many axes, spread round the centre
    double angle = 0.0;      // degrees, first axis without pts
    double clearance = 0.0;  // cm cleared each side; 0 from the street width
};

/** A district or an era's region: where its generator runs. */
struct GenRegion
{
    std::vector<Vec2> polygon; // closed; empty for the whole site
    double inner = 0.0;        // fractions of the settlement radius: the era runs between them
    double outer = 1.0;
};

/** One era: a profile and a generator, run after the eras before it. */
struct EraSpec
{
    std::string name;
    std::string profile;     // empty: the settlement's
    json overrides;          // profile fields
    std::optional<Pattern> pattern;
    double growth = 1.0;
    GenRegion region;
    std::optional<WallSpec> walls;
    bool demolishWalls = false; // earlier walls come down and their moats are filled
    bool ring = false;          // a boulevard on the line of the demolished walls (or at the region's inner edge)
    std::vector<AxisSpec> axes;
    double highwayRing = 0.0;   // fraction of the radius: a highway ring road; 0 none
    int highwaySpurs = 0;       // highways from the ring out to the edge
};

struct DistrictSpec
{
    std::string name;
    std::vector<Vec2> polygon;
    std::string profile;
    json overrides;
    std::optional<Pattern> pattern;
    double growth = 1.0;
    int era = -1;            // -1: the settlement's last era
    uint32_t seed = 0;       // 0: from the settlement's
    std::vector<EraSpec> eras;
};

/** A street read from OpenStreetMap (or anywhere): points and class, and the (fictional) name and OSM id it came with. */
struct ImportedStreet
{
    std::vector<Vec2> pts;
    StreetClass cls = StreetClass::Local;
    std::string name;
    std::string osmId;
};

/**
 * A building read from OpenStreetMap (a building curve terrainImportOsm made): footprint and what the tags say. Lengths and
 * heights cm, NaN when not tagged; levels -1. name is already fictional.
 */
struct OsmBuildingIn
{
    std::string id; // "w123", "r9.1"
    std::vector<Vec2> outer;
    std::vector<std::vector<Vec2>> holes;
    bool part = false;
    std::string building, program, kind, name;
    double height = std::numeric_limits<double>::quiet_NaN(), minHeight = std::numeric_limits<double>::quiet_NaN();
    double roofHeight = std::numeric_limits<double>::quiet_NaN();
    int levels = -1, minLevel = -1, roofLevels = -1;
    std::string roofShape, colour, roofColour, material, roofMaterial, startDate;
};

/** A place read from OpenStreetMap: a point (shop, office, town) or an area (cemetery, farmland, port, school grounds). */
struct OsmPlaceIn
{
    std::string id, key, value, program, kind, name;
    Vec2 at{0.0};
    std::vector<std::vector<Vec2>> rings; // empty for a point
};

/** What a street network looks like in numbers: steers a new layout towards the same character. */
struct StreetStats
{
    double bins[36] = {}; // street length by bearing, 10 degree bins, both directions
    double fraction[6] = {}; // nodes by degree 1..5+ (index = degree), as fractions
    double blockMedian = 0.0; // m2
    double meanSegment = 0.0; // cm between junctions
    double axis = std::numeric_limits<double>::quiet_NaN(); // degrees, 0 to 90: the dominant grid direction, NaN when unknown
    bool valid = false;
};

struct ContourStreet
{
    std::vector<glm::dvec3> pts;
    std::string kind = "contour"; // contour, cross, switchback, stairs
};

/** A deck: a flat level over the ground, with its own streets. */
struct DeckSpec
{
    std::string name;
    std::vector<Vec2> polygon;
    double height = 2000.0; // cm above the highest ground under it
    std::optional<Pattern> pattern; // grid (default) or radial or none
    int ramps = 2;
};

struct ElevatedSpec
{
    std::vector<Vec2> pts;
    double height = 800.0;
    StreetClass cls = StreetClass::Arterial;
    bool ramps = true;
};

struct LevelsSpec
{
    std::vector<DeckSpec> decks;
    std::vector<ElevatedSpec> elevated;
    double skybridgeSpan = 0.0;  // cm: tall buildings this close are joined by skybridges; 0 none
    double skybridgeHeight = 0.5; // fraction of the lower building's height
    std::vector<double> airHeights; // cm above the ground's highest point: one air-traffic layer each
    double airSpacing = 20000.0;
    double megablock = 0.0;      // m2: blocks this large become megablock footprints; 0 none
};

/** Everything the generators read, from Options::generators. */
struct GenSpec
{
    std::optional<TensorSpec> tensor;
    std::optional<WallSpec> walls;
    std::vector<EraSpec> eras;
    std::vector<DistrictSpec> districts;
    std::vector<ImportedStreet> osm;
    std::vector<OsmBuildingIn> osmBuildings;
    std::vector<OsmPlaceIn> osmPlaces;
    /** Blocks OSM buildings cover less than osmCoverage of get Settlement's own lots around them: sparse, all (always) or none. */
    std::string osmFill = "sparse";
    double osmCoverage = 0.25;
    /** Real names (as hashes) the names Settlement makes must not match. */
    lila::names::NameHashes avoid;
    StreetStats example;
    std::vector<ContourStreet> contour;
    std::optional<LevelsSpec> levels;
};

/**
 * Reads the generators JSON: {"tensor": {...}, "walls": {...}, "eras": [...] or a preset name, "districts": [...], "osm":
 * [{points, class}], "osmBuildings": [{id, outer, holes, part, building, program, kind, name, height, minHeight, roofHeight,
 * levels, minLevel, roofLevels, roofShape, colour, roofColour, material, roofMaterial, startDate}], "osmPlaces": [{id, key,
 * value, program, kind, name, at, rings}], "osmFill": {mode, coverage}, "avoid": {exact, contained, longest}, "example":
 * {...}, "contour": [{points, kind}], "levels": {...}}. Throws std::runtime_error naming what is wrong.
 */
GenSpec parseGenerators(const json& j);
/** The era presets: walledTown, medievalToModern, gridToSprawl. */
std::vector<std::string> eraPresetNames();
/** A preset as eras JSON; null for an unknown name. */
json eraPreset(std::string_view name);
/** An OSM highway tag (motorway, residential, footway...) or a class name as a street class; Local when unknown. */
StreetClass classFromOsm(std::string_view tag);

// ---------------------------------------------------------------------------------------------------------------------
// What generators make, besides streets in the graph: Layout::gen.

struct WallLine
{
    std::vector<Vec2> pts; // closed
    int era = 0;
    int demolished = -1;  // the era that took it down, -1 standing
    double thickness = 300.0, height = 900.0;
    bool starFort = false;
    double towerSpacing = 4000.0;
};

struct Gate
{
    Vec2 p{0.0};
    Vec2 dir{1.0, 0.0}; // along the road, outward
    int wall = 0;
    double at = 0.0;    // cm along the wall from its first point
    double width = 0.0; // the road's width
    StreetClass cls = StreetClass::Arterial;
};

struct Tower
{
    Vec2 p{0.0};
    int wall = 0;
    bool gate = false;  // flanking a gate
    bool bastion = false;
};

struct EraInfo
{
    std::string name;
    Profile profile;
};

/** A level above the ground: its own planar street graph at a height. Edges carry layer = the level's index + 1. */
struct Level
{
    std::string name;
    std::string kind; // deck, elevated, skybridge
    double height = 0.0; // world Y of the deck surface (decks); elevated roads keep heights per point in pts3
    StreetGraph graph;
    Polygon outline;     // decks
    std::vector<std::vector<glm::dvec3>> lines3; // elevated roads and skybridges: their 3D centrelines
};

/** A ramp between two levels (or a level and the ground, level -1... 0 is the ground). */
struct Ramp
{
    std::vector<glm::dvec3> pts;
    int from = 0, to = 0; // layers: 0 the ground, k the level k - 1
    StreetClass cls = StreetClass::Collector;
};

/** A 3D lane layer for air traffic: nodes and one-way links, vertical links between altitudes. */
struct AirLanes
{
    std::vector<glm::dvec3> nodes;
    std::vector<std::pair<int, int>> links;
    std::vector<double> heights;
};

struct Megablock
{
    int block = -1;
    Polygon footprint;
    double base = 0.0, top = 0.0;
};

struct OsmIndex;

/** An imported place as the settlement placed it: the lots it covers or stands on, and the landmark it made. */
struct OsmSite
{
    std::string id, kind, program, name;
    Vec2 at{0.0};
    std::vector<Polygon> rings;
    std::vector<uint64_t> lots; // stable lot ids
    uint64_t landmarkLot = 0;   // 0 for none
    std::string placed;         // building, lot, area, district, site (stands on nothing the settlement made)
};

struct GenLayout
{
    std::vector<WallLine> walls;
    std::vector<Gate> gates;
    std::vector<Tower> towers;
    std::vector<EraInfo> eras;   // index = era
    std::vector<std::string> districts;
    std::vector<Polygon> districtOutlines; // as laid, with the profile each ran
    std::vector<Profile> districtProfiles;
    std::vector<int> districtEras;
    std::vector<Level> levels;
    std::vector<Ramp> ramps;
    AirLanes air;
    std::vector<Megablock> megablocks;
    std::vector<Polygon> cleared; // corridors axes were cut through
    int demolishedStreets = 0;   // older streets removed by axes
    int imported = 0;            // imported streets laid
    std::vector<ImportedStreet> importedRuns; // as laid (clipped to the outline), for naming the streets they made
    std::vector<std::string> warnings; // fallbacks taken, and why
    std::shared_ptr<const GenSpec> spec;
    std::vector<OsmSite> osmSites;
    /** OSM buildings made lots, lots Settlement cut beside them, blocks taken whole (dense) or filled (sparse), buildings off every block. */
    int osmLots = 0, osmFillLots = 0, osmDenseBlocks = 0, osmSparseBlocks = 0, osmOutside = 0, osmParts = 0, osmOverlapping = 0;
    std::shared_ptr<OsmIndex> osmIndex;
};

// ---------------------------------------------------------------------------------------------------------------------
// Stages, called from build.

/** Before the pattern: imported streets, walls and moats of a settlement without eras. */
void prepareGenerators(Layout& l, const Ground& g, const Options& o);
/** Runs the eras in order when there are any. False when there are none (the pattern runs as usual). */
bool layEras(Layout& l, const Ground& g, const Options& o);
/** The patterns this file adds: tensor, contour, osm, example. */
void layPattern(Layout& l, const Ground& g, const Options& o, Pattern p, double limit = 0.0, double inner = 0.0);
/** After the pattern: districts, then levels. */
void afterPattern(Layout& l, const Ground& g, const Options& o);
/** After street ids: gates and towers on the final streets. */
void afterStreets(Layout& l, const Ground& g, const Options& o);
/** After envelopes: eras on blocks, lots and envelopes (with their era's style), megablocks and skybridges. */
void afterLots(Layout& l, const Site& site, const Ground& g, const Options& o);

/**
 * Imported OpenStreetMap buildings and places onto the blocks from firstBlock on: each building becomes a lot (stable id
 * from its OSM id) with use and program from its tags, dense blocks lose the lots Settlement cut, sparse ones keep those not
 * under a building; points of interest become businesses on the building or lot they stand on, areas set the use of the
 * lots inside and a landmark. fresh (when given) grows with the lots added, marked 1. Tags on lots are still edge ids.
 */
void placeOsmBuildings(Layout& l, const Site& site, const Ground& g, const Options& o, size_t firstBlock, std::vector<char>* fresh);
/** True when the layout has imported OSM buildings to place. */
bool hasOsmBuildings(const Layout& l);
/** The envelope of an imported building's lot: its own footprint, heights from tags or the district's default by use, tiers from its parts, roof from roof:shape. */
Envelope osmEnvelope(const Lot& lot, const Layout& l, const Ground& g, const Options& o, const Site& site);
/** Program, name, businesses and a style guess onto an envelope from its lot (any envelope, imported or cut). */
void finishProgram(Envelope& env, const Lot& lot, const Layout& l);
json osmSitesJson(const Layout& l);
/** settlementCheck's realName issues: street and district names against the imported real names, building, business and place names against those and the brand blocklist. */
json realNameIssues(const Layout& l, const lila::names::RealNames& real);

/** Streets traced along a tensor field inside the layout's outline. */
void layTensor(Layout& l, const Ground& g, const Options& o, const TensorSpec& spec, double limit = 0.0);
/** The field's major direction at p (unit), and how strong it is; strength 0 where the field is degenerate. */
Vec2 tensorDirection(const Layout& l, const Ground& g, const TensorSpec& spec, Vec2 p, double* strength = nullptr);
/** Walls (and moats) laid into the graph as of era, at radius cm round the centre unless the spec gives a polygon. */
void layWalls(Layout& l, const Ground& g, const Profile& p, const WallSpec& spec, int era, double radius);
/** Gates where streets cross standing walls, towers along them; streets that cross a wall without a gate are cut. */
void finishWalls(Layout& l, const Ground& g);
/** The wall's line: a polygon round the centre at radius r, on defensible ground where the ground allows. */
std::vector<Vec2> wallLine(const Layout& l, const Ground& g, const WallSpec& spec, double r);
/** A bastioned trace with n points round c at radius r. */
std::vector<Vec2> starTrace(Vec2 c, double r, int n, double rotation);
void layImported(Layout& l, std::span<const ImportedStreet> streets, int era);
/** Streets grown to match an example's numbers. */
void layExample(Layout& l, const Ground& g, const Options& o, const StreetStats& stats, double limit = 0.0);
void layContour(Layout& l, const Ground& g, const Options& o, std::span<const ContourStreet> streets, double limit = 0.0);
void layLevels(Layout& l, const Ground& g, const Options& o, const LevelsSpec& spec);
/** After envelopes: megablock footprints and skybridges between tall buildings. */
void finishLevels(Layout& l, const Ground& g, const Options& o, const LevelsSpec& spec);

/** Numbers of a street graph (orientation, degrees, segment length) and the layout's blocks. */
StreetStats streetStats(const StreetGraph& graph, std::span<const Block> blocks = {});
StreetStats statsFromStreets(std::span<const ImportedStreet> streets);
json toJson(const StreetStats& s);
StreetStats statsFromJson(const json& j);

/** Every level's streets as lines with heights (layer per line) and per level road networks, for lane graphs. */
struct LevelLine
{
    std::vector<glm::dvec3> pts;
    StreetClass cls = StreetClass::Local;
    int layer = 0;
    std::string kind; // deck, elevated, ramp, skybridge, air
};
std::vector<LevelLine> levelLines(const Layout& l, const Ground& g);
/** The road network of one level (junctions, lanes) as roadNetwork makes it for the ground. */
RoadNetwork levelNetwork(const Layout& l, int level);

/** The profile's target ranges for its metrics, each {value, min, max, ok}, and ok overall. */
json targetReport(const Layout& l);
/** A hash of everything a layout holds: streets, blocks, lots, envelopes and generator output. */
uint64_t layoutHash(const Layout& l);

/**
 * Blocks, lots and envelopes again inside lo-hi after the street graph changed there (a street added, moved or removed),
 * the rest kept (replanParcels). Returns how many blocks were remade.
 */
int updateBlocks(Layout& l, const Site& site, const Ground& g, const Options& o, Vec2 lo, Vec2 hi);

/** Builtin metric targets for a profile by its name. */
void applyBuiltinTargets(Profile& p);
// ---------------------------------------------------------------------------------------------------------------------
// Shared helpers of the generator files.

namespace gen
{
/** Stable random numbers: the same seed and stream give the same sequence on every machine. */
struct Rng
{
    uint64_t base;
    uint64_t n = 0;
    Rng(uint32_t seed, uint64_t stream);
    double next();
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

Vec2 rotate(Vec2 d, double radians);
double length(std::span<const Vec2> pts);
/** The parts of a polyline inside the outline, outside the holes and between two radii round c (0: no limit). */
std::vector<std::vector<Vec2>> clip(std::span<const Vec2> pts, const Polygon& outline, Vec2 c, double inner, double outer,
                                    std::span<const Polygon> holes = {});
/** A run split where it is steeper than maxGrade. */
std::vector<std::vector<Vec2>> climbable(const std::vector<Vec2>& run, const Ground& g, double maxGrade, double step);
/** A run gapped at every water crossing. */
std::vector<std::vector<Vec2>> dry(const std::vector<Vec2>& run, std::span<const WaterLine> water);
/**
 * A planned line laid as streets of era: clipped to the outline, the radii and the exclusions, split where too steep,
 * gapped at water unless arterial. Returns the edges made.
 */
std::vector<int> lay(Layout& l, const Ground& g, const Profile& p, std::span<const Vec2> pts, StreetClass cls, int era,
                     double inner = 0.0, double outer = 0.0, double width = 0.0);
/** A circle of radius r round c as a closed polyline (first point repeated) of at least n points. */
std::vector<Vec2> circle(Vec2 c, double r, int n);
/** Chaikin corner cutting on a closed polygon. */
std::vector<Vec2> smoothClosed(std::vector<Vec2> pts, int iterations);
/** The polygon cut to the inside of a convex clip polygon. */
Polygon intersectConvex(const Polygon& p, std::span<const Vec2> convex);
/** Distance from q to a polyline, and the unit tangent there. */
double lineDistance(std::span<const Vec2> line, Vec2 q, Vec2* tangent = nullptr, bool closed = false);
/** Where the closed polyline a crosses segment p-q: parameters along p-q and arc length along a. */
void crossings(std::span<const Vec2> ring, Vec2 p, Vec2 q, std::vector<std::pair<double, double>>& out);
/** Runs f with every edge made from now on tagged era; edges split off older ones keep their era. */
void withEra(Layout& l, int era, const std::function<void()>& f);
/** Dead ends of the given edges run on to the street ahead when it is within reach (and not across water or too steep). */
void closeEnds(Layout& l, const Ground& g, const Profile& p, std::span<const int> edges, double reach);
/** A profile by name with overrides, or base when the name is empty. Throws std::runtime_error for an unknown name. */
Profile resolveProfile(const Profile& base, const std::string& name, const json& overrides);
} // namespace gen
} // namespace settlement
