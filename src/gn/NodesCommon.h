#pragma once

#include "core/Export.h"

#include "gn/NodeLib.h"

#include <cmath>

/** Shorthand used by the Nodes*.cpp files that fill the node library. */
namespace lila::gn::nodes
{
using geo::Index;
using T = SocketType;
using D3 = glm::dvec3;
inline constexpr double kPi = 3.14159265358979323846;

inline SocketDecl in(std::string id, SocketType t, json def = nullptr, std::string doc = {})
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .doc = std::move(doc)};
}

/** An input that accepts a field. */
inline SocketDecl fin(std::string id, SocketType t, json def = nullptr, Implicit imp = Implicit::None, std::string doc = {})
{
    return {.id = std::move(id), .type = t, .defaultValue = std::move(def), .field = true, .implicit = imp, .doc = std::move(doc)};
}

/** s with its value held at lo or above, and at hi or below when given. */
inline SocketDecl bounded(SocketDecl s, std::optional<double> lo, std::optional<double> hi = std::nullopt)
{
    s.min = lo;
    s.max = hi;
    return s;
}

/** A String input with a richer editor (SocketDecl::editor). */
inline SocketDecl edited(SocketDecl s, std::string editor)
{
    s.editor = std::move(editor);
    return s;
}

inline SocketDecl geoIn(std::string id = "geometry", std::string doc = {})
{
    return {.id = std::move(id), .type = SocketType::Geometry, .doc = std::move(doc)};
}

inline SocketDecl multiIn(std::string id = "geometry", std::string doc = {})
{
    return {.id = std::move(id), .type = SocketType::Geometry, .multi = true, .doc = std::move(doc)};
}

inline SocketDecl selectionIn() { return fin("selection", SocketType::Bool, true, Implicit::None, "Elements to affect."); }

inline SocketDecl out(std::string id, SocketType t, std::string doc = {})
{
    return {.id = std::move(id), .type = t, .doc = std::move(doc)};
}

/** An output that is a field. */
inline SocketDecl fout(std::string id, SocketType t, std::string doc = {})
{
    return {.id = std::move(id), .type = t, .field = true, .doc = std::move(doc)};
}

inline SocketDecl geoOut(std::string id = "geometry", std::string doc = {})
{
    return {.id = std::move(id), .type = SocketType::Geometry, .doc = std::move(doc)};
}

inline PropDecl prop(std::string id, std::vector<std::string> options, std::string def, std::string doc = {})
{
    return {.id = std::move(id), .options = std::move(options), .defaultValue = std::move(def), .doc = std::move(doc)};
}

/** A free-text property edited in a box of several lines ("multiline") or as code ("code"). */
inline PropDecl textProp(std::string id, std::string editor, std::string def, std::string doc = {})
{
    return {.id = std::move(id), .defaultValue = std::move(def), .doc = std::move(doc), .editor = std::move(editor)};
}

inline json vec(double x, double y, double z) { return json::array({x, y, z}); }

inline const std::vector<std::string>& dataTypes()
{
    static const std::vector<std::string> v = {"float", "int", "vector", "bool", "rotation", "color"};
    return v;
}

/** A socket default of the type's zero: [0, 0, 0] for vectors, opaque black for colours. */
inline json zeroJson(SocketType t)
{
    if (t == SocketType::Vector || t == SocketType::Rotation)
        return json::array({0, 0, 0});
    if (t == SocketType::Color)
        return json::array({0, 0, 0, 1});
    return json(0);
}

inline SocketType dataTypeOf(const std::string& s)
{
    SocketType t = SocketType::Float;
    parseSocketType(s, &t);
    return t;
}

inline const std::vector<std::string>& meshDomains()
{
    static const std::vector<std::string> v = {"point", "edge", "face", "corner", "curve", "instance"};
    return v;
}

inline Domain domainOf(const std::string& s)
{
    Domain d = Domain::Point;
    parseDomain(s, &d);
    return d;
}

/** Blender's Euler XYZ: X applied first, so R = Rz * Ry * Rx. Radians. */
inline glm::dmat3 eulerToMatrix(const glm::dvec3& e)
{
    double cx = std::cos(e.x), sx = std::sin(e.x), cy = std::cos(e.y), sy = std::sin(e.y), cz = std::cos(e.z), sz = std::sin(e.z);
    glm::dmat3 rx(1, 0, 0, 0, cx, sx, 0, -sx, cx);
    glm::dmat3 ry(cy, 0, -sy, 0, 1, 0, sy, 0, cy);
    glm::dmat3 rz(cz, sz, 0, -sz, cz, 0, 0, 0, 1);
    return rz * ry * rx;
}

inline glm::dvec3 matrixToEuler(const glm::dmat3& m)
{
    // m[col][row]; for R = Rz*Ry*Rx, row 2 col 0 is -sin(y).
    double sy = -m[0][2];
    sy = std::clamp(sy, -1.0, 1.0);
    double y = std::asin(sy);
    double x, z;
    if (std::abs(sy) < 0.999999)
    {
        x = std::atan2(m[1][2], m[2][2]);
        z = std::atan2(m[0][1], m[0][0]);
    }
    else
    {
        x = std::atan2(-m[2][1], m[1][1]);
        z = 0.0;
    }
    return {x, y, z};
}

