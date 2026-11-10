#include "curve/CurveNodes.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "curve/AlongNodes.h"
#include "curve/CurveControls.h"
#include "curve/NodeHelpers.h"
#include "curve/PipeToolsNodes.h"
#include "curve/StripUvAttrs.h"
#include "curve/EdgeToCurve.h"
#include "curve/EnvironmentNodes.h"
#include "curve/Fit.h"
#include "curve/Ops.h"
#include "curve/PipeNode.h"
#include "curve/SurfaceConform.h"
#include "curve/SketchNode.h"
#include "curve/StrapNode.h"
#include "curve/PlanarNodes.h"
#include "curve/Sweep.h"
#include "curve/Tessellate.h"
#include "curve/VehicleNodes.h"
#include "geo/Bvh.h"
#include "scene/Components.h"
#include "scene/ConformTargets.h"
#include "scene/Graph.h"
#include "scene/Materials.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <map>
#include <mutex>
#include <unordered_map>

namespace lila::curve
{
using namespace scene;
using namespace helpers;
using json = nlohmann::json;
using D3 = glm::dvec3;

namespace
{
json vec(const D3& v) { return json::array({v.x, v.y, v.z}); }

D3 vecFrom(const json& j)
{
    if (!j.is_array() || j.size() != 3)
        throw std::invalid_argument("expected [x, y, z]");
    return D3(j[0].get<double>(), j[1].get<double>(), j[2].get<double>());
}

json vecs(const std::vector<D3>& v)
{
    json a = json::array();
    for (const D3& p : v)
        a.push_back(vec(p));
    return a;
}

std::vector<D3> vecsFrom(const json& j)
{
    std::vector<D3> out;
    for (const json& p : j)
        out.push_back(vecFrom(p));
    return out;
}
} // namespace

json splineToJson(const Spline& s)
{
    json j = {{"type", kindName(s.kind)}, {"closed", s.closed}, {"points", vecs(s.points)}};
    if (s.kind == Kind::Nurbs)
    {
        j["degree"] = s.degree;
        if (!s.weights.empty())
            j["weights"] = s.weights;
        if (!s.knots.empty())
            j["knots"] = s.knots;
    }
    if (s.kind == Kind::Bezier)
    {
        j["handleL"] = vecs(s.handleL);
        j["handleR"] = vecs(s.handleR);
        json tl = json::array(), tr = json::array();
        for (HandleType t : s.typeL)
            tl.push_back(handleTypeName(t));
        for (HandleType t : s.typeR)
            tr.push_back(handleTypeName(t));
        j["typeL"] = tl;
        j["typeR"] = tr;
    }
    if (!s.tilt.empty())
        j["tilt"] = s.tilt;
    if (!s.radius.empty())
        j["radius"] = s.radius;
    if (!s.cornerRadius.empty())
        j["cornerRadius"] = s.cornerRadius;
    if (!s.attrs.empty())
        j["attrs"] = s.attrs;
    if (!s.pointAttrs.empty())
        j["pointAttrs"] = s.pointAttrs;
    return j;
}

Spline splineFromJson(const json& j)
{
    Spline s;
    if (!parseKind(j.value("type", std::string("nurbs")), &s.kind))
        throw std::invalid_argument("unknown curve type '" + j.value("type", std::string()) + "'");
    s.closed = j.value("closed", false);
    s.degree = j.value("degree", 3);
    s.points = vecsFrom(j.at("points"));
    if (j.contains("weights"))
        s.weights = j["weights"].get<std::vector<double>>();
    if (j.contains("knots"))
        s.knots = j["knots"].get<std::vector<double>>();
    if (j.contains("handleL"))
        s.handleL = vecsFrom(j["handleL"]);
    if (j.contains("handleR"))
        s.handleR = vecsFrom(j["handleR"]);
    auto types = [](const json& a, std::vector<HandleType>& out) {
        for (const json& t : a)
        {
            HandleType h = HandleType::Auto;
            if (!parseHandleType(t.get<std::string>(), &h))
                throw std::invalid_argument("unknown handle type '" + t.get<std::string>() + "'");
            out.push_back(h);
        }
    };
    if (j.contains("typeL"))
        types(j["typeL"], s.typeL);
    if (j.contains("typeR"))
        types(j["typeR"], s.typeR);
    if (j.contains("tilt"))
        s.tilt = j["tilt"].get<std::vector<double>>();
    if (j.contains("radius"))
        s.radius = j["radius"].get<std::vector<double>>();
    if (j.contains("cornerRadius"))
        s.cornerRadius = j["cornerRadius"].get<std::vector<double>>();
    if (j.contains("attrs"))
        s.attrs = j["attrs"].get<std::map<std::string, double>>();
    if (j.contains("pointAttrs"))
        s.pointAttrs = j["pointAttrs"].get<std::map<std::string, std::vector<double>>>();
    bool bezierGiven = s.kind == Kind::Bezier && s.handleL.size() == s.size() && s.handleR.size() == s.size();
    if (s.kind == Kind::Bezier && bezierGiven && (s.typeL.size() != s.size() || s.typeR.size() != s.size()))
    {
        s.typeL.assign(s.size(), HandleType::Free);
        s.typeR.assign(s.size(), HandleType::Free);
    }
    s.normalize();
    validate(s);
    return s;
}

json CurveData::toJson() const
{
    json a = json::array();
    for (const Spline& s : splines)
        a.push_back(splineToJson(s));
    return {{"kind", "curve"}, {"splines", a}};
}

size_t CurveData::bytes() const
{
    size_t n = sizeof(*this);
    for (const Spline& s : splines)
    {
        n += s.size() * (sizeof(D3) * 3 + 64);
        for (const auto& [name, v] : s.pointAttrs)
            n += v.size() * sizeof(double) + 48;
    }
    return n;
}

scene::DataPtr CurveData::fromJson(const json& j)
{
    auto d = std::make_shared<CurveData>();
    const json& src = j.contains("value") && !j.contains("splines") ? j["value"] : j;
    for (const json& s : src.value("splines", json::array()))
        d->splines.push_back(splineFromJson(s));
    return d;
}

scene::DataPtr makeCurveData(Splines s)
{
    auto d = std::make_shared<CurveData>();
    d->splines = std::move(s);
    return d;
}

const Splines& splinesOf(const scene::PlugValue& v)
{
    static const Splines none;
    if (const DataPtr* d = std::get_if<DataPtr>(&v); d && *d && std::string((*d)->kind()) == "curve")
        return static_cast<const CurveData&>(**d).splines;
    return none;
}

namespace
{
/** Object-space BVH of a surface snapshot, rebuilt only when the snapshot changes. */
std::shared_ptr<const geo::MeshBvh> liveSurfaceBvh(const MeshPtr& mesh)
{
    struct Entry
    {
        std::weak_ptr<const geo::Mesh> source;
        std::shared_ptr<const geo::MeshBvh> bvh;
    };
    static std::mutex mutex;
    static std::unordered_map<const geo::Mesh*, Entry> cache;
    std::scoped_lock lock(mutex);
    auto it = cache.find(mesh.get());
    if (it != cache.end() && it->second.source.lock() == mesh)
        return it->second.bvh;
    if (cache.size() > 16)
        std::erase_if(cache, [](const auto& kv) { return kv.second.source.expired(); });
    auto bvh = std::make_shared<geo::MeshBvh>(*mesh);
    cache[mesh.get()] = {mesh, bvh};
    return bvh;
}

/** The splines conformed to the live surface, worked out in the surface's object space and brought back. */
Splines onLiveSurface(const Splines& in, const MeshPtr& surface, const glm::dmat4& surfaceWorld, const glm::dmat4& curveWorld, double offset)
{
    LILA_ZONE();

    const glm::dmat4 toSurface = glm::inverse(surfaceWorld) * curveWorld, back = glm::inverse(toSurface);
    // The offset is world cm; a scaled surface takes it in its own units.
    const double scale = std::cbrt(std::abs(glm::determinant(glm::dmat3(surfaceWorld))));
    auto bvh = liveSurfaceBvh(surface);
    Splines out = in;
    for (Spline& s : out)
    {
        transform(s, toSurface);
        conformToSurface(s, *surface, *bvh, scale > 1e-12 ? offset / scale : offset);
        transform(s, back);
    }
    return out;
}
} // namespace

void CurveShape::compute(const EvalContext& ctx)
{
    Transform::compute(ctx);
    DataPtr in = as<DataPtr>(kCreate);
    if (hasControls(*this) && !splinesOf(in).empty())
        in = makeCurveData(drivenSplines(*this, splinesOf(in)));
    if (const MeshPtr& surface = as<MeshPtr>(kLiveSurface); surface && surface->numFaces() > 0 && !splinesOf(in).empty())
        in = makeCurveData(onLiveSurface(splinesOf(in), surface, as<glm::dmat4>(kLiveSurfaceMatrix), as<glm::dmat4>(kWorldMatrix),
                                         as<double>(kSurfaceOffset)));
    setOutput(kLocal, in);
    const glm::dmat4& w = as<glm::dmat4>(kWorldMatrix);
    const Splines& s = splinesOf(in);
    const int axis = symmetryAxis(*this);
    if (s.empty() || (w == glm::dmat4(1.0) && axis < 0))
    {
        setOutput(kWorldSpace, in);
        return;
    }
    Splines ws = s;
    for (Spline& sp : ws)
        transform(sp, w);
    if (axis >= 0)
        for (Spline& sp : mirroredSplines(s, w, axis, as<int64_t>(type().attrIndex("symmetrySpace")) == 1))
            ws.push_back(std::move(sp));
    setOutput(kWorldSpace, makeCurveData(std::move(ws)));
}

int symmetryAxis(const Node& curve)
{
    const int a = curve.type().attrIndex("symmetry");
    return a < 0 ? -1 : int(std::clamp<int64_t>(curve.as<int64_t>(a), 0, 3)) - 1;
}

Splines mirroredSplines(const Splines& local, const glm::dmat4& world, int axis, bool objectSpace)
{
    glm::dmat4 flip(1.0);
    flip[axis][axis] = -1.0;
    const glm::dmat4 m = objectSpace ? world * flip : flip * world;
    Splines out = local;
    for (Spline& sp : out)
        transform(sp, m);
    return out;
}

size_t ownSplineCount(const Node& curve)
{
    return splinesOf(curve.value(CurveShape::kLocal)).size();
}

uint32_t componentId(size_t spline, size_t point, PointRole role) { return uint32_t(spline << 22) | uint32_t(point << 2) | uint32_t(role); }

void decodeComponent(uint32_t id, size_t* spline, size_t* point, PointRole* role)
{
    *spline = id >> 22;
    *point = (id >> 2) & 0xFFFFF;
    *role = PointRole(id & 3);
}

const Splines& shapeSplines(Graph& g, NodeId node)
{
    static const Splines none;
    Node* n = g.node(node);
    if (!n || !n->type().isA("curve"))
        return none;
    g.evaluate();
    return splinesOf(n->value(CurveShape::kLocal));
}

std::vector<std::pair<uint32_t, glm::dvec3>> componentPositions(const Splines& ss)
{
    std::vector<std::pair<uint32_t, glm::dvec3>> out;
    for (size_t si = 0; si < ss.size(); ++si)
    {
        const Spline& s = ss[si];
        for (size_t i = 0; i < s.size(); ++i)
            out.push_back({componentId(si, i, PointRole::Point), s.points[i]});
        if (s.kind == Kind::Bezier && s.handleL.size() == s.size())
            for (size_t i = 0; i < s.size(); ++i)
            {
                out.push_back({componentId(si, i, PointRole::HandleL), s.handleL[i]});
                out.push_back({componentId(si, i, PointRole::HandleR), s.handleR[i]});
            }
        if (s.kind == Kind::Nurbs && s.degree > 1)
        {
            std::vector<D3> eps = editPoints(s);
            for (size_t i = 0; i < eps.size(); ++i)
                out.push_back({componentId(si, i, PointRole::Edit), eps[i]});
        }
    }
    return out;
}

namespace
{
/** The curve's frame at span parameter u as columns tangent, normal and binormal, between the tessellated frames. */
glm::dmat3 frameAt(const Spline& s, const Polyline& pl, const std::vector<Frame>& fr, double u)
{
    const size_t n = pl.size();
    const double spans = spanCount(s);
    if (s.closed && u >= spans)
        u -= spans;
    size_t j = 0;
    while (j + 1 < n && pl.params[j + 1] <= u + 1e-9)
        ++j;
    Frame f = fr[j];
    const double next = j + 1 < n ? pl.params[j + 1] : spans;
    if (u > pl.params[j] + 1e-9 && next > pl.params[j] + 1e-12 && (j + 1 < n || s.closed))
    {
        const Frame& g = fr[(j + 1) % n];
        const double k = (u - pl.params[j]) / (next - pl.params[j]);
        D3 t = evaluate(s, u).d1;
        t = glm::length(t) > 1e-12 ? t : glm::mix(f.t, g.t, k);
        if (glm::length(t) > 1e-12)
        {
            f.t = glm::normalize(t);
            for (const D3& c : {glm::mix(f.n, g.n, k), f.n})
                if (D3 nn = c - f.t * glm::dot(c, f.t); glm::length(nn) > 1e-6)
                {
                    f.n = glm::normalize(nn);
                    break;
                }
            f.b = glm::normalize(glm::cross(f.t, f.n));
        }
    }
    return glm::dmat3(f.t, f.n, f.b);
}
} // namespace

std::vector<glm::dmat3> componentFrames(const Splines& ss, const std::vector<uint32_t>& ids)
{
    LILA_ZONE();

    std::vector<glm::dmat3> out(ids.size(), glm::dmat3(0.0));
    std::map<size_t, std::pair<Polyline, std::vector<Frame>>> along;
    for (size_t k = 0; k < ids.size(); ++k)
    {
        size_t si, i;
        PointRole role;
        decodeComponent(ids[k], &si, &i, &role);
        if (si >= ss.size())
            continue;
        const Spline& s = ss[si];
        const int spans = spanCount(s);
        if (spans == 0 || (role == PointRole::Edit ? i > size_t(spans) : i >= s.size()))
            continue;
        auto it = along.find(si);
        if (it == along.end())
        {
            Polyline pl = tessellate(s);
            std::vector<Frame> fr = frames(pl);
            it = along.emplace(si, std::pair{std::move(pl), std::move(fr)}).first;
        }
        if (it->second.first.size() == 0)
            continue;
        // NURBS control points are off the curve, so they take the frame where the curve passes closest.
        const double u = s.kind == Kind::Nurbs && role == PointRole::Point ? closestParam(s, s.points[i]) : double(i);
        // U, V, N = tangent, normal, binormal: on a planar curve N lies in its plane and V is the plane normal.
        glm::dmat3 f = frameAt(s, it->second.first, it->second.second, u);
        if ((role == PointRole::HandleL || role == PointRole::HandleR) && s.handleL.size() == s.size())
        {
            const D3 d = role == PointRole::HandleR ? s.handleR[i] - s.points[i] : s.points[i] - s.handleL[i];
            const D3 nrm = f[2] - d * (glm::dot(f[2], d) / std::max(glm::dot(d, d), 1e-300));
            if (glm::length(d) > 1e-9 && glm::length(nrm) > 1e-6)
            {
                const D3 t = glm::normalize(d), nn = glm::normalize(nrm);
                f = glm::dmat3(t, glm::cross(nn, t), nn);
            }
        }
        out[k] = f;
    }
    return out;
}

Splines movedComponents(const Splines& in, const std::vector<std::pair<uint32_t, glm::dvec3>>& moves)
{
    Splines out = in;
    std::map<size_t, std::vector<std::pair<uint32_t, D3>>> bySpline;
    for (auto& m : moves)
        bySpline[m.first >> 22].push_back(m);
    for (auto& [si, list] : bySpline)
    {
        if (si >= out.size())
            continue;
        Spline& s = out[si];
        std::map<std::pair<size_t, int>, D3> handles;
        std::vector<std::pair<size_t, int>> edited;
        std::map<size_t, D3> eps;
        for (auto& [id, p] : list)
        {
            size_t sp, i;
            PointRole role;
            decodeComponent(id, &sp, &i, &role);
            if (role == PointRole::Point && i < s.size())
            {
                D3 delta = p - s.points[i];
                s.points[i] = p;
                if (s.kind == Kind::Bezier && s.handleL.size() == s.size())
                {
                    s.handleL[i] += delta;
                    s.handleR[i] += delta;
                }
            }
            else if (role == PointRole::HandleL || role == PointRole::HandleR)
                handles[{i, role == PointRole::HandleL ? 0 : 1}] = p;
            else if (role == PointRole::Edit)
                eps[i] = p;
        }
        if (s.kind == Kind::Bezier && s.handleL.size() == s.size())
        {
            for (auto& [key, p] : handles)
                if (key.first < s.size())
                {
                    (key.second == 0 ? s.handleL : s.handleR)[key.first] = p;
                    edited.push_back(key);
                }
            updateHandles(s, edited);
        }
        if (!eps.empty() && s.kind == Kind::Nurbs)
        {
            std::vector<D3> all = editPoints(s);
            for (auto& [i, p] : eps)
                if (i < all.size())
                    all[i] = p;
            Spline refit = interpolate(all, s.degree, s.closed);
            if (isValid(refit))
                s = refit;
        }
    }
    return out;
}

namespace
{
std::vector<AttrDef> tessAttrs(int64_t mode)
{
    return {eAttr("tessellation", {"fixed", "adaptive", "game"}, mode,
                  "How curves become points: fixed segments per span, adaptive to the error and angle, or game (straight spans one "
                  "segment, curved ones by angle, corners exact)."),
            iAttr("segments", 8, "Segments per curved span (fixed).", 1), fAttr("chordError", 0.02, "Largest gap from the true curve, cm (adaptive).", 1e-5),
            fAttr("maxAngle", 10.0, "Largest turn between segments, degrees (adaptive and game).", 0.1, 90.0)};
}

TessOptions tessOf(const Node& n)
{
    TessOptions o;
    o.mode = TessMode(std::clamp<int64_t>(i(n, "tessellation"), 0, 2));
    o.perSpan = int(std::max<int64_t>(1, i(n, "segments")));
    o.chordError = std::max(1e-5, f(n, "chordError"));
    o.maxAngle = glm::radians(std::clamp(f(n, "maxAngle"), 0.1, 90.0));
    return o;
}

/** A history node: inputs in, one output out, rebuilt by `run` whenever an input changes. */
class CurveOpNode final : public Node
{
public:
    std::function<PlugValue(const Node&)> run;
    int outAttr = -1;

