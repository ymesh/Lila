#pragma once

// Coast and water for open worlds: bathymetry blended under the land, beach, surf, shallows and reef masks, mangroves,
// navigable lagoon and estuary channels, reservoirs behind dams, and the boat and air route graphs with their checks.
// Everything here works in the heightfield's own space (lengths and heights cm on an unscaled terrain).

#include "Analysis.h"
#include "Inputs.h"
#include "Projection.h"
#include "Terrain.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace terrain
{
struct WaterResult;

// ---------------------------------------------------------------------------------------------------- craft

/** A boat or aircraft class that routes are sized for. Boats: draft and the channel it needs; aircraft: approach geometry. */
struct CraftClass
{
    std::string name;
    std::string label;
    /** boat or air. */
    std::string kind;
    /** Boats, cm: hull draft, the shallowest and narrowest channel it runs, and beam. */
    double draft = 0.0;
    double minDepth = 0.0;
    double minWidth = 0.0;
    double beam = 0.0;
    /** Cruise speed, m/s. */
    double speed = 0.0;
    /** Aircraft: approach angle and climb-out angle, degrees; clearance over the ground along the corridor, cm; corridor length and half width at its far end, cm. */
    double approachAngle = 0.0;
    double climbAngle = 0.0;
    double clearance = 0.0;
    double corridorLength = 0.0;
    double corridorHalfWidth = 0.0;
    /** Shortest runway, cm. */
    double runway = 0.0;
};

/** skiff, airboat, goFast, semiSubmersible, cutter (boats), then light and twin (aircraft). */
const std::vector<CraftClass>& craftClasses();
const CraftClass* craftClass(std::string_view name);
nlohmann::json craftJson(const CraftClass& c);

/** Exact Euclidean distance (in cells times cell) from every sample to the nearest set one; +inf when none is set. */
std::vector<float> distanceToSet(const std::vector<uint8_t>& set, int nx, int nz, double cell);
/** The sea: samples below level joined to the heightfield's edge (the sea mask when present and any is set). */
std::vector<uint8_t> seaOf(const Heightfield& h, double level);

// ---------------------------------------------------------------------------------------------------- bathymetry

/** How the seabed is made under the sea a land DEM leaves flat. */
struct BathymetrySpec
{
    /** A GEBCO grid (GeoTIFF, or anything readDem reads); empty uses the profile alone. */
    std::string file;
    /** Sea level, local height. */
    double seaLevel = 0.0;
    /** Land at or below seaLevel + landTolerance and joined to open sea counts as sea in the DEM. */
    double landTolerance = 50.0;
    /** Seabed slope off the shore before the grid takes over, degrees. */
    double shoreSlope = 1.5;
    /** Distance from the shore over which the profile hands over to the grid, cm. */
    double blendWidth = 30000.0;
    /** Blur of the grid, cm, smoothing its coarse cells into a shelf; 0 is the grid's own cell. */
    double smoothing = 0.0;
    /** Multiplies the grid's depths. */
    double depthScale = 1.0;
    /** Profile (and where the grid has no data): shelf depth at shelfWidth from the shore, then down to dropDepth over slopeWidth. */
    double shelfDepth = 3000.0;
    double shelfWidth = 200000.0;
    double dropDepth = 20000.0;
    double slopeWidth = 300000.0;
    /** Shallowest seabed anywhere off the shore, cm below sea level, so no sea sample stands dry. */
    double minDepth = 30.0;

    nlohmann::json toJson() const;
    static BathymetrySpec fromJson(const nlohmann::json& j);
};

struct BathymetryResult
{
    /** Samples replaced by seabed, and how many of them the grid (rather than the profile alone) shaped. */
    size_t seaSamples = 0;
    size_t gridSamples = 0;
    double maxDepth = 0.0;
    double meanDepth = 0.0;
    /** "grid", "profile" or "grid+profile": what made the seabed, so nothing falls back silently. */
    std::string source;
    std::vector<std::string> notes;
};

/**
 * Lays a seabed under the sea of h: samples at or below sea level (plus landTolerance) that reach open sea (the edge, or
 * a grid depth past a few metres) take the grid's depth blended in from a shoreline profile, so the shore is continuous
 * and the shelf smooth. grid may be null (profile only); georef places it. Writes mask seabed (1 where replaced).
 */
BathymetryResult applyBathymetry(Heightfield& h, const BathymetrySpec& s, const Dem* grid, const Georef* georef);

/** GEBCO's 15 arc-second grid: samples per degree. */
constexpr double kGebcoPerDegree = 240.0;
/** Estimated download for a box of a 15 arc-second float grid, bytes. */
uint64_t gebcoEstimateBytes(double west, double south, double east, double north);
/** The credit GEBCO's terms ask for. */
const char* gebcoAttribution();

// ---------------------------------------------------------------------------------------------------- coast

/** Beach, surf zone, shallows and reef masks. Depths cm below sea level, distances cm from the shore. */
struct CoastMasks
{
    bool enabled = true;
    double beachSlope = 10.0;
    double beachHeight = 300.0;
    /** How far below the water the beach runs. */
    double beachBelow = 150.0;
    double surfDepth = 250.0;
    double surfWidth = 4000.0;
    double shallowDepth = 600.0;
    double reefMinDepth = 100.0;
    double reefMaxDepth = 1500.0;
    double reefMinDistance = 2000.0;
    double reefMaxDistance = 60000.0;
    /** Share of the reef band that is reef, 0 to 1. */
    double reefCover = 0.35;
    double reefScale = 6000.0;
    uint32_t seed = 0;
};

/** Mangroves: land cover's mangrove class, or wetland in the tidal band, or the tidal band alone. */
struct MangroveSettings
{
    bool enabled = false;
    /** The band, cm relative to sea level, they grow in. */
    double tidalLow = -80.0;
    double tidalHigh = 150.0;
    /** Steepest ground, degrees. */
    double maxSlope = 6.0;
    /** The land cover mask (WorldCover codes / 255) and the code that is mangrove. */
    std::string landcover = "landcover";
    int code = 95;
    /** With no mangrove class, take wetland (landcover_wetland or WorldCover 90) in the band; and with neither, the band itself where it is sheltered. */
    bool fromWetland = true;
    bool fromBand = true;
    /** Sheltered: open water within this distance is under this share of the circle (lagoons, estuaries, not open beach). */
    double shelterRadius = 20000.0;
    double shelterMax = 0.45;
};

/** A navigable channel carved through a lagoon, estuary or mangrove: braided, sized for a craft. */
struct ChannelSpec
{
    std::string id;
    std::vector<glm::dvec2> path;
    std::string craft = "skiff";
    /** Strands that split from and rejoin the main one; 1 is a single channel. */
    int braids = 1;
    /** How far strands wander from the path, cm; 0 is three widths. */
    double braidSpread = 0.0;
    /** 0: the craft's minimum times margin. */
    double width = 0.0;
    double depth = 0.0;
    double margin = 1.3;
    /** Local height of the water; NaN is sea level. */
    double waterLevel = std::numeric_limits<double>::quiet_NaN();
    double bankAngle = 25.0;
    /** Smoothing passes over the banks after carving, so they settle into the ground. */
    int reErode = 4;
    /** Strand meander amplitude in widths. */
    double meander = 0.6;
    uint32_t seed = 0;

    nlohmann::json toJson() const;
    static ChannelSpec fromJson(const nlohmann::json& j);
};

/** One strand of a channel after carving, with what it measures at its narrowest and shallowest. */
struct ChannelStrand
{
    std::vector<glm::dvec2> points;
    /** Clear width (bed at least the craft's depth below the water) and depth, per point. */
    std::vector<double> width;
    std::vector<double> depth;
    double minWidth = 0.0;
    double minDepth = 0.0;
};

struct ChannelResult
{
    std::string id;
    std::string craft;
    double waterLevel = 0.0;
    double width = 0.0;
    double depth = 0.0;
    std::vector<ChannelStrand> strands;
    double cut = 0.0;
};

/** Where to look for places to hide a boat. */
struct StashSettings
{
    bool enabled = true;
    /** Overhung bank: ground this much above the water within reach of the channel edge, cm. */
    double bankHeight = 250.0;
    double bankReach = 600.0;
    /** Canopy: mangrove at least this dense beside the water. */
    double canopy = 0.5;
    /** Fewest cm between two candidates; most candidates. */
    double spacing = 8000.0;
    int maxCount = 64;
};

struct StashSpot
{
    std::string id;
    /** deadEnd, overhang or canopy. */
    std::string kind;
    glm::dvec3 at{0.0};
    std::string channel;
    double score = 0.0;
};

/** An authored lane: a harbour approach (auto-routed from at to the coast lanes) or a drawn lane. */
struct LaneSpec
{
    std::string id;
    /** harbour, channel, river, coast or air (air: see AirstripSpec). */
    std::string kind = "harbour";
    std::string craft = "goFast";
    std::vector<glm::dvec2> path;
    /** harbour: the berth; routed over water deep and wide enough for the craft out to the nearest boat lane. */
    std::optional<glm::dvec2> at;

    nlohmann::json toJson() const;
    static LaneSpec fromJson(const nlohmann::json& j);
};

/** An airstrip whose approach and departure corridors become air lanes. */
struct AirstripSpec
{
    std::string id;
    glm::dvec2 a{0.0};
    glm::dvec2 b{0.0};
    std::string aircraft = "light";
    /** Runway height, local; NaN is the ground at its middle. */
    double elevation = std::numeric_limits<double>::quiet_NaN();
    /** Corridors at both ends (else only past b: land toward b, take off toward b). */
    bool bothEnds = true;

    nlohmann::json toJson() const;
    static AirstripSpec fromJson(const nlohmann::json& j);
};

/** Lanes the coast makes for itself. */
struct RouteSettings
{
    /** Coast lanes along the shore where the sea is depth deep, for craft. */
    bool coastLanes = true;
    double coastDepth = 800.0;
    std::string coastCraft = "goFast";
    double minLaneLength = 20000.0;
    /** River reaches deep enough for at least a skiff. */
    bool riverLanes = true;
    /** Channel strands as lanes. */
    bool channelLanes = true;
    /** Lane ends closer than this join, cm. */
    double snap = 3000.0;
    /** A boat lane ending in open water with no lane near is linked over water to the nearest other lane within this, cm. */
    double linkReach = 300000.0;
    /** Lane point spacing, cm. */
    double spacing = 2000.0;
};

/** Everything the Terrain Coast node holds. */
struct CoastSpec
{
    double seaLevel = 0.0;
    CoastMasks masks;
    MangroveSettings mangroves;
    std::vector<ChannelSpec> channels;
    StashSettings stash;
    RouteSettings routes;
    std::vector<LaneSpec> lanes;
    std::vector<AirstripSpec> airstrips;

    nlohmann::json settingsJson() const;
    void readSettings(const nlohmann::json& j);
};

/** One lane of the route graph. Points are local (x, water level or altitude, z). */
struct Lane
{
    std::string id;
    /** coast, channel, river, harbour, approach or departure. */
    std::string kind;
    std::string craft;
    bool air = false;
    std::vector<glm::dvec3> points;
    /** Per point: clear width and depth (boats), or clearance over the ground (air), cm. */
    std::vector<double> width;
    std::vector<double> depth;
    std::vector<double> clearance;
    /** What it came from: a channel, river or airstrip id. */
    std::string source;
    /** Graph nodes at its ends, indices into CoastResult::nodes. */
    int from = -1;
    int to = -1;
    /** Connected network it belongs to (boats and aircraft apart); -1 before buildRoutes joins them. */
    int component = -1;
};

struct LaneNode
{
    std::string id;
    glm::dvec3 at{0.0};
    std::vector<int> lanes;
};

struct Runway
{
    std::string id;
    std::string aircraft;
    glm::dvec3 a{0.0};
    glm::dvec3 b{0.0};
    double length = 0.0;
};

struct CoastResult
{
    double seaLevel = 0.0;
    std::vector<ChannelResult> channels;
    std::vector<StashSpot> stash;
    std::vector<Lane> lanes;
    std::vector<LaneNode> nodes;
    std::vector<Runway> runways;
    /** Harbour lanes no water route reaches a boat lane from: id and berth. */
    std::vector<std::pair<std::string, glm::dvec3>> unrouted;
    /** The boat network most lanes belong to. */
    int mainComponent = -1;
    /** Area of each mask written, m2 on an unscaled terrain. */
    double beachArea = 0.0, surfArea = 0.0, shallowArea = 0.0, reefArea = 0.0, mangroveArea = 0.0;
    /** worldcover, wetland, band or none: where the mangroves came from. */
    std::string mangroveSource = "none";
    std::vector<std::string> warnings;
    std::vector<std::string> notes;
};

/** Mask names the coast writes. */
const std::vector<std::string>& coastMasks();

/** Beach, surf, shallows and reef from the shore of the sea mask (or below seaLevel joined to the edge). */
void applyCoastMasks(Heightfield& h, double seaLevel, const CoastMasks& s, CoastResult& r);
/** Writes tidal (the band) and mangrove; sets r.mangroveSource and notes what it used. */
void applyMangroves(Heightfield& h, double seaLevel, const MangroveSettings& s, CoastResult& r);
/** Carves one channel; writes masks channel and water. */
ChannelResult carveChannel(Heightfield& h, const ChannelSpec& s, double seaLevel, std::vector<std::string>& warnings);
/** Measures clear width and depth along a strand for a craft's depth. */
void measureStrand(const Heightfield& h, ChannelStrand& s, double waterLevel, double craftDepth, double searchWidth);
void findStashSpots(const Heightfield& h, const StashSettings& s, CoastResult& r);

/** Everything the Terrain Coast node does, in order: channels, masks, mangroves, stash spots, then the route graph. */
CoastResult applyCoast(Heightfield& h, const CoastSpec& spec, const WaterResult* water);

// ---------------------------------------------------------------------------------------------------- dams

/** A dam across a valley holding a reservoir, with a spillway notch and the river below. */
struct DamSpec
{
    std::string id;
    /** The dam's ends on the ground. */
    glm::dvec2 a{0.0};
    glm::dvec2 b{0.0};
    /** Crest height, local; NaN is the lower end's ground. */
    double crest = std::numeric_limits<double>::quiet_NaN();
    /** Crest thickness and the slope of its faces (horizontal per vertical). */
    double thickness = 800.0;
    double faceSlope = 1.5;
    /** Spillway notch width and its sill below the crest, cm. */
    double spillwayWidth = 2000.0;
    double freeboard = 300.0;
    /** Where along the dam (0 at a, 1 at b) the spillway is. */
    double spillwayAt = 0.85;

    nlohmann::json toJson() const;
    static DamSpec fromJson(const nlohmann::json& j);
    double sill() const { return crest - freeboard; }
};

struct DamResult
{
    std::string id;
    double crest = 0.0;
    double sill = 0.0;
    double height = 0.0;
    double length = 0.0;
    glm::dvec2 spillway{0.0};
    /** Downstream unit direction across the dam (from the higher water side). */
    glm::dvec2 downstream{0.0};
    double fill = 0.0;
};

/** Raises each dam into h (mask dam) and notches its spillway; resolves auto crests. */
std::vector<DamResult> applyDams(Heightfield& h, std::vector<DamSpec>& dams);
/** Which side of the line a-b the valley falls toward: the side whose ground is lower within reach. */
glm::dvec2 downstreamSide(const Heightfield& h, glm::dvec2 a, glm::dvec2 b, double reach);
/** Steepest descent from start for up to maxLength, stopping at water (masks sea, water) or the edge. */
std::vector<glm::dvec2> traceDownhill(const Heightfield& h, glm::dvec2 start, double maxLength);

// ---------------------------------------------------------------------------------------------------- routes

/** Builds r.lanes and r.nodes from the coast, channels, rivers and authored lanes and airstrips. */
void buildRoutes(const Heightfield& h, const CoastSpec& spec, const WaterResult* water, CoastResult& r);
/** A path over water at least minDepth deep with minWidth clear from start to the nearest goal sample; empty when none. */
std::vector<glm::dvec2> routeOverWater(const Heightfield& h, double waterLevel, double minDepth, double minWidth, glm::dvec2 start,
                                       const std::vector<glm::dvec2>& goals, double maxLength);

/**
 * waterCheck's issues: channelShallow, channelNarrow (strands under their craft's minimum), laneShallow, laneNarrow
 * (lanes under their craft's), laneOverLand (any point on dry ground), laneDisconnected (a boat lane not joined to the
 * largest network), airClearance (a corridor point lower over the ground than the aircraft's clearance), shortRunway.
 * Each {type, about, position (local), message}.
 */
nlohmann::json routeIssues(const Heightfield& h, const CoastResult& r);

/** Lanes and nodes as JSON in world space (format lila.waterRoutes), with a ZoneGraph-style lane-profile block. */
nlohmann::json routesJson(const CoastResult& r, const WorldMap& w);
/** Channels (as water splines with width, depth, waterLevel), stash spots and dams as locators, format lila.terrainWater. */
nlohmann::json coastWaterJson(const CoastResult& r, const std::vector<DamResult>& dams, const WorldMap& w);

/** What the coast nodes of a modifier made on their last evaluation. */
void publishCoast(uint64_t modifier, const std::string& node, CoastResult r);
bool publishedCoast(uint64_t modifier, const std::string& node, CoastResult* out);
void publishDams(uint64_t modifier, const std::string& node, std::vector<DamResult> r);
bool publishedDams(uint64_t modifier, const std::string& node, std::vector<DamResult>* out);
void publishBathymetry(uint64_t modifier, const std::string& node, BathymetryResult r);
bool publishedBathymetry(uint64_t modifier, const std::string& node, BathymetryResult* out);

/** The bathymetry, dam and coast nodes, their commands, spec step, Level menu and toolkit entries. */
void registerCoast(lila::ext::Context& ctx);
void registerCoastUi(lila::ext::Context& ctx);
/** Draw Channel, Draw Dam and Draw Airstrip: viewport tools that click the feature onto the terrain. */
void registerCoastTools(lila::ext::Context& ctx);
/** terrainCheck's coast issues (waterCheck's) for a terrain; empty when it has no coast. */
nlohmann::json coastIssues(const std::string& terrain);
/** Writes <name>_coast.json and <name>_routes.json next to a terrain export; null when the terrain has no coast. */
nlohmann::json exportTerrainCoast(const std::string& terrain, const std::filesystem::path& folder, const std::string& name);
/** Sends channels, stash spots and dams as water splines and locators, and the routes, to Unreal; null with no coast. */
nlohmann::json sendTerrainCoast(const std::string& terrain, double timeoutSeconds);
} // namespace terrain
