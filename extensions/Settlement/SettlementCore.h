#pragma once

#include <lila/Mesh.h>

#include <nlohmann/json.hpp>

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * Settlement layout: streets, blocks, lots and building envelopes on a plane or any ground mesh. Layout happens in the
 * XZ plane (Vec2 x is world X, y is world Z); heights come from the Ground. Lengths are cm, areas in profiles are m2.
 */
namespace settlement
{
namespace geo = lila::geo;
using Vec2 = glm::dvec2;
using json = nlohmann::json;

inline constexpr double kPi = std::numbers::pi;
inline constexpr double kM2 = 1.0e4; // cm2 per m2

inline double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline Vec2 leftNormal(Vec2 d) { return {-d.y, d.x}; }

// ---------------------------------------------------------------------------------------------------------------------
// Polygons

/** A simple polygon in XZ, counter-clockwise (positive signedArea). tag per side: side k runs from pts[k] to pts[k + 1]. */
struct Polygon
{
    std::vector<Vec2> pts;
    std::vector<int> tag;

    size_t size() const { return pts.size(); }
};

double signedArea(std::span<const Vec2> pts);
double area(const Polygon& p);
Vec2 centroid(const Polygon& p);
bool contains(std::span<const Vec2> pts, Vec2 q);
/** Keeps the part of p where dot(q - origin, normal) >= 0. Sides along the cut get cutTag. */
Polygon clipHalfPlane(const Polygon& p, Vec2 origin, Vec2 normal, int cutTag = -1);
/**
 * Moves each side inward by its own distance. Concave corners are mitred; when that folds the polygon over itself the
 * result is the intersection of the inset half-planes instead, which is exact for convex polygons. Empty when nothing
 * is left.
 */
Polygon inset(const Polygon& p, std::span<const double> distances);
bool selfIntersects(std::span<const Vec2> pts);
/** Closed polygon from points, made counter-clockwise, duplicate and collinear points dropped. */
Polygon makePolygon(std::vector<Vec2> pts, int tag = -1);
/** Distance from q to the polygon's boundary. */
double boundaryDistance(std::span<const Vec2> pts, Vec2 q);
/** True when the segment a-b crosses or touches any side of the polygon. */
bool segmentCrosses(std::span<const Vec2> pts, Vec2 a, Vec2 b);
/** Distance from the segment a-b to the polygon's boundary, 0 where they cross. */
double segmentDistance(std::span<const Vec2> pts, Vec2 a, Vec2 b);
/** The polygon grown outward by margin (mitred where that holds, else traced from a raster). Empty when it has no area. */
Polygon grow(const Polygon& p, double margin);

/** A boolean raster over the XZ plane, for footprints of meshes and curves. */
struct Mask
{
    Vec2 origin{0.0};
    double cell = 100.0;
    int w = 0, h = 0;
    std::vector<uint8_t> on;

    static Mask around(Vec2 lo, Vec2 hi, double cell, double pad);
    void fillTriangle(Vec2 a, Vec2 b, Vec2 c);
    void fillSegment(Vec2 a, Vec2 b);
    void fillPolygon(std::span<const Vec2> pts);
    /** Sets every cell within r of a set one. */
    void dilate(double r);
    /** Outer outlines of the set cells, holes filled, simplified to within about a cell. */
    std::vector<Polygon> outlines() const;
};

/** What a mesh covers seen from above, grown by margin: one polygon per separate piece. */
std::vector<Polygon> meshFootprint(const geo::Mesh& mesh, double margin, double cell = 0.0);
/** A corridor of half-width margin along polylines, as polygons. */
std::vector<Polygon> lineFootprint(std::span<const std::vector<Vec2>> lines, double margin, double cell = 0.0);

// ---------------------------------------------------------------------------------------------------------------------
// Ground

/**
 * What streets and buildings stand on: a plane, or a mesh baked to a height grid (its top surface) for routing and slope,
 * with the mesh kept for exact heights where geometry is draped on it.
 */
class Ground
{
public:
    static Ground plane(double height = 0.0);
    /**
     * Heights over the mesh's XZ bounds at about cell cm spacing. The grid grows with the mesh up to maxSide a side (so a
     * 10 km terrain keeps cells near 2.5 m); past that the cells widen.
     */
    static Ground fromMesh(const geo::Mesh& mesh, double cell = 100.0, int maxSide = 4096);

    double height(Vec2 p) const;
    /** The mesh's own top surface at p (a raycast), else the grid height. For draping ribbons and outlines. */
    double surfaceHeight(Vec2 p) const;
    double cellSize() const { return mCell; }
    int gridWidth() const { return mW; }
    int gridHeight() const { return mH; }
    /** Rise over run at p, from central differences over one cell. */
    double grade(Vec2 p) const;
    /** Grade along the segment a-b, the steepest of a few samples. */
    double gradeAlong(Vec2 a, Vec2 b) const;
    /** False off the ground mesh, or where it has holes. Always true on a plane. */
    bool solid(Vec2 p) const;
    bool isPlane() const { return mHeights.empty(); }

private:
    double mPlane = 0.0;
    Vec2 mOrigin{0.0};
    double mCell = 100.0;
    int mW = 0, mH = 0;
    std::vector<float> mHeights; // NaN where the mesh has no surface
    std::shared_ptr<const geo::MeshBvh> mBvh;
    double mTop = 0.0, mDepth = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Walkable surfaces: ramps, terraces and floors of a structure, any number of levels

struct WalkOptions
{
    double maxSlope = 35.0; // degrees: steeper faces are walls
    double step = 60.0;     // cm: pieces this close in height (and gap apart) join, as a step or a threshold
    double gap = 100.0;
    double spacing = 400.0; // cm between route points along mesh edges
};

/**
 * A walkable mesh as a graph over its upward faces: their corners and points along their edges, every pair on a face
 * joined. Routes on it are shortest paths that stay on the surface, from floor to ramp to terrace.
 */
class WalkSurface
{
public:
    /** A point on a walkable face. */
    struct Spot
    {
        glm::dvec3 p{0.0};
        int face = -1;
    };

