#pragma once

#include <lila/Mesh.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <random>
#include <string>
#include <vector>

/**
 * The Tree Generator's geometry, free of the scene: trunks and branches, canopy blobs, leaf scattering, the leaf card
 * and the opacity raster. Everything takes plain values (centimetres, Lila space) and returns geo::Mesh or plain data,
 * and gives the same result for the same seed on any machine and thread count.
 */
namespace treegen
{
using lila::geo::Index;
using lila::geo::Mesh;
using json = nlohmann::json;

inline constexpr double kEps = 1e-9;
inline constexpr double kPhyllotaxis = 137.507764;

/** Seeded random numbers: a 64 bit Mersenne Twister mapped to doubles by hand, so every platform draws the same values. */
class Rng
{
public:
    explicit Rng(uint64_t seed) : mEngine(seed) {}
    /** [0, 1). */
    double random() { return double(mEngine() >> 11) * (1.0 / 9007199254740992.0); }
    double uniform(double a, double b) { return a + (b - a) * random(); }

private:
    std::mt19937_64 mEngine;
};

/** A low and a high value, as the schema's range fields hold them. */
struct Span
{
    double low = 0.0, high = 0.0;
};
Span spanOf(const json& v);
/** low when the span is empty, else a uniform value between the two (either order). */
double between(Rng& rng, const Span& s);

/** A vector perpendicular to d, measured off world Y (or X when d is near Y). */
glm::dvec3 perpendicular(const glm::dvec3& d);
/** perpendicular(d) spun degrees around d. */
glm::dvec3 spun(const glm::dvec3& d, double degrees);
glm::dvec3 rotateAbout(const glm::dvec3& v, const glm::dvec3& axis, double radians);
/** d tipped up to degrees off itself in a random direction. */
glm::dvec3 jitter(const glm::dvec3& d, double degrees, Rng& rng);

// ------------------------------------------------------------------------------------------------ trunk

struct TrunkSpec
{
    double height = 600.0, radius = 22.0, lean = 6.0, taper = 0.35;
    double branchStart = 0.45, branchAngle = 45.0, angleVariance = 12.0;
    double lengthRatio = 0.62, lengthFalloff = 0.0, radiusRatio = 0.6, gnarl = 7.0, upPull = 0.45;
    int sections = 8, levels = 2, branches = 3;
    glm::dvec3 up{0.0, 1.0, 0.0};
};

/** Trunk settings (sectionValues "trunk", centimetres) as a spec; zero height or radius take the six metre defaults. */
TrunkSpec trunkSpec(const json& trunk, const glm::dvec3& up);

struct Limb
{
    std::vector<glm::dvec3> points;
    std::vector<double> radii;
};

struct Tip
{
    glm::dvec3 point{0.0};
    glm::dvec3 heading{0.0, 1.0, 0.0};
    double radius = 0.0;
};

struct Skeleton
{
    std::vector<Limb> limbs; // trunk first, then branches depth first
    std::vector<Tip> tips;   // one per limb at the deepest level
};

Skeleton skeleton(const TrunkSpec& spec, Rng& rng);

/** A polygon soup with per-corner UVs indexing us/vs, what the tube, blob and plane builders produce. */
struct Arrays
{
    std::vector<glm::dvec3> points;
    std::vector<Index> counts;
    std::vector<Index> connects;
    std::vector<double> us, vs;
    std::vector<Index> uvIds; // per corner, parallel to connects; empty for no UVs
    struct Island
    {
        size_t start, end; // uv index range
        double width, height;
    };
    std::vector<Island> islands;

    void append(const Arrays& other);
};

/** One closed limb with world unit UVs: the bark as one island, each end cap as another. A tip thinner than pointBelow closes as a
    cone from the last ring at least that thick, so a twig's cap never shrinks to a zero-area fan. */
Arrays tube(const std::vector<glm::dvec3>& points, const std::vector<double>& radii, int sides, double pointBelow = 0.0);

/** Rescales tube UVs in place: "tiled" repeats every tile cm, "packed" lays the islands out in 0-1. Returns the packed density in px/m, 0 for tiled. */
double layoutUvs(std::vector<Arrays>& pieces, const std::string& mode, double tile, double density, int mapSize, double baseRadius, std::string* warning = nullptr);

/** A mesh from arrays: faces that fail to build are skipped, UVs land in uv0. */
Mesh toMesh(const Arrays& a);

/** The whole trunk mesh and its tips. Limb tubes build on the TBB pool. */
struct Trunk
{
    Mesh mesh;
    std::vector<Tip> tips;
    double density = 0.0;
    std::string warning;
};
Trunk buildTrunk(const json& trunk, const glm::dvec3& up);

// ------------------------------------------------------------------------------------------------ blobs

/** A unit blob of a kind (icosphere, quadsphere, uvsphere) at a detail, clipped, aimed along up and spun. Throws std::runtime_error. */
Arrays blobArrays(const std::string& kind, int detail, double clip, const glm::dvec3& up, double spin);

struct BlobPlacement
{
    double radius = 0.0;
    glm::dvec3 centre{0.0};
    glm::dvec3 up{0.0, 1.0, 0.0};
    double spin = 0.0;
};

/** One blob per tip, sized by the tip's thickness against the average, offset along its axis. Deterministic for a seed. */
std::vector<BlobPlacement> placeBlobs(const std::vector<Tip>& tips, double radius, double variation, double taper, double offset, double align,
                                      double tilt, const glm::dvec3& up, int64_t seed);
/** The blob meshes for placements, built on the TBB pool. */
std::vector<Mesh> buildBlobs(const std::vector<BlobPlacement>& placements, const std::string& kind, int detail, double clip);

// ------------------------------------------------------------------------------------------------ leaves

/** A canopy surface in world space: one mesh with its owner's centre, anchor and heading. */
struct CanopyInput
{
    const Mesh* mesh = nullptr;
    glm::dmat4 world{1.0};
    bool hasAnchor = false;
    glm::dvec3 anchor{0.0};
    glm::dvec3 anchorHeading{0.0, 1.0, 0.0};
};

struct Sample
{
    glm::dvec3 point, normal, center, anchor, heading;
    int clump = -1;
    int owner = 0;
};

/** Area weighted triangles over every canopy mesh. Throws std::runtime_error when there is nothing to sample. */
class Canopy
{
public:
    explicit Canopy(const std::vector<CanopyInput>& inputs);
    std::vector<Sample> sample(int count, Rng& rng, double minDistance, double minNormalUp, const glm::dvec3& up, int clumps, double clumpRadius,
                               double clumpFalloff) const;
    double area() const { return mArea; }

private:
    struct Tri
    {
        glm::dvec3 p[3], n[3];
        int owner;
    };
    Sample point(Rng& rng) const;

