#pragma once

#include "SettlementCore.h"

#include <cstdint>
#include <string>
#include <vector>

/**
 * Streetscape: road and sidewalk meshes, street furniture, the lane graph with transit routes, and its checks. All built
 * from a Layout and its RoadNetwork; nothing here changes the layout.
 */
namespace settlement
{
/** Ids made here fit in 52 bits, so they survive JSON readers that keep numbers as doubles. */
inline constexpr uint64_t kStreetscapeIdMask = (1ull << 52) - 1;

// ---------------------------------------------------------------------------------------------------------------------
// Road meshes

/** What a road is paved with. Medieval styles get cobble and packed earth with a gutter, the rest asphalt and paving. */
enum class Surface : uint8_t
{
    Asphalt,
    Cobble,
    Earth,
    Paving,
};

const char* surfaceName(Surface s);
bool parseSurface(std::string_view name, Surface* out);
/** True for the styles whose roads are cobble and earth without kerbs (hamlets, villages, medieval towns). */
bool medievalStyle(const Layout& l);
/** The surface a street of this class and profile takes in this layout. */
Surface streetSurface(const Layout& l, StreetClass c, const RoadProfile& p);

/** A road's cross-section at its own width: the profile scaled so its parts add up to the street's width. cm. */
struct RoadSection
{
    double carriage = 0.0; // half width of the driving surface, parking included
    double median = 0.0;   // full width
    double sidewalk = 0.0, verge = 0.0;
    bool kerbless = false; // medieval: one surface edge to edge, with a gutter
    Surface surface = Surface::Asphalt;
    double half() const { return carriage + sidewalk + verge; }
};

/** The section of a road network line; kerbless only where the style asks for it and allowKerbless. */
RoadSection roadSection(const Layout& l, const StreetLine& line, bool allowKerbless = true);

struct RoadMeshOptions
{
    double curb = 15.0;      // cm a sidewalk stands above the road
    double gutter = 8.0;     // cm the gutter of a kerbless street dips at its middle
    double segment = 500.0;  // cm between cross-sections along a road
    double lift = 2.0;       // cm above the ground
    bool markings = true;    // crosswalk stripes as mesh
    bool forceSurface = false;
    Surface surface = Surface::Asphalt; // every road, when forceSurface
};

/** A crosswalk marking as a decal: its middle, size across and along the road (cm), and heading (degrees about +Y). */
struct DecalPlacement
{
    glm::dvec3 p{0.0};
    double width = 0.0, depth = 0.0;
    double heading = 0.0;
    int junction = -1;
};

/**
 * The road surfaces (carriageways swept along each road, junction patches), sidewalks (kerb faces, sidewalks, medians,
 * verges and the curb returns rounding each corner) and crosswalk stripes. Face attributes: part (RoadPart), surface,
 * line (RoadNetwork line, -1 for junction pieces), junction (-1 along roads).
 */
struct RoadMeshes
{
    geo::Mesh roads, sidewalks, markings;
    std::vector<DecalPlacement> crosswalks;
    double roadArea = 0.0, junctionArea = 0.0, sidewalkArea = 0.0, curbReturnArea = 0.0, markingArea = 0.0; // m2, seen from above
    int curbReturns = 0;
};

enum class RoadPart : uint8_t
{
    Carriageway,
    Junction,
    Sidewalk,
    Curb,
    CurbReturn,
    Median,
    Verge,
    Marking,
};

RoadMeshes roadMeshes(const Layout& l, const RoadNetwork& net, const Ground& ground, const RoadMeshOptions& o = {});

// ---------------------------------------------------------------------------------------------------------------------
// Street furniture

/** One kind of furniture repeated along a road: where across it, how far apart, on which sides. */
struct FurnitureRule
{
    std::string kind;          // lamp, tree, hydrant, bench, bollard, bin, lantern...
    double spacing = 3000.0;   // cm along the road
    std::string where = "kerb"; // kerb, back (of the sidewalk), verge, median, edge (of a kerbless road)
    std::string sides = "both"; // both, alternate, right, left
    double minWidth = 0.0;     // cm the place must have (sidewalk, verge or median), else the rule is skipped there
};

/** The furniture a road profile carries, by profile name. Medieval styles use their own sets. */
struct FurnitureSet
{
    std::string profile;
    std::vector<FurnitureRule> rules;
};

/** The built-in sets: modern ones per road profile, and medieval ones (lanterns, no hydrants, signals or signs). */
std::vector<FurnitureSet> furnitureSets(bool medieval);
json toJson(const FurnitureSet& s);
/** Sets over base from {profile: [{kind, spacing, where, sides, minWidth}, ...]}. Throws std::runtime_error on bad data. */
std::vector<FurnitureSet> applyFurnitureJson(std::vector<FurnitureSet> base, const json& j);

/** A placed piece: stable id (by street, kind, side and slot, or junction and approach, or plaza and slot). */
struct FurniturePlacement
{
    uint64_t id = 0;
    std::string kind;
    glm::dvec3 p{0.0};
    double heading = 0.0; // degrees about +Y, 0 facing +Z
    uint64_t street = 0;  // 0 at junctions and plazas
    int junction = -1;
    int block = -1;       // plaza block
    std::string profile;  // road profile the rule came from; "junction" or "plaza" otherwise
};

/**
 * Furniture along every road from its profile's set, signals at signalised junctions (one per vehicle approach), stop
 * and yield signs by junction control, a street-name sign per junction, and squares furnished (medieval: a well, market
 * stalls and carts; modern: benches, trees and bollards).
 */
std::vector<FurniturePlacement> placeFurniture(const Layout& l, const RoadNetwork& net, const Ground& ground, const std::vector<FurnitureSet>& sets);

/**
 * The furniture sets a settlement uses: the built-in ones for its style, with edits["furniture"]["sets"] over them
 * ({profile: [rule, ...]}). Throws std::runtime_error on bad data.
 */
std::vector<FurnitureSet> furnitureSetsFor(const Layout& l, const json& edits);
/** The engine asset a kind is placed as: edits["furniture"]["assets"][kind], else /Game/Settlement/Furniture/SM_<Kind>. */
std::string furnitureAsset(const json& edits, const std::string& kind);
/**
 * Fills l.placements with the furniture (id as decimal text, asset, position, rotation about +Y as a quaternion, tags
 * settlement, furniture, the kind and the profile), for exporters and Unreal.
 */
void addFurniturePlacements(Layout& l, const Ground& ground, const json& edits);

// ---------------------------------------------------------------------------------------------------------------------
// Lane graph and transit

enum class LaneMode : uint8_t
{
    Vehicle,
    Pedestrian, // two-way: sidewalks, footpaths, and the edge of streets without sidewalks
};

const char* laneModeName(LaneMode m);

struct Lane
{
    int id = 0;
    LaneMode mode = LaneMode::Vehicle;
    int line = 0;     // RoadNetwork line
    int dir = 1;      // +1 along the line's points, -1 against; 0 for two-way pedestrian lanes
    int index = 0;    // 0 innermost (by the centre or median)
    int side = 0;     // pedestrian: +1 right of the line's direction, -1 left, 0 middle
    std::vector<glm::dvec3> pts; // in travel direction (line direction for pedestrian lanes)
    double width = 0.0;
    double speed = 0.0; // km/h
    double length = 0.0; // cm
    uint64_t street = 0;
    bool exit = false;  // ends at the settlement's edge: traffic leaves here
    bool entry = false; // starts at the settlement's edge: traffic arrives here
};

enum class Movement : uint8_t
{
    Through,
    Left,
    Right,
    UTurn,  // turning round at a dead end
    Corner, // pedestrian: round a corner
    Crosswalk,
    Crossing,   // pedestrian: across a road without a marked crosswalk
    LaneChange, // to the next lane the same way along a road
};

const char* movementName(Movement m);

/** A connection from the end of one lane to the start of another (pedestrian links go both ways). */
struct LaneLink
{
    int from = 0, to = 0;
    int junction = -1; // RoadNetwork junction, -1 at a plain node or dead end
    Movement move = Movement::Through;
    int phase = -1;      // signal phase it moves in, -1 unsignalled
    bool yields = false; // moves on green but gives way (left turns across an opposing approach)
};

struct TransitStop
{
    uint64_t id = 0;
    int lane = 0;
    glm::dvec3 p{0.0};
    std::string name;
};

/** A bus or tram route one way: lanes in order and its stops. Routes come in pairs, out and back. */
struct TransitRoute
{
    uint64_t id = 0;
    std::string mode; // bus or tram
    std::string name;
    std::string direction; // out or back
    std::vector<int> lanes;
    std::vector<TransitStop> stops;
    double length = 0.0; // cm
    bool reachable = false; // every lane leads to the next, and the far end leads back to the start
    std::string note;       // why it is missing or what it fell back to
};

struct LaneGraph
{
    std::vector<Lane> lanes;
    std::vector<LaneLink> links;
    std::vector<TransitRoute> routes;
    int surfaceLinesSkipped = 0; // walkable-surface paths: not in the street network
};

/**
 * Lanes per road per direction (two-way single-lane roads get one each way), pedestrian lanes on sidewalks, links for
 * allowed movements at junctions (no U-turns except at dead ends, minor alleys turn right only onto arterials at
 * unsignalised junctions), corners, crosswalks and unmarked crossings, each signalised movement with its phase.
 */
LaneGraph laneGraph(const Layout& l, const RoadNetwork& net);

/**
 * Settles each signalised junction's phases from its lane graph (skewed junctions' extra phases included) and gives
 * each phase its pedestrian walks, adding a pedestrian-only phase for crosswalks no vehicle phase leaves clear.
 * roadNetwork calls it, so every exporter sees the final phases.
 */
void finishSignals(RoadNetwork& net, const Layout& l);

struct TransitOptions
{
    int busRoutes = -1;  // -1: from the style (none in medieval styles, else one per two districts, at most 4)
    int tramRoutes = -1; // -1: one in cities with arterials, none otherwise
    double busStopSpacing = 40000.0;  // cm
    double tramStopSpacing = 60000.0; // cm
};

/**
 * Bus routes along arterials and collectors and tram routes along arterials' inner lanes, between districts across the
 * centre, out and back, stops by spacing. Appended to g.routes; each says whether it is reachable.
 */
void addTransit(LaneGraph& g, const Layout& l, const RoadNetwork& net, const TransitOptions& o = {});

/**
 * The lane graph's checks: per mode, strongly connected components (every lane reachable from every other), lanes that
 * cannot reach an exit and exits that cannot be reached; per signalised junction, pairs of protected movements green in
 * the same phase whose paths cross or merge; transit routes that are not reachable. ok is true when all are empty.
 * This is what settlementCheck reads.
 */
json checkLaneGraph(const LaneGraph& g, const RoadNetwork& net);
/** The lane graph of a layout with its transit, checked. */
json checkLanes(const Layout& l, const Ground& ground, const TransitOptions& o = {});

json toJson(const LaneGraph& g, bool lanes);
} // namespace settlement