/** Rotation turning unit vector a onto unit vector b by the shortest arc. */
inline glm::dmat3 rotationBetween(const glm::dvec3& a, const glm::dvec3& b)
{
    glm::dvec3 v = glm::cross(a, b);
    double c = glm::dot(a, b);
    if (c < -0.999999)
    {
        glm::dvec3 axis = std::abs(a.x) < 0.9 ? glm::normalize(glm::cross(a, glm::dvec3(1, 0, 0))) : glm::normalize(glm::cross(a, glm::dvec3(0, 1, 0)));
        return glm::dmat3(2.0 * glm::outerProduct(axis, axis) - glm::dmat3(1.0));
    }
    glm::dmat3 vx(0, v.z, -v.y, -v.z, 0, v.x, v.y, -v.x, 0);
    return glm::dmat3(1.0) + vx + vx * vx * (1.0 / (1.0 + c));
}

inline glm::dmat3 axisAngle(glm::dvec3 axis, double angle)
{
    if (glm::length(axis) < 1e-12)
        return glm::dmat3(1.0);
    axis = glm::normalize(axis);
    double c = std::cos(angle), s = std::sin(angle), t = 1.0 - c;
    glm::dmat3 m;
    m[0] = {t * axis.x * axis.x + c, t * axis.x * axis.y + s * axis.z, t * axis.x * axis.z - s * axis.y};
    m[1] = {t * axis.x * axis.y - s * axis.z, t * axis.y * axis.y + c, t * axis.y * axis.z + s * axis.x};
    m[2] = {t * axis.x * axis.z + s * axis.y, t * axis.y * axis.z - s * axis.x, t * axis.z * axis.z + c};
    return m;
}

inline glm::dmat4 composeTransform(const glm::dvec3& t, const glm::dvec3& euler, const glm::dvec3& s)
{
    glm::dmat3 r = eulerToMatrix(euler);
    glm::dmat4 m(1.0);
    for (int c = 0; c < 3; ++c)
        m[c] = glm::dvec4(r[c] * s[c], 0.0);
    m[3] = glm::dvec4(t, 1.0);
    return m;
}

/** Copy-on-write edit of each component present. Pass null for components the node leaves alone. */
struct ComponentEdits
{
    std::function<void(geo::Mesh&)> mesh;
    std::function<void(geo::Mesh&)> points;
    std::function<void(CurveSet&)> curves;
    std::function<void(Instances&)> instances;
};

inline GeometryPtr editComponents(const GeometryPtr& g, const ComponentEdits& e)
{
    Geometry out = *g;
    if (g->mesh && e.mesh)
    {
        geo::Mesh m = *g->mesh;
        e.mesh(m);
        m.compact();
        out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g->points && e.points)
    {
        geo::Mesh m = *g->points;
        e.points(m);
        out.points = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g->curves && e.curves)
    {
        CurveSet c = *g->curves;
        e.curves(c);
        out.curves = std::make_shared<const CurveSet>(std::move(c));
    }
    if (g->instances && e.instances)
    {
        Instances i = *g->instances;
        e.instances(i);
        out.instances = std::make_shared<const Instances>(std::move(i));
    }
    return makeGeometry(std::move(out));
}

/** Context on a component for a requested domain, falling back to the component's natural domain. */
inline FieldContext contextFor(const geo::Mesh& m, Domain d) { return FieldContext::forMesh(m, d > Domain::Corner ? Domain::Point : d); }

/** Registration functions of the node files. */
LILA_SCENE_API void registerInputNodes();
LILA_SCENE_API void registerUtilityNodes();
LILA_SCENE_API void registerGeometryNodes();
LILA_SCENE_API void registerMeshNodes();
LILA_SCENE_API void registerCurveNodes();
LILA_SCENE_API void registerSampleNodes();
LILA_SCENE_API void registerBooleanNode();
LILA_SCENE_API void registerTextureNodes();
LILA_SCENE_API void registerShadingNodes();
LILA_SCENE_API void registerGroupNodes();
LILA_SCENE_API void registerRoutingNodes();
LILA_SCENE_API void registerDestructionNodes();
void registerFractureNode();
void registerMeshOpsNodes();
void registerTopologyNodes();
void registerAnalysisNodes();
void registerFieldNodes();
void registerPatternNodes();
void registerCurveOpsNodes();
void registerInstanceNodes();
void registerGameNodes();
void registerDataNodes();
void registerProceduralNodes();
void registerVolumeNodes();
void registerGridNodes();
void registerToolNodes();
void registerWrangleNode();
void registerZoneNodes();
void registerMatrixNodes();
void registerBundleNodes();
void registerBuildNodes();
void registerDeform2Nodes();
void registerCurveNodes2();
void registerCurveSurfaceNodes();
void registerCurveBooleanNodes();
void registerInstanceNodes2();
void registerFieldStatsNodes();
void registerRotationNodes2();
void registerStringNodes2();
void registerSceneInfoNodes();
void registerTopology2Nodes();
void registerSelectionNodes();
void registerRemeshNodes();

/** A link between two pieces (b = -1: the world) at an anchor, as the Rigid Body Solver reads them. */
struct PieceLink
{
    int a = -1, b = -1;
    glm::dvec3 anchor{0.0};
    double strength = 0.0; // N
    double area = 0.0;     // cm2 the pieces share, which sets how much bending the link takes
};

/**
 * Pieces as the destruction nodes pass them: one instance per mesh, each mesh moved so its origin is origins[k] (the
 * centre of its bounds when not given) and that origin its instance's translation; points and instances get "piece".
 */
GeometryPtr piecesFromMeshes(std::vector<geo::Mesh> parts, const std::vector<glm::dvec3>& origins);
/** Links as two-point curves with pieceA, pieceB, type and strength curve attributes. */
GeometryPtr linkGeometry(const std::vector<PieceLink>& links, const std::string& type);
/** glue, spring, hard: the "type" property options of nodes making links. */
const std::vector<std::string>& pieceLinkTypes();
} // namespace lila::gn::nodes