    std::vector<Tri> mTris;
    std::vector<double> mCumulative;
    std::vector<glm::dvec3> mCenters, mAnchors, mHeadings;
    double mArea = 0.0;
};

/** A leaf mesh in its own frame: up through its thinnest axis, forward along its longest, pivot moved to the base. */
struct Leaf
{
    Mesh mesh; // the source, compacted
    std::vector<glm::dvec3> points; // shifted by the pivot
    glm::dvec3 up{0.0, 1.0, 0.0}, forward{0.0, 0.0, 1.0}, side{1.0, 0.0, 0.0}, shift{0.0};
    double length = 1.0;
};
/** Throws std::runtime_error for an empty mesh or clashing axes. */
Leaf makeLeaf(const Mesh& source, const std::string& upAxis, const std::string& forwardAxis, const std::string& pivot);
/** Another detail level of reference: its frame, shift and length copied so the levels place and bend identically. */
Leaf makeLeaf(const Mesh& source, const Leaf& reference);

struct LeafPlan
{
    double factor = 1.0, curl = 0.0, radius = 0.0, wrap = 0.0;
    glm::dvec3 axisUp{0.0, 1.0, 0.0}, axisForward{0.0, 0.0, 1.0}, axisSide{1.0, 0.0, 0.0};
    glm::dvec3 origin{0.0}, normal{0.0, 1.0, 0.0};
    glm::vec3 shade{1.0f};
};

struct ScatterSettings
{
    int count = 400;
    int64_t seed = 0;
    double size = 0.0;
    Span scale{0.8, 1.3}, push{0.0, 0.0}, spin{0.0, 360.0}, tilt{0.0, 25.0}, curl{-5.0, 20.0}, wrap{0.5, 1.0}, gather{0.0, 0.0}, align{0.0, 0.0};
    double wrapRadius = 0.0;
    int clumps = 0;
    double clumpRadius = 0.0, clumpFalloff = 1.0, shadeBias = 0.0, minDistance = 0.0, minNormalUp = -1.0, normalBlend = 1.0;
    std::string colorSet = "leafShade", pivot = "base", leafUp = "auto", leafForward = "auto";
    bool combine = true;
};
/** The leaf section (sectionValues "leaf", centimetres) as settings. */
ScatterSettings scatterSettings(const json& leaf);

/** Where every leaf goes, how it bends and how it is shaded. Sequential, so the seed decides everything. */
std::vector<LeafPlan> planLeaves(const Canopy& canopy, const Leaf& primary, const ScatterSettings& s, const glm::dvec3& up);

/** One leaf point bent by a plan: curled along its length, wrapped onto the canopy and placed. */
glm::dvec3 placeLeafPoint(const glm::dvec3& point, const Leaf& leaf, const LeafPlan& plan);

/** The leaves as one mesh (combine) or one per plan, normals blended toward the canopy and shading in the colour set. */
std::vector<Mesh> emitLeaves(const Leaf& leaf, const std::vector<LeafPlan>& plans, bool combine, double normalBlend, const std::string& colorSet);

// ------------------------------------------------------------------------------------------------ leaf plane

/** A leaf card along +Z, base at the origin, tapering toward the tip, 0-1 UVs. Zero sizes take 12 x 5 cm. */
Arrays leafPlaneArrays(double length, double width, int segments, int cross, double taper, bool doubleSided);

// ------------------------------------------------------------------------------------------------ maps and export maths

/** The UV shells of a mesh rasterised to a size x size coverage mask (row 0 at the top, V up), grown by padding pixels. */
std::vector<uint8_t> opacityMask(const Mesh& mesh, int size, double padding);

/** {count, out, in, out99, in99} of signed distances. */
json spread(std::vector<double> distances);

/** Faces of a mesh. */
inline Index faceCount(const Mesh& m) { return m.numFaces(); }
} // namespace treegen
