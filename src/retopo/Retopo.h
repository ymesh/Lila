#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * Retopology: input repair, voxel remeshing, field-aligned auto quad remeshing, and relax/shrinkwrap onto a surface.
 * Everything works in the mesh's own (object) space and is deterministic for a given input.
 */
namespace lila::retopo
{
using geo::Index;

// Repair

/** A problem findIssues looks for. Each lives on one kind of element (issueElement). */
enum class MeshIssue : uint8_t
{
    Coincident,       // points: within the tolerance of another point
    ZeroLengthEdge,   // edges: shorter than the tolerance
    Lamina,           // faces: all their points shared with an earlier face (the extra copies)
    ZeroAreaFace,     // faces: thinner than the tolerance
    NonManifoldEdge,  // edges: three or more faces
    NonManifoldPoint, // points: faces around them form more than one fan
    Hole,             // edges: on an open border
    Triangle,         // faces: three sides
    Ngon,             // faces: more than four sides
    Concave,          // faces: a corner bending inward
    ZeroAreaUv,       // faces: no area in uv set 0
    Flipped,          // faces: wound against their shell (conformNormals would turn them)
    Count
};
inline constexpr size_t kIssueCount = size_t(MeshIssue::Count);

enum class IssueElement : uint8_t
{
    Point,
    Edge,
    Face,
};

LILA_SCENE_API IssueElement issueElement(MeshIssue issue);
/** Lower camel case name, as the meshRepair flag. */
LILA_SCENE_API const char* issueName(MeshIssue issue);
/** Plural words for reports, e.g. "lamina faces". */
LILA_SCENE_API const char* issueLabel(MeshIssue issue);

struct LILA_SCENE_API RepairOptions
{
    /** Which issues to look for and fix, by MeshIssue. */
    std::array<bool, kIssueCount> checks = {true, true, true, true, true, true, false, false, false, false, false, true};
    /** Distance under which points coincide and edges and faces are degenerate; negative scales with the mesh (1e-6 of the bounds diagonal). */
    double tolerance = -1.0;
    /** Repair also removes edges and points left without faces. */
    bool removeLoose = true;

    bool check(MeshIssue i) const { return checks[size_t(i)]; }
    void set(MeshIssue i, bool on) { checks[size_t(i)] = on; }
    /** The tolerance in use for this mesh. */
    double toleranceFor(const geo::Mesh& mesh) const;
};

/** Offending element ids (sorted) per issue, of that issue's element kind. Unchecked issues stay empty. */
struct LILA_SCENE_API MeshIssues
{
    std::array<std::vector<Index>, kIssueCount> ids;