    static WalkSurface fromMesh(const geo::Mesh& mesh, const WalkOptions& o = {});

    bool empty() const { return mNodes.empty(); }
    size_t nodeCount() const { return mNodes.size(); }
    glm::dvec3 position(int node) const { return mNodes[size_t(node)]; }
    /** The walkable point nearest p within reach. False when there is none. */
    bool locate(glm::dvec3 p, double reach, Spot* out) const;
    /** Shortest walk from a to b on the surface, both included (simplified). Empty when they are not connected. */
    std::vector<glm::dvec3> route(const Spot& a, const Spot& b) const;
    /** Shortest walk from a to a node. */
    std::vector<glm::dvec3> route(const Spot& a, int node) const;
    /** Walk lengths from a to every node (infinity where unreachable). */
    std::vector<double> distances(const Spot& a) const;

private:
    std::vector<glm::dvec3> mNodes;
    std::vector<std::vector<std::pair<int, float>>> mLinks;
    std::vector<std::vector<int>> mRings; // per walkable face (the BVH's face ids), the nodes around it
    std::shared_ptr<const geo::MeshBvh> mBvh;
    std::vector<glm::dvec3> search(const Spot& a, const Spot* b, int node, std::vector<double>* all) const;
};

// ---------------------------------------------------------------------------------------------------------------------
// Street graph

enum class StreetClass : uint8_t
{
    Arterial,  // main roads between the centre, gates and attractors
    Collector, // streets that grow off them
    Local,
    Alley,
    Path,     // footpaths, stairs and service walks: people, not vehicles
    Boundary, // the settlement's outline and its exclusions: closes blocks, is not a street
    Water,    // a river or canal: closes blocks, only arterials bridge it
};

const char* className(StreetClass c);
bool parseClass(std::string_view name, StreetClass* out);

struct StreetNode
{
    Vec2 p{0.0};
    bool alive = true;
};

struct StreetEdge
{
    int a = -1, b = -1;
    StreetClass cls = StreetClass::Local;
    int era = 0;
    bool alive = true;
    double width = 0.0; // overrides the class's width when above 0 (rivers, given widths)
    int profile = -1;   // road profile (Layout::roads), -1 for the class's
    int path = -1;      // the edit (street or route) it was laid for, -1 when generated
    uint64_t street = 0; // stable id of the street it is part of (streetId), 0 until the layout is finished
    bool bridge = false; // carried over water or a gap: graded as a span, never as a cut through the bank
    bool tunnel = false;
    int layer = 0;       // 0 the ground; k a level above it (Layout::gen levels[k - 1])
    bool moat = false;   // water dug as a moat
};

/** Planar street network: inserting a street splits every street it crosses and snaps its ends to what is near. */
class StreetGraph
{
public:
    StreetGraph(double snap = 150.0, double cell = 2000.0);

    std::vector<StreetNode> nodes;
    std::vector<StreetEdge> edges;

    double snap() const { return mSnap; }
    /** Adds the segment p-q, split where it crosses or touches existing edges. Returns the edges made, in order p to q. */
    std::vector<int> addStreet(Vec2 p, Vec2 q, StreetClass cls, int era = 0, double width = 0.0);
    std::vector<int> addPolyline(std::span<const Vec2> pts, StreetClass cls, int era = 0, bool closed = false, double width = 0.0);
    /** As above, every edge made a copy of like (class, era, width, profile, path). */
    std::vector<int> addStreet(Vec2 p, Vec2 q, const StreetEdge& like);
    std::vector<int> addPolyline(std::span<const Vec2> pts, const StreetEdge& like, bool closed = false);

    /** Nearest node within r, or -1. */
    int nearestNode(Vec2 p, double r) const;
    /** Nearest alive edge within r: its id, the closest point and the parameter along it. -1 if none. */
    int nearestEdge(Vec2 p, double r, Vec2* at = nullptr, double* t = nullptr, int skipNode = -1) const;
    /** The first edge the segment p-q crosses, by distance from p: edge, point, parameter along p-q. -1 if none. */
    int firstCrossing(Vec2 p, Vec2 q, Vec2* at, double* s, int skipNodeA = -1, int skipNodeB = -1) const;

