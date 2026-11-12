#pragma once

#include "core/Export.h"

#include "curve/Spline.h"
#include "curve/Tessellate.h"
#include "geo/Heightfield.h"
#include "geo/Mesh.h"

#include <glm/glm.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace lila::gn
{
enum class SocketType : uint8_t
{
    Geometry,
    Float,
    Int,
    Bool,
    Vector,
    Rotation, // Euler XYZ, radians
    String,
    Color, // linear RGBA
    Matrix, // 4x4 transform, column-major
    /** Named values of any type travelling as one (Combine Bundle). */
    Bundle,
    /** A piece of node graph run where it is evaluated (Closure zone, Evaluate Closure). */
    Closure,
};

LILA_SCENE_API const char* socketTypeName(SocketType t);
LILA_SCENE_API bool parseSocketType(std::string_view name, SocketType* out);
LILA_SCENE_API bool isDataType(SocketType t);

/** Domains a field can be evaluated on. Mesh and point-cloud data use the first four. */
enum class Domain : uint8_t
{
    Point,
    Edge,
    Face,
    Corner,
    Curve,
    Instance,
};

LILA_SCENE_API const char* domainName(Domain d);
LILA_SCENE_API bool parseDomain(std::string_view name, Domain* out);

/**
 * Curves: curve c owns points [offsets[c], offsets[c + 1]). Without spline attributes every curve is poly. Spline data
 * rides in builtin attributes, so filtering, joining and fields carry it: per curve "curve_type" (curve::Kind), "degree"
 * and "resolution"; per point "handle_left", "handle_right", "handle_type_left", "handle_type_right" (curve::HandleType),
 * "weight", "tilt", "radius" and "cornerRadius". Positions are Bezier anchors or NURBS control points; nodes that want the
 * shape read evaluated().
 */
struct LILA_SCENE_API CurveSet
{
    std::vector<glm::dvec3> positions;
    std::vector<geo::Index> offsets{0};
    std::vector<uint8_t> cyclic;
    geo::AttributeSet pointAttrs;
    geo::AttributeSet curveAttrs;
    /** Per curve NURBS knots in curve::Spline's form, empty for uniform. Nodes that rebuild curves point by point drop them. */
    std::vector<std::vector<double>> knots;

    size_t numCurves() const { return cyclic.size(); }
    size_t numPoints() const { return positions.size(); }
    size_t curveSize(size_t c) const { return offsets[c + 1] - offsets[c]; }
    std::span<const glm::dvec3> curvePoints(size_t c) const { return {positions.data() + offsets[c], curveSize(c)}; }

    void addCurve(std::span<const glm::dvec3> points, bool closed);
    /** Adds a spline of any kind, its data in the builtin attributes. */
    void addSpline(const curve::Spline& s);
    /** Curve c as a spline (poly when the set has no spline attributes). */
    curve::Spline spline(size_t c) const;
    /** True when every curve is poly, so positions are the shape. */
    bool isPoly() const;
    /** Every curve tessellated to poly, point attributes interpolated along; a poly set comes back unchanged. */
    CurveSet evaluated(const curve::TessOptions& o = {}) const;
    /** Merges attribute layers by name, like Mesh::append. */
    void append(const CurveSet& other);
    /** Brings attribute sizes in line after positions or curves were added directly. */
    void syncSizes();
};

struct Geometry;
using GeometryPtr = std::shared_ptr<const Geometry>;
struct Volume;

/** Transformed references to other geometry, one per instance. */
struct LILA_SCENE_API Instances
{
    std::vector<glm::dmat4> transforms;
    std::vector<int> reference; // index into references
    std::vector<GeometryPtr> references;
    geo::AttributeSet attrs;

    size_t size() const { return transforms.size(); }
    void add(const glm::dmat4& xf, int ref);
};

/**
 * What flows along geometry sockets: up to one of each component, shared and immutable. A node that changes one
 * copies just that component (copy on write).
 */
struct LILA_SCENE_API Geometry
{
    std::shared_ptr<const geo::Mesh> mesh;   // always compact
    std::shared_ptr<const geo::Mesh> points; // point cloud: a mesh with only loose points
    std::shared_ptr<const CurveSet> curves;
    std::shared_ptr<const Instances> instances;
    /** Terrain heights on a grid. It reaches the modifier stack, and anything that only takes meshes, as a mesh. */
    std::shared_ptr<const geo::Heightfield> heightfield;
    /** A density, signed-distance or vector grid (gn/Volume.h). Never reaches the modifier stack; Volume to Mesh makes it a mesh. */
    std::shared_ptr<const Volume> volume;
    /** A label for the whole geometry (Set Geometry Name); joins keep the first named part's. */
    std::string name;

    bool empty() const;
};

LILA_SCENE_API GeometryPtr makeGeometry(Geometry g);
LILA_SCENE_API GeometryPtr meshGeometry(geo::Mesh m);
LILA_SCENE_API GeometryPtr pointGeometry(geo::Mesh m);
LILA_SCENE_API GeometryPtr curveGeometry(CurveSet c);
LILA_SCENE_API GeometryPtr heightfieldGeometry(geo::Heightfield h);

/**
 * Every component transformed. Instances transform their own matrices only. A heightfield stays one under translation
 * and scale with equal X and Z; any other transform turns it into a mesh. A volume keeps its values and moves its grid;
 * signed distances are scaled by the transform's average scale and vectors turned by its linear part.
 */
LILA_SCENE_API Geometry transformed(const Geometry& g, const glm::dmat4& xf);
LILA_SCENE_API void transformMesh(geo::Mesh& m, const glm::dmat4& xf);
LILA_SCENE_API void transformCurves(CurveSet& c, const glm::dmat4& xf);

/**
 * Component-wise union. Meshes are appended, instances concatenated. The first heightfield stays one; later ones become
 * meshes. The first volume is kept; later volumes of the same kind on the same grid merge into it (signed distances by
 * union, densities and vectors summed) and others are dropped.
 */
LILA_SCENE_API Geometry join(std::span<const GeometryPtr> parts);

/** Instances replaced by the geometry they point at, recursively. */
LILA_SCENE_API Geometry realize(const Geometry& g);

/** The result handed back to the modifier stack: realized, and only the mesh, with any heightfield added as one (every few samples past about 4 million). Anonymous attributes are dropped. */
LILA_SCENE_API geo::Mesh toOutputMesh(const Geometry& g);

/** A point cloud has positions and point attributes; this keeps those and drops everything else. */
LILA_SCENE_API geo::Mesh pointsOnly(const geo::Mesh& m);

/** Removes "__"-prefixed attributes made for fields inside a group. */
LILA_SCENE_API void stripAnonymous(geo::Mesh& m);
} // namespace lila::gn