    const std::vector<Index>& of(MeshIssue i) const { return ids[size_t(i)]; }
    Index count(MeshIssue i) const { return Index(ids[size_t(i)].size()); }
    Index total() const;
    /** Union of the ids of every issue on that element kind, sorted. */
    std::vector<Index> elements(IssueElement kind) const;
    /** "3 lamina faces, 2 n-gons", or "no issues". */
    std::string summary() const;
};

/** Looks for the checked issues without changing the mesh. Ids are the mesh's own (not compacted). */
LILA_SCENE_API MeshIssues findIssues(const geo::Mesh& mesh, const RepairOptions& opts = {});

struct LILA_SCENE_API RepairReport
{
    /** How many of each issue were found when its fix ran. Zero-area uvs are only counted: they need a new unwrap. */
    std::array<Index, kIssueCount> fixed{};
    Index looseElements = 0;
    std::string summary() const;
};

/**
 * Fixes the checked issues in order: welds coincident points, collapses zero-length edges, drops lamina copies, merges
 * zero-area slivers into a neighbour, splits non-manifold edges and points apart, fills holes, joins triangles into quads,
 * cuts n-gons and concave faces into quads and triangles, conforms winding. Compacts.
 */
LILA_SCENE_API RepairReport repairMesh(geo::Mesh& mesh, const RepairOptions& opts = {});

struct VoxelOptions
{
    /** Voxels along the longest side of the bounds. */
    int resolution = 128;
    /** Pull the result onto the input surface where it lies close. */
    bool project = true;
};

/**
 * Rebuilds a dirty mesh (holes, self-intersections, inner shells) as one closed manifold quad surface: a narrow-band
 * distance field signed by flooding the outside, contoured with surface nets.
 */
LILA_SCENE_API geo::Mesh voxelRemesh(const geo::Mesh& mesh, const VoxelOptions& opts = {});

// Auto quad remesh

/** A point the edge flow turns round: valence 3 (a corner like a cube's) or 5 (where a loop splits in two). */
struct Pole
{
    glm::dvec3 position{0.0};
    int valence = 3;
};

/** A soft brush on face size: faces inside radius get scale times the edge length, easing back to 1 at the rim. */
struct DensityHandle
{
    glm::dvec3 position{0.0};
    double radius = 1.0;
    /** Edge length multiplier at the centre: 0.5 halves the edges (four times the faces), 2 doubles them. 0.35 to 2.8. */
    double scale = 1.0;
};

/** How quadRemesh lays the quads out. */
enum class RemeshMethod
{
    /**
     * Lines traced from the poles along the edge flow cut the surface into four-sided patches, each filled with a
     * regular grid: edge loops run unbroken between poles and the center line of a symmetric result is one of them.
     */
    Patches,
    /**
     * Each point placed from the edge flow nearby: loops break and poles come in pairs more often. Not a command method;
     * Patches falls back to it, with a note, when its layout fails or comes out with more flaws.
     */
    Field,
    /** A straight grid seen down the sheet's normal, for open sheets like terrain tiles and panels; Patches otherwise. */
    Grid,
};

struct QuadRemeshOptions
{
    RemeshMethod method = RemeshMethod::Patches;
    int targetFaces = 3000;
    /** Retries at a corrected size until the face count is within this fraction of targetFaces; 0 takes the first. */
    double countTolerance = 0.1;
    /** Positive overrides targetFaces. */
    double targetEdgeLength = 0.0;
    /** 0 uniform, 1 strongly denser where curved. The point float "density" attribute multiplies on top. */
    double adaptivity = 0.25;
    /** Make the lattice finer where the surface comes close to itself across a gap, so it doesn't bridge it. */
    bool resolveGaps = true;
    /** Thin parts of a closed shape, like fingers, get at least this many edges round them; 0 off. */
    int edgesAround = 12;
    /** Edges bending at least this many degrees (gentler than sharpAngle, like a bevel's segments) steer the edge flow; 0 off. */
    double softCreaseAngle = 0.0;
    /** How hard poles are kept out of joints (crotch, armpits, finger webs) onto flatter parts nearby; 0 off. */
    double jointPoleAvoidance = 30.0;
    /** Extra faces at joints (crotch, armpits, finger webs), where the mesh bends: 0 none, 1 about three times as dense there. */
    double jointDensity = 0.5;
    /** How far Joint Density's extra faces reach from a fold, in faces; they halve every two. */
    double jointDensityReach = 6.0;
    /** Align to edges flagged "hard". */
    bool useHardEdges = true;
    /** Also align to edges whose faces meet sharper than this many degrees; negative disables. */
    double sharpAngle = 40.0;
    /** -1 off, 0/1/2 for a mirror plane through the origin across x/y/z: the result is exactly symmetric. */
    int symmetryAxis = -1;
    /** Polylines on (or near) the surface the edge flow should follow. */
    std::vector<std::vector<glm::dvec3>> guides;
    /** Polylines on the surface an edge loop lands on, like a hard edge: the flow follows them and points snap onto them. */
    std::vector<std::vector<glm::dvec3>> featureGuides;
    std::vector<DensityHandle> densityHandles;
    /** All quads (extract at half density and split each polygon); otherwise quad dominant. */
    bool pureQuads = true;
    int relaxIterations = 6;
    /**
     * Source points (object space, exact) the result keeps as vertices, like a patch's border points to stitch it back by.
     * Laid out by one global parametrization, which holds them exactly, whatever the method.
     */
    std::vector<glm::dvec3> pinnedPoints;
    /** Seconds before giving up with an error saying how far it got; 0 is unlimited. The progress overlay can cancel too. */
    double timeout = 0.0;
    /**
     * When set, the edge flow turns exactly at these poles and nowhere else: start from findPoles, then move, add or
     * remove them. On a closed shell the poles must balance: (4 - valence) over them sums to the shell's total.
     */
    std::optional<std::vector<Pole>> poles;
};

struct LILA_SCENE_API QualityMetrics
{
    Index faces = 0;
    Index quads = 0;
    Index triangles = 0;
    Index ngons = 0;
    double quadPercent = 0.0;
    /** Interior points by valence: valence[k] points have k edges. */
    std::vector<Index> valence;
    Index boundaryPoints = 0;
    /** Interior points of valence other than 4. */
    Index irregular = 0;
    /** Largest and mean deviation of quad corner angles from 90, degrees. */
    double maxAngleDeviation = 0.0;
    double meanAngleDeviation = 0.0;
    /** Distance from points and face centres to the source surface, when one is given. */
    double maxDistance = 0.0;
    double meanDistance = 0.0;
    std::string summary() const;
};

struct PoleResult
{
    std::vector<Pole> poles;
    /** Per closed shell, what (4 - valence) over its poles must sum to; empty for a shell with a border, which takes any. */
    std::vector<std::optional<int>> shellTotals;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** The poles quadRemesh's edge flow has with these options, in the mesh's space, without extracting a mesh. */
LILA_SCENE_API PoleResult findPoles(const geo::Mesh& source, const QuadRemeshOptions& opts = {});

/** Measures a mesh; distances need the source's Bvh. */
LILA_SCENE_API QualityMetrics measureQuality(const geo::Mesh& mesh, const geo::MeshBvh* source = nullptr);

/** Where a requested pole ended up: the nearest irregular result point, or none within a few faces of it. */
struct PoleMatch
{
    Index point = geo::kInvalid;
    int valence = 0;
    double distance = 0.0;
    bool valenceMatched = false;
};

/** One layout quadRemesh built on the way to its result. */
struct RemeshAttempt
{
    std::string layout;
    Index faces = 0;
    int defects = 0;
    Index irregular = 0;
};

struct QuadRemeshResult
{
    geo::Mesh mesh;
    QualityMetrics metrics;
    /** The layout kept: patches, field, shifted, grid, or seamless for pinned points. */
    std::string layout;
    /** Every layout built, in order, the kept one among them. */
    std::vector<RemeshAttempt> attempts;
    /** Per QuadRemeshOptions::poles entry, in order. */
    std::vector<PoleMatch> poleMatches;
    /** Wall time per stage, seconds. */
    std::vector<std::pair<std::string, double>> timings;
    /** Folds, holes, pinched edges and fans left in the mesh when no lattice tried came out clean; 0 normally. */
    int defects = 0;
    /** Where the result departs from what was asked and why, such as a face count kept outside countTolerance. */
    std::vector<std::string> notes;
    std::string error;
    bool ok() const { return error.empty(); }
};

/**
 * Field-aligned quad remesh: a smooth 4-RoSy cross field aligned to curvature, features and guides; an Instant Meshes
 * style position field solved on a multiresolution hierarchy; graph extraction into quads; cleanup; reprojection onto
 * the source with feature-preserving tangential relaxation.
 */
LILA_SCENE_API QuadRemeshResult quadRemesh(const geo::Mesh& source, const QuadRemeshOptions& opts = {});

// Relax and shrinkwrap

struct RelaxOptions
{
    int iterations = 10;
    /** Fraction of the way to the neighbour average per iteration. */
    double strength = 0.5;
    /** Border points stay put; otherwise they slide along the border, keeping its shape. */
    bool pinBoundary = true;
    /** Per point capacity; nonzero points stay put. Optional. */
    std::vector<uint8_t> pinned;
    /** Per point capacity; when given only these move. Optional. */
    std::vector<uint8_t> active;
    /** Move within the tangent plane only, so the shape stays when there is no target. */
    bool tangential = true;
    /** Keep points on the starting surface (or the target). Off is plain smoothing, which shrinks and rounds. */
    bool project = true;
    /** Edges whose faces meet sharper than this many degrees are creases: points slide along them, and their corners and ends stay. Negative turns it off. */
    double sharpAngle = 12.0;
    /** Edges flagged hard are creases too. */
    bool hardEdgesAreCreases = true;
    /** Points whose face angles fall short of (or exceed) a flat 360 by more than this many degrees, like a cone's tip, stay. Negative turns it off. */
    double cornerAngle = 60.0;
};

struct FeatureOptions
{
    bool boundary = true;
    /** Edges with a "crease" above zero. */
    bool creases = false;
    bool hardEdges = false;
    /** Edges marked "seam", and edges where any uv set splits. */
    bool uvSeams = false;
};

/** Points on the chosen kinds of feature edge, per point capacity: the points to pin when relaxing. */
LILA_SCENE_API std::vector<uint8_t> featurePoints(const geo::Mesh& mesh, const FeatureOptions& opts);

/**
 * Laplacian relax keeping points on a surface: the target when given, otherwise the mesh's own starting shape.
 * Parallel and deterministic.
 */
LILA_SCENE_API void relaxMesh(geo::Mesh& mesh, const geo::MeshBvh* target, const RelaxOptions& opts = {});

/** Projects every point onto the target, then relaxes on it so the projection doesn't bunch up. */
LILA_SCENE_API void shrinkwrap(geo::Mesh& mesh, const geo::MeshBvh& target, int relaxIterations = 3);

enum class ProjectMethod : uint8_t
{
    Closest,
    /** Along each point's normal, both ways. */
    Normal,
    /** Along one direction, both ways. */
    Direction,
};

struct ProjectOptions
{
    ProjectMethod method = ProjectMethod::Closest;
    /** For Direction, in the mesh's space. */
    glm::dvec3 direction{0.0, 0.0, 1.0};
    /** Distance to stay off the target, along its normal: positive outside. */
    double offset = 0.0;
    /** Points farther than this stay put; negative is unlimited. */
    double maxDistance = -1.0;
    /** Per point capacity; when given only these move. Optional. */
    std::vector<uint8_t> active;
};

/**
 * Moves points onto the target: to the closest point, or to the nearest hit along a ray (falling back to the closest
 * point where the ray misses). The target mesh must be the one bvh was built from. Returns how many points moved.
 */
LILA_SCENE_API Index projectMesh(geo::Mesh& mesh, const geo::Mesh& target, const geo::MeshBvh& bvh, const ProjectOptions& opts = {});

// Borders

/** A stretch of open border, in order. Open runs have one more point than edges; closed loops the same count. */
struct BorderRun
{
    std::vector<Index> edges;
    /** edges[i] runs from points[i] to points[i + 1] (wrapping for a closed loop). */
    std::vector<Index> points;
    bool closed = false;
};

/**
 * The border run through a border edge, grown both ways while the turn between successive edges stays at or under
 * maxTurnDegrees (180 takes the whole loop). It stops where the border ends or branches. A loop cut by a single sharp
 * corner comes back open, with that corner's point first and last.
 */
LILA_SCENE_API BorderRun walkBorder(const geo::Mesh& mesh, Index edge, double maxTurnDegrees = 180.0);

// Retopo Draw

/** Where Retopo Draw's angle snap puts a point, and the plane it snapped in. */
struct AngleSnap
{
    glm::dvec3 point{0.0};
    /** The frame axis normal to the snap plane. Landing the point on the surface along it keeps the line straight seen down it. */
    glm::dvec3 axis{0.0};
    /** The snapped direction in the plane, degrees from the plane's first axis, in (-180, 180]. */
    double degrees = 0.0;
};

/**
 * Snaps the line from anchor toward target to the nearest multiple of stepDegrees. frame's columns are orthonormal axes
 * (the reference object's rotation, or identity for world). The snap happens in whichever of their three principal planes
 * faces surfaceNormal most, so on a surface the line follows the frame's axes and their diagonals. The point is target
 * projected onto the snapped direction, in that plane through anchor. stepDegrees <= 0 only flattens into the plane.
 */
LILA_SCENE_API AngleSnap angleSnap(const glm::dvec3& anchor, const glm::dvec3& target, const glm::dvec3& surfaceNormal,
                                   const glm::dmat3& frame, double stepDegrees);

/** A world matrix's rotation as orthonormal columns, without its scale and shear; identity where it is degenerate. */
LILA_SCENE_API glm::dmat3 rotationFrame(const glm::dmat4& world);
} // namespace lila::retopo