    int degree(int node, bool streetsOnly = true) const;
    /** Edges at a node (alive), any class. */
    const std::vector<int>& incident(int node) const { return mIncident[size_t(node)]; }
    double length(int edge) const;
    bool isStreet(int edge) const { return edges[size_t(edge)].alive && edges[size_t(edge)].cls < StreetClass::Boundary; }
    /** Splits the edge at parameter t, returning the new node. */
    int splitEdge(int edge, double t);
    void removeEdge(int edge);
    /** Rebuilds the junction lists and spatial hashes after nodes and edges were set directly. */
    void reindex();

private:
    double mSnap;
    double mCell;
    std::vector<std::vector<int>> mIncident;
    std::unordered_map<int64_t, std::vector<int>> mEdgeCells;
    std::unordered_map<int64_t, std::vector<int>> mNodeCells;

    int addNode(Vec2 p);
    int nodeAt(Vec2 p);
    int addEdge(int a, int b, const StreetEdge& like);
    int64_t key(int x, int y) const { return (int64_t(x) << 32) ^ int64_t(uint32_t(y)); }
    template <class F> void forCells(Vec2 lo, Vec2 hi, F&& f) const;
};

/** A bounded face of the street graph: its outline (tag = edge id per side) and its block id. */
struct Face
{
    Polygon outline;
};

/** Every bounded face, dead-end spikes removed. */
std::vector<Face> planarFaces(const StreetGraph& g);
/** The bounded faces either side of the given edges only (each once). */
std::vector<Face> planarFaces(const StreetGraph& g, const std::vector<int>* fromEdges);

// ---------------------------------------------------------------------------------------------------------------------
// Profiles: what makes a medieval town different from Manhattan

enum class Pattern : uint8_t
{
    Organic,     // roads to the centre from gates and attractors, streets growing off them
    Grid,
    Radial,      // rings and spokes
    Linear,      // one main street through, lots and lanes off it
    OrganicGrid, // an organic core inside a ring road, a grid beyond it
    None,        // no generated streets: only the given streets, roads and routes
    Tensor,      // streets traced along a tensor field: grids, rings, coasts and contours blended
    Contour,     // streets along the contours of the ground, cross streets and switchbacks between, growth on the flat
    Osm,         // imported streets as they are, joined and cut into blocks
    Example,     // grown to match another street network's numbers
};

const char* patternName(Pattern p);
bool parsePattern(std::string_view name, Pattern* out);

/**
 * What makes a place a good site, as weighted terms: flat (gentle slope), water (close to a river, lake or sea), trade
 * (crossings, fords, bridge sites, confluences, harbours), defense (prominent, defensible high ground), resource (the
 * kind's own: farmland, quarries, harbours, mill races) and climate (sunny, sheltered). flood scales the score down by
 * flood risk. kind names the preset the weights started from (sitingKinds). Read and written as JSON.
 */
struct SitingRules
{
    std::string kind = "town";
    double flat = 0.45, water = 0.4, trade = 0.15, defense = 0.0, resource = 0.1, climate = 0.05;
    double flood = 0.8;
};

/** Every rule a style sets. Lengths cm, areas m2. Read and written as JSON (field names as here). */
struct Profile
{
    std::string name = "medievalTown";
    Pattern pattern = Pattern::Organic;

    double widthArterial = 900.0, widthCollector = 650.0, widthLocal = 450.0, widthAlley = 250.0, widthPath = 200.0;
    int gates = 4;            // arterials leave through this many points on the outline
    double segment = 2500.0;  // growing street segment length
    double turn = 18.0;       // degrees a growing street may bend per segment
    double minAngle = 35.0;   // degrees between streets at a junction
    double spacing = 2200.0;  // streets closer than this side by side are refused
    double maxGrade = 0.12;   // rise over run; steeper streets turn along the slope
    double terraceGrade = 0.5; // rise over run: streets and lots up to this are still laid, cut into terraces
    double density = 1.0;     // street length per area, relative to the pattern's own
    double falloff = 1.6;     // how fast streets and heights thin out from the centre
    double maxBlock = 6000.0; // m2: larger blocks are cut by a lane
    bool plaza = true;        // a market square at the centre

    double gridAngle = 0.0;   // degrees
    double gridWidth = 9000.0, gridDepth = 9000.0; // block size along and across the grid, cm
    int arterialEvery = 4;
    double coreRadius = 0.45; // OrganicGrid: core radius as a fraction of the settlement radius
    int rings = 4, spokes = 8; // Radial

    double lotWidth = 700.0, lotDepth = 3000.0; // street frontage and depth
    double minLot = 60.0;                       // m2: smaller pieces become open ground
    double frontSetback = 0.0, sideSetback = 0.0, buildDepth = 1400.0;
    int minFloors = 2, maxFloors = 4;
    double floorHeight = 320.0;
    double foundation = 50.0; // cm below the lowest ground under a footprint
    std::string roof = "gable";
    double commercial = 0.35; // lots on main streets within this fraction of the radius are commercial
    std::string era;                // medieval, early, industrial, modern or future; empty takes it from the style's name and pattern
    std::string blockType = "auto"; // the type ordinary blocks take (blockTypeName), auto chooses by era and block shape
    std::string lotMethod = "auto"; // how ordinary blocks are cut into lots (lotMethodName), auto chooses by block type and era
    double windAngle = 0.0;         // degrees: the prevailing wind blows toward this heading (0 is +X); industry goes downwind
    SitingRules siting;

