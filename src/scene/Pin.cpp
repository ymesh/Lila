#include "scene/Pin.h"

#include "core/Profile.h"
#include "curve/Along.h"
#include "curve/CurveNodes.h"
#include "curve/Spline.h"
#include "curve/Tessellate.h"
#include "deform/Surface.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/CommandOptions.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/AnimLayer.h"
#include "scene/Components.h"
#include "scene/Constraint.h"
#include "scene/Deformers.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Repeat.h"
#include "scene/Rig.h"
#include "scene/RigUtil.h"
#include "scene/Skin.h"
#include "scene/Transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>

namespace lila::scene::pins
{
using namespace rigutil;
using D3 = glm::dvec3;
using geo::Index;

namespace
{
constexpr const char* kType = "pin";

enum Source : int64_t
{
    kCurve,
    kVertex,
    kEdge,
    kFace,
    kSurface,
};
const std::vector<std::string> kSources = {"curve", "vertex", "edge", "face", "surface"};

enum CurveMode : int64_t
{
    kByParameter,
    kByLength,
    kByFraction,
};
const std::vector<std::string> kCurveModes = {"parameter", "length", "fraction"};

enum UpMode : int64_t
{
    kUpCurveFrames,
    kUpVector,
    kUpObject,
    kUpAimCurve,
};
const std::vector<std::string> kUpModes = {"curveFrames", "upVector", "upObject", "aimCurve"};

const std::vector<std::string> kShapes = {"final", "rest"};
const std::vector<std::string> kFaceModes = {"centre", "barycentric"};
const std::vector<std::string> kSurfaceModes = {"uv", "nearest"};
const std::vector<std::string> kFollows = {"translate", "translateRotate", "full"};

int64_t indexIn(const std::vector<std::string>& names, const std::string& v, const char* what)
{
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == v)
            return int64_t(i);
    std::string all;
    for (const std::string& n : names)
        all += (all.empty() ? "" : " | ") + n;
    throw CommandError(std::format("pin: {} is {}", what, all));
}

D3 xform(const glm::dmat4& m, const D3& p) { return D3(m * glm::dvec4(p, 1.0)); }

/** z along primary, x as near secondary as stays perpendicular, y = z x x. */
glm::dmat4 frameZX(const D3& origin, D3 z, D3 x)
{
    z = glm::length(z) > 1e-12 ? glm::normalize(z) : D3(0, 0, 1);
    x -= glm::dot(x, z) * z;
    if (glm::length(x) < 1e-12)
    {
        x = std::abs(z.x) < 0.9 ? D3(1, 0, 0) : D3(0, 1, 0);
        x = glm::normalize(x - glm::dot(x, z) * z);
    }
    else
        x = glm::normalize(x);
    glm::dmat4 m(1.0);
    m[0] = glm::dvec4(x, 0.0);
    m[1] = glm::dvec4(glm::cross(z, x), 0.0);
    m[2] = glm::dvec4(z, 0.0);
    m[3] = glm::dvec4(origin, 1.0);
    return m;
}

/** Twice the area vector of a polygon (Newell). */
D3 newell(std::span<const D3> pts)
{
    D3 n(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
        n += glm::cross(pts[i], pts[(i + 1) % pts.size()]);
    return n;
}

/** The arc length at span parameter u, through the tessellation's parameters. */
double lengthAtParam(const curve::Polyline& pl, double u)
{
    if (pl.size() == 0)
        return 0.0;
    if (u <= pl.params.front())
        return 0.0;
    for (size_t i = 1; i < pl.size(); ++i)
        if (u <= pl.params[i])
        {
            const double span = pl.params[i] - pl.params[i - 1];
            const double f = span > 1e-15 ? (u - pl.params[i - 1]) / span : 0.0;
            return pl.arcLength[i - 1] + f * (pl.arcLength[i] - pl.arcLength[i - 1]);
        }
    return pl.closed ? pl.length : pl.arcLength.back();
}

struct Located
{
    glm::dmat4 frame{1.0};
    double size = 1.0; // the local scale of the source, compared with the size at bind for full follow
};

class PinNode : public Node
{
public:
    enum Attr
    {
        kSource,
        kShape,
        kWeight,
        kRoll,
        kRebindByUv,
        kInMesh,
        kInRestMesh,
        kMeshMatrix,
        kInCurve,
        kAimCurve,
        kUpMatrix,
        kCurveMode,
        kAlong,
        kUpModeAttr,
        kUpVectorAttr,
        kIndex,
        kNeighbor,
        kFraction,
        kFaceMode,
        kSurfaceMode,
        kUvSet,
        kU,
        kV,
        kTriangle,
        kBary,
        kBindU,
        kBindV,
        kBindPoints,
        kBindFaces,
        kBindSize,
        kRestScale,
        kOutMatrix,
        kOutTranslate,
        kOutRotate,
        kOutScale,
        kOutSize,
    };

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        const int64_t source = as<int64_t>(kSource);
        std::optional<Located> at = source == kCurve ? curveFrame() : meshFrame(source);
        glm::dmat4 frame = at ? at->frame : as<glm::dmat4>(kOutMatrix);
        if (at)
        {
            const double roll = glm::radians(as<double>(kRoll));
            if (roll != 0.0)
                frame = frame * glm::dmat4(glm::dmat3(eulerToMatrix(source == kCurve ? D3(glm::degrees(roll), 0, 0) : D3(0, 0, glm::degrees(roll)),
                                                                    RotateOrder::XYZ)));
        }
        const double size = at ? at->size : as<double>(kOutSize);
        const double bind = as<double>(kBindSize);
        const double ratio = bind > 1e-12 && size > 1e-12 ? size / bind : 1.0;
        setOutput(kOutMatrix, frame);
        setOutput(kOutTranslate, D3(frame[3]));
        setOutput(kOutRotate, matrixToEuler(glm::dmat3(frame), RotateOrder::XYZ));
        setOutput(kOutScale, as<D3>(kRestScale) * ratio);
        setOutput(kOutSize, size);
    }

private:
    std::optional<Located> curveFrame()
    {
        const curve::Splines& splines = curve::splinesOf(value(kInCurve));
        if (splines.empty())
            return std::nullopt;
        const int64_t up = as<int64_t>(kUpModeAttr);
        const D3 upVector = as<D3>(kUpVectorAttr);
        const void* curveKey = as<DataPtr>(kInCurve).get();
        const void* aimKey = as<DataPtr>(kAimCurve).get();
        if (curveKey != mRailCurve || aimKey != mRailAim || up != mRailUp || upVector != mRailUpVector)
        {
            curve::RailOptions ro;
            ro.up = up == kUpVector ? curve::UpMode::AutoNormals : curve::UpMode::CurveFrames;
            ro.upVector = glm::length(upVector) > 1e-12 ? upVector : D3(0, 1, 0);
            if (up == kUpAimCurve)
                if (const curve::Splines& aim = curve::splinesOf(value(kAimCurve)); !aim.empty())
                {
                    ro.up = curve::UpMode::AimCurve;
                    ro.aim = aim.front();
                }
            mRail = curve::Rail(splines.front(), ro);
            mPolyline = curve::tessellate(splines.front(), ro.tess);
            mRailCurve = curveKey;
            mRailAim = aimKey;
            mRailUp = up;
            mRailUpVector = upVector;
        }
        if (!mRail.valid())
            return std::nullopt;
        const double along = as<double>(kAlong);
        double s = along;
        switch (as<int64_t>(kCurveMode))
        {
        case kByParameter: s = lengthAtParam(mPolyline, along); break;
        case kByFraction: s = along * mRail.length(); break;
        default: break;
        }
        curve::Rail::Station st = mRail.at(s);
        D3 t = st.f.t, n = st.f.n;
        if (up == kUpObject)
        {
            D3 toUp = D3(as<glm::dmat4>(kUpMatrix)[3]) - st.p;
            toUp -= glm::dot(toUp, t) * t;
            if (glm::length(toUp) > 1e-9)
                n = glm::normalize(toUp);
        }
        Located out;
        out.frame[0] = glm::dvec4(t, 0.0);
        out.frame[1] = glm::dvec4(n, 0.0);
        out.frame[2] = glm::dvec4(glm::cross(t, n), 0.0);
        out.frame[3] = glm::dvec4(st.p, 1.0);
        out.size = st.scale;
        return out;
    }

    /** The surface point at uv, cached while the topology, uv and set stay the same. */
    bool locateUv(const geo::Mesh& m, const glm::dvec2& uv, deform::SurfacePoint& out)
    {
        const std::string& set = as<std::string>(kUvSet);
        if (mUvPoints != m.pointCapacity() || mUvFaces != m.numFaces() || mUv != uv || mUvSet != set)
        {
            mUvFound = deform::locateUv(m, set, uv, mUvHit);
            mUvPoints = m.pointCapacity();
            mUvFaces = m.numFaces();
            mUv = uv;
            mUvSet = set;
        }
        out = mUvHit;
        return mUvFound;
    }

    std::optional<Located> meshFrame(int64_t source)
    {
        const MeshPtr* pick = &as<MeshPtr>(as<int64_t>(kShape) == 1 ? kInRestMesh : kInMesh);
        if (!*pick)
            pick = &as<MeshPtr>(kInMesh);
        if (!*pick || pick->get()->numFaces() == 0)
            return std::nullopt;
        const geo::Mesh& m = **pick;
        const glm::dmat4 w = as<glm::dmat4>(kMeshMatrix);
        auto P = [&](Index i) { return xform(w, m.position(i)); };

        const int64_t bindPoints = as<int64_t>(kBindPoints), bindFaces = as<int64_t>(kBindFaces);
        const bool sameTopology = bindPoints == 0 || (int64_t(m.numPoints()) == bindPoints && int64_t(m.numFaces()) == bindFaces);
        const bool uvSource = source == kSurface && as<int64_t>(kSurfaceMode) == 0;
        if (!uvSource && (!sameTopology || !indicesValid(m, source)))
        {
            if (!as<bool>(kRebindByUv))
                return std::nullopt;
            deform::SurfacePoint sp;
            if (!locateUv(m, {as<double>(kBindU), as<double>(kBindV)}, sp))
                return std::nullopt;
            if (source == kVertex)
            {
                const int k = sp.bary.x >= sp.bary.y && sp.bary.x >= sp.bary.z ? 0 : sp.bary.y >= sp.bary.z ? 1 : 2;
                return vertexFrame(m, P, sp.points[k], -1);
            }
            return surfaceFrame(P, sp);
        }
        switch (source)
        {
        case kVertex: return vertexFrame(m, P, Index(as<int64_t>(kIndex)), as<int64_t>(kNeighbor));
        case kEdge: return edgeFrame(m, P, Index(as<int64_t>(kIndex)), std::clamp(as<double>(kFraction), 0.0, 1.0));
        case kFace: return faceFrame(m, P, Index(as<int64_t>(kIndex)));
        default: break;
        }
        deform::SurfacePoint sp;
        if (uvSource)
        {
            if (!locateUv(m, {as<double>(kU), as<double>(kV)}, sp))
                return std::nullopt;
        }
        else
            sp = boundPoint();
        return surfaceFrame(P, sp);
    }

    deform::SurfacePoint boundPoint() const
    {
        deform::SurfacePoint sp;
        const D3 tri = as<D3>(kTriangle);
        for (int k = 0; k < 3; ++k)
            sp.points[k] = uint32_t(std::max(0.0, tri[k]));
        sp.bary = as<D3>(kBary);
        return sp;
    }

    bool indicesValid(const geo::Mesh& m, int64_t source) const
    {
        const int64_t i = as<int64_t>(kIndex);
        auto triValid = [&] {
            const D3 tri = as<D3>(kTriangle);
            for (int k = 0; k < 3; ++k)
                if (tri[k] < 0.0 || !m.pointValid(Index(tri[k])))
                    return false;
            return true;
        };
        switch (source)
        {
        case kVertex:
        {
            const int64_t nb = as<int64_t>(kNeighbor);
            return i >= 0 && m.pointValid(Index(i)) && (nb < 0 || m.pointValid(Index(nb)));
        }
        case kEdge: return i >= 0 && m.edgeValid(Index(i));
        case kFace: return i >= 0 && m.faceValid(Index(i)) && (as<int64_t>(kFaceMode) == 0 || triValid());
        default: return triValid();
        }
    }

    template <class F> static Located vertexFrame(const geo::Mesh& m, F&& P, Index p, int64_t neighbor)
    {
        const D3 at = P(p);
        D3 normal(0.0);
        for (Index f : m.pointFaces(p))
        {
            std::vector<D3> pts;
            for (Index q : m.facePoints(f))
                pts.push_back(P(q));
            normal += newell(pts);
        }
        double total = 0.0;
        int count = 0;
        Index first = geo::kInvalid;
        m.forEachPointEdge(p, [&](Index e) {
            if (first == geo::kInvalid)
                first = m.edgeOtherPoint(e, p);
            total += glm::length(P(m.edgeOtherPoint(e, p)) - at);
            ++count;
        });
        const Index toward = neighbor >= 0 ? Index(neighbor) : first;
        Located out;
        out.frame = frameZX(at, normal, toward != geo::kInvalid ? P(toward) - at : D3(1, 0, 0));
        out.size = count ? total / count : 1.0;
        return out;
    }

    template <class F> static Located edgeFrame(const geo::Mesh& m, F&& P, Index e, double fraction)
    {
        const D3 a = P(m.edgePoint(e, 0)), b = P(m.edgePoint(e, 1));
        D3 normal(0.0);
        m.forEachEdgeCorner(e, [&](Index c) {
            std::vector<D3> pts;
            for (Index q : m.facePoints(m.cornerFace(c)))
                pts.push_back(P(q));
            const D3 n = newell(pts);
            if (glm::length(n) > 1e-15)
                normal += glm::normalize(n);
        });
        const D3 x = b - a;
        if (glm::length(normal) < 1e-12)
            normal = std::abs(x.y) < 0.9 * glm::length(x) ? D3(0, 1, 0) : D3(1, 0, 0);
        Located out;
        out.frame = frameZX(glm::mix(a, b, fraction), normal, x);
        out.size = glm::length(x);
        return out;
    }

    template <class F> Located faceFrame(const geo::Mesh& m, F&& P, Index f) const
    {
        std::vector<D3> pts;
        for (Index q : m.facePoints(f))
            pts.push_back(P(q));
        D3 at(0.0);
        if (as<int64_t>(kFaceMode) == 1)
        {
            const deform::SurfacePoint sp = boundPoint();
            for (int k = 0; k < 3; ++k)
                at += P(sp.points[k]) * sp.bary[k];
        }
        else
        {
            for (const D3& p : pts)
                at += p;
            at /= double(pts.size());
        }
        const D3 n = newell(pts);
        Located out;
        out.frame = frameZX(at, n, pts.size() > 1 ? pts[1] - pts[0] : D3(1, 0, 0));
        out.size = std::sqrt(0.5 * glm::length(n));
        return out;
    }

    template <class F> static Located surfaceFrame(F&& P, const deform::SurfacePoint& sp)
    {
        const std::array<D3, 3> pts = {P(sp.points[0]), P(sp.points[1]), P(sp.points[2])};
        deform::SurfacePoint local = sp;
        for (int k = 0; k < 3; ++k)
            local.points[k] = uint32_t(k);
        Located out;
        out.frame = deform::surfaceFrame(pts, local);
        out.size = std::sqrt(0.5 * glm::length(glm::cross(pts[1] - pts[0], pts[2] - pts[0])));
        return out;
    }

    const void* mRailCurve = nullptr;
    const void* mRailAim = nullptr;
    int64_t mRailUp = -1;
    D3 mRailUpVector{0.0};
    curve::Rail mRail;
    curve::Polyline mPolyline;

    Index mUvPoints = ~Index(0), mUvFaces = 0;
    glm::dvec2 mUv{-1e300};
    std::string mUvSet;
    deform::SurfacePoint mUvHit;
    bool mUvFound = false;
};

// ------------------------------------------------------------------------------------------------ attrs

AttrDef attr(std::string name, PlugType type, PlugValue def, uint32_t flags, std::string doc, std::string category = {})
{
    AttrDef a{.name = std::move(name), .type = type, .defaultValue = std::move(def), .flags = flags, .doc = std::move(doc)};
    if (type == PlugType::Vec3)
        for (int k = 0; k < 3; ++k)
            a.childNames[size_t(k)] = a.name + char('X' + k);
    a.category = std::move(category);
    return a;
}
constexpr uint32_t kStored = AttrInput | AttrStorable;
constexpr uint32_t kKeyable = AttrInput | AttrStorable | AttrKeyable;
constexpr uint32_t kHiddenStored = AttrInput | AttrStorable | AttrHidden;
constexpr uint32_t kConnected = AttrInput | AttrHidden;

AttrDef enumAttr(std::string name, int64_t def, std::vector<std::string> names, std::string doc, std::string category = {})
{
    AttrDef a = attr(std::move(name), PlugType::Enum, def, kStored, std::move(doc), std::move(category));
    a.enumNames = std::move(names);
    return a;
}

void registerTypesImpl()
{
    NodeType t;
    t.name = kType;
    t.icon = "pin";
    t.doc = "A world frame riding a curve, a vertex, an edge, a face or a mesh's surface; drives a pin locator that objects and "
            "joints are constrained to.";
    t.ownAttrs = {
        enumAttr("source", kSurface, kSources, "What the pin rides."),
        enumAttr("shape", 0, kShapes, "Meshes: ride the final deformed shape or the rest shape (before deformers and skinning)."),
        attr("weight", PlugType::Float, 1.0, kKeyable, "This pin's share when several pins drive one object."),
        attr("roll", PlugType::Float, 0.0, kKeyable, "Turn of the frame, degrees: about the curve on curves, about the normal on meshes."),
        attr("rebindByUv", PlugType::Bool, true, kStored,
             "When the mesh's topology changes, find the pin again at the uv it had when made instead of holding still."),
        attr("inMesh", PlugType::Mesh, MeshPtr(), kConnected, "The mesh's outMesh."),
        attr("inRestMesh", PlugType::Mesh, MeshPtr(), kConnected, "The mesh before its deformers."),
        attr("meshMatrix", PlugType::Matrix, glm::dmat4(1.0), kConnected, "The mesh's worldMatrix."),
        attr("inCurve", PlugType::Data, DataPtr(), kConnected, "The curve's worldSpace."),
        attr("aimCurve", PlugType::Data, DataPtr(), kConnected, "The curve the frame's up aims at (upMode aimCurve)."),
        attr("upMatrix", PlugType::Matrix, glm::dmat4(1.0), kConnected, "The up object's worldMatrix (upMode upObject)."),
        enumAttr("curveMode", kByFraction, kCurveModes, "How along reads: the curve's parameter, cm of arc length, or a fraction of its length.",
                 "Curve"),
        attr("along", PlugType::Float, 0.0, kKeyable, "Where on the curve, in curveMode's units.", "Curve"),
        enumAttr("upMode", kUpCurveFrames, kUpModes,
                 "How the frame turns about the curve: rotation-minimising curve frames, leaning toward the up vector, toward the up "
                 "object, or aimed at a second curve.",
                 "Curve"),
        attr("upVector", PlugType::Vec3, D3(0, 1, 0), kKeyable, "Up for upMode upVector, and where curve frames start.", "Curve"),
        attr("index", PlugType::Int, int64_t(0), kStored, "The vertex, edge or face id.", "Mesh"),
        attr("neighbor", PlugType::Int, int64_t(-1), kStored, "Vertex: the point x aims toward; -1 is along the first edge.", "Mesh"),
        attr("fraction", PlugType::Float, 0.5, kKeyable, "Edge: how far from its first point to its second.", "Mesh"),
        enumAttr("faceMode", 0, kFaceModes, "Face: its centre, or the bound point inside it.", "Mesh"),
        enumAttr("surfaceMode", 0, kSurfaceModes,
                 "Surface: find the point by uv every evaluation (survives topology changes), or keep the triangle bound when made.", "Mesh"),
        attr("uvSet", PlugType::String, std::string("uv0"), kStored, "The uv set the pin reads.", "Mesh"),
        attr("u", PlugType::Float, 0.5, kKeyable, "U of the pin, surface uv mode.", "Mesh"),
        attr("v", PlugType::Float, 0.5, kKeyable, "V of the pin, surface uv mode.", "Mesh"),
        attr("triangle", PlugType::Vec3, D3(-1.0), kHiddenStored, "The bound triangle's point ids."),
        attr("bary", PlugType::Vec3, D3(1.0 / 3.0), kHiddenStored, "Weights of the bound triangle's points."),
        attr("bindU", PlugType::Float, 0.0, kHiddenStored, "U where the pin sat when made, for rebinding."),
        attr("bindV", PlugType::Float, 0.0, kHiddenStored, "V where the pin sat when made, for rebinding."),
        attr("bindPoints", PlugType::Int, int64_t(0), kHiddenStored, "The mesh's point count when made."),
        attr("bindFaces", PlugType::Int, int64_t(0), kHiddenStored, "The mesh's face count when made."),
        attr("bindSize", PlugType::Float, 0.0, kHiddenStored, "The source's local size when made; outScale is the size now over it."),
        attr("restScale", PlugType::Vec3, D3(1.0), kHiddenStored, "The pinned object's scale when made (follow full)."),
        attr("outMatrix", PlugType::Matrix, glm::dmat4(1.0), AttrOutput, "The pin's world frame."),
        attr("outTranslate", PlugType::Vec3, D3(0.0), AttrOutput, "Drives the pin locator's translate."),
        attr("outRotate", PlugType::Vec3, D3(0.0), AttrOutput, "Drives the pin locator's rotate."),
        attr("outScale", PlugType::Vec3, D3(1.0), AttrOutput, "restScale times how much the source has grown since the pin was made."),
        attr("outSize", PlugType::Float, 1.0, AttrOutput | AttrHidden, "The source's local size now."),
    };
    t.create = [] { return std::make_unique<PinNode>(); };
    types::registerType(std::move(t));
}

// ------------------------------------------------------------------------------------------------ pin command

void setPlug(Graph& g, NodeId node, int attr, PlugValue v)
{
    guarded([&] {
        g.setValue({node, attr, -1}, std::move(v));
        return 0;
    });
}

void connectPlug(Graph& g, const PlugRef& src, const PlugRef& dst)
{
    guarded([&] { return g.connect(src, dst, true); });
}

/** What feeds the first deformer or skin of a mesh's stack: the shape before deformation. outMesh when it has none. */
PlugRef restPlug(Graph& g, const Node& mesh)
{
    for (NodeId id : modifiers::stack(g, mesh.id()))
    {
        Node* n = g.node(id);
        if (!deformers::isDeformer(*n) && !n->type().isA("skinCluster"))
            continue;
        if (const ModifierNode* mod = modifiers::asModifier(n))
            if (auto src = g.source({id, mod->inAttr, -1}))
                return *src;
        break;
    }
    return {mesh.id(), MeshNode::kOutMesh, -1};
}

geo::Mesh worldMesh(const geo::Mesh& m, const glm::dmat4& w)
{
    geo::Mesh out = m;
    for (Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
            out.setPosition(p, xform(w, out.position(p)));
    return out;
}

/** The first selected component of a kind on a mesh, in pick order. */
std::optional<Index> selectedComponent(NodeId mesh, ComponentMode kind)
{
    for (const auto& [node, id] : components::ordered(kind))
        if (node == mesh)
            return Index(id);
    return std::nullopt;
}

Index nearestPoint(const geo::Mesh& world, const D3& at)
{
    Index best = geo::kInvalid;
    double bestD = std::numeric_limits<double>::max();
    for (Index p = 0; p < world.pointCapacity(); ++p)
        if (world.pointValid(p))
            if (const double d = glm::length(world.position(p) - at); d < bestD)
            {
                bestD = d;
                best = p;
            }
    return best;
}

/** The edge nearest a point and the fraction along it of the closest point. */
std::pair<Index, double> nearestEdge(const geo::Mesh& world, const D3& at)
{
    std::pair<Index, double> best{geo::kInvalid, 0.5};
    double bestD = std::numeric_limits<double>::max();
    for (Index e = 0; e < world.edgeCapacity(); ++e)
    {
        if (!world.edgeValid(e))
            continue;
        const D3 a = world.position(world.edgePoint(e, 0)), b = world.position(world.edgePoint(e, 1));
        const double len2 = glm::dot(b - a, b - a);
        const double f = len2 > 1e-24 ? std::clamp(glm::dot(at - a, b - a) / len2, 0.0, 1.0) : 0.5;
        if (const double d = glm::length(glm::mix(a, b, f) - at); d < bestD)
        {
            bestD = d;
            best = {e, f};
        }
    }
    return best;
}

/** Fraction along an edge of the point nearest at. */
double fractionOn(const geo::Mesh& world, Index e, const D3& at)
{
    const D3 a = world.position(world.edgePoint(e, 0)), b = world.position(world.edgePoint(e, 1));
    const double len2 = glm::dot(b - a, b - a);
    return len2 > 1e-24 ? std::clamp(glm::dot(at - a, b - a) / len2, 0.0, 1.0) : 0.5;
}

struct FaceHit
{
    Index face = geo::kInvalid;
    std::array<uint32_t, 3> points{};
    D3 bary{1.0 / 3.0};
};

/** The closest point to at on the mesh, or on one face of it. */
std::optional<FaceHit> closestOnFaces(const geo::Mesh& world, const D3& at, Index onlyFace = geo::kInvalid)
{
    geo::MeshBvh bvh(world);
    const geo::Triangulation& tris = bvh.triangles();
    auto hitOf = [&](Index tri, const D3& bary) {
        FaceHit h;
        const Index* c = &tris.corners[size_t(tri) * 3];
        for (int k = 0; k < 3; ++k)
            h.points[size_t(k)] = uint32_t(world.cornerPoint(c[k]));
        h.face = world.cornerFace(c[0]);
        h.bary = bary;
        return h;
    };
    if (onlyFace == geo::kInvalid)
    {
        geo::BvhClosest hit;
        if (bvh.empty() || !bvh.closestPoint(at, hit))
            return std::nullopt;
        return hitOf(hit.triangle, hit.bary);
    }
    std::optional<FaceHit> best;
    double bestD = std::numeric_limits<double>::max();
    for (size_t tri = 0; tri * 3 < tris.corners.size(); ++tri)
    {
        const Index* c = &tris.corners[tri * 3];
        if (world.cornerFace(c[0]) != onlyFace)
            continue;
        const D3 a = world.position(world.cornerPoint(c[0])), b = world.position(world.cornerPoint(c[1])), d = world.position(world.cornerPoint(c[2]));
        // Closest point on a triangle (Ericson, Real-Time Collision Detection 5.1.5), as barycentrics.
        const D3 ab = b - a, ac = d - a, ap = at - a;
        const double d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
        D3 bary;
        if (d1 <= 0 && d2 <= 0)
            bary = {1, 0, 0};
        else
        {
            const D3 bp = at - b;
            const double d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
            const D3 cp = at - d;
            const double d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
            const double vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
            if (d3 >= 0 && d4 <= d3)
                bary = {0, 1, 0};
            else if (d6 >= 0 && d5 <= d6)
                bary = {0, 0, 1};
            else if (vc <= 0 && d1 >= 0 && d3 <= 0)
            {
                const double v = d1 / (d1 - d3);
                bary = {1 - v, v, 0};
            }
            else if (vb <= 0 && d2 >= 0 && d6 <= 0)
            {
                const double w = d2 / (d2 - d6);
                bary = {1 - w, 0, w};
            }
            else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0)
            {
                const double w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                bary = {0, 1 - w, w};
            }
            else
            {
                const double denom = 1.0 / (va + vb + vc);
                const double v = vb * denom, w = vc * denom;
                bary = {1 - v - w, v, w};
            }
        }
        const D3 p = a * bary.x + b * bary.y + d * bary.z;
        if (const double dist = glm::length(p - at); dist < bestD)
        {
            bestD = dist;
            best = hitOf(Index(tri), bary);
        }
    }
    return best;
}

/** The constraint target slot fed by a node's worldMatrix, or -1. */
int slotOf(Graph& g, NodeId constraint, NodeId target)
{
    for (const PlugRef& e : g.elements({constraint, ConstraintNode::kTargetMatrix, -1}))
        if (auto src = g.source(e); src && src->node == target)
            return e.element;
    return -1;
}

const Node& upNode(const std::string& name, const char* kind)
{
    Node& n = nodeNamed(name, "pin");
    if (!n.type().isA(kind))
        throw CommandError(std::format("pin: {} is not a {}", name, kind));
    return n;
}

json makePins(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("pin: give the objects to pin (optional), then what they ride last: a curve or a mesh");
    Node& src = nodeNamed(objs.back(), "pin");
    objs.pop_back();
    const bool isCurve = src.type().isA("curve");
    const bool isMesh = src.type().isA("mesh") && !src.type().isA("lattice");
    if (!isCurve && !isMesh)
        throw CommandError("pin: " + src.name() + " is neither a curve nor a mesh");

    std::string sourceName = a.get<std::string>("source", "auto");
    if (sourceName == "auto")
    {
        sourceName = "curve";
        if (isMesh)
        {
            const ComponentMode mode = components::mode();
            sourceName = mode == ComponentMode::Vertex && selectedComponent(src.id(), mode) ? "vertex"
                         : mode == ComponentMode::Edge && selectedComponent(src.id(), mode) ? "edge"
                         : mode == ComponentMode::Face && selectedComponent(src.id(), mode) ? "face"
                                                                                             : "surface";
        }
    }
    const int64_t source = indexIn(kSources, sourceName, "source");
    if ((source == kCurve) != isCurve)
        throw CommandError(std::format("pin: source {} needs a {}", sourceName, isCurve ? "mesh" : "curve"));
    const int64_t follow = indexIn(kFollows, a.get<std::string>("follow", "translateRotate"), "follow");
    const int64_t shape = indexIn(kShapes, a.get<std::string>("shape", "final"), "shape");
    const std::string uvSet = a.get<std::string>("uvSet", "uv0");

    g.evaluate();
    std::optional<geo::Mesh> world;
    std::optional<curve::Spline> spline;
    PlugRef rest;
    bool hasUv = false;
    if (isMesh)
    {
        rest = restPlug(g, src);
        PlugValue v = g.value(shape == 1 ? rest : PlugRef{src.id(), MeshNode::kOutMesh, -1});
        const MeshPtr* m = std::get_if<MeshPtr>(&v);
        if (!m || !*m || (*m)->numFaces() == 0)
            throw CommandError("pin: " + src.name() + " has no faces");
        world = worldMesh(**m, worldOf(src));
        hasUv = !(*m)->cornerAttrs().get<glm::vec2>(uvSet).empty();
    }
    else
    {
        const curve::Splines& s = curve::splinesOf(src.value(curve::CurveShape::kWorldSpace));
        if (s.empty())
            throw CommandError("pin: " + src.name() + " has no curve");
        spline = s.front();
    }

    std::string surfaceModeName = a.get<std::string>("surfaceMode", "auto");
    if (surfaceModeName == "auto")
        surfaceModeName = hasUv ? "uv" : "nearest";
    const int64_t surfaceMode = indexIn(kSurfaceModes, surfaceModeName, "surfaceMode");
    if (source == kSurface && surfaceMode == 0 && !hasUv)
        throw CommandError(std::format("pin: {} has no uv set {}; use surfaceMode nearest", src.name(), uvSet));

    std::vector<std::pair<Node*, D3>> wanted;
    for (const std::string& o : objs)
    {
        Node& n = nodeNamed(o, "pin");
        if (!n.type().isA("transform"))
            throw CommandError("pin: " + n.name() + " is not a transform");
        if (n.id() == src.id())
            throw CommandError("pin: " + n.name() + " can't be pinned to itself");
        wanted.push_back({&n, positionOf(n)});
    }
    if (wanted.empty())
    {
        D3 at = isMesh ? world->bounds().center() : curve::pointAt(*spline, 0.0);
        if (a.has("position"))
            at = vec3Of(a.flags["position"]);
        wanted.push_back({nullptr, at});
    }

    std::optional<curve::Polyline> pl;
    double railLength = 0.0;
    if (isCurve)
    {
        curve::RailOptions ro;
        pl = curve::tessellate(*spline, ro.tess);
        railLength = curve::Rail(*spline, ro).length();
        if (railLength <= 1e-9)
            throw CommandError("pin: " + src.name() + " has no length");
    }
    const int64_t curveMode = indexIn(kCurveModes, a.get<std::string>("curveMode", "fraction"), "curveMode");
    const int64_t upMode = indexIn(kUpModes, a.get<std::string>("upMode", "curveFrames"), "upMode");
    const int64_t faceMode = indexIn(kFaceModes, a.get<std::string>("faceMode", "centre"), "faceMode");
    if (upMode == kUpObject && !a.has("upObject"))
        throw CommandError("pin: upMode upObject needs upObject");
    if (upMode == kUpAimCurve && !a.has("aimCurve"))
        throw CommandError("pin: upMode aimCurve needs aimCurve");

    json out = json::array();
    for (auto& [obj, at] : wanted)
    {
        const std::string base = a.has("name") ? a.get<std::string>("name") : (obj ? obj->name() : src.name()) + "_pin";
        Node* pin = guarded([&] { return g.createNode(*types::find(kType), base + "Node", kNoNode, true); });
        const NodeId pid = pin->id();
        setPlug(g, pid, PinNode::kSource, source);
        setPlug(g, pid, PinNode::kShape, shape);
        setPlug(g, pid, PinNode::kWeight, a.get<double>("weight", 1.0));
        setPlug(g, pid, PinNode::kRoll, a.get<double>("roll", 0.0));
        setPlug(g, pid, PinNode::kRebindByUv, a.get<bool>("rebindByUv", true));
        setPlug(g, pid, PinNode::kUvSet, uvSet);

        if (isCurve)
        {
            connectPlug(g, {src.id(), curve::CurveShape::kWorldSpace, -1}, {pid, PinNode::kInCurve, -1});
            setPlug(g, pid, PinNode::kCurveMode, curveMode);
            setPlug(g, pid, PinNode::kUpModeAttr, upMode);
            if (a.has("upVector"))
                setPlug(g, pid, PinNode::kUpVectorAttr, vec3Of(a.flags["upVector"]));
            if (upMode == kUpObject)
                connectPlug(g, {upNode(a.get<std::string>("upObject"), "transform").id(), Transform::kWorldMatrix, -1}, {pid, PinNode::kUpMatrix, -1});
            if (upMode == kUpAimCurve)
                connectPlug(g, {upNode(a.get<std::string>("aimCurve"), "curve").id(), curve::CurveShape::kWorldSpace, -1}, {pid, PinNode::kAimCurve, -1});
            double along = 0.0;
            if (a.has("along"))
                along = a.get<double>("along");
            else
            {
                const double u = curve::closestParam(*spline, at);
                const double s = lengthAtParam(*pl, u);
                along = curveMode == kByParameter ? u : curveMode == kByLength ? s : s / railLength;
            }
            setPlug(g, pid, PinNode::kAlong, along);
        }
        else
        {
            connectPlug(g, {src.id(), MeshNode::kOutMesh, -1}, {pid, PinNode::kInMesh, -1});
            connectPlug(g, rest, {pid, PinNode::kInRestMesh, -1});
            connectPlug(g, {src.id(), Transform::kWorldMatrix, -1}, {pid, PinNode::kMeshMatrix, -1});
            setPlug(g, pid, PinNode::kBindPoints, int64_t(world->numPoints()));
            setPlug(g, pid, PinNode::kBindFaces, int64_t(world->numFaces()));
            std::optional<Index> chosen;
            if (a.has("index"))
                chosen = Index(a.get<int64_t>("index"));
            else if (source == kVertex || source == kEdge || source == kFace)
                chosen = selectedComponent(src.id(), source == kVertex ? ComponentMode::Vertex : source == kEdge ? ComponentMode::Edge : ComponentMode::Face);
            switch (source)
            {
            case kVertex:
            {
                const Index p = chosen ? *chosen : nearestPoint(*world, at);
                if (!world->pointValid(p))
                    throw CommandError(std::format("pin: {} has no vertex {}", src.name(), p));
                setPlug(g, pid, PinNode::kIndex, int64_t(p));
                if (a.has("neighbor"))
                {
                    const int64_t nb = a.get<int64_t>("neighbor");
                    if (nb >= 0 && !world->pointValid(Index(nb)))
                        throw CommandError(std::format("pin: {} has no vertex {}", src.name(), nb));
                    setPlug(g, pid, PinNode::kNeighbor, nb);
                }
                break;
            }
            case kEdge:
            {
                std::pair<Index, double> e = chosen ? std::pair{*chosen, 0.5} : nearestEdge(*world, at);
                if (!world->edgeValid(e.first))
                    throw CommandError(std::format("pin: {} has no edge {}", src.name(), e.first));
                if (chosen && obj)
                    e.second = fractionOn(*world, e.first, at);
                setPlug(g, pid, PinNode::kIndex, int64_t(e.first));
                setPlug(g, pid, PinNode::kFraction, a.has("fraction") ? a.get<double>("fraction") : e.second);
                break;
            }
            case kFace:
            {
                std::optional<FaceHit> hit = closestOnFaces(*world, at, chosen ? *chosen : geo::kInvalid);
                if (chosen && !world->faceValid(*chosen))
                    throw CommandError(std::format("pin: {} has no face {}", src.name(), *chosen));
                if (!hit)
                    throw CommandError("pin: no face found on " + src.name());
                setPlug(g, pid, PinNode::kIndex, int64_t(hit->face));
                setPlug(g, pid, PinNode::kFaceMode, faceMode);
                setPlug(g, pid, PinNode::kTriangle, D3(hit->points[0], hit->points[1], hit->points[2]));
                setPlug(g, pid, PinNode::kBary, hit->bary);
                break;
            }
            default:
            {
                deform::SurfacePoint sp;
                if (!deform::locateNearest(*world, at, uvSet, sp))
                    throw CommandError("pin: no surface point found on " + src.name());
                setPlug(g, pid, PinNode::kSurfaceMode, surfaceMode);
                if (surfaceMode == 0)
                {
                    glm::dvec2 uv = sp.uv[0] * sp.bary.x + sp.uv[1] * sp.bary.y + sp.uv[2] * sp.bary.z;
                    if (a.has("uv"))
                    {
                        const json& j = a.flags["uv"];
                        if (!j.is_array() || j.size() != 2)
                            throw CommandError("pin: uv is [u, v]");
                        uv = {j[0].get<double>(), j[1].get<double>()};
                    }
                    setPlug(g, pid, PinNode::kU, uv.x);
                    setPlug(g, pid, PinNode::kV, uv.y);
                }
                setPlug(g, pid, PinNode::kTriangle, D3(sp.points[0], sp.points[1], sp.points[2]));
                setPlug(g, pid, PinNode::kBary, sp.bary);
                break;
            }
            }
        }

        g.evaluate();
        setPlug(g, pid, PinNode::kBindSize, pin->as<double>(PinNode::kOutSize));
        if (isMesh && hasUv)
        {
            deform::SurfacePoint sp;
            if (deform::locateNearest(*world, pin->as<D3>(PinNode::kOutTranslate), uvSet, sp) && sp.hasUv)
            {
                const glm::dvec2 uv = sp.uv[0] * sp.bary.x + sp.uv[1] * sp.bary.y + sp.uv[2] * sp.bary.z;
                setPlug(g, pid, PinNode::kBindU, uv.x);
                setPlug(g, pid, PinNode::kBindV, uv.y);
            }
        }

        Node* loc = guarded([&] { return g.createNode(*types::find("locator"), base, kNoNode, true); });
        connectPlug(g, {pid, PinNode::kOutTranslate, -1}, {loc->id(), Transform::kTranslate, -1});
        connectPlug(g, {pid, PinNode::kOutRotate, -1}, {loc->id(), Transform::kRotate, -1});
        g.evaluate();
        out.push_back(loc->name());
        if (!obj)
            continue;

        if (follow == 2)
        {
            setPlug(g, pid, PinNode::kRestScale, obj->as<D3>(Transform::kScale));
            if (!g.source({obj->id(), Transform::kScale, -1}))
                connectPlug(g, {pid, PinNode::kOutScale, -1}, {obj->id(), Transform::kScale, -1});
        }
        const char* cmd = follow == 0 ? "pointConstraint" : "parentConstraint";
        json c = ops::execute(cmd, {{"args", {loc->path(), obj->path()}}, {"maintainOffset", a.get<bool>("maintainOffset", true)}});
        if (Node* con = g.find(c.get<std::string>()))
            if (const int slot = slotOf(g, con->id(), loc->id()); slot >= 0)
                connectPlug(g, {pid, PinNode::kWeight, -1}, {con->id(), ConstraintNode::kTargetWeight, -1, slot});
        g.evaluate();
    }
    return out;
}

// ------------------------------------------------------------------------------------------------ bake

/** The pin node, given it or the locator it drives; null for anything else. */
Node* pinOf(Graph& g, Node& n)
{
    if (n.type().name == kType)
        return &n;
    if (auto src = g.source({n.id(), Transform::kTranslate, -1}))
        if (Node* p = g.node(src->node); p && p->type().name == kType)
            return p;
    return nullptr;
}

/** The objects a pin's locators constrain, directly or through the constraint blend on their channels. */
std::vector<NodeId> drivenBy(Graph& g, const Node& pin)
{
    std::vector<NodeId> out;
    auto add = [&](NodeId id) {
        if (id != kNoNode && std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    };
    for (const PlugRef& locIn : g.destinations({pin.id(), PinNode::kOutTranslate, -1}))
        for (const PlugRef& conIn : g.destinations({locIn.node, Transform::kWorldMatrix, -1}))
        {
            if (!dynamic_cast<ConstraintNode*>(g.node(conIn.node)))
                continue;
            for (int attr : {int(ConstraintNode::kConstraintTranslate), int(ConstraintNode::kConstraintRotate)})
                for (const PlugRef& d : g.destinations({conIn.node, attr, -1}))
                {
                    const Node& dn = *g.node(d.node);
                    if (anim::isBlend(dn))
                        add(anim::memberOf(g, d.node).node);
                    else if (dn.type().isA("transform"))
                        add(d.node);
                }
        }
    return out;
}

/** The mesh or curve node a pin rides. */
NodeId sourceOf(Graph& g, const Node& pin)
{
    const int attr = pin.as<int64_t>(PinNode::kSource) == kCurve ? PinNode::kInCurve : PinNode::kMeshMatrix;
    auto src = g.source({pin.id(), attr, -1});
    return src ? src->node : kNoNode;
}

NodeId jointAbove(Graph& g, NodeId id)
{
    for (NodeId p = g.parent(id); p != kNoNode; p = g.parent(p))
        if (rig::isJoint(*g.node(p)))
            return p;
    return kNoNode;
}

/** The mesh points under a pin now, with their shares: what skin weights to read there. */
std::vector<std::pair<uint32_t, double>> anchorOf(const Node& pin, const geo::Mesh& m)
{
    const int64_t source = pin.as<int64_t>(PinNode::kSource);
    const int64_t index = pin.as<int64_t>(PinNode::kIndex);
    auto tri = [&] {
        std::vector<std::pair<uint32_t, double>> out;
        const D3 t = pin.as<D3>(PinNode::kTriangle), b = pin.as<D3>(PinNode::kBary);
        for (int k = 0; k < 3; ++k)
            if (t[k] >= 0.0)
                out.push_back({uint32_t(t[k]), b[k]});
        return out;
    };
    auto byUv = [&](double u, double v) {
        std::vector<std::pair<uint32_t, double>> out;
        deform::SurfacePoint sp;
        if (deform::locateUv(m, pin.as<std::string>(PinNode::kUvSet), {u, v}, sp))
            for (int k = 0; k < 3; ++k)
                out.push_back({sp.points[k], sp.bary[k]});
        return out;
    };
    const bool sameTopology = int64_t(m.numPoints()) == pin.as<int64_t>(PinNode::kBindPoints) && int64_t(m.numFaces()) == pin.as<int64_t>(PinNode::kBindFaces);
    if (source == kSurface && pin.as<int64_t>(PinNode::kSurfaceMode) == 0)
        return byUv(pin.as<double>(PinNode::kU), pin.as<double>(PinNode::kV));
    if (!sameTopology)
        return byUv(pin.as<double>(PinNode::kBindU), pin.as<double>(PinNode::kBindV));
    switch (source)
    {
    case kVertex: return m.pointValid(Index(index)) ? std::vector<std::pair<uint32_t, double>>{{uint32_t(index), 1.0}} : tri();
    case kEdge:
        if (m.edgeValid(Index(index)))
        {
            const double f = std::clamp(pin.as<double>(PinNode::kFraction), 0.0, 1.0);
            return {{uint32_t(m.edgePoint(Index(index), 0)), 1.0 - f}, {uint32_t(m.edgePoint(Index(index), 1)), f}};
        }
        return {};
    case kFace:
        if (pin.as<int64_t>(PinNode::kFaceMode) == 0 && m.faceValid(Index(index)))
        {
            std::vector<std::pair<uint32_t, double>> out;
            std::vector<Index> pts = m.facePoints(Index(index));
            for (Index p : pts)
                out.push_back({uint32_t(p), 1.0 / double(pts.size())});
            return out;
        }
        return tri();
    default: return tri();
    }
}

/** The joint that carries a pin: the heaviest skin influence under it, else the nearest joint above its source. */
NodeId carrierOf(Graph& g, const Node& pin)
{
    const NodeId src = sourceOf(g, pin);
    if (src == kNoNode)
        throw CommandError("bakePin: " + pin.name() + " rides nothing");
    if (pin.as<int64_t>(PinNode::kSource) != kCurve)
        if (const NodeId skinId = skin::find(g, src); skinId != kNoNode)
        {
            const MeshPtr& m = pin.as<MeshPtr>(PinNode::kInMesh);
            if (!m)
                throw CommandError("bakePin: " + pin.name() + " has no mesh");
            const WeightTable t = skin::table(g, skinId);
            std::map<uint32_t, double> sum;
            for (const auto& [p, share] : anchorOf(pin, *m))
                if (p < t.points.size())
                    for (const auto& [inf, w] : t.points[p])
                        sum[inf] += double(w) * share;
            std::optional<uint32_t> best;
            double most = 0.0;
            for (const auto& [inf, w] : sum)
                if (w > most)
                {
                    most = w;
                    best = inf;
                }
            if (best)
                for (const skin::Influence& i : skin::influences(g, skinId))
                    if (i.index == *best)
                        return i.joint;
            throw CommandError("bakePin: no joint weights the surface under " + pin.name());
        }
    if (const NodeId j = jointAbove(g, src); j != kNoNode)
        return j;
    throw CommandError(std::format("bakePin: {} isn't skinned or under a joint, so no joint carries {}", g.node(src)->name(), pin.name()));
}

struct Range
{
    double start = 0.0, end = 0.0, fps = 30.0;
    std::vector<double> times() const
    {
        const long long frames = std::max<long long>(0, std::llround((end - start) * fps));
        std::vector<double> out;
        for (long long f = 0; f <= frames; ++f)
            out.push_back(start + double(f) / fps);
        return out;
    }
};

/** Evaluates the scene at every time of the range, calling fn after each; the time is put back after. */
template <class F> void sample(Graph& g, const std::vector<double>& times, F&& fn)
{
    const double was = g.time();
    for (size_t i = 0; i < times.size(); ++i)
    {
        g.setTime(times[i]);
        g.evaluate();
        fn(i);
    }
    g.setTime(was);
    g.evaluate();
}

/** Writes per-frame values of a channel as linear keys. */
void keyChannel(Graph& g, NodeId node, int attr, const std::vector<double>& times, const std::vector<D3>& values)
{
    for (int k = 0; k < 3; ++k)
    {
        anim::Curve c;
        for (size_t i = 0; i < times.size(); ++i)
            c.keys.push_back({.time = times[i], .value = values[i][k], .in = anim::Tangent::Linear, .out = anim::Tangent::Linear});
        const NodeId curve = anim::setKey(g, {node, attr, k}, c.keys.front().time, c.keys.front().value);
        anim::setCurve(g, curve, c);
    }
}

bool sameMatrices(const std::vector<glm::dmat4>& rel)
{
    for (const glm::dmat4& m : rel)
        for (int c = 0; c < 4; ++c)
        {
            const double tol = c == 3 ? 1e-4 * std::max(1.0, glm::length(D3(rel.front()[3]))) : 1e-5;
            if (glm::length(glm::dvec4(m[c]) - glm::dvec4(rel.front()[c])) > tol)
                return false;
        }
    return true;
}

/**
 * A joint under the carrier following what world reads: a socket (no keys) when it holds still relative to the carrier
 * over the range, else a bone keyed every frame.
 */
NodeId bakeFollower(Graph& g, NodeId carrier, const std::string& base, const std::optional<std::string>& name, const Range& r,
                    const std::function<glm::dmat4()>& world)
{
    const std::vector<double> times = r.times();
    std::vector<glm::dmat4> rel(times.size());
    sample(g, times, [&](size_t i) { rel[i] = glm::inverse(worldOf(*g.node(carrier))) * world(); });
    const bool rigid = sameMatrices(rel);
    Node* j = guarded([&] { return g.createNode(*types::find("joint"), name ? *name : base + (rigid ? "_socket" : "_bone"), carrier); });
    const NodeId jid = j->id();
    g.setValue({jid, Joint::kRadius}, g.node(carrier)->value(Joint::kRadius));
    if (rigid)
    {
        g.evaluate();
        repeat::setWorldMatrix(g, jid, world());
        return jid;
    }
    g.setValue({jid, Transform::kRotationInterpolation}, int64_t(RotationInterpolation::Euler));
    std::vector<D3> tr, ro, sc;
    D3 last(0.0);
    for (const glm::dmat4& local : rel)
    {
        D3 scale(glm::length(D3(local[0])), glm::length(D3(local[1])), glm::length(D3(local[2])));
        const glm::dmat3 rot = orthonormal(glm::dmat3(local));
        last = eulerNear(rot, RotateOrder::XYZ, last);
        tr.push_back(D3(local[3]));
        ro.push_back(last);
        sc.push_back(scale);
    }
    keyChannel(g, jid, Transform::kTranslate, times, tr);
    keyChannel(g, jid, Transform::kRotate, times, ro);
    keyChannel(g, jid, Transform::kScale, times, sc);
    g.evaluate();
    return jid;
}

/** A pinned joint keeps its place in the skeleton: its pinned channels become keys every frame and the pins let go. */
NodeId bakeJoint(Graph& g, NodeId joint, const Range& r)
{
    const std::vector<double> times = r.times();
    const bool scaled = [&] {
        auto src = g.source({joint, Transform::kScale, -1});
        return src && g.node(src->node)->type().name == kType;
    }();
    std::vector<D3> tr(times.size()), ro(times.size()), sc(times.size());
    sample(g, times, [&](size_t i) {
        const Node& n = *g.node(joint);
        tr[i] = n.as<D3>(Transform::kTranslate);
        ro[i] = n.as<D3>(Transform::kRotate);
        sc[i] = n.as<D3>(Transform::kScale);
    });
    ops::execute("removeConstraint", {{"args", json::array({g.node(joint)->path()})}});
    if (scaled)
        g.disconnect({joint, Transform::kScale, -1});
    // Per-channel keys need Euler curves; exporters write baked joints that way too.
    g.setValue({joint, Transform::kRotationInterpolation, -1}, int64_t(RotationInterpolation::Euler));
    keyChannel(g, joint, Transform::kTranslate, times, tr);
    keyChannel(g, joint, Transform::kRotate, times, ro);
    if (scaled)
        keyChannel(g, joint, Transform::kScale, times, sc);
    g.evaluate();
    return joint;
}

/** Puts the joint a prop attaches to on the prop, for exporters (kSocketAttr). */
void recordSocket(Graph& g, NodeId prop, const std::string& joint)
{
    int attr = g.node(prop)->type().attrIndex(kSocketAttr);
    if (attr < 0)
        attr = g.addAttr(prop, {.name = kSocketAttr,
                                .type = PlugType::String,
                                .defaultValue = std::string(),
                                .doc = "The joint this prop attaches to in Unreal, with no offset (bakePin)."});
    g.setValue({prop, attr, -1}, joint);
}

json bakePins(const Args& a)
{
    Graph& g = graph();
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("bakePin: give pins, their locators or pinned objects");
    std::optional<std::string> name;
    if (a.has("name"))
    {
        name = a.get<std::string>("name");
        if (objs.size() > 1)
            throw CommandError("bakePin: name takes one pin");
    }
    const anim::Playback& pb = anim::playback();
    Range r{a.get<double>("start", pb.minTime), a.get<double>("end", pb.maxTime), a.get<double>("fps", pb.fps)};
    if (r.end < r.start)
        throw CommandError("bakePin: end is before start");
    if (r.fps <= 0.0)
        r.fps = 30.0;
    g.evaluate();

    std::vector<NodeId> allPins;
    for (NodeId id : g.nodes())
        if (g.node(id)->type().name == kType)
            allPins.push_back(id);

    // What to bake: a pin's own frame when it drives nothing, else each object it drives.
    std::vector<std::pair<NodeId, NodeId>> subjects; // (pin with no objects, kNoNode) or (kNoNode, object)
    auto addSubject = [&](NodeId pin, NodeId obj) {
        if (std::find(subjects.begin(), subjects.end(), std::pair{pin, obj}) == subjects.end())
            subjects.push_back({pin, obj});
    };
    for (const std::string& s : objs)
    {
        Node& n = nodeNamed(s, "bakePin");
        if (Node* pin = pinOf(g, n))
        {
            std::vector<NodeId> driven = drivenBy(g, *pin);
            if (driven.empty())
                addSubject(pin->id(), kNoNode);
            for (NodeId d : driven)
                addSubject(kNoNode, d);
            continue;
        }
        bool pinned = false;
        for (NodeId p : allPins)
            for (NodeId d : drivenBy(g, *g.node(p)))
                pinned |= d == n.id();
        if (!pinned)
            throw CommandError("bakePin: " + n.name() + " is neither a pin, a pin locator nor pinned");
        addSubject(kNoNode, n.id());
    }

    json out = json::array();
    for (const auto& [pinId, obj] : subjects)
    {
        if (pinId != kNoNode)
        {
            const Node& pin = *g.node(pinId);
            std::string base = pin.name();
            for (const PlugRef& dst : g.destinations({pinId, PinNode::kOutTranslate, -1}))
                base = g.node(dst.node)->name();
            const NodeId j = bakeFollower(g, carrierOf(g, pin), base, name, r, [&] { return g.node(pinId)->as<glm::dmat4>(PinNode::kOutMatrix); });
            out.push_back(g.node(j)->path());
            continue;
        }
        if (rig::isJoint(*g.node(obj)))
        {
            out.push_back(g.node(bakeJoint(g, obj, r))->path());
            continue;
        }
        // A prop: its socket or bone sits on the carrier of the heaviest pin driving it, where the prop is, so it attaches there with no offset.
        const Node* heaviest = nullptr;
        for (NodeId p : allPins)
            for (NodeId d : drivenBy(g, *g.node(p)))
                if (d == obj && (!heaviest || g.node(p)->as<double>(PinNode::kWeight) > heaviest->as<double>(PinNode::kWeight)))
                    heaviest = g.node(p);
        if (!heaviest)
            throw CommandError("bakePin: " + g.node(obj)->name() + " is not pinned");
        const NodeId j = bakeFollower(g, carrierOf(g, *heaviest), g.node(obj)->name(), name, r, [&] { return worldOf(*g.node(obj)); });
        recordSocket(g, obj, g.node(j)->name());
        out.push_back(g.node(j)->path());
    }
    g.evaluate();
    return out;
}

void registerOptionBox()
{
    ops::options::Override o;
    o.order = {"source", "follow", "maintainOffset", "weight", "shape", "rebindByUv", "curveMode", "along", "upMode", "upVector", "upObject",
               "aimCurve", "roll", "index", "neighbor", "fraction", "faceMode", "surfaceMode", "uvSet"};
    o.hidden = {"uv", "position", "name"};
    o.sections = {{"curveMode", "Curve"}, {"along", "Curve"},       {"upMode", "Curve"},     {"upVector", "Curve"}, {"upObject", "Curve"},
                  {"aimCurve", "Curve"},  {"roll", "Curve"},        {"index", "Mesh"},       {"neighbor", "Mesh"},  {"fraction", "Mesh"},
                  {"faceMode", "Mesh"},   {"surfaceMode", "Mesh"},  {"uvSet", "Mesh"}};
    o.labels = {{"rebindByUv", "Rebind by UV"}, {"maintainOffset", "Maintain Offset"}};
    o.choices = {{"follow", {{"translate", "Translate"}, {"translateRotate", "Translate and Rotate"}, {"full", "Translate, Rotate and Scale"}}},
                 {"shape", {{"final", "Final (deformed)"}, {"rest", "Rest"}}}};
    o.ranges = {{"weight", {0.0, 1.0, 0.05, ""}}, {"roll", {-180.0, 180.0, 1.0, " deg"}}, {"fraction", {0.0, 1.0, 0.05, ""}}};
    ops::options::setOverride("pin", std::move(o));
}
} // namespace

std::string socketOf(const Node& n)
{
    const int attr = n.type().attrIndex(kSocketAttr);
    if (attr < 0)
        return {};
    const auto* joint = std::get_if<std::string>(&n.value(attr));
    return joint ? *joint : std::string();
}

std::string socketBoneOf(const Node& n)
{
    const int attr = n.type().attrIndex(kSocketBoneAttr);
    if (attr < 0)
        return {};
    const auto* bone = std::get_if<std::string>(&n.value(attr));
    return bone ? *bone : std::string();
}

glm::dmat4 socketOffsetOf(const Node& n)
{
    const int attr = n.type().attrIndex(kSocketOffsetAttr);
    if (attr < 0)
        return glm::dmat4(1.0);
    const auto* m = std::get_if<glm::dmat4>(&n.value(attr));
    return m ? *m : glm::dmat4(1.0);
}

void registerTypes()
{
    if (types::find(kType))
        return;
    registerTypesImpl();
}

void registerCommands()
{
    if (ops::find("pin"))
        return;
    using T = ArgType;
    ops::registerCommand({
        .name = "pin",
        .doc = "Pin objects or joints to a curve, a vertex, an edge, a face or a mesh's surface: each gets a pin locator riding the "
               "source and is constrained to it. Pinning one object several times blends the pins by weight, like a parent "
               "constraint with several targets. Give the objects first and the curve or mesh last; with no objects one pin is "
               "made. Returns the pin locators.",
        .positionalDoc = "objects..., curve or mesh",
        .flags = {
            {"source", "src", T::String, "auto", "auto | curve | vertex | edge | face | surface. auto: curve for a curve; for a mesh the selected component kind, else surface.",
             {"auto", "curve", "vertex", "edge", "face", "surface"}},
            {"follow", "fo", T::String, "translateRotate", "translate | translateRotate | full. full also scales with the source.",
             {"translate", "translateRotate", "full"}},
            {"maintainOffset", "mo", T::Bool, true, "Keep each object where it is relative to its pin; off snaps it onto the pin."},
            {"weight", "w", T::Float, 1.0, "The pin's share when the object has several.", {}, {}, 0.0},
            {"shape", "sh", T::String, "final", "final | rest: ride the deformed mesh or its shape before deformers.", {"final", "rest"}},
            {"rebindByUv", "ruv", T::Bool, true, "Find the pin again by uv when the mesh's topology changes."},
            {"curveMode", "cm", T::String, "fraction", "parameter | length | fraction: how along reads.", {"parameter", "length", "fraction"}},
            {"along", "al", T::Float, nullptr, "Where on the curve, in curveMode's units. Default: nearest each object."},
            {"upMode", "um", T::String, "curveFrames", "curveFrames | upVector | upObject | aimCurve: how the frame turns about the curve.",
             {"curveFrames", "upVector", "upObject", "aimCurve"}},
            {"upVector", "upv", T::Vec3, nullptr, "Up direction for upMode upVector. Default [0, 1, 0]."},
            nodeFlag({"upObject", "uo", T::String, nullptr, "The object up points at (upMode upObject)."}, "transform"),
            nodeFlag({"aimCurve", "ac", T::String, nullptr, "The curve up aims at (upMode aimCurve)."}, "curve"),
            {"roll", "r", T::Float, 0.0, "Turn of the frame, degrees: about the curve, or about a mesh's normal."},
            {"index", "i", T::Int, nullptr, "The vertex, edge or face id. Default: the selected one, else the nearest each object.", {}, {}, 0.0},
            {"neighbor", "nb", T::Int, nullptr, "Vertex: the point the frame's x aims at. Default: along the first edge."},
            {"fraction", "fr", T::Float, nullptr, "Edge: how far along it. Default: nearest each object.", {}, {}, 0.0, 1.0},
            {"faceMode", "fm", T::String, "centre", "centre | barycentric: the face's centre or the point on it nearest each object.",
             {"centre", "barycentric"}},
            {"surfaceMode", "sm", T::String, "auto", "auto | uv | nearest. auto: uv when the mesh has the uv set.", {"auto", "uv", "nearest"}},
            {"uv", "", T::List, nullptr, "Surface uv mode: [u, v] of the pin. Default: the uv nearest each object."},
            {"uvSet", "", T::String, "uv0", "The uv set read."},
            {"position", "", T::Vec3, nullptr, "With no objects: where the pin goes, world space (the nearest point of the source)."},
            {"name", "n", T::String, nullptr, "Name for the pin locator."},
        },
        .run = makePins,
        .examples = {"pin(\"Button\", \"Jacket\", source=\"face\", index=12)", "pin(\"Bead\", \"Wire\", curveMode=\"fraction\", along=0.25)"},
    });

    ops::registerCommand({
        .name = "bakePin",
        .doc = "Turn pins into joints for Unreal. A pinned joint gets its pinned channels keyed every frame and lets go of the pins. "
               "A pin driving nothing, or a pinned prop, gets a joint under the joint carrying the source (the heaviest skin "
               "influence there, or the joint above the curve or mesh): a socket when it holds still relative to that joint over "
               "the range, else a bone keyed every frame. A prop's joint sits where the prop is, so it attaches with no offset; the "
               "prop records it in unrealSocket, which FBX exports write and the Unreal link turns into a socket on the character's "
               "skeletal mesh. Returns the joints.",
        .positionalDoc = "pins, pin locators or pinned objects",
        .flags = {{"start", "st", T::Float, nullptr, "Seconds; default the playback start."},
                  {"end", "et", T::Float, nullptr, "Seconds; default the playback end."},
                  {"fps", "fps", T::Float, nullptr, "Frames per second; default the scene's.", {}, {}, 1.0},
                  {"name", "n", T::String, nullptr, "Name for the joint (one pin). Default: <locator or prop>_socket or _bone."}},
        .run = bakePins,
    });
    registerOptionBox();
}
} // namespace lila::scene::pins
