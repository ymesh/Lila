#pragma once

#include "core/Export.h"

#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "geo/Symmetry.h"

#include <glm/glm.hpp>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/**
 * Topology constructs: patches, strips, bridges, sweeps, cylinders and auto regions that stay parametric after they are
 * made. Each is a spec (its inputs and settings) that generates faces; the faces carry the construct's id in a face
 * attribute so the construct can delete and regenerate them when a setting changes. Everything here is in the retopo
 * mesh's object space.
 */
namespace lila::retopo
{
using geo::Index;

/** Face int attribute holding the id of the construct that made the face, 0 for none. */
inline constexpr const char* kConstructAttr = "retopoConstruct";

enum class ConstructKind : uint8_t
{
    Patch,
    Strip,
    Stroke,
    Bridge,
    Sweep,
    Cylinder,
    Region,
};

LILA_SCENE_API const char* constructKindName(ConstructKind k);
/** False when the name is not a kind. */
LILA_SCENE_API bool constructKindFromName(std::string_view name, ConstructKind& out);

/** One side of a patch, corner to corner: existing points kept exactly, or a free path resampled to count edges. */
struct PatchSide
{
    std::vector<glm::dvec3> points;
    bool fixed = false;
    /** Edges along a free side; zero or negative takes the path length over the edge length. */
    int count = 0;
};

/** A closed boundary filled with quads (see fillPatch). */
struct PatchSpec
{
    std::vector<PatchSide> sides;
    int padding = -1;
    int variant = 0;
    std::optional<glm::dvec3> pull;
    /** A cap's outward direction; see PatchOptions::lift. */
    std::optional<glm::dvec3> lift;
};

/** A band of quads along a path, its knots the handles that shape it. */
struct StripSpec
{
    std::vector<glm::dvec3> knots;
    /** Per knot width multiplier (scale handles), 1 when missing. */
    std::vector<double> scales;
    /** Per knot twist of the cross direction about the surface normal, degrees (rotate handles). */
    std::vector<double> twists;
    /** Per knot: nonzero where the path makes a corner instead of flowing round. */
    std::vector<uint8_t> corners;
    double width = 1.0;
    /** Quads along; zero or negative keeps them near square. */
    int divisions = 0;
    /** Quads across. */
    int rows = 1;
    bool closed = false;
    /** Existing border edges (their two points) the strip's first and last rows join exactly; one quad across only. */
    std::optional<std::array<glm::dvec3, 2>> startRow, endRow;
};

/** A loose edge chain on the surface, waiting to be bridged; makes no faces. */
struct StrokeSpec
{
    std::vector<glm::dvec3> path;
    int divisions = 8;
};

/** A grid between two sides, each a free path or a run of existing points. */
struct BridgeSpec
{
    std::vector<glm::dvec3> a, b;
    bool aFixed = false, bFixed = false;
    /** Edges along when neither side is fixed; zero or negative from the length. */
    int divisions = 0;
    /** Edges across; zero or negative from the gap. */
    int rows = 0;
};

/** A border run carried along a path that starts on the border: an extrusion whose side is the stroke. */
struct SweepSpec
{
    /** The whole border the run is taken from, exact existing points, in order. */
    std::vector<glm::dvec3> border;
    bool borderClosed = false;
    /** Where the path starts on the border. */
    int anchor = 0;
    /** Border edges carried, signed: positive runs forward along border from anchor, negative backward. */
    int span = 1;
    std::vector<glm::dvec3> path;
    /** Rows along the path; zero or negative from its length. */
    int rows = 0;
};

/** A cross-section ring: the reference cut by the plane through origin with normal, keeping the loop nearest pick. */
struct CylinderRing
{
    glm::dvec3 origin{0.0};
    glm::dvec3 normal{0.0, 1.0, 0.0};
    glm::dvec3 pick{0.0};
};

/** Rings wrapped round the reference, bridged in order. */
struct CylinderSpec
{
    std::vector<CylinderRing> rings;
    int points = 8;
    /** Turns the first ring's start point round its axis, degrees. */
    double rotate = 0.0;
    /** Extra turn per ring, degrees. */
    double twist = 0.0;
    /** Multiplies the number of rows between rings (1 keeps quads near square). */
    double rowDensity = 1.0;
    bool capStart = false;
    bool capEnd = false;
};

/** Part of the reference auto retopologised and stitched to the retopo mesh around it. */
struct RegionSpec
{
    /** Reference face ids, valid while the reference has referenceFaces faces. */
    std::vector<Index> faces;
    Index referenceFaces = 0;
    double edgeLength = 1.0;
    /** Positive sets the density from the region's area, overriding edgeLength. */
    int targetFaces = 0;
    std::vector<std::vector<glm::dvec3>> guides;
    /** Existing retopo border points the region's border must keep, in loop order; may be empty. */
    std::vector<glm::dvec3> border;
};

using ConstructSpec = std::variant<PatchSpec, StripSpec, StrokeSpec, BridgeSpec, SweepSpec, CylinderSpec, RegionSpec>;

LILA_SCENE_API ConstructKind constructKind(const ConstructSpec& spec);

/** What a generator needs to know about the scene. */
struct GenContext
{
    /** The reference surface, in the retopo mesh's space; both may be null. */
    const geo::Mesh* surfaceMesh = nullptr;
    const geo::MeshBvh* surface = nullptr;
    /** Default edge length for counts left on auto. */
    double edgeLength = 1.0;
    /** Seconds an auto region may take; zero is unlimited. */
    double timeout = 0.0;
};

struct GenResult
{
    std::vector<glm::dvec3> positions;
    std::vector<std::vector<Index>> faces;
    /** Per position: nonzero where it is an existing retopo point that must weld exactly and stay put. */
    std::vector<uint8_t> fixed;
    /** Irregular interior points, as position indices. */
    std::vector<Index> poles;
    /** Patch only: how many variants it can cycle through. */
    int variants = 0;
    /** The counts it settled on: per side for a patch; along and rows for a strip or bridge; rows for a sweep. */
    std::vector<int> sideCounts;
    std::string error;
    bool ok() const { return error.empty(); }
};

/** Builds a construct's faces from its spec. Strokes give no faces. */
LILA_SCENE_API GenResult generate(const ConstructSpec& spec, const GenContext& ctx);

struct StitchResult
{
    /** Faces added, including mirrored copies. */
    std::vector<Index> faces;
    /** Points added that weren't fixed: what settling may move. */
    std::vector<Index> movable;
};

/**
 * Adds generated faces to the mesh tagged with id: positions within weld of an existing point reuse it, faces wind to
 * agree with neighbours they share edges with, faces that already exist or would make a non-manifold edge are skipped.
 * With a symmetry plane, points within snap of it land on it and the mirrored faces are added too. The construct's own
 * border points within snap of an open border point of the mesh join it, one each, nearest first.
 */
LILA_SCENE_API StitchResult stitchConstruct(geo::Mesh& mesh, const GenResult& gen, int id, double weld,
                                            const std::optional<geo::SymmetryPlane>& symmetry, double snap = 0.0);

/** Faces tagged with a construct id, ascending. */
LILA_SCENE_API std::vector<Index> constructFaces(const geo::Mesh& mesh, int id);

/** Deletes a construct's faces and the points only they used. Points shared with other faces stay. */
LILA_SCENE_API void removeConstructFaces(geo::Mesh& mesh, int id);

/** Clears a construct's tag from its faces, leaving them as plain geometry. */
LILA_SCENE_API void untagConstruct(geo::Mesh& mesh, int id);

/**
 * Fingerprint of a construct's faces: their corner positions in face order, rounded to a fraction of the mesh size.
 * Unchanged until someone moves, deletes or rebuilds those faces, which is what freezes a construct.
 */
LILA_SCENE_API uint64_t constructHash(const geo::Mesh& mesh, int id);

/** Quads of a regular grid between two rails of equal length: rows + 1 lines of points from a to b. */
LILA_SCENE_API GenResult gridBetween(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, int rows, const geo::MeshBvh* surface);
} // namespace lila::retopo