    // Ranges the street metrics should land in (Boeing 2019 measures), checked by targetReport.
    double targetEntropyMin = 0.0, targetEntropyMax = 3.6;
    double targetDeadEndMin = 0.0, targetDeadEndMax = 1.0;
    double targetFourWayMin = 0.0, targetFourWayMax = 1.0; // share of intersections where four or more streets meet
    double targetBlockMin = 0.0, targetBlockMax = 1.0e9;    // median block area, m2
};

/** Steepest ground a street or lot is laid on: mult times the style's maxGrade, or its terraceGrade where that is more. */
inline double steepLimit(const Profile& p, double mult) { return std::max(mult * p.maxGrade, p.terraceGrade); }

/** The built-in styles, by name. */
std::vector<std::string> profileNames();
/** A built-in style; false for an unknown name. */
bool builtinProfile(std::string_view name, Profile* out);
json toJson(const Profile& p);
/** Applies the fields present in j over p. Throws std::runtime_error naming an unknown field or a bad value. */
void applyJson(Profile& p, const json& j);
double streetWidth(const Profile& p, StreetClass c);

/**
 * A street's cross-section, what lane graphs and road meshes are made from: driving lanes (both directions together),
 * then a median, sidewalks and verges on each side. Widths cm, speed km/h.
 */
struct RoadProfile
{
    std::string name = "street";
    int lanes = 2;
    double laneWidth = 325.0;
    double median = 0.0;
    double sidewalk = 0.0;
    double verge = 0.0;
    double parking = 0.0; // a parking lane each side, between the driving lanes and the sidewalk
    bool oneWay = false;
    double speed = 50.0;

