#pragma once

#include "Terrain.h"

#include <lila/Mesh.h>

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace terrain::cliffs
{
// ---------------------------------------------------------------------------------------------------- C0 regions

/** How terrainCliffs finds cliffs. Lengths cm, areas m2, angles degrees, all in the heightfield's own space. */
struct RegionParams
{
    double slope = 50.0;
    double minArea = 40.0;
    double minHeight = 400.0;
    /** Steep patches closer than this join one region. */
    double mergeDistance = 400.0;
    int maxRegions = 64;
    /** Tilt of the strata planes toward each cliff's facing. */
    double strataDip = 0.0;
};

struct Region
{
    std::string name;
    /** Steep samples of the region. */
    std::vector<size_t> cells;
    /** Outer ring first. */
    std::vector<std::vector<glm::dvec2>> rings;
    glm::dvec2 centroid{0.0};
    glm::dvec2 point{0.0};
    double areaM2 = 0.0;
    double low = 0.0;
    double high = 0.0;
    double meanSlope = 0.0;
    double maxSlope = 0.0;
    /** Horizontal outward (downhill) normal and its bearing, as the aspect field reads it (0 facing -Z, 90 facing +X). */
    glm::dvec3 facing{1.0, 0.0, 0.0};
    double facingDegrees = 0.0;
    /** Normal of the strata planes. */
    glm::dvec3 strata{0.0, 1.0, 0.0};
};

/** Slope in degrees at every sample, from central differences. */
std::vector<float> slopeDegrees(const Heightfield& h);

/** Regions where the slope passes the threshold, largest first, named cliff_1, cliff_2, ... dropped counts the ones left out. */
std::vector<Region> findRegions(const Heightfield& h, const RegionParams& p, size_t* dropped = nullptr);

/** Chamfer distance (cm) from every sample to the nearest sample not in set; 0 outside it. */
std::vector<float> distanceInside(const Heightfield& h, const std::vector<uint8_t>& set);
/** Chamfer distance (cm) from every sample to the nearest sample in set; infinity when none is set. */
std::vector<float> distanceTo(const Heightfield& h, const std::vector<uint8_t>& set);
/** Samples covered by the rings (even-odd), dilated by margin samples. */
std::vector<uint8_t> ringSamples(const Heightfield& h, const std::vector<std::vector<glm::dvec2>>& rings, int margin);
/** A grid value at a point, bilinear, clamped to the grid. */
double sampleGrid(const Heightfield& h, const std::vector<float>& g, double x, double z);

// ---------------------------------------------------------------------------------------------------- triangle meshes

/**
 * Triangles with per-vertex float attributes (va, vertexStride per vertex) and per-corner ones (ca, cornerStride per
 * corner, three corners per triangle), the working form every cliff step uses before it becomes a geo::Mesh.
 */
struct TriMesh
{
    std::vector<glm::dvec3> p;
    std::vector<glm::uvec3> t;
    int vertexStride = 0;
    int cornerStride = 0;
    std::vector<float> va;
    std::vector<float> ca;
    std::vector<std::string> vertexNames;
    std::vector<std::string> cornerNames;

    size_t addVertex(const glm::dvec3& q);
    float* vattr(size_t v) { return va.data() + v * size_t(vertexStride); }
    const float* vattr(size_t v) const { return va.data() + v * size_t(vertexStride); }
    float* cattr(size_t tri, int corner) { return ca.data() + (tri * 3 + size_t(corner)) * size_t(cornerStride); }
    const float* cattr(size_t tri, int corner) const { return ca.data() + (tri * 3 + size_t(corner)) * size_t(cornerStride); }
    int vertexIndex(const std::string& name) const;
    int cornerIndex(const std::string& name) const;
    /** Adds a vertex attribute of n floats (filled with fill) and returns its first index. */
    int addVertexAttr(const std::string& name, int n, float fill = 0.0f);
    int addCornerAttr(const std::string& name, int n, float fill = 0.0f);
    lila::geo::Bounds bounds() const;
};

/**
 * Splits every edge longer than maxEdge at its midpoint, attributes interpolated, until none is or passes run out. The
 * same edge is split once for both its triangles, so the result stays watertight where the input was.
 */
void refine(TriMesh& m, double maxEdge, int passes = 12);
/** Area-weighted vertex normals. */
std::vector<glm::dvec3> vertexNormals(const TriMesh& m);
/** 1 on vertices of open edges. */
std::vector<uint8_t> borderVertices(const TriMesh& m);

/** Triangles of a geo::Mesh with its float point attributes and corner uv0 and normal. */
TriMesh fromMesh(const lila::geo::Mesh& m);
/**
 * A geo::Mesh: vertex attributes named "color" become the point colour (float4), "nx" "ny" "nz" become corner normals,
 * other single floats point attributes; corner "u0" "v0" become uv0.
 */
lila::geo::Mesh toMesh(const TriMesh& m);

// ---------------------------------------------------------------------------------------------------- C1 displacement

/** A single-channel rock height map (0 to 1), tiled. */
struct RockMap
{
    int width = 0;
    int height = 0;
    std::vector<float> v;

    bool empty() const { return v.empty(); }
    double sample(double u, double w) const;
};

/** How a cliff surface is shaped. Lengths cm. */
struct Shape
{
    /** How far the surface is pushed in before displacement brings it back out; the displacement never passes it. */
    double depth = 150.0;
    double strataHeight = 250.0;
    /** Variation of each layer's thickness, 0 to 1. */
    double strataJitter = 0.35;
    /** How deep the bottom of each layer is cut back, as a fraction of depth. */
    double undercut = 0.55;
    /** Chance a layer stands out over the one above it, leaving a ledge on top. */
    double ledges = 0.3;
    double noise = 0.3;
    double noiseSize = 300.0;
    /** 0 an even face, 1 the top leaning out over a deeply cut base. */
    double overhang = 0.0;
    double rock = 0.0;
    double rockSize = 400.0;
    uint32_t seed = 0;
    glm::dvec3 strata{0.0, 1.0, 0.0};
    glm::dvec3 facing{1.0, 0.0, 0.0};
};

/** One displacement sample: how far out (0 at full depth in, 1 at the surface), and the masks it makes. */
struct DisplaceSample
{
    double out = 1.0;
    /** Where in its layer the point is (0 bottom, 1 top), and the layer. */
    double strataPhase = 0.0;
    int layer = 0;
    /** The layer above this one is set back: the top of this one is a ledge. */
    bool ledgeBelow = false;
};

DisplaceSample displace(const Shape& s, const RockMap* rock, const glm::dvec3& p, const glm::dvec3& n, double heightFraction);

/** Which side of a cliff mesh sits on what. */
enum class Mode : uint8_t
{
    Proud,
    Inset,
};

struct MeshParams
{
    Mode mode = Mode::Proud;
    /** Mesh edges per heightfield cell. */
    double detail = 4.0;
    /** Samples beyond the region the mesh covers. */
    int margin = 1;
    /** Width of the band at the mesh border where the shape fades onto the ground, cm. */
    double blend = 300.0;
    /** Proud: how far the bottom edge sinks below the ground and the top edge below the plateau, cm. */
    double baseSink = 150.0;
    double lipSink = 15.0;
    /** Proud: the ground under the cliff is set back this many times depth, so the mesh stands in front of it. */
    double recess = 1.2;
    /** Inset: how far a skirt drops below the ground at the seam, cm. */
    double skirt = 200.0;
    /** The band below a cliff's base that takes talus (ground cover reads the cliffTalus mask), cm. */
    double talusWidth = 800.0;
    double uvScale = 400.0;
    Shape shape;
    /** Most triangles; past it detail drops, with a warning. */
    size_t maxTriangles = 4'000'000;
};

/**
 * What the ground under one cliff needs: the samples the mesh covers (hole or recess band), the weight of the shape over
 * each sample (0 at the mesh border, 1 inside the blend band) and the region's facing and height range.
 */
struct Footprint
{
    std::vector<uint8_t> cover;
    std::vector<float> weight;
    double low = 0.0;
    double high = 0.0;
};

Footprint footprintOf(const Heightfield& h, const std::vector<std::vector<glm::dvec2>>& rings, const MeshParams& p);

struct MeshStats
{
    size_t vertices = 0;
    size_t triangles = 0;
    size_t layers = 0;
    size_t ledgeVertices = 0;
    double maxOut = 0.0;
    /** Proud: the highest bottom-edge vertex relative to the ground under it (negative is below), cm. */
    double baseClearance = 0.0;
    /** Interior vertices behind the final ground (hidden), as a fraction. */
    double hidden = 0.0;
    double detail = 0.0;
    std::vector<std::string> warnings;
};

/**
 * A cliff mesh in the heightfield's own space. Vertex attributes: color (strata, openness = 1 - cavity, up-facing,
 * scatter, as RGBA), strata, cavity, upFacing, ledge, scatter, talus, weight, and corner u0 v0 (the low-distortion uv0:
 * projected along the cliff's facing, one island per stratum).
 */
struct CliffMesh
{
    TriMesh mesh;
    MeshStats stats;
};

/** A cliff over the ground the rings cover: ground is the heightfield before any cliff edit. */
CliffMesh buildFromRegion(const Heightfield& ground, const std::vector<std::vector<glm::dvec2>>& rings, const MeshParams& p, const RockMap* rock = nullptr);
/** A cliff from any mesh (a blockout): refined and displaced along its normals; ground (optional) sinks the base. */
CliffMesh buildFromMesh(const lila::geo::Mesh& blockout, const Heightfield* ground, const MeshParams& p, const RockMap* rock = nullptr);

/** The masks every cliff mesh carries, from its shape: colour, cavity, scatter zones, uv0. Used by every builder. */
void bakeMasks(TriMesh& m, const std::vector<DisplaceSample>& samples, const std::vector<glm::dvec3>& baseNormals, const MeshParams& p);

// ---------------------------------------------------------------------------------------------------- ground edits

/** One cliff's edit of the ground, in the heightfield's own space, as the Terrain Cliffs node stores it. */
struct GroundEdit
{
    std::string name;
    Mode mode = Mode::Proud;
    std::vector<std::vector<glm::dvec2>> rings;
    double depth = 150.0;
    double recess = 1.2;
    double blend = 300.0;
    int margin = 1;
    double talusWidth = 800.0;
    double detail = 4.0;

    nlohmann::json toJson() const;
    static GroundEdit fromJson(const nlohmann::json& j);
};

/**
 * An extruded cliff's stamp: plateau behind the wall, base cut in front. Lines in the heightfield's space; the base is
 * the ground on the line, the top that plus height.
 */
struct WallStamp
{
    std::string name;
    std::vector<glm::dvec2> line;
    double height = 1500.0;
    /** Ground behind the wall (left of the line's direction, as seen from above) raised to the top over this width, cm; in front cut to the base. */
    double plateau = 1500.0;
    double cut = 600.0;
    double falloff = 800.0;
    bool flip = false;
};

struct GroundResult
{
    size_t recessed = 0;
    size_t holes = 0;
    size_t stamped = 0;
};

/**
 * Applies the edits to h: proud cliffs set the ground back under the mesh, inset cliffs open holes ("holes" mask, kept
 * with any already there), wall stamps raise a plateau and cut a base. Writes masks cliff, cliffTalus and cliffTop.
 */
GroundResult applyGroundEdits(Heightfield& h, const std::vector<GroundEdit>& edits, const std::vector<WallStamp>& walls);

// ---------------------------------------------------------------------------------------------------- C2 extrusion

enum class Profile : uint8_t
{
    Vertical,
    Battered,
    Overhang,
    Stepped,
};

bool parseProfile(const std::string& s, Profile* out);

struct ExtrudeParams
{
    double height = 1500.0;
    Profile profile = Profile::Vertical;
    /** A custom profile: (out, up) pairs from the base (0, 0) to the top (any out, 1), up as a fraction of height. */
    std::vector<glm::dvec2> custom;
    /** Battered and stepped: how far the top sits back, as a fraction of height. */
    double batter = 0.2;
    int steps = 3;
    double sink = 150.0;
    double detail = 4.0;
    double cell = 100.0;
    MeshParams mesh;
};

/** The wall's cross-section from base to top: (out, up) pairs, up 0 to 1, out in fractions of height (positive outward). */
std::vector<glm::dvec2> profilePoints(const ExtrudeParams& p);

/**
 * A wall along a polyline on the ground: base follows the ground (or 0 without one) sunk by sink, top at base + height.
 * Outward is the right of the line's direction (flip reverses it). Strata and displacement as buildFromRegion.
 */
CliffMesh extrudeWall(const std::vector<glm::dvec2>& line, const Heightfield* ground, const ExtrudeParams& p, bool flip, const RockMap* rock = nullptr);

// ---------------------------------------------------------------------------------------------------- C3 kits

/** One kit piece: its name and size (bounds, cm, Y up, base at 0). */
struct KitPiece
{
    std::string name;
    glm::dvec3 size{100.0};
    glm::dvec3 lo{0.0};
    /** Points spread over its surface, in its own space, for floating and penetration checks. */
    std::vector<glm::dvec3> samples;
};

struct KitParams
{
    /** Spacing along the face as a fraction of a piece's width: under 1 overlaps. */
    double overlap = 0.8;
    double rotate = 15.0;
    double scaleJitter = 0.2;
    /** Base sunk this far into the ground, as a fraction of the piece's height. */
    double sink = 0.1;
    /** A piece hanging more than this above the ground is floating, cm. */
    double floatTolerance = 25.0;
    /**
     * Deepest the cliff face behind a piece may reach into it, as a fraction of its depth: past 1 its front is in the
     * ground too. Without depthOf, the share of its samples under the ground.
     */
    double maxPenetration = 0.8;
    /**
     * How deep, cm, a piece's back sits in the ground: how far its hull (its samples, placed, in the ground's space, the
     * sunk base raised to the sink depth above its foot) must move along its outward facing to clear it. A physics check
     * (overlap::Session); null judges by the share of samples under the ground instead.
     */
    std::function<double(std::span<const glm::dvec3>, const glm::dvec3&)> depthOf;
    /** Gaps wider than this along the face get smaller pieces, cm. */
    double gap = 150.0;
    int maxPieces = 4000;
    uint32_t seed = 1;
};

struct KitPlacement
{
    int piece = 0;
    glm::dmat4 m{1.0};
    double along = 0.0;
    bool filler = false;
};

struct KitResult
{
    std::vector<KitPlacement> placed;
    size_t rejectedFloating = 0;
    size_t rejectedPenetrating = 0;
    size_t fillers = 0;
    double coverage = 0.0;
};

/** Kit pieces fitted along the region's outline on the ground: outward normal, height, overlap, sunk, checked, gaps filled. */
KitResult fitKit(const Heightfield& ground, const Region& r, const std::vector<KitPiece>& kit, const KitParams& p);

// ---------------------------------------------------------------------------------------------------- C5 cells

/** Slices the mesh on the grid lines x = origin.x + k * cell and z = origin.y + k * cell; shared edges split once. */
void sliceOnGrid(TriMesh& m, glm::dvec2 origin, double cell);
/** The triangles in each cell (by centroid), as meshes of their own keyed (ix, iz). */
std::map<std::pair<int, int>, TriMesh> splitCells(const TriMesh& m, glm::dvec2 origin, double cell);

/** How cliff meshes become World Partition tiles. Lengths cm, on the ground plane (X and Z). */
struct TileParams
{
    /** The World Partition cell; tiles never cross a cell line. */
    double cell = 12800.0;
    glm::dvec2 origin{0.0};
    int lods = 3;
    double lodRatio = 0.35;
    double collisionRatio = 0.1;
    /** on, off, or auto: Nanite for tiles over naniteTriangles. */
    std::string nanite = "auto";
    size_t naniteTriangles = 50000;
};

/** One tile: its cell, the LODs and collision mesh about its centre, and a hash of what it holds. */
struct Tile
{
    std::pair<int, int> cell;
    glm::dvec3 centre{0.0};
    lila::geo::Bounds bounds;
    std::vector<lila::geo::Mesh> lods;
    lila::geo::Mesh collision;
    bool nanite = false;
    size_t triangles = 0;
    uint64_t hash = 0;
};

/**
 * A mesh (world space, Y up) cut into tiles on the grid: borders shared exactly between neighbours, every LOD and the
 * collision mesh reduced with their borders kept, each about the tile's centre (its pivot) so the actor lands in its cell.
 */
std::vector<Tile> cutTiles(const TriMesh& mesh, const TileParams& p);

/** One scatter tier: its grid, density per m2 where its mask is 1, and how its pieces stand. */
struct Tier
{
    std::string name;
    double grid = 3200.0;
    double density = 0.05;
    std::string mask = "scatter";
    double align = 0.6;
    double scaleMin = 0.7;
    double scaleMax = 1.3;
    std::vector<std::string> items;
};

struct ScatterPoint
{
    int tier = 0;
    int item = 0;
    glm::dvec3 position{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 downhill{1.0, 0.0, 0.0};
    double slope = 0.0;
    double scale = 1.0;
    double turn = 0.0;
    std::pair<int, int> cell;
};

/**
 * Points over the mesh per tier, each tier on its own grid with a seed made from the cell, so editing one place only
 * changes the cells it touches. Deterministic.
 */
std::vector<ScatterPoint> scatterTiers(const TriMesh& m, const std::vector<Tier>& tiers, uint32_t seed, glm::dvec2 origin);

// ---------------------------------------------------------------------------------------------------- C4 projection

/**
 * The slope-aware projection MF_LilaTerrainLayer and the viewport use: weights of the top, X and Z projections for a
 * normal. Flat ground below slopeStart gets the top alone; above it the sides fade in over blend degrees (two samples
 * with biplanar, the dominant side only).
 */
glm::dvec3 projectionWeights(const glm::dvec3& n, double slopeStart, double blend, bool biplanar);
/**
 * Texels per cm a face gets from the projection at tile cm per repeat: the largest stretch of the weighted projections,
 * for the stretch test (1 / tile on every face when nothing stretches).
 */
double texelDensity(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& c, double tile, double slopeStart, double blend, bool biplanar);

inline constexpr double kSlopeStart = 35.0;
inline constexpr double kSlopeBlend = 15.0;

// ---------------------------------------------------------------------------------------------------- shared

uint32_t hash3(uint32_t a, uint32_t b, uint32_t c);
double unit(uint32_t h);
double noise3(const glm::dvec3& p, uint32_t seed);
double fbm3(const glm::dvec3& p, uint32_t seed, int octaves = 4);
nlohmann::json ringsJson(const std::vector<std::vector<glm::dvec2>>& rings);
std::vector<std::vector<glm::dvec2>> ringsFrom(const nlohmann::json& j);
} // namespace terrain::cliffs
