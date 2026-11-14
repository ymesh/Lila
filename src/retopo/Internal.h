#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace lila::progress
{
class Scope;
}

namespace lila::retopo
{
struct QuadRemeshOptions;
struct QuadRemeshResult;
} // namespace lila::retopo

namespace lila::retopo::detail
{
/** Why a remesh stopped early: it ran past its time budget, or was cancelled from the progress overlay. */
struct Interrupted : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

/**
 * The running remesh's time budget on this thread. reached() records a finished stage, moves the progress bar and checks;
 * checkpoint() only checks. Both throw Interrupted past the deadline or once the progress root is cancelled, and do
 * nothing while no Budget is open on the thread.
 */
class LILA_SCENE_API Budget
{
public:
    /** seconds <= 0 is unlimited. scope may be null. */
    Budget(double seconds, progress::Scope* scope);
    ~Budget();
    Budget(const Budget&) = delete;
    Budget& operator=(const Budget&) = delete;

    static void reached(const char* stage, double fraction);
    static void checkpoint();
    /** Seconds left before the deadline; infinite with no Budget open or no limit. */
    static double remaining();
    /** Facts for the error message if it runs out ("24k working triangles"). */
    static void note(std::string fact);
    /** "after cross field (24k working triangles)", for the error. */
    std::string where() const;
    double seconds() const { return mSeconds; }

private:
    void check() const;

    double mSeconds;
    double mStart;
    progress::Scope* mScope;
    std::string mStage;
    std::vector<std::string> mNotes;
    Budget* mPrev;
};

using geo::Index;
using geo::kInvalid;

inline uint64_t edgeKey(Index a, Index b)
{
    if (a > b)
        std::swap(a, b);
    return (uint64_t(a) << 32) | b;
}

enum class FeatureKind : uint8_t
{
    None,
    Line,
    Corner,
};

/** The triangle mesh the remesher works on, plus the adjacency and per-vertex data the fields need. */
struct LILA_SCENE_API Surface
{
    std::vector<glm::dvec3> pos;
    std::vector<std::array<Index, 3>> tris;
    std::vector<float> density;           // per vertex, 1 is neutral
    std::unordered_set<uint64_t> hard;    // vertex pairs of user feature edges
    std::vector<glm::dvec3> pins;         // positions that must stay result vertices; detectFeatures makes them corners

    // From buildTopology()
    std::vector<std::array<Index, 2>> edges; // sorted endpoints
    std::vector<std::array<Index, 2>> edgeTris;
    std::vector<uint8_t> edgeTriCount;    // 3 means three or more
    std::vector<Index> adjStart;          // CSR, size pos+1
    std::vector<Index> adj;
    std::vector<Index> adjEdge;

    // From buildGeometry()
    std::vector<glm::dvec3> triNormal;
    std::vector<double> triArea;
    std::vector<glm::dvec3> normal;
    std::vector<double> area;

    // From detectFeatures()
    std::vector<uint8_t> edgeFeature;
    std::vector<uint8_t> edgeBoundary;
    std::vector<FeatureKind> feature;
    std::vector<glm::dvec3> featureDir; // unit, in the vertex tangent plane, for Line
    std::vector<uint8_t> onBoundary;
    std::vector<uint8_t> pinned; // per vertex: sits on one of pins