    double width() const { return lanes * laneWidth + median + 2.0 * (parking + sidewalk + verge); }
};

/** highway, avenue, street, parkedStreet, lane, alley, serviceRoad, track, footpath, stairs. */
std::vector<std::string> roadProfileNames();
bool builtinRoadProfile(std::string_view name, RoadProfile* out);
/** The profile streets of a class take when they name none. */
const char* defaultRoadProfile(StreetClass c);
json toJson(const RoadProfile& p);
/** Applies the fields present in j over p. Throws std::runtime_error for an unknown field or a bad value. */
void applyJson(RoadProfile& p, const json& j);

/**
 * A cross-section as the slots of a road shape grammar, left to right looking along the road: Verge, Sidewalk,
 * Parking, Lane (one per lane), Median, then the same back out. Widths cm; zero-width slots are left out.
 */
struct CrossSlot
{
    std::string symbol;
    double size = 0.0;
};

std::vector<CrossSlot> crossSection(const RoadProfile& p);
/** The grammar string for the slots, as PCG's Subdivide Segment reads it: [Verge,Sidewalk,Lane,Lane,Sidewalk,Verge]. */
std::string grammarOf(std::span<const CrossSlot> slots);
/** The symbols a grammar string lays out, in order, with [A,B]N repeats expanded (* repeats count once). */
std::vector<std::string> grammarSymbols(std::string_view grammar);
/**
 * A profile from the slots of an existing road definition, by what each symbol says it is (lane, drive, carriageway;
 * median, divider; parking; sidewalk, pavement, footway, curb; verge, grass, planter, shoulder). Symbols it cannot place
 * count as verge and are listed in unknown.
 */
RoadProfile profileFromSlots(std::string name, std::span<const CrossSlot> slots, std::vector<std::string>* unknown = nullptr);

// ---------------------------------------------------------------------------------------------------------------------
// Layout

/** A river, stream or canal as a centreline. */
struct WaterLine
{
    std::vector<Vec2> pts;
    double width = 2000.0;
    std::string kind; // empty for natural water; canal, moat, quay or river for water the settlement builds
};

/** A region nothing is laid in: no street, block, lot or building. The space a structure stands in, a ruin, a lake. */
struct Exclusion
{
    int id = -1;
    std::string name, source;
    std::vector<Vec2> pts; // closed
    double margin = 0.0;   // cm kept clear around it
};

enum class PathMode : uint8_t
{
    Ground,  // on the ground, in the street network
    Surface, // on the walkable surface: ramps, terraces, floors
    Auto,    // each stop where it is: given heights on the walkable surface go there, the rest on the ground
};

const char* pathModeName(PathMode m);
bool parsePathMode(std::string_view name, PathMode* out);

/**
 * A street the user draws (points used as they are) or a route between stops (each leg the cheapest way over the ground
 * or the walkable surface). Points are world XYZ; y is NaN where only [x, z] was given.
 */
struct SitePath
{
    int id = -1;
    std::string name;
    bool routed = false;
    std::vector<glm::dvec3> pts;
    StreetClass cls = StreetClass::Collector;
    std::string profile; // road profile name; empty for the class's
    double width = 0.0;  // overrides the profile's width when above 0
    PathMode mode = PathMode::Auto;
};

/**
 * A change to a generated street, found by its stable id (streetId): removed, or another class, profile or width. A
 * street the layout no longer makes leaves the override unused (checks report it as staleOverrides).
 */
struct StreetOverride
{
    uint64_t street = 0;
    bool remove = false;
    bool hasClass = false;
    StreetClass cls = StreetClass::Local;
    std::string profile;
    double width = 0.0;
    std::string name; // the street's name, over the naming culture's
};

/** Generated street ids carry this bit, so they never collide with edit ids. */
inline constexpr uint64_t kStreetIdBit = 1ull << 48;

/** What a block is for, which decides how it is cut into lots and what stands on them. */
enum class BlockType : uint8_t
{
    Perimeter,  // buildings along every street side, filling the block
    Courtyard,  // a ring of buildings along the streets around an open courtyard
    Open,       // detached buildings standing free on their lots
    Row,        // row houses sharing party walls
    Superblock, // a few very large lots: towers in open ground
    Park,
    Plaza,
    Market,     // the market square
    Churchyard, // the church, its graveyard around it
    Field,      // farm strips outside the town
};

/** How a block is cut into lots. Every method gives each lot a street side. */
enum class LotMethod : uint8_t
{
    Frontage, // lots along the longest street side, back to back when deep
    Obb,      // recursive splits across the long side of each piece's oriented box
    Skeleton, // strips off each street side along the straight skeleton, then cut across
    Burgage,  // narrow deep plots behind the main street
    Voronoi,  // irregular cells around seeds along the streets
    Strips,   // long farm strips
    Whole,    // the block is one lot
};

/** What builds the buildings on a lot. */
enum class BuildingRoute : uint8_t
{
    Outline, // envelopes only: footprint curves and the envelope volume
    Massing, // greybox blocks with roofs from the envelopes
    Library, // prebuilt assets placed through Stagehand
    Masonry, // stone buildings through Masonry
    Unreal,  // envelopes sent as PCG data for Unreal to build
};

/** A change to a block found by its stable id: another type, another lot method. */
struct BlockOverride
{
    uint64_t block = 0;
    bool hasType = false, hasMethod = false;
    BlockType type = BlockType::Perimeter;
    LotMethod method = LotMethod::Frontage;
};

/** A change to a lot found by its stable id: another use, or reserved as a landmark under a name. */
struct LotOverride
{
    uint64_t lot = 0;
    bool hasUse = false;
    uint8_t use = 0; // Use
    std::string landmark;
};

/** The building route of the whole settlement (no polygon) or of the lots whose centre lies in the polygon. */
struct RouteRegion
{
    BuildingRoute route = BuildingRoute::Outline;
    std::string name;
    std::vector<Vec2> polygon;
};

/** The library asset settlementBuildings fitted to a lot: its id, its engine path, and the heading its front faces at rest (degrees). */
struct LibraryPick
{
    std::string asset, unreal;
    double assetHeading = 0.0;
};

/** Ground under a lot's footprint as measured on a Terrain (terrainRegionStats), world Y. */
struct Foundation
{
    double min = 0.0, max = 0.0;
};
struct RouteBook;
struct PlanSnapshot;

/** What the user gives: an outline, fixed streets and roads, water, a centre and attractors, exclusions and edits. */
struct Site
{
    std::vector<Vec2> outline;                 // closed; empty for a square of size
    double size = 40000.0;                     // cm, the square's side when there is no outline
    std::vector<std::vector<Vec2>> streets;    // kept as they are; generators fill around them
    std::vector<std::vector<Vec2>> roads;      // main roads arriving from outside, kept as arterials
    std::vector<WaterLine> water;              // streets other than arterials stop at its banks
    std::vector<Vec2> attractors;              // the first is the centre when hasCenter
    bool hasCenter = false;
    std::vector<Exclusion> exclusions;
    std::vector<SitePath> paths;
    std::vector<RoadProfile> roadProfiles; // custom profiles paths may name, besides the built-in ones
    std::vector<StreetOverride> overrides;
    std::string culture; // naming culture (cultureNames); empty for the profile's
    std::vector<std::pair<uint64_t, std::string>> districtNames; // given district names, by district id
    std::vector<BlockOverride> blockOverrides;
    std::vector<LotOverride> lotOverrides;
    std::vector<RouteRegion> buildingRoutes;              // later ones win where they overlap
    std::unordered_map<uint64_t, Foundation> foundations; // by stable lot id (settlementFoundations)
    std::unordered_map<uint64_t, LibraryPick> libraryPicks; // by stable lot id (settlementBuildings route=library)
    std::shared_ptr<RouteBook> routeBook;        // main roads routed over a Terrain (SettlementTerrain.h)
    std::shared_ptr<const PlanSnapshot> plan;    // the pinned plan, while only the ground changed
    uint64_t editsKey = 0;                              // hash of the edits besides the plan and terrain records
};

/**
 * The edits a settlement carries as JSON (its edits input): {"next": id, "streets": [...], "routes": [...],
 * "exclusions": [...], "streetOverrides": [...], "roadProfiles": {name: profile}}. Each street {id, name, points, class,
 * profile, width, mode}; each route the same with stops for points; each exclusion {id, name, polygon, margin, source};
 * each override {street, remove, class, profile, width}. Throws std::runtime_error naming what is wrong.
 */
void applyEdits(Site& site, const json& edits);

struct Options
{
    Profile profile;
    uint32_t seed = 1;
    double growth = 1.0;      // 0 to 1: how far the settlement has grown
    double heightScale = 1.0;
    double widthScale = 1.0;
    double lotScale = 1.0;
    bool blocks = true;    // false: streets and paths only, no blocks, lots or buildings
    bool buildings = true; // false: blocks and lots without building envelopes
    WalkOptions walk;
    json generators = json::object(); // tensor fields, eras, districts, walls, imported streets, levels (Generators.h)
};

enum class Use : uint8_t
{
    Residential,
    Commercial,
    Civic,
    Plaza,
    Open, // too small or too odd to build on
    Market,
    Religious,
    Industrial,
    Port,
    Farm,
    Park,
    Cemetery,
};

const char* useName(Use u);

struct Block
{
    int id = 0;
    Polygon outline; // inside the street edges; tag = class of the street on that side, -1 none
    double area = 0.0;
    Use use = Use::Residential;
    uint64_t key = 0; // stable id: the same block keeps it while its streets stay
    BlockType type = BlockType::Perimeter;
    LotMethod method = LotMethod::Frontage;
    std::vector<Polygon> courtyards; // open ground inside a courtyard block, in no lot
    bool water = false;              // a side on a river or canal
    std::vector<std::string> reasons;
    int era = 0;
};

struct Lot
{
    int id = 0;
    int block = 0;
    Polygon outline; // tag = class of the street the side faces, -1 none
    double area = 0.0;
    int frontages = 0;
    bool corner = false;
    StreetClass frontClass = StreetClass::Local;
    Use use = Use::Residential;
    float random = 0.0f;
    uint64_t key = 0;   // stable id while its block and the block's lot method stay
    int wealth = 1;     // residential tier: 0 low, 1 middle, 2 high
    double access = 0.0; // 0 to 1: integration and betweenness of the streets it faces
    std::string landmark; // a reserved landmark's name (market_square, church, castle, axis_end_1), empty for none
    std::vector<std::string> reasons; // why a landmark is where it is
    BuildingRoute route = BuildingRoute::Outline;
    int era = 0;
};

/** The space a building may fill on its lot, and what it is for. Buildings themselves are made elsewhere. */
struct Envelope
{
    int lot = 0;
    Polygon footprint; // tag: 0 street side, 1 party wall (shared with a neighbour), 2 open side
    double base = 0.0;   // bottom of the foundation, world Y
    double ground = 0.0; // lowest ground under the footprint
    double top = 0.0;    // highest ground plus height
    double height = 0.0;
    int floors = 0;
    Use use = Use::Residential;
    std::string roof;
    Vec2 front{0.0, 1.0}; // facing the street
    float random = 0.0f;
    uint64_t key = 0; // the lot's stable id
    /** A step of the volume: its footprint from bottom to top (world Y). The first starts at base; each next one is stepped in. */
    struct Tier
    {
        Polygon footprint;
        double bottom = 0.0, top = 0.0;
    };
    std::vector<Tier> tiers;
    std::vector<int> partyWith;    // per footprint side: the lot across a party wall, -1 none
    double frontSetback = 0.0;     // kept from the lot's street sides
    double sideSetback = 0.0;      // kept from its other sides
    Vec2 ridge{1.0, 0.0};          // along the roof's ridge
    double roofHeight = 0.0;       // from the wall top to the ridge or dome top
    std::vector<Vec2> entrances;   // on the street sides, the main one first
    Use groundUse = Use::Residential;
    std::string corner = "none";   // none, wrap, chamfer or tower
    double foundationDepth = 0.0;  // from the highest ground under the footprint to the bottom of the foundation
    std::string foundationSource;  // terrainRegionStats or ground
    std::string landmark;
    std::string eraName; // the style era (Profile::era names): medieval, early, industrial, modern or future
    int wealth = 1;
    BuildingRoute route = BuildingRoute::Outline;
    int era = 0; // the era its lot was laid out in; floors and roof follow that era's profile
};

/** A path off the ground: on the walkable surface, across ramps, terraces and floors at any height. */
struct SurfacePath
{
    int path = -1; // the edit it was laid for
    std::vector<glm::dvec3> pts;
    StreetClass cls = StreetClass::Path;
    int profile = 0; // Layout::roads
    double width = 0.0;
};

/** How an edit came out: laid, and how long, or why not. */
struct PathResult
{
    int id = -1;
    bool ok = false;
    double length = 0.0;        // cm, ground and surface together
    double surfaceLength = 0.0; // cm on the walkable surface
    std::string error;
};

/**
 * An asset placed in the settlement (a library building, street furniture), from whatever stage places them. Exports and
 * settlementSendToUnreal send these: as HISM instances in Unreal, as entries in the engine files.
 */
struct Placement
{
    std::string id;                  // stable across rebuilds: live sync matches on it
    std::string asset;               // engine asset path, such as /Game/Props/SM_Bench
    glm::dvec3 position{0.0};        // world, cm
    glm::dvec4 rotation{0, 0, 0, 1}; // quaternion x, y, z, w, world
    glm::dvec3 scale{1.0};
    std::vector<std::string> tags;
};

/**
 * A quarter between main roads (arterials, the outline, water and exclusions), named by the naming culture. Its id comes
 * from the streets and edges around it, so it stays while they do.
 */
struct District
{
    uint64_t id = 0;
    std::string name;
    Polygon outline;
    Vec2 center{0.0};
    double area = 0.0; // m2
};

struct Layout
{
    StreetGraph graph;
    Profile profile; // as built, with the options' scales applied
    std::vector<Block> blocks;
    std::vector<Lot> lots;
    std::vector<Envelope> envelopes;
    Polygon outline;
    Vec2 center{0.0};
    double radius = 0.0;
    int refused = 0; // grown streets turned down by the local rules
    std::vector<WaterLine> water;
    std::vector<Vec2> bridges; // where streets cross water
    std::vector<Polygon> exclusions; // grown by their margins
    std::vector<int> exclusionIds;
    std::vector<RoadProfile> roads; // the classes' profiles first (index = class), then the ones paths name
    std::vector<SurfacePath> surfacePaths;
    std::vector<PathResult> pathResults;
    std::vector<uint64_t> staleOverrides; // overrides naming a street this layout does not make
    std::vector<Placement> placements;
    std::string culture;                            // the naming culture used
    std::unordered_map<uint64_t, std::string> streetNames; // by stable street id
    std::vector<District> districts;
    std::vector<double> edgeAccess;       // per graph edge, 0 to 1 (streetAccess)
    /** Where the town's ranking of lots put its cuts, so lots cut again after an edit rank the same way. */
    struct UseCuts
    {
        bool set = false;
        double industry = 1e300, q60 = 1.0, q92 = 1.0, wealth1 = 0.0, wealth2 = 0.0;
    } useCuts;
    std::vector<std::string> parcelWarnings; // what blocks, lots and envelopes fell back to and why
    std::vector<std::string> streetWarnings; // what the street network fell back to and why
    std::shared_ptr<RouteBook> routeBook;
    std::string plan; // none, pinned (the pinned plan shown) or stale (inputs changed since it was pinned)
    std::shared_ptr<struct GenLayout> gen; // walls, gates, eras, levels and the rest generators make (Generators.h)
};

/** Old English, Latin, French, Spanish, numbered US grid and sci-fi sector codes, as culture names. */
std::vector<std::string> cultureNames();
/** The culture a style profile names its streets in when the site gives none. */
std::string defaultCulture(std::string_view profileName);
/**
 * Districts and street names: every street through junctions (a run carried on by its straightest continuation of the
 * same class) gets one name, stable by the ids of the streets in it, from the site's culture; overrides and edit names
 * win. Throws std::runtime_error for an unknown culture.
 */
void nameStreets(Layout& l, const Site& site);
/** A street's name, empty when it has none. */
std::string streetName(const Layout& l, uint64_t street);

/**
 * Gives every street edge the stable id of its street: a run of edges of one class and path (generated, or one edit's)
 * through every junction where no other street of that class and path meets it, keyed by its end points, length and the
 * edit it came from. The same street gets the same id whenever the layout makes it again, whatever else changed: side
 * streets and drawn streets joining or crossing it leave it whole. A street a new one of its own class meets gets new ids.
 */
void assignStreetIds(Layout& l);

/** An edge's width: its own (rivers, given widths), else its profile's when a path named one, else its class's. */
double edgeWidth(const Layout& l, int edge);
/** The road profile of an edge. Only for streets. */
const RoadProfile& edgeProfile(const Layout& l, int edge);
/** True inside any exclusion. */
bool excluded(const Layout& l, Vec2 q);
/** True when the segment crosses or ends in an exclusion. */
bool crossesExclusion(const Layout& l, Vec2 a, Vec2 b);

/** Streets, blocks, lots and envelopes for a site. Same inputs give the same layout. */
Layout build(const Site& site, const Ground& ground, const Options& o, const WalkSurface* walk = nullptr);

// Stages, exposed for tests.
void growOrganic(Layout& l, const Ground& ground, const Options& o, double radiusLimit = 0.0);
void layGrid(Layout& l, const Ground& ground, const Options& o, double innerRadius = 0.0);
void layRadial(Layout& l, const Ground& ground, const Options& o);
void cutLargeBlocks(Layout& l, const Ground& ground, const Options& o);
std::vector<Lot> subdivide(const Block& b, const Options& o, int firstId);
Envelope envelopeFor(const Lot& lot, const Layout& l, const Ground& ground, const Options& o);
/**
 * Least-cost path over the ground between a and b inside the outline, at least clearance from every exclusion (half
 * the arterial width when negative), smoothed. Empty when there is none.
 */
std::vector<Vec2> route(const Layout& l, const Ground& ground, const Profile& p, Vec2 a, Vec2 b, double clearance = -1.0);

// ---------------------------------------------------------------------------------------------------------------------
// Measures, checks, output

/** Street measures comparable with real cities (Boeing 2019): orientation entropy and order, degrees, dead ends. */
json metrics(const Layout& l);
/** Problems an agent should fix: disconnected streets, sharp junctions, steep streets, lots without frontage, crossings. */
json checks(const Layout& l, const Ground& ground, const Options& o);
/** Counts and sizes. */
json stats(const Layout& l);
/** Each edit path: laid or not, its length on the ground and on the walkable surface, and why it failed. */
json pathResults(const Layout& l);
/** The whole layout as data: streets, blocks, lots, envelopes (the schema building generators read). */
json toJson(const Layout& l);

struct MeshParts
{
    geo::Mesh streets;   // ribbons on the ground, face attrs street, class
    geo::Mesh lots;      // lot outlines as loose edges, edge attr lot
    geo::Mesh envelopes; // closed volumes, face attrs lot, use, floors
    geo::Mesh water;     // river ribbons, face attr water
    std::vector<std::string> warnings; // what the meshes fell back to and why
};

struct OutputOptions
{
    double segment = 500.0; // ribbons and outlines are draped at about this spacing
    double lift = 2.0;      // cm above the ground, so streets do not z-fight it
};

MeshParts toMeshes(const Layout& l, const Ground& ground, const OutputOptions& o = {});

/**
 * Street centrelines joined through plain junctions (the same class, profile and path on both sides), then the surface
 * paths, as polylines.
 */
struct StreetLine
{
    std::vector<glm::dvec3> pts;
    StreetClass cls = StreetClass::Local;
    int profile = 0; // Layout::roads
    double width = 0.0;
    int path = -1;
    bool surface = false;
    int startNode = -1, endNode = -1; // street graph nodes at the ends; -1 on surface paths
    uint64_t street = 0;              // stable id (0 on surface paths: they go by path)
};
std::vector<StreetLine> streetLines(const Layout& l, const Ground& ground);

/**
 * What engines need of the street network: roads as lines with their cross-sections, and junctions where three or more
 * meet with the road ends that touch them (trimmed back by the junction's radius, pointing out along the road).
 * Lila space, cm. Built by roadNetwork.
 */
struct RoadEnd
{
    int line = 0;
    bool start = true;      // the line's start touches the junction
    glm::dvec3 p{0.0};      // where the trimmed road ends
    glm::dvec3 out{1, 0, 0}; // unit, from the junction along the road
    bool vehicle = true;    // has driving lanes
};

enum class Control : uint8_t
{
    None,   // paths only
    Yield,  // minor streets meeting
    Stop,   // a stop line on every approach
    Signal, // traffic lights, in phases
};

const char* controlName(Control c);

/** A crossing for people over one approach, at the stop line. */
struct Crosswalk
{
    int end = 0; // RoadEnd index
    glm::dvec3 a{0.0}, b{0.0};
};

struct Junction
{
    glm::dvec3 p{0.0};
    double radius = 0.0;
    std::vector<RoadEnd> ends;
    Control control = Control::None;
    /**
     * Signalised: groups of ends that get green together (opposite approaches), never two that cross. A skewed junction
     * gives a through movement that would cross another one a phase of its own; an empty group is a pedestrian-only phase.
     */
    std::vector<std::vector<int>> phases;
    std::vector<Crosswalk> crosswalks;
    /**
     * Per phase, the crosswalks people cross on during it: those over an approach whose traffic is held and that no
     * through movement of the phase leaves by. Turns across a walking crosswalk give way to the people on it.
     */
    std::vector<std::vector<int>> walks;
};

/** An on-street parking space beside a road with a parking lane. */
struct ParkingSpot
{
    glm::dvec3 p{0.0};
    glm::dvec3 dir{1, 0, 0}; // along the kerb
    double length = 600.0, width = 220.0;
    int line = 0;
};

struct RoadNetwork
{
    std::vector<StreetLine> lines; // trimmed back at junctions
    std::vector<Junction> junctions;
    std::vector<ParkingSpot> parking;
};

RoadNetwork roadNetwork(const Layout& l, const Ground& ground);

// ---------------------------------------------------------------------------------------------------------------------
// Siting and regions: where settlements go, and many of them on one map

/** A candidate site and why it scored: flat ground, water close but out of the flood, confluences. */
struct SiteScore
{
    Vec2 p{0.0};
    double score = 0.0;
    json reasons;
};

struct SiteOptions
{
    int count = 5;
    double spacing = 0.0;  // cm between chosen sites; 0 picks one from the area and count
    double floodMargin = 3000.0; // no centre closer than this beyond a river's bank
    double waterReach = 40000.0; // water further than this adds nothing
    double maxGrade = 0.12;
    uint32_t seed = 1;
    SitingRules rules;
};

/** How far q is from the nearest river bank: negative inside the water, very large when there is none. */
double bankDistance(Vec2 q, std::span<const WaterLine> water);

/** Best sites inside the outline, best first, no two closer than the spacing. */
std::vector<SiteScore> suggestSites(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const SiteOptions& o);

enum class Tier : uint8_t
{
    City,
    Town,
    Village,
};

const char* tierName(Tier t);

struct RegionOptions
{
    int cities = 1, towns = 3, villages = 8;
    double cityRadius = 120000.0, townRadius = 45000.0, villageRadius = 15000.0;
    std::string cityProfile = "europeanCity", townProfile = "medievalTown", villageProfile = "hamlet";
    uint32_t seed = 1;
};

struct RegionSettlement
{
    Tier tier = Tier::Village;
    std::string profile;
    Site site;
    double radius = 0.0;
    SiteScore score;
};

/** Settlements placed by tier (central place spacing: the city on the best site, villages between), joined by routed roads. */
struct RegionPlan
{
    std::vector<RegionSettlement> settlements;
    std::vector<std::vector<Vec2>> roads;
    std::vector<Vec2> bridges;
};

/** candidates, when given, are the scored sites to place on (best first); else suggestSites finds them. */
RegionPlan planRegion(const Polygon& outline, const Ground& ground, std::span<const WaterLine> water, const RegionOptions& o,
                      const std::vector<SiteScore>* candidates = nullptr);

uint64_t hash64(uint64_t x);
double unitRandom(uint64_t key); // [0, 1)
} // namespace settlement
