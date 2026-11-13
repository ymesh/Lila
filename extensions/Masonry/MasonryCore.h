#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace masonry
{
namespace geo = lila::geo;

inline constexpr double kPi = 3.14159265358979323846;
inline const glm::dvec3 kUp{0.0, 1.0, 0.0};

/** How stones are arranged in each course. */
enum class Bond : uint8_t
{
    Running,    // stretchers, each course offset by half a stone
    Stack,      // stretchers, no offset
    English,    // a course of stretchers, then a course of headers
    Flemish,    // stretcher and header alternating in every course
    Header,     // headers only, offset by half a header
    GardenWall, // three courses of stretchers, then one of headers
    Herringbone, // stones at 45 degrees, leaning the other way each course
    Ashlar,      // coursed random ashlar: stones of many lengths in level courses
    Rubble,      // random rubble: rough stones of many lengths and heights, wide joints
    DryStone,    // dry stone: rough stones, through stones at intervals, a coping of through stones on top
};

const char* bondName(Bond b);
bool parseBond(std::string_view name, Bond* out);
std::vector<std::string> bondNames();

/** What a stone is in the bond or the building. Instances carry it as the int attribute "role". */
enum class Role : uint8_t
{
    Stretcher, // long side on the face
    Header,    // end on the face, running into the wall
    Bat,       // half stretcher, at wall ends and corners
    Closer,    // narrow piece that keeps the bond at corners, or a stone trimmed against another
    Filler,    // cut to fit the length of the course
    Voussoir,  // wedge of an arch, vault or dome
    Keystone,  // the crown of an arch
    Lintel,    // one long stone over a square opening
    Sill,      // one long stone under a window
    Jamb,      // the course stone beside an opening
    Column,    // plinth, drum or capital
    Step,      // stair tread
    Band,      // string course or cornice stone, projecting from the face
    Slab,      // a piece of a floor or roof
    Rubble,    // fallen stone at the foot of a ruin
    Paving,    // a cobble, flagstone or roof tile on a floor
};
inline constexpr int kRoleCount = 16;
const char* roleName(Role r);

/** Which face of the wall a stone belongs to. Through stones show on both. */
enum class Face : uint8_t
{
    Outer,
    Inner,
    Through,
};

/** The solid a stone mesh is. */
enum class Shape : uint8_t
{
    Box,
    Wedge, // a box whose -Y end is narrower along X by taper: voussoirs
    Drum,  // a cylinder along Y, diameter size.x, its -Y end scaled by taper: column drums
};

/** The nominal stone and the joint between stones, cm. Defaults are a UK brick. */
struct Units
{
    double length = 21.5;
    double height = 6.5;
    double depth = 10.25;
    double joint = 1.0;
};

/** How an opening is closed at the top. */
enum class ArchKind : uint8_t
{
    Lintel,    // one long stone
    Round,     // semicircle
    Pointed,   // two arcs meeting at a point (equilateral by default)
    Segmental, // an arc flatter than a semicircle
    Flat,      // jack arch: voussoirs in a band, joints fanning from below
};
const char* archName(ArchKind k);
bool parseArch(std::string_view name, ArchKind* out);

/** A door, window or niche in a wall, centred `at` cm along its footprint. */
struct Opening
{
    std::string name;
    double at = 0.0;
    double width = 100.0;
    double height = 200.0;  // sill to the top of the opening, arch included
    double sill = 0.0;      // above the ground under the opening
    ArchKind arch = ArchKind::Round;
    double rise = 0.0;      // arch height above the springing; 0 is the natural one for the kind
    double ring = 0.0;      // voussoir depth across the arch; 0 is one and a half stone heights
    bool through = true;    // false: a niche, cut into the outer face only
    bool sillStone = true;  // a projecting sill under windows
};

struct WallParams
{
    Bond bond = Bond::Running;
    Units stone;
    double height = 200.0;     // cm, where the footprint gives no per-point heights
    double thickness = 21.5;   // cm, at the base
    double recess = 0.6;       // how far the mortar sits behind the stone faces, cm
    double cornerAngle = 30.0; // deg: turns sharper than this are corners, gentler ones curve the wall
    double jitter = 1.0;       // 0 to 1: how much of the joint stones may wander and tilt into
    double protrude = 0.3;     // cm: stones pushed out of the face by up to this
    double batter = 0.0;       // deg: each face leans in by this, so the wall thins upward
    std::vector<double> bands; // heights above the lowest base of string courses, cm
    double bandProject = 0.0;  // how far a string course stands out of the face, cm; 0 is a third of a stone depth
    int cornice = 0;           // top courses corbelled out, each further than the one below
    double corniceProject = 0.0; // per corbelled course, cm; 0 is a quarter of a stone depth
    std::vector<Opening> openings;
    uint32_t seed = 1;
};

/** One stone: a box (or wedge, or drum) in its own frame. */
struct Stone
{
    glm::dvec3 center{0.0};
    /** Columns: along the face, up, into the wall. Always a rotation. */
    glm::dmat3 frame{1.0};
    glm::dvec3 size{0.0};
    int32_t wall = 0;
    int32_t course = 0;
    int32_t run = 0;
    Role role = Role::Stretcher;
    Face face = Face::Outer;
    Shape shape = Shape::Box;
    float taper = 1.0f;  // Wedge and Drum: the -Y end's width over the +Y end's, (0, 1]
    float random = 0.0f;
    int32_t region = 0;  // index into Built::regions
    /** Where along its run's centre line the stone sits, cm, for the core behind it; t1 < t0 for stones off the courses. */
    double t0 = 0.0, t1 = -1.0;
    int32_t mesh = -1;   // an own mesh in Built::pieces (slab pieces) instead of a shaped one
    bool grounded = false; // stands on the ground, not on another stone
    bool loose = false;    // tipped or slid by the ruin
    float age = 0.0f, damage = 0.0f, wet = 0.0f, moss = 0.0f; // 0 to 1
};

/** A wall's base line. Y is up; each point's Y is the ground under it. */
struct Footprint
{
    std::vector<glm::dvec3> points;
    bool closed = false;
    /** Wall height above each point, cm; empty uses WallParams::height everywhere. */
    std::vector<double> heights;
};

struct Layout
{
    std::vector<Stone> stones;
    int courses = 0;
    double pitch = 0.0;  // course height including the bed joint
    double base = 0.0;   // the lowest ground under the wall, where course 0 starts
    int trimmed = 0;     // stones cut back where they met another
    int dropped = 0;     // stones that had nothing left after trimming
};

/** Lays every course of a wall along a footprint. Deterministic for the same inputs. */
Layout layoutWall(const Footprint& footprint, const WallParams& p, int wall = 0);

/**
 * The mortar body the stones sit in, recessed behind the faces: one convex prism per footprint segment and band of
 * courses, covering only where stones are (so openings, steps and ruined tops show through). Stones are this wall's,
 * laid by layoutWall with the same footprint and params; stones without a centre-line range are ignored. With grid,
 * cells are also cut where the wall crosses the grid's lines (x and z multiples of grid cm), so they chunk cleanly.
 */
std::vector<geo::Mesh> coreCells(const Footprint& footprint, const WallParams& p, std::span<const Stone> stones, int wall = 0, double grid = 0.0);

/** coreCells of the intact wall, appended into one mesh on the mortar slot. */
geo::Mesh coreMesh(const Footprint& footprint, const WallParams& p);

glm::dmat4 stoneMatrix(const Stone& s);

/** The stone's eight corners (a wedge's bottom four pulled in), world space. */
std::array<glm::dvec3, 8> stoneCorners(const Stone& s);

/** Stones intersecting by more than tolerance (cm), by the separating axis test on their solids' hulls. */
bool overlaps(const Stone& a, const Stone& b, double tolerance = 0.01);

/** Pairs of stones intersecting by more than tolerance (cm). What masonryInfo reports; 0 for a clean layout. */
size_t countOverlaps(std::span<const Stone> stones, double tolerance = 0.01);

/**
 * Cuts each stone back along its length or depth where it comes within a joint of one of fixed, and drops the ones left
 * too small. Returns how many were dropped.
 */
int fitAgainst(std::vector<Stone>& stones, std::span<const Stone> fixed, double joint);

/** How detailed a stone mesh is. Every level fills the same solid, so they swap as LODs. */
enum class Detail : uint8_t
{
    Worn,    // chamfered, corners knocked off by wear
    Clean,   // chamfered
    Box,     // six quads (a drum: eight sides)
};

struct StoneShape
{
    glm::dvec3 size{21.5, 6.5, 10.25};
    double bevel = 0.5;   // cm
    double wear = 0.5;    // 0 to 1
    uint32_t seed = 0;
    Detail detail = Detail::Worn;
    double tile = 100.0;  // cm of stone per UV unit
    Shape shape = Shape::Box;
    double taper = 1.0;
    double chips = 0.0;   // 0 to 1: chips knocked out of the worn level's edges and corners
};

/** A closed stone mesh centred on the origin, in the stone's frame, with uv0 and hard edges. */
geo::Mesh stoneMesh(const StoneShape& s);

/** The stone at every detail, LOD0 first: worn, clean chamfer, plain box. All fit the same box. */
std::array<geo::Mesh, 3> stoneLods(StoneShape s);

/** Footprints from a mesh's loose edges (edges with no face), chained into lines and loops. Point float "wallHeight" gives heights. */
std::vector<Footprint> footprintsFromEdges(const geo::Mesh& m);

/** A mesh holding footprints as loose edges (and their heights as point float "wallHeight"), what masonryWall stores on the mesh it makes. */
geo::Mesh edgesFromFootprints(std::span<const Footprint> footprints);

/** Point attribute of a footprint mesh holding the wall height above each point. */
inline constexpr const char* kWallHeightAttr = "wallHeight";

/** Stones plus the meshes they reference, ready for instances or a merged mesh. */
struct Built
{
    std::vector<Stone> stones;
    std::vector<int> reference;           // per stone, into meshes
    std::vector<geo::Mesh> meshes;        // one per distinct shape, size and variant, then one per piece
    std::vector<std::array<geo::Mesh, 3>> lods; // per mesh, when BuildOptions::lods
    std::vector<int> variant;             // per stone
    std::vector<geo::Mesh> pieces;        // own meshes of stones with Stone::mesh (slab pieces), in their stone's frame
    std::vector<std::string> regions;     // names: "wall_0", "north_wall", "north_wall/door_1", "tower_1"
    geo::Mesh core;
    std::vector<geo::Mesh> coreHulls;     // convex pieces of the core, its collision
    std::vector<geo::Mesh> shellCells;    // convex pieces of the walls' full volume where stones stand, for low poly shells
    geo::Mesh dressing;                   // plaster and ivy
    int courses = 0;
    double pitch = 0.0;
    int trimmed = 0;
    int dropped = 0;
    /** Per stone, 0 at its wall's bottom course to 1 at its top: the "height" the material reads. */
    std::vector<float> heightFraction;
};

struct BuildOptions
{
    WallParams wall;
    double bevel = 0.5;
    double wear = 0.5;
    int variants = 6;
    Detail detail = Detail::Worn;
    double tile = 100.0; // cm of stone per UV unit
    double chips = 0.3;  // 0 to 1: chipped edges on the worn level, more on stones that are older, wetter or damaged
    bool lods = false;   // also make every mesh's LOD chain
};

/** Erosion level 0 to 2 a stone's variant is picked at, from its age, wet and damage. */
int erosionLevel(const Stone& s);

/** Every footprint laid with its core, the same work the Masonry Wall node does before ruin and age. */
Built buildWalls(std::span<const Footprint> footprints, const BuildOptions& o);

/** Shares meshes between stones of one shape, size and variant and makes them (and their LODs). Fills reference, variant, meshes, lods, heightFraction. */
void makeMeshes(Built& b, const BuildOptions& o);

/**
 * Pairs of the given stones (indices into b.stones) interpenetrating by more than tolerance. Slab pieces lying in place
 * are tested by their own outline rather than their box, and never against other pieces of the same slab.
 */
std::vector<std::pair<int, int>> overlappingPairs(const Built& b, std::span<const int> which, double tolerance);

/** Material slots: every stone mesh is on the first, the core on the second. Unreal finds or makes materials by these names. */
inline constexpr const char* kStoneSlot = "MasonryStone";
inline constexpr const char* kMortarSlot = "MasonryMortar";
inline constexpr const char* kPlasterSlot = "MasonryPlaster";
inline constexpr const char* kIvySlot = "MasonryIvy";

/** A closed prism between heights y0 and y1 over a convex ground quad (any winding), faces outward. */
geo::Mesh prismMesh(const std::array<glm::dvec3, 4>& ground, double y0, double y1);

/** A closed hull of eight corners ordered like stoneCorners (bit 0 x, bit 1 y, bit 2 z), faces outward. */
geo::Mesh hexMesh(const std::array<glm::dvec3, 8>& corners);

/** Offset of the vertex between two directions in the ground plane, so lines offset sideways by dist meet there. */
glm::dvec3 miterOffset(const glm::dvec3& dIn, const glm::dvec3& dOut, double dist);

/** Convex pieces covering the wall's full volume, one per footprint segment per height band: its UCX collision. */
std::vector<geo::Mesh> collisionHulls(const Footprint& footprint, const WallParams& p, int bands = 1);

/** Vertex colour set of merged stones: R the stone's random, G its height up the wall (0 bottom course, 1 top), B moss, A wet. uv2 holds age and damage. */
inline constexpr const char* kStoneColorSet = "masonry";

/** The given stones at one LOD (0 to 2) merged into one mesh, positioned relative to origin, with kStoneColorSet. */
geo::Mesh mergeStones(const Built& b, std::span<const int> stones, int lod, const glm::dvec3& origin);

/** Stones grouped by the square ground cell (size cm) their centre is in, for streaming and culling. */
struct Chunks
{
    std::vector<int> of;            // per stone, into cells
    std::vector<glm::ivec2> cells;  // x and z cell index, sorted
};
Chunks chunkStones(std::span<const Stone> stones, double size);

/** splitmix64 hash: stable random numbers keyed by seed and indices. */
uint64_t hash64(uint64_t x);
double unitRandom(uint64_t key); // [0, 1)
uint64_t mixKey(uint64_t a, uint64_t b, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0);

/** Stones in a coarse grid, for finding the ones a stone might touch. */
struct StoneGrid
{
    double cell = 20.0;
    std::vector<std::pair<uint64_t, int>> entries; // sorted by cell once built
    void build(std::span<const Stone> stones, double pad = 0.0);
    /** Indices of stones whose cells overlap s's bounds grown by pad, sorted, unique. */
    std::vector<int> around(const Stone& s, double pad) const;
    static uint64_t pack(int64_t x, int64_t y, int64_t z);
};

/** World-space half extents of a stone's bounding box. */
glm::dvec3 halfExtentAabb(const Stone& s);
} // namespace masonry