    Index vertexCount() const { return Index(pos.size()); }
    void buildTopology();
    void buildGeometry();
    /**
     * Feature edges: borders, hard edges, and creases sharper than sharpAngleDeg that run at least minCreaseLength.
     * Also makes a corner of any point whose face angles fall short of (or exceed) a flat 360 by more than
     * cornerDeficitDeg, like a cone's tip.
     */
    void detectFeatures(double sharpAngleDeg, double cornerDeficitDeg = 75.0, double minCreaseLength = 0.0);
    /**
     * Angle creases, open or closed, must run three times minCreaseLength once the noise is gone: a shorter one holds
     * no loop and only walls off a sliver of the layout.
     */
    void dropShortCreases(const std::vector<uint8_t>& byAngle, double minCreaseLength);
    /** Keeps only corners at least spacing apart (the most constrained first), so the lattice can honour them. */
    void thinCorners(const std::vector<double>& spacing);
    double totalArea() const;
    double meanEdgeLength() const;
    /**
     * Per vertex: a corner made by its own sharp saddle (the bottom of a finger web) with no crease through it. Lines may
     * turn there, but no result point is pulled onto it.
     */
    std::vector<uint8_t> saddleTips() const;
};

/**
 * An inner vertex where creases meet at a corner, with its triangle fan in counter-clockwise order split into sectors
 * between consecutive creases. A sector of angle theta can only hold a whole number k = round(theta / 90 deg) of quad
 * corners, so the lattice maps it like z^alpha, alpha = k * 90 deg / theta.
 */
struct CreaseJunction
{
    struct Wedge
    {
        Index tri = kInvalid;
        Index next = kInvalid, prev = kInvalid; // the fan corners after the junction, counter-clockwise
        double angle = 0.0;                     // the triangle's angle at the junction
        double before = 0.0;                    // the sector's angle up to this wedge's first edge
        double alpha = 1.0;                     // the sector's map exponent
    };
    Index vertex = kInvalid;
    std::vector<Wedge> fan;
};
LILA_SCENE_API std::vector<CreaseJunction> creaseJunctions(const Surface& s);

/**
 * The thickness straight through a round part (a finger, a limb) from vertex v, or 0 where v isn't on one: on or next to
 * a crease, in a hollow, or where the ray finds no wall facing straight back within maxThickness.
 */
LILA_SCENE_API double roundThickness(const Surface& s, const geo::Mesh& src, const geo::MeshBvh& bvh, const std::vector<double>& curvature,
                                     Index v, double maxThickness);

/** Triangulates a polygon mesh into a surface; hard edges become feature pairs when asked. */
LILA_SCENE_API Surface surfaceFromMesh(const geo::Mesh& mesh, bool useHardEdges);
/** Splits edges longer than maxLength until none are. */
LILA_SCENE_API void refineSurface(Surface& s, double maxLength);
/** QEM simplification to about targetTris, keeping borders and user feature edges. */
LILA_SCENE_API void decimateSurface(Surface& s, Index targetTris);
/** Evens out triangle shapes by tangential relaxation projected onto the source; feature and border vertices stay. */
LILA_SCENE_API void smoothSurface(Surface& s, const geo::MeshBvh& source, int iterations);
/** Keeps the part with plane.distance >= 0, cutting triangles; cut points land exactly on the plane. */
LILA_SCENE_API void clipSurface(Surface& s, const geo::SymmetryPlane& plane, int axis);
/**
 * The whole shape from a half clipped at plane: the half's vertices and triangles first, then the reflections of those
 * off the plane, winding reversed. mirrorOf maps each vertex to its reflection (plane vertices to themselves); triangle t
 * of the half reflects to t + half.tris.size(). Needs buildTopology and the rest afterwards.
 */
LILA_SCENE_API Surface mirrorSurface(const Surface& half, const geo::SymmetryPlane& plane, std::vector<Index>& mirrorOf);
/** Drops vertices no triangle uses. */
LILA_SCENE_API void compactSurface(Surface& s);

/** Smooth unit 4-RoSy field as one tangent direction per vertex. */
struct FieldOptions
{
    double scale = 1.0;           // target lattice spacing, for the smoothing length
    double curvatureWeight = 1.0; // alignment to principal directions
    double softCreaseAngle = 0.0; // edges bending at least this many degrees (short of features) pull the field along them; <= 0 off
    double jointStiffness = 30.0; // smoothness gain in saddles (crotch, armpit, finger webs), which keeps poles out of joints; 0 off
    std::vector<std::vector<glm::dvec3>> guides;
    /** Vertices held to a direction like a feature line, without being one (a symmetry seam). */
    std::vector<std::pair<Index, glm::dvec3>> aligned;
    /** Per surface edge, an angle (in the field's fourth power) added to its transport; from prescribeSingularities. */
    std::vector<double> twist;
};
LILA_SCENE_API std::vector<glm::dvec3> solveCrossField(const Surface& s, const FieldOptions& opts);

/**
 * Per surface edge, the angle a direction at edges[e][0] turns by on its way to edges[e][1], in the field's fourth
 * power (a quarter turn is 2pi), measured in each vertex's own tangent frame.
 */
LILA_SCENE_API std::vector<double> transportAngles(const Surface& s);
/** Per vertex, the field's direction as a fourth-power angle in the vertex frame. */
LILA_SCENE_API std::vector<double> fieldAngles(const Surface& s, const std::vector<glm::dvec3>& q);
/** The inverse of fieldAngles. */
LILA_SCENE_API std::vector<glm::dvec3> fieldFromAngles(const Surface& s, const std::vector<double>& phi);

/** A cross field singularity: a triangle the field turns round in, by index quarter turns. */
struct FieldSingularity
{
    Index tri = kInvalid;
    /** +1 becomes a three-edge pole, -1 a five-edge pole. */
    int index = 0;
};

/** The singular triangles of a field, in triangle order. */
LILA_SCENE_API std::vector<FieldSingularity> fieldSingularities(const Surface& s, const std::vector<glm::dvec3>& q);

/**
 * The smallest twist (a trivial connection, Crane et al. 2010) whose smooth fields turn exactly at the given triangles.
 * Every closed shell's indices must add up to its total turning (shellIndexTotals); error says which doesn't.
 */
struct PrescribedTwist
{
    std::vector<double> twist;
    std::string error;
};
LILA_SCENE_API PrescribedTwist prescribeSingularities(const Surface& s, std::span<const FieldSingularity> singularities);

/**
 * The poles left when each three-edge and five-edge pole closer than `reach` lattice steps (scale per vertex) cancel,
 * closest pairs first. Such a pair is a dislocation: the flow kinks there but turns no further.
 */
LILA_SCENE_API std::vector<FieldSingularity> cancelClosePoles(const Surface& s, const std::vector<FieldSingularity>& poles,
                                                              const std::vector<double>& scale, double reach);
/**
 * Poles moved apart until no two are closer than minSep lattice steps over the surface. Each pole too close to one
 * already placed moves to the nearest clear spot within two steps. With allowed, a pole on a triangle it rejects moves to
 * the nearest allowed clear spot within four steps. A pole with no such spot goes to the nearest clear one within four
 * steps, allowed or not, or stays where it is.
 */
LILA_SCENE_API std::vector<FieldSingularity> spreadPoles(const Surface& s, const std::vector<FieldSingularity>& poles,
                                                         const std::vector<double>& scale, double minSep,
                                                         const std::function<bool(Index)>& allowed = {});

/** Per triangle, its shell (connected through edges); and per shell, the index total its singularities must reach, or INT_MIN when it has a border. */
struct Shells
{
    std::vector<Index> shellOfTri;
    std::vector<int> indexTotal;
};
LILA_SCENE_API Shells shellIndexTotals(const Surface& s);

/** A field parallel under the twisted transport, spread from one vertex per component and turned to fit the features best. Instant. */
LILA_SCENE_API std::vector<glm::dvec3> propagateField(const Surface& s, const std::vector<double>& twist);

/** Per-vertex principal curvature tensor in the vertex frame: direction angle doubled and magnitudes. */
struct Curvature
{
    std::vector<glm::dvec3> maxDir; // unit, tangent
    std::vector<double> anisotropy; // |k1 - k2|
    std::vector<double> magnitude;  // sqrt(k1^2 + k2^2)
    std::vector<double> coherence;  // 0 to 1: the share of the anisotropy left after averaging over coherenceRadius
};
LILA_SCENE_API Curvature estimateCurvature(const Surface& s, double coherenceRadius = 0.0);
/** 0 to 1 per vertex: how much it sits in a saddle (crotch, armpit, finger web) at the scale of radius. */
std::vector<double> saddleAmount(const Surface& s, double radius);

/** q1 turned by a multiple of 90 degrees about n1 to lie closest to q0. */
LILA_SCENE_API glm::dvec3 matchQuarterRotation(const glm::dvec3& q0, const glm::dvec3& q1, const glm::dvec3& n1);

/** Nearest-point queries against the surface's feature edges, for keeping vertices on creases. */
class LILA_SCENE_API FeatureLines
{
public:
    /**
     * The surface's feature edges and corners; with boundaryOnly just its open borders and the corners on them, with
     * innerOnly just those off the borders.
     */
    void build(const Surface& s, double cellSize, bool boundaryOnly = false, bool innerOnly = false);
    bool empty() const { return mSegs.empty(); }
    const std::vector<glm::dvec3>& corners() const { return mCorners; }
    /** Closest point on any feature segment within maxDist; false if none. tangent is the segment direction. */
    bool closest(const glm::dvec3& p, double maxDist, glm::dvec3& out, glm::dvec3* tangent = nullptr) const;
    /** Corner points within maxDist; false if none. */
    bool closestCorner(const glm::dvec3& p, double maxDist, glm::dvec3& out) const;

private:
    std::array<int64_t, 3> cell(const glm::dvec3& p) const;
    uint64_t hash(const std::array<int64_t, 3>& c) const;