    void compute(const EvalContext&) override
    {
        LILA_ZONE();

        try
        {
            setOutput(outAttr, run(*this));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            setOutput(outAttr, type().attrs[size_t(outAttr)].type == PlugType::Mesh ? PlugValue(MeshPtr()) : PlugValue(DataPtr()));
        }
    }
};

void registerOp(std::string name, std::string icon, std::string doc, std::vector<AttrDef> attrs, std::function<PlugValue(const Node&)> run)
{
    if (types::find(name))
        return;
    NodeType t;
    t.name = std::move(name);
    t.icon = std::move(icon);
    t.doc = std::move(doc);
    t.ownAttrs = std::move(attrs);
    int out = -1;
    for (size_t k = 0; k < t.ownAttrs.size(); ++k)
        if (t.ownAttrs[k].flags & AttrOutput)
            out = int(k);
    t.create = [run, out] {
        auto n = std::make_unique<CurveOpNode>();
        n->run = run;
        n->outAttr = out;
        return n;
    };
    types::registerType(std::move(t));
}

PlugValue sweepRun(const Node& n)
{
    const Splines& paths = in(n, "path");
    const Splines& profs = in(n, "profile");
    TessOptions tess = tessOf(n);
    FrameOptions fo;
    fo.mode = FrameMode(std::clamp<int64_t>(i(n, "frameMode"), 0, 3));
    if (fo.mode == FrameMode::SurfaceNormal)
        fo.mode = FrameMode::Rmf;
    fo.up = v(n, "upVector");
    SweepOptions so;
    double twist = glm::radians(f(n, "twist")), s0 = f(n, "scaleStart"), s1 = f(n, "scaleEnd");
    CapMode caps[4] = {CapMode::None, CapMode::Ngon, CapMode::Grid, CapMode::Round};
    so.startCap = caps[std::clamp<int64_t>(i(n, "startCap"), 0, 3)];
    so.endCap = caps[std::clamp<int64_t>(i(n, "endCap"), 0, 3)];
    so.roundSegments = int(std::max<int64_t>(1, i(n, "roundSegments")));
    so.miter = b(n, "miter");
    so.uv = stripUvOf(n, {});
    so.flip = b(n, "flip");
    double every = f(n, "splitEvery");
    int64_t shape = i(n, "profileShape");
    geo::Mesh out;
    for (const Spline& sp : paths)
    {
        Path path = pathFrom(sp, tess, fo);
        if (path.size() < 2)
            continue;
        double L = path.closed ? path.length : path.s.back();
        so.scale = [=](double s) { return L > 0.0 ? s0 + (s1 - s0) * (s / L) : s0; };
        so.twist = [=](double s) { return L > 0.0 ? twist * s / L : 0.0; };
        so.splits.clear();
        if (every > 1e-6)
            for (double s = every; s < L - 1e-6; s += every)
                so.splits.push_back(s);
        std::vector<Profile> profiles;
        if ((shape == 0 && !profs.empty()))
        {
            for (const Spline& ps : profs)
                profiles.push_back(profileFromCurve(tessellate(ps, tess), path.points[0], path.frames[0], b(n, "alignProfile")));
        }
        else if (shape == 2)
            profiles.push_back(rectProfile(f(n, "width"), f(n, "height")));
        else
            profiles.push_back(circleProfile(int(std::max<int64_t>(3, i(n, "sides"))), f(n, "radius")));
        for (const Profile& p : profiles)
            out.append(sweep(p, path, so).mesh);
    }
    return meshValue(std::move(out));
}

PlugValue revolveRun(const Node& n)
{
    TessOptions tess = tessOf(n);
    RevolveOptions o;
    o.origin = v(n, "axisOrigin");
    o.axis = v(n, "axis");
    o.startAngle = glm::radians(f(n, "startSweep"));
    o.endAngle = glm::radians(f(n, "endSweep"));
    o.segments = int(std::max<int64_t>(1, i(n, "sections")));
    o.caps = b(n, "caps");
    o.flip = b(n, "flip");
    geo::Mesh out;
    for (const Spline& s : in(n, "profile"))
        out.append(revolve(tessellate(s, tess), o));
    if (const std::string& slot = n.as<std::string>(n.type().attrIndex("material")); !slot.empty())
    {
        std::vector<uint32_t> faces;
        for (geo::Index fi = 0; fi < out.faceCapacity(); ++fi)
            if (out.faceValid(fi))
                faces.push_back(fi);
        materials::assign(out, slot, faces);
    }
    return meshValue(std::move(out));
}

/**
 * One curve's splines as loft sections: pieces that meet end to end (a half and its live symmetry mirror) join into
 * one section, closed when the joined ends meet; pieces that don't touch stay sections of their own.
 */
std::vector<Spline> loftSectionsOf(std::vector<Spline> parts)
{
    if (parts.size() < 2)
        return parts;
    auto ends = [](const Spline& s) -> std::pair<glm::dvec3, glm::dvec3> {
        std::vector<Piece> ps = pieces(s);
        return ps.empty() ? std::pair{glm::dvec3(0.0), glm::dvec3(0.0)} : std::pair{ps.front().start(), ps.back().end()};
    };
    std::vector<Spline> out;
    while (!parts.empty())
    {
        Spline cur = parts.front();
        parts.erase(parts.begin());
        for (bool grew = true; grew && !cur.closed;)
        {
            grew = false;
            for (size_t k = 0; k < parts.size(); ++k)
            {
                if (parts[k].closed)
                    continue;
                auto [as, ae] = ends(cur);
                auto [bs, be] = ends(parts[k]);
                const double tol = 1e-5 * std::max({1.0, glm::length(as), glm::length(ae)});
                if (std::min({glm::distance(ae, bs), glm::distance(ae, be), glm::distance(as, bs), glm::distance(as, be)}) > tol)
                    continue;
                cur = attach(cur, parts[k], false, AttachEnds::Nearest, tol);
                parts.erase(parts.begin() + std::ptrdiff_t(k));
                grew = true;
                break;
            }
        }
        if (auto [s, e] = ends(cur); !cur.closed && glm::distance(s, e) <= 1e-5 * std::max(1.0, glm::length(s)))
            cur = closeCurve(cur, false);
        out.push_back(std::move(cur));
    }
    return out;
}

PlugValue loftRun(const Node& n)
{
    std::vector<Spline> secs, rails;
    for (const auto& [k, v] : n.elements(n.type().attrIndex("sections")))
        for (const Spline& s : loftSectionsOf(splinesOf(v)))
            secs.push_back(s);
    for (int k = 0; k < 2; ++k)
        for (const Spline& s : in(n, ("rail" + std::to_string(k)).c_str()))
            rails.push_back(s);
    LoftOptions o;
    o.tess = tessOf(n);
    o.samples = int(i(n, "samples"));
    o.spansBetween = int(std::max<int64_t>(1, i(n, "spansBetween")));
    o.smooth = b(n, "smooth");
    o.closeLoft = b(n, "closeLoft");
    o.caps = b(n, "caps");
    o.autoAlign = b(n, "autoAlign");
    o.flip = b(n, "flip");
    return meshValue(loft(secs, o, rails));
}

PlugValue patchRun(const Node& n)
{
    std::array<Spline, 4> sides;
    std::vector<Spline> all;
    for (int k = 0; k < 4; ++k)
        for (const Spline& s : in(n, ("side" + std::to_string(k)).c_str()))
            all.push_back(s);
    if (all.size() < 4)
        return meshValue(geo::Mesh());
    // Order and orient the sides so each starts where the last ended.
    sides[0] = all[0];
    std::vector<bool> used(all.size(), false);
    used[0] = true;
    for (int k = 1; k < 4; ++k)
    {
        D3 end = pointAt(sides[size_t(k) - 1], spanCount(sides[size_t(k) - 1]));
        double best = 1e300;
        size_t bi = 0;
        bool rev = false;
        for (size_t c = 0; c < all.size(); ++c)
        {
            if (used[c])
                continue;
            double ds = glm::distance(pointAt(all[c], 0.0), end), de = glm::distance(pointAt(all[c], spanCount(all[c])), end);
            if (std::min(ds, de) < best)
            {
                best = std::min(ds, de);
                bi = c;
                rev = de < ds;
            }
        }
        used[bi] = true;
        sides[size_t(k)] = all[bi];
        if (rev)
            reverse(sides[size_t(k)]);
    }
    return meshValue(coonsPatch(sides, int(std::max<int64_t>(1, i(n, "spansU"))), int(std::max<int64_t>(1, i(n, "spansV")))));
}

PlugValue filletRun(const Node& n)
{
    FilletOptions o;
    o.mode = CornerMode(std::clamp<int64_t>(i(n, "mode"), 0, 2));
    o.radius = std::max(0.0, f(n, "radius"));
    o.perPoint = b(n, "perPoint");
    o.exactArcs = b(n, "exactArcs");
    Splines out;
    for (const Spline& s : in(n, "inCurve"))
        out.push_back(fillet(s, o));
    return makeCurveData(std::move(out));
}

PlugValue offsetRun(const Node& n)
{
    OffsetOptions o;
    o.distance = f(n, "distance");
    o.join = JoinType(std::clamp<int64_t>(i(n, "join"), 0, 2));
    o.miterLimit = std::max(1.0, f(n, "miterLimit"));
    o.normal = v(n, "normal");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    Splines out;
    for (const Spline& s : in(n, "inCurve"))
        for (Spline& r : offset(s, o))
            out.push_back(std::move(r));
    return makeCurveData(std::move(out));
}

PlugValue rebuildRun(const Node& n)
{
    RebuildOptions o;
    o.kind = Kind(std::clamp<int64_t>(i(n, "kind"), 0, 2));
    o.spans = int(std::max<int64_t>(1, i(n, "spans")));
    o.degree = int(std::clamp<int64_t>(i(n, "degree"), 1, 7));
    o.keepCorners = b(n, "keepCorners");
    o.keepPointCount = b(n, "keepPointCount");
    o.pointPercent = std::max(1.0, f(n, "pointPercent"));
    Splines out;
    for (const Spline& s : in(n, "inCurve"))
        out.push_back(rebuild(s, o));
    return makeCurveData(std::move(out));
}

PlugValue fitRun(const Node& n)
{
    double tol = std::max(1e-5, f(n, "tolerance"));
    Splines out;
    for (const Spline& s : in(n, "inCurve"))
        out.push_back(simplify(s, tol));
    return makeCurveData(std::move(out));
}

PlugValue projectRun(const Node& n)
{
    MeshPtr mesh = n.as<MeshPtr>(n.type().attrIndex("inMesh"));
    if (!mesh || mesh->numFaces() == 0)
        return makeCurveData({});
    geo::Mesh world = *mesh;
    glm::dmat4 m = n.as<glm::dmat4>(n.type().attrIndex("meshMatrix"));
    for (geo::Index p = 0; p < world.pointCapacity(); ++p)
        if (world.pointValid(p))
            world.setPosition(p, D3(m * glm::dvec4(world.position(p), 1.0)));
    geo::MeshBvh bvh(world);
    ProjectOptions o;
    o.direction = v(n, "direction");
    o.offset = f(n, "offset");
    o.tolerance = std::max(1e-5, f(n, "tolerance"));
    Splines out;
    for (const Spline& s : in(n, "inCurve"))
        for (Spline& r : project(s, world, bvh, o))
            out.push_back(std::move(r));
    return makeCurveData(std::move(out));
}

AttrDef curveIn() { return dataIn("inCurve", "The curves it works on."); }

void registerHistoryTypes()
{
    std::vector<AttrDef> sweep = {dataIn("path", "Path curves, world space."), dataIn("profile", "Profile curves (profileShape auto)."),
                                  eAttr("profileShape", {"auto", "circle", "rectangle"}, 0,
                                        "auto sweeps the profile curves when there are any, else a circle."),
                                  fAttr("radius", 5.0, "Circle radius, cm.", 0.0), iAttr("sides", 8, "Circle sides.", 3),
                                  fAttr("width", 10.0, "Rectangle width, cm.", 0.0), fAttr("height", 5.0, "Rectangle height, cm.", 0.0),
                                  bAttr("alignProfile", true,
                                        "Turn a profile curve to face along the path and centre it on the path start; off sweeps it from where it lies."),
                                  fAttr("twist", 0.0, "Twist from start to end, degrees."), fAttr("scaleStart", 1.0, "Profile scale at the start.", 0.0),
                                  fAttr("scaleEnd", 1.0, "Profile scale at the end.", 0.0),
                                  eAttr("startCap", {"none", "ngon", "grid", "round"}, 0, "Close the start: one face, a quad grid, or a rounded dome."),
                                  eAttr("endCap", {"none", "ngon", "grid", "round"}, 0, "Close the end: one face, a quad grid, or a rounded dome."),
                                  iAttr("roundSegments", 4, "Rings in a rounded cap.", 1),
                                  eAttr("frameMode", {"rmf", "fixedUp", "surfaceNormal", "frenet"}, 0,
                                        "How the profile turns along the path: rotation minimising (no twist), toward the up vector, or Frenet."),
                                  vAttr("upVector", D3(0, 1, 0), "Up for fixedUp frames, and the start orientation of others."),
                                  bAttr("miter", true, "At corners, set the profile in the bisector plane so walls keep their width."),
                                  fAttr("splitEvery", 0.0, "Split the body into parts (own UV island and part id) every this many cm; 0 is none.", 0.0),
                                  bAttr("flip", false, "Flip the faces.")};
    for (AttrDef& a : stripUvAttrs({}, true))
        sweep.push_back(std::move(a));
    for (AttrDef& a : tessAttrs(2))
        sweep.push_back(a);
    sweep.push_back(meshOut());
    registerOp("curveSweep", "route", "Sweeps a profile (a curve, circle or rectangle) along path curves into quads, with twist, scale, caps and UVs.",
               sweep, sweepRun);

    std::vector<AttrDef> rev = {dataIn("profile", "Profile curves, world space."), vAttr("axisOrigin", D3(0.0), "A point on the axis."),
                                vAttr("axis", D3(0, 1, 0), "Axis direction."), fAttr("startSweep", 0.0, "Start angle, degrees."),
                                fAttr("endSweep", 360.0, "End angle, degrees."), iAttr("sections", 16, "Segments around the axis.", 1),
                                bAttr("caps", true, "Close the ends of a partial sweep of a closed profile."), bAttr("flip", false, "Flip the faces."),
                                AttrDef{.name = "material", .type = PlugType::String, .defaultValue = std::string(), .flags = kIn,
                                        .doc = "The material every face uses, by name; empty is the default."}};
    for (AttrDef& a : tessAttrs(1))
        rev.push_back(a);
    rev.push_back(meshOut());
    registerOp("curveRevolve", "rotate-360", "Revolves profile curves around an axis into quads; points on the axis become poles.", rev, [](const Node& n) {
        return revolveRun(n);
    });

    std::vector<AttrDef> lo;
    lo.push_back(dataArray("sections", "Section curves, world space, lofted in element order.", "section"));
    lo.push_back(dataIn("rail0", "First rail: in-between sections follow it.", false));
    lo.push_back(dataIn("rail1", "Second rail: sections also scale and turn to meet it.", false));
    for (AttrDef a : {iAttr("samples", 0, "Points across each section; 0 matches the sections, corners lined up."),
                      iAttr("spansBetween", 1, "Segments between consecutive sections.", 1), bAttr("smooth", true, "Curve through the sections rather than straight."),
                      bAttr("closeLoft", false, "Join the last section back to the first."), bAttr("caps", false, "Cap the ends of closed sections."),
                      bAttr("autoAlign", true, "Line closed sections up by turning and reversing them."), bAttr("flip", false, "Flip the faces.")})
        lo.push_back(a);
    for (AttrDef& a : tessAttrs(1))
        lo.push_back(a);
    lo.push_back(meshOut());
    registerOp("curveLoft", "stack-3", "Lofts quads through section curves, optionally along one or two rails.", lo, loftRun);

    std::vector<AttrDef> pa;
    for (int k = 0; k < 4; ++k)
        pa.push_back(dataIn("side" + std::to_string(k), "A boundary curve.", false));
    pa.push_back(iAttr("spansU", 8, "Quads along the first side.", 1));
    pa.push_back(iAttr("spansV", 8, "Quads along the second side.", 1));
    pa.push_back(meshOut());
    registerOp("curvePatch", "border-all", "A Coons patch of quads over four boundary curves.", pa, patchRun);

    registerOp("filletCurve", "vector-bezier-arc", "Rounds, chamfers or blends curve corners.",
               {curveIn(), eAttr("mode", {"arc", "chamfer", "g2"}, 0, "Circular arc, straight cut, or a curvature-continuous blend."),
                fAttr("radius", 1.0, "Radius for corners without their own, cm.", 0.0), bAttr("perPoint", true, "Use each point's cornerRadius where set."),
                bAttr("exactArcs", true, "Exact rational arcs; off keeps Beziers Bezier with cubic arcs."), curveOut()},
               filletRun);
    registerOp("offsetCurve", "ripple", "Offsets curves within their plane, trimming the loops it makes.",
               {curveIn(), fAttr("distance", 1.0, "cm; positive is outward (closed) or to the left (open)."),
                eAttr("join", {"round", "miter", "square"}, 0, "How corners on the outside join."), fAttr("miterLimit", 4.0, "Longest miter, in distances.", 1.0),
                vAttr("normal", D3(0.0), "Offset plane normal; zero uses the curve's own plane."), fAttr("tolerance", 0.01, "Fitting tolerance, cm.", 1e-5),
                curveOut()},
               offsetRun);
    registerOp("rebuildCurve", "refresh", "Rebuilds curves with an even parameterisation and a given span count, keeping corners.",
               {curveIn(), eAttr("kind", {"poly", "bezier", "nurbs"}, 2, "What it rebuilds them as."), iAttr("spans", 8, "Spans.", 1),
                iAttr("degree", 3, "NURBS degree.", 1, 7.0), bAttr("keepCorners", true, "Keep tangent breaks sharp."),
                bAttr("keepPointCount", false,
                      "Even spacing: keep the kind and point count and spread the points evenly along the curve (kind, spans and degree are ignored)."),
                fAttr("pointPercent", 100.0, "With keepPointCount: the point count as a percentage of the curve's own, so 50 halves it.", 1.0),
                curveOut()},
               rebuildRun);
    registerOp("fitCurve", "scribble", "Refits curves as Beziers with as few points as the tolerance allows, corners kept.",
               {curveIn(), fAttr("tolerance", 0.05, "Largest distance from the original, cm.", 1e-5), curveOut()}, fitRun);
    registerOp("projectCurve", "arrow-bar-to-down", "Projects curves onto a mesh, along a direction or to the closest point, kept live.",
               {curveIn(),
                {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden, .doc = "The surface."},
                {.name = "meshMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
                 .doc = "The surface's world matrix."},
                vAttr("direction", D3(0.0), "Projection direction; zero projects to the closest surface point."),
                fAttr("offset", 0.0, "Lift off the surface, cm."), fAttr("tolerance", 0.01, "Sampling and fitting tolerance, cm.", 1e-5), curveOut()},
               projectRun);
}

PlugRef curveEditPlug(Graph& g, NodeId id)
{
    PlugRef create{id, CurveShape::kCreate, -1};
    std::optional<PlugRef> src = g.source(create);
    if (!src)
        return create;
    // History feeds the curve: freeze what it makes, like editing a generated mesh.
    g.evaluate();
    PlugValue now = g.value(create);
    g.disconnect(create);
    g.setValue(create, now);
    if (Node* up = g.node(src->node); up && !up->type().dag && g.destinations(*src).empty())
        g.deleteNode(up->id());
    return create;
}

void registerProvider()
{
    components::registerPointProvider({
        .nodeType = "curve",
        .points = [](Graph& g, NodeId id) { return componentPositions(shapeSplines(g, id)); },
        .setPoints =
            [](Graph& g, NodeId id, const std::vector<std::pair<uint32_t, glm::dvec3>>& moves) {
                // A point a control drives would snap back to it, so its control moves instead.
                const auto free = hasControls(*g.node(id)) ? moveDrivenComponents(g, *g.node(id), moves) : moves;
                if (free.empty())
                    return;
                PlugRef plug = curveEditPlug(g, id);
                Splines s = splinesOf(g.storedValue(plug));
                g.setValue(plug, makeCurveData(sketchMoved(*g.node(id), s, free)));
            },
        .editPlug = curveEditPlug,
        .frames = [](Graph& g, NodeId id, const components::Ids& ids) { return componentFrames(shapeSplines(g, id), ids); },
        .bake =
            [](Graph& g, NodeId id, const glm::dmat4& m) {
                PlugRef plug = curveEditPlug(g, id);
                Splines s = splinesOf(g.storedValue(plug));
                for (Spline& sp : s)
                    transform(sp, m);
                g.setValue(plug, makeCurveData(std::move(s)));
            },
        // Edit points are refitted through, so moving them along with the control points would fight it.
        .soft = [](uint32_t id) { return PointRole(id & 3) != PointRole::Edit; },
    });
    conformTargets::add("curve", [](Graph& g, Node& n) -> std::optional<geo::Mesh> {
        g.evaluate();
        TessOptions o;
        o.chordError = 0.005;
        o.maxAngle = 0.05;
        geo::Mesh m;
        for (const Spline& s : splinesOf(n.value(CurveShape::kWorldSpace)))
        {
            Polyline pl = tessellate(s, o);
            std::vector<geo::Index> pts;
            for (const D3& p : pl.points)
                pts.push_back(m.addPoint(p));
            size_t segs = pl.closed ? pts.size() : (pts.empty() ? 0 : pts.size() - 1);
            for (size_t k = 0; k < segs; ++k)
                m.addEdge(pts[k], pts[(k + 1) % pts.size()]);
        }
        return m;
    });
}
} // namespace

void registerTypes()
{
    if (types::find("curve"))
        return;
    data::registerKind("curve", &CurveData::fromJson);
    NodeType t;
    t.name = "curve";
    t.icon = "vector-spline";
    t.base = types::find("transform");
    t.dag = true;
    t.doc = "A curve: poly, Bezier or NURBS splines. Sweep, revolve and loft it into meshes; edit its points and handles in vertex mode.";
    t.ownAttrs = {
        dataIn("create", "The splines, object space, or the output of a curve history node."),
        dataOut("local", "The splines, object space."),
        dataOut("worldSpace", "The splines in world space, as history nodes read them."),
        bAttr("displayHull", true, "Show control polygons and handles while editing."),
        bAttr("displayPoints", true, "Show points while editing."),
        bAttr("displayComb", false, "Show the curvature comb."),
        fAttr("combScale", 20.0, "Comb tooth length per unit curvature, cm.", 0.0),
        iAttr("combSamples", 12, "Comb teeth per span.", 1),
    };
    for (AttrDef a : tessAttrs(1))
    {
        if (a.name == "segments" || a.name == "tessellation")
        {
            a.name = a.name == "segments" ? "displaySegments" : "displayTessellation";
            a.doc = "Viewport drawing: " + a.doc;
            t.ownAttrs.push_back(a);
        }
    }
    t.ownAttrs.push_back({.name = "liveSurface", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden,
                          .doc = "A mesh the curve's points stay on (conformCurve): they are placed on it however they are moved."});
    t.ownAttrs.push_back({.name = "liveSurfaceMatrix", .type = PlugType::Matrix, .defaultValue = glm::dmat4(1.0), .flags = AttrInput | AttrHidden,
                          .doc = "The live surface's world matrix."});
    t.ownAttrs.push_back(fAttr("surfaceOffset", 0.0, "Lift off the live surface along its normal, cm."));
    // After the attrs CurveShape::Attr indexes; found by name.
    t.ownAttrs.push_back({.name = "sketch", .type = PlugType::String, .defaultValue = std::string(), .flags = kIn | AttrHidden,
                          .doc = "Sketch constraints and dimensions between its points, lines and circles, as JSON (see sketchInfo)."});
    t.ownAttrs.push_back({.name = "groomSurface", .type = PlugType::String, .defaultValue = std::string(), .flags = kIn,
                          .doc = "Mesh the curves' roots grow from: curve sculpting keeps them on it and plants new ones there."});
    t.ownAttrs.push_back(eAttr("symmetry", {"off", "x", "y", "z"}, 0,
                               "Mirror the curve across the plane facing this axis: the copy follows every edit, and history built from the "
                               "curve (pipes, sweeps) gets both sides."));
    t.ownAttrs.push_back(eAttr("symmetrySpace", {"world", "object"}, 0, "Mirror across the world plane through the origin, or the curve's own through its pivot."));
    addControlAttrs(t);
    t.create = [] { return std::make_unique<CurveShape>(); };
    types::registerType(std::move(t));
    registerHistoryTypes();
    registerPipeType();
    registerPipeToolTypes();
    registerPlanarTypes();
    registerEdgeToCurveType();
    registerAlongTypes();
    registerContourType();
    registerCableType();
    registerStitchType();
    registerStrapTypes();
    registerVehicleTypes();
    registerProvider();
}
} // namespace lila::curve