    std::vector<std::array<glm::dvec3, 2>> mSegs;
    std::vector<glm::dvec3> mCorners;
    double mCell = 1.0;
    std::vector<std::vector<Index>> mBuckets;      // segments per hashed cell
    std::vector<std::vector<Index>> mCornerBuckets;
};

/** The point nearest x on any of the segments; x itself when there are none. */
inline glm::dvec3 closestOnSegments(const glm::dvec3& x, std::span<const std::array<glm::dvec3, 2>> segs)
{
    glm::dvec3 best = x;
    double bestD = std::numeric_limits<double>::max();
    for (const auto& [a, b] : segs)
    {
        const glm::dvec3 ab = b - a;
        const double len2 = glm::dot(ab, ab);
        const double t = len2 > 0.0 ? std::clamp(glm::dot(x - a, ab) / len2, 0.0, 1.0) : 0.0;
        const glm::dvec3 q = a + ab * t;
        const double d = glm::dot(x - q, x - q);
        if (d < bestD)
        {
            bestD = d;
            best = q;
        }
    }
    return best;
}

/** A straight grid over an open sheet seen down its normal (RemeshMethod::Grid); an error when the sheet doesn't suit one. */
QuadRemeshResult gridRemesh(const geo::Mesh& source, const QuadRemeshOptions& opts);

/** Unit vector perpendicular to n, chosen deterministically. */
inline glm::dvec3 anyTangent(const glm::dvec3& n)
{
    glm::dvec3 a = std::abs(n.x) < 0.6 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    return glm::normalize(glm::cross(n, glm::cross(a, n)));
}
} // namespace lila::retopo::detail
