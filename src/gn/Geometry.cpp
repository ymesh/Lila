#include "gn/Geometry.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "gn/Volume.h"

#include <tbb/parallel_for.h>

#include <array>
#include <cmath>
#include <mutex>
#include <optional>

namespace lila::gn
{
namespace
{
constexpr std::array<const char*, 11> kSocketNames = {"geometry", "float", "int", "bool", "vector", "rotation", "string", "color", "matrix", "bundle", "closure"};
constexpr std::array<const char*, 6> kDomainNames = {"point", "edge", "face", "corner", "curve", "instance"};

/**
 * Many instances of plain meshes joined once, untransformed, so realizing them again with new transforms (a simulation
 * playing) only moves points instead of rebuilding the topology. Kept while its references are alive.
 */
struct JoinedTopology
{
    std::vector<std::weak_ptr<const Geometry>> refs;
    std::vector<int> reference;
    std::shared_ptr<const geo::Mesh> rest;
    std::vector<geo::Index> pointStart, cornerStart;
};

std::mutex gJoinedMutex;
std::vector<std::shared_ptr<const JoinedTopology>> gJoined;

bool sameTopology(const JoinedTopology& t, const Instances& inst)
{
    if (t.reference != inst.reference || t.refs.size() != inst.references.size())
        return false;
    for (size_t r = 0; r < t.refs.size(); ++r)
        if (t.refs[r].expired() || t.refs[r].lock() != inst.references[r])
            return false;
    return true;
}

std::shared_ptr<const JoinedTopology> joinedTopology(const Instances& inst)
{
    {
        std::lock_guard lock(gJoinedMutex);
        for (const auto& t : gJoined)
            if (sameTopology(*t, inst))
                return t;
    }
    auto t = std::make_shared<JoinedTopology>();
    t->reference = inst.reference;
    for (const GeometryPtr& r : inst.references)
        t->refs.push_back(r);
    // The first mesh copied and the rest appended, as join() builds it, so both paths give the same mesh.
    geo::Mesh rest;
    for (size_t k = 0; k < inst.size(); ++k)
    {
        t->pointStart.push_back(rest.pointCapacity());
        t->cornerStart.push_back(rest.cornerCapacity());
        const geo::Mesh& piece = *inst.references[size_t(inst.reference[k])]->mesh;
        if (k == 0)
            rest = piece;
        else
            rest.append(piece);
    }
    t->pointStart.push_back(rest.pointCapacity());
    t->cornerStart.push_back(rest.cornerCapacity());
    t->rest = std::make_shared<const geo::Mesh>(std::move(rest));
    std::lock_guard lock(gJoinedMutex);
    std::erase_if(gJoined, [](const auto& j) { return std::any_of(j->refs.begin(), j->refs.end(), [](const auto& r) { return r.expired(); }); });
    if (gJoined.size() >= 4)
        gJoined.erase(gJoined.begin());
    gJoined.push_back(t);
    return t;
}

/** Instances of compact meshes only, none mirrored: what joinedTopology can place by moving points alone. */
bool joinable(const Geometry& g)
{
    if (g.mesh || g.points || g.curves || g.heightfield || g.volume || !g.instances || g.instances->size() < 2)
        return false;
    const Instances& inst = *g.instances;
    for (const GeometryPtr& r : inst.references)
        if (!r || !r->mesh || r->points || r->curves || r->instances || r->heightfield || r->volume || r->mesh->hasDeleted())
            return false;
    for (size_t k = 0; k < inst.size(); ++k)
        if (inst.reference[k] < 0 || size_t(inst.reference[k]) >= inst.references.size() || glm::determinant(glm::dmat3(inst.transforms[k])) <= 0.0)
            return false;
    return true;
}

Geometry realizeJoined(const Instances& inst)
{
    LILA_ZONE();

    std::shared_ptr<const JoinedTopology> t = joinedTopology(inst);
    geo::Mesh m = *t->rest;
    std::span<glm::dvec3> pos = m.positions();
    std::span<glm::vec3> normals = m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal);
    tbb::parallel_for(size_t(0), inst.size(), [&](size_t k) {
        const glm::dmat4& xf = inst.transforms[k];
        for (geo::Index p = t->pointStart[k]; p < t->pointStart[k + 1]; ++p)
            pos[p] = glm::dvec3(xf * glm::dvec4(pos[p], 1.0));
        if (normals.empty())
            return;
        const glm::mat3 nm = glm::mat3(glm::transpose(glm::inverse(glm::dmat3(xf))));
        for (geo::Index c = t->cornerStart[k]; c < t->cornerStart[k + 1]; ++c)
            normals[c] = glm::normalize(nm * normals[c]);
    });
    Geometry out;
    out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    return out;
}
} // namespace

const char* socketTypeName(SocketType t) { return kSocketNames[size_t(t)]; }

bool parseSocketType(std::string_view name, SocketType* out)
{
    for (size_t i = 0; i < kSocketNames.size(); ++i)
        if (name == kSocketNames[i])
        {
            *out = SocketType(i);
            return true;
        }
    return false;
}

bool isDataType(SocketType t)
{
    return t != SocketType::Geometry && t != SocketType::String && t != SocketType::Bundle && t != SocketType::Closure;
}

const char* domainName(Domain d) { return kDomainNames[size_t(d)]; }

bool parseDomain(std::string_view name, Domain* out)
{
    for (size_t i = 0; i < kDomainNames.size(); ++i)
        if (name == kDomainNames[i])
        {
            *out = Domain(i);
            return true;
        }
    return false;
}

void CurveSet::addCurve(std::span<const glm::dvec3> points, bool closed)
{
    positions.insert(positions.end(), points.begin(), points.end());
    offsets.push_back(geo::Index(positions.size()));
    cyclic.push_back(closed ? 1 : 0);
    if (!knots.empty())
        knots.resize(cyclic.size());
    syncSizes();
}

namespace
{
constexpr std::string_view kSplinePointAttrs[] = {"handle_left", "handle_right", "handle_type_left", "handle_type_right", "weight"};
constexpr std::string_view kSplineCurveAttrs[] = {"curve_type", "degree", "resolution"};

/** A float point layer, created with def on the points that existed before it. */
std::span<float> floatLayer(geo::AttributeSet& set, std::string_view name, float def)
{
    bool had = set.has(name);
    std::span<float> v = set.getOrAdd<float>(name);
    if (!had)
    {
        set.find(name)->setDefault(def);
        for (float& x : v)
            x = def;
    }
    return v;
}
} // namespace

void CurveSet::addSpline(const curve::Spline& sp)
{
    size_t p0 = positions.size();
    addCurve(sp.points, sp.closed);
    size_t c = numCurves() - 1, n = sp.size();
    if (sp.kind != curve::Kind::Poly || curveAttrs.has("curve_type"))
    {
        curveAttrs.getOrAdd<int32_t>("curve_type")[c] = int32_t(sp.kind);
        curveAttrs.getOrAdd<int32_t>("degree")[c] = sp.degree;
    }
    if (sp.kind == curve::Kind::Bezier && sp.handleL.size() == n)
    {
        auto hl = pointAttrs.getOrAdd<glm::vec3>("handle_left");
        auto hr = pointAttrs.getOrAdd<glm::vec3>("handle_right");
        auto tl = pointAttrs.getOrAdd<int32_t>("handle_type_left");
        auto tr = pointAttrs.getOrAdd<int32_t>("handle_type_right");
        for (size_t i = 0; i < n; ++i)
        {
            hl[p0 + i] = glm::vec3(sp.handleL[i]);
            hr[p0 + i] = glm::vec3(sp.handleR[i]);
            tl[p0 + i] = int32_t(sp.typeL[i]);
            tr[p0 + i] = int32_t(sp.typeR[i]);
        }
    }
    auto put = [&](const std::vector<double>& v, std::string_view name, float def) {
        if (v.size() != n)
            return;
        std::span<float> layer = floatLayer(pointAttrs, name, def);
        for (size_t i = 0; i < n; ++i)
            layer[p0 + i] = float(v[i]);
    };
    put(sp.weights, "weight", 1.0f);
    put(sp.tilt, "tilt", 0.0f);
    put(sp.radius, "radius", 1.0f);
    put(sp.cornerRadius, "cornerRadius", 0.0f);
    if (!sp.knots.empty())
    {
        knots.resize(numCurves());
        knots[c] = sp.knots;
    }
}

curve::Spline CurveSet::spline(size_t c) const
{
    curve::Spline s;
    s.closed = cyclic[c] != 0;
    size_t p0 = offsets[c], n = curveSize(c);
    s.points.assign(positions.begin() + ptrdiff_t(p0), positions.begin() + ptrdiff_t(p0 + n));
    if (auto t = curveAttrs.get<int32_t>("curve_type"); !t.empty())
        s.kind = curve::Kind(std::clamp(t[c], 0, 2));
    if (auto d = curveAttrs.get<int32_t>("degree"); !d.empty())
        s.degree = std::clamp(d[c], 1, 7);
    if (s.kind == curve::Kind::Nurbs && c < knots.size())
        s.knots = knots[c];
    auto read = [&](std::string_view name, std::vector<double>& dst) {
        if (auto v = pointAttrs.get<float>(name); !v.empty())
            for (size_t i = 0; i < n; ++i)
                dst.push_back(v[p0 + i]);
    };
    if (s.kind == curve::Kind::Bezier)
    {
        auto hl = pointAttrs.get<glm::vec3>("handle_left"), hr = pointAttrs.get<glm::vec3>("handle_right");
        auto tl = pointAttrs.get<int32_t>("handle_type_left"), tr = pointAttrs.get<int32_t>("handle_type_right");
        if (!hl.empty() && !hr.empty())
            for (size_t i = 0; i < n; ++i)
            {
                s.handleL.push_back(glm::dvec3(hl[p0 + i]));
                s.handleR.push_back(glm::dvec3(hr[p0 + i]));
                s.typeL.push_back(tl.empty() ? curve::HandleType::Free : curve::HandleType(std::clamp(tl[p0 + i], 0, 3)));
                s.typeR.push_back(tr.empty() ? curve::HandleType::Free : curve::HandleType(std::clamp(tr[p0 + i], 0, 3)));
            }
    }
    if (s.kind == curve::Kind::Nurbs)
        read("weight", s.weights);
    read("tilt", s.tilt);
    read("radius", s.radius);
    read("cornerRadius", s.cornerRadius);
    s.normalize();
    return s;
}

bool CurveSet::isPoly() const
{
    auto t = curveAttrs.get<int32_t>("curve_type");
    return t.empty() || std::ranges::all_of(t, [](int32_t k) { return k == 0; });
}

CurveSet CurveSet::evaluated(const curve::TessOptions& o) const
{
    if (isPoly())
        return *this;
    LILA_ZONE();

    CurveSet out;
    out.pointAttrs.mergeLayout(pointAttrs);
    out.curveAttrs.mergeLayout(curveAttrs);
    for (std::string_view name : kSplinePointAttrs)
        out.pointAttrs.remove(name);
    for (std::string_view name : kSplineCurveAttrs)
        out.curveAttrs.remove(name);
    for (size_t c = 0; c < numCurves(); ++c)
    {
        curve::Spline sp = spline(c);
        size_t p0 = offsets[c], n = curveSize(c);
        if (!curve::isValid(sp) || n == 0)
            continue;
        curve::TessOptions co = o;
        if (auto res = curveAttrs.get<int32_t>("resolution"); !res.empty() && res[c] > 0)
        {
            co.mode = curve::TessMode::Fixed;
            co.perSpan = res[c];
        }
        curve::Polyline pl = sp.kind == curve::Kind::Poly ? curve::polylineFrom(sp.points, sp.closed) : curve::tessellate(sp, co);
        size_t q0 = out.positions.size();
        out.addCurve(pl.points, sp.closed);
        out.curveAttrs.copyFrom(curveAttrs, out.numCurves() - 1, c);
        int spans = std::max(1, curve::spanCount(sp));
        for (size_t k = 0; k < pl.size(); ++k)
        {
            // Point values blend between the two control points around the sample.
            double x = sp.kind == curve::Kind::Nurbs ? pl.params[k] / spans * double(sp.closed ? n : n - 1) : pl.params[k];
            x = std::clamp(x, 0.0, double(sp.closed ? n : n - 1));
            size_t a = std::min(size_t(std::floor(x)), sp.closed ? n - 1 : (n > 1 ? n - 2 : 0));
            float t = float(x - double(a));
            geo::Index srcs[2] = {geo::Index(p0 + a), geo::Index(p0 + (a + 1) % n)};
            float w[2] = {1.0f - t, t};
            out.pointAttrs.mixFrom(pointAttrs, q0 + k, srcs, w);
        }
    }
    return out;
}

void CurveSet::syncSizes()
{
    pointAttrs.resize(positions.size());
    curveAttrs.resize(cyclic.size());
}

void CurveSet::append(const CurveSet& other)
{
    pointAttrs.mergeLayout(other.pointAttrs);
    curveAttrs.mergeLayout(other.curveAttrs);
    size_t p0 = positions.size(), c0 = cyclic.size();
    positions.insert(positions.end(), other.positions.begin(), other.positions.end());
    for (size_t c = 0; c < other.numCurves(); ++c)
        offsets.push_back(geo::Index(p0 + other.offsets[c + 1]));
    if (!knots.empty() || !other.knots.empty())
    {
        knots.resize(c0);
        for (size_t c = 0; c < other.numCurves(); ++c)
            knots.push_back(c < other.knots.size() ? other.knots[c] : std::vector<double>{});
    }
    cyclic.insert(cyclic.end(), other.cyclic.begin(), other.cyclic.end());
    syncSizes();
    for (size_t p = 0; p < other.numPoints(); ++p)
        pointAttrs.copyFrom(other.pointAttrs, p0 + p, p);
    for (size_t c = 0; c < other.numCurves(); ++c)
        curveAttrs.copyFrom(other.curveAttrs, c0 + c, c);
}

void Instances::add(const glm::dmat4& xf, int ref)
{
    transforms.push_back(xf);
    reference.push_back(ref);
    attrs.resize(transforms.size());
}

bool Geometry::empty() const
{
    return (!mesh || mesh->numPoints() == 0) && (!points || points->numPoints() == 0) && (!curves || curves->numPoints() == 0) &&
           (!instances || instances->size() == 0) && (!heightfield || heightfield->empty()) && (!volume || volume->empty());
}

GeometryPtr makeGeometry(Geometry g) { return std::make_shared<const Geometry>(std::move(g)); }

GeometryPtr meshGeometry(geo::Mesh m)
{
    m.compact();
    Geometry g;
    g.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    return makeGeometry(std::move(g));
}

GeometryPtr pointGeometry(geo::Mesh m)
{
    Geometry g;
    g.points = std::make_shared<const geo::Mesh>(std::move(m));
    return makeGeometry(std::move(g));
}

GeometryPtr curveGeometry(CurveSet c)
{
    Geometry g;
    g.curves = std::make_shared<const CurveSet>(std::move(c));
    return makeGeometry(std::move(g));
}

GeometryPtr heightfieldGeometry(geo::Heightfield h)
{
    Geometry g;
    g.heightfield = std::make_shared<const geo::Heightfield>(std::move(h));
    return makeGeometry(std::move(g));
}

namespace
{
/** A heightfield moved by a translation and a scale with equal X and Z stays a grid; anything else does not. */
bool keepsGrid(const glm::dmat4& xf)
{
    const glm::dmat3 m(xf);
    const double eps = 1e-12;
    for (int c = 0; c < 3; ++c)
        for (int r = 0; r < 3; ++r)
            if (c != r && std::abs(m[c][r]) > eps)
                return false;
    return m[0][0] > eps && m[1][1] > eps && std::abs(m[0][0] - m[2][2]) <= 1e-9 * m[0][0];
}

geo::Heightfield transformedGrid(const geo::Heightfield& h, const glm::dmat4& xf)
{
    geo::Heightfield out = h;
    const double s = xf[0][0], sy = xf[1][1], ty = xf[3][1];
    glm::dvec4 o = xf * glm::dvec4(h.origin.x, 0.0, h.origin.y, 1.0);
    out.origin = {o.x, o.z};
    out.cell = h.cell * s;
    for (float& v : out.heights)
        v = float(v * sy + ty);
    return out;
}
} // namespace

void transformMesh(geo::Mesh& m, const glm::dmat4& xf)
{
    LILA_ZONE();

    std::span<glm::dvec3> pos = m.positions();
    tbb::parallel_for(size_t(0), pos.size(), [&](size_t p) { pos[p] = glm::dvec3(xf * glm::dvec4(pos[p], 1.0)); });
    glm::mat3 nm = glm::mat3(glm::transpose(glm::inverse(glm::dmat3(xf))));
    for (auto& n : m.cornerAttrs().get<glm::vec3>(geo::attr::kNormal))
        n = glm::normalize(nm * n);
    if (glm::determinant(glm::dmat3(xf)) < 0.0)
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.flipFace(f);
}

void transformCurves(CurveSet& c, const glm::dmat4& xf)
{
    for (glm::dvec3& p : c.positions)
        p = glm::dvec3(xf * glm::dvec4(p, 1.0));
    for (const char* name : {"handle_left", "handle_right"})
        for (glm::vec3& h : c.pointAttrs.get<glm::vec3>(name))
            h = glm::vec3(xf * glm::dvec4(glm::dvec3(h), 1.0));
}

Geometry transformed(const Geometry& g, const glm::dmat4& xf)
{
    LILA_ZONE();

    Geometry out;
    out.name = g.name;
    if (g.mesh)
    {
        geo::Mesh m = *g.mesh;
        transformMesh(m, xf);
        out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g.points)
    {
        geo::Mesh m = *g.points;
        transformMesh(m, xf);
        out.points = std::make_shared<const geo::Mesh>(std::move(m));
    }
    if (g.curves)
    {
        CurveSet c = *g.curves;
        transformCurves(c, xf);
        out.curves = std::make_shared<const CurveSet>(std::move(c));
    }
    if (g.instances)
    {
        Instances i = *g.instances;
        for (glm::dmat4& m : i.transforms)
            m = xf * m;
        out.instances = std::make_shared<const Instances>(std::move(i));
    }
    if (g.heightfield)
    {
        if (keepsGrid(xf))
            out.heightfield = std::make_shared<const geo::Heightfield>(transformedGrid(*g.heightfield, xf));
        else
        {
            geo::Mesh m = geo::heightfieldToMesh(*g.heightfield);
            transformMesh(m, xf);
            if (out.mesh)
            {
                geo::Mesh joined = *out.mesh;
                joined.append(m);
                m = std::move(joined);
            }
            out.mesh = std::make_shared<const geo::Mesh>(std::move(m));
        }
    }
    if (g.volume)
    {
        Volume v = *g.volume;
        v.indexToWorld = xf * v.indexToWorld;
        if (v.isSdf())
        {
            const float s = float(std::cbrt(std::abs(glm::determinant(glm::dmat3(xf)))));
            for (float& x : v.values)
                x *= s;
            v.background *= s;
        }
        if (v.isVector())
        {
            const glm::mat3 linear{glm::dmat3(xf)};
            std::vector<glm::vec3> turned(v.vectors.size());
            for (size_t n = 0; n < turned.size(); ++n)
                turned[n] = linear * v.vectors[n];
            v.setVectors(std::move(turned));
        }
        out.volume = std::make_shared<const Volume>(std::move(v));
    }
    return out;
}

namespace
{
bool sameVolumeGrid(const Volume& a, const Volume& b) { return a.kind == b.kind && a.dims == b.dims && a.indexToWorld == b.indexToWorld; }
} // namespace

Geometry join(std::span<const GeometryPtr> parts)
{
    LILA_ZONE();

    Geometry out;
    geo::Mesh mesh, points;
    CurveSet curves;
    Instances inst;
    bool hasMesh = false, hasPoints = false, hasCurves = false, hasInst = false;
    std::optional<Volume> merged;
    for (const GeometryPtr& g : parts)
    {
        if (!g)
            continue;
        if (out.name.empty())
            out.name = g->name;
        if (g->volume)
        {
            if (!out.volume)
                out.volume = g->volume;
            else if (sameVolumeGrid(*out.volume, *g->volume))
            {
                if (!merged)
                    merged = *out.volume;
                if (merged->isVector())
                {
                    std::vector<glm::vec3> sum = merged->vectors;
                    for (size_t k = 0; k < sum.size() && k < g->volume->vectors.size(); ++k)
                        sum[k] += g->volume->vectors[k];
                    merged->setVectors(std::move(sum));
                }
                else
                {
                    const std::vector<float>& add = g->volume->values;
                    for (size_t k = 0; k < merged->values.size(); ++k)
                        merged->values[k] = merged->isSdf() ? std::min(merged->values[k], add[k]) : merged->values[k] + add[k];
                }
            }
        }
        if (g->mesh)
        {
            // A single mesh is shared, not copied.
            if (!hasMesh && !out.mesh)
                out.mesh = g->mesh;
            else
            {
                if (!hasMesh)
                {
                    mesh = *out.mesh;
                    hasMesh = true;
                }
                mesh.append(*g->mesh);
            }
        }
        if (g->points)
        {
            points.append(*g->points);
            hasPoints = true;
        }
        if (g->curves)
        {
            curves.append(*g->curves);
            hasCurves = true;
        }
        if (g->heightfield)
        {
            if (!out.heightfield)
                out.heightfield = g->heightfield;
            else
            {
                if (!hasMesh)
                {
                    if (out.mesh)
                        mesh = *out.mesh;
                    hasMesh = true;
                }
                mesh.append(geo::heightfieldToMesh(*g->heightfield));
            }
        }
        if (g->instances)
        {
            const Instances& o = *g->instances;
            inst.attrs.mergeLayout(o.attrs);
            int refBase = int(inst.references.size());
            inst.references.insert(inst.references.end(), o.references.begin(), o.references.end());
            size_t first = inst.size();
            for (size_t k = 0; k < o.size(); ++k)
                inst.add(o.transforms[k], o.reference[k] + refBase);
            for (size_t k = 0; k < o.size(); ++k)
                inst.attrs.copyFrom(o.attrs, first + k, k);
            hasInst = true;
        }
    }
    if (hasMesh)
    {
        mesh.compact();
        out.mesh = std::make_shared<const geo::Mesh>(std::move(mesh));
    }
    if (hasPoints)
        out.points = std::make_shared<const geo::Mesh>(std::move(points));
    if (hasCurves)
        out.curves = std::make_shared<const CurveSet>(std::move(curves));
    if (hasInst)
        out.instances = std::make_shared<const Instances>(std::move(inst));
    if (merged)
        out.volume = std::make_shared<const Volume>(std::move(*merged));
    return out;
}

Geometry realize(const Geometry& g)
{
    LILA_ZONE();

    if (!g.instances || g.instances->size() == 0)
    {
        Geometry out = g;
        out.instances = nullptr;
        return out;
    }
    if (joinable(g))
    {
        Geometry out = realizeJoined(*g.instances);
        out.name = g.name;
        return out;
    }
    const Instances& inst = *g.instances;
    // Each reference is realized once, then copied per instance.
    std::vector<Geometry> refs(inst.references.size());
    for (size_t r = 0; r < refs.size(); ++r)
        if (inst.references[r])
            refs[r] = realize(*inst.references[r]);

    std::vector<GeometryPtr> parts;
    parts.reserve(inst.size() + 1);
    Geometry self = g;
    self.instances = nullptr;
    parts.push_back(makeGeometry(self));
    std::vector<GeometryPtr> placed(inst.size());
    tbb::parallel_for(size_t(0), inst.size(), [&](size_t k) {
        int r = inst.reference[k];
        if (r >= 0 && size_t(r) < refs.size())
            placed[k] = makeGeometry(transformed(refs[r], inst.transforms[k]));
    });
    parts.insert(parts.end(), placed.begin(), placed.end());
    return join(parts);
}

geo::Mesh pointsOnly(const geo::Mesh& m)
{
    geo::Mesh out;
    out.pointAttrs().mergeLayout(m.pointAttrs());
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            geo::Index np = out.addPoint(m.position(p));
            out.pointAttrs().copyFrom(m.pointAttrs(), np, p);
        }
    return out;
}

void stripAnonymous(geo::Mesh& m)
{
    for (size_t d = 0; d < size_t(geo::Domain::Count); ++d)
    {
        geo::AttributeSet& set = m.attributes(geo::Domain(d));
        for (const std::string& n : set.names())
            if (n.starts_with("__"))
                set.remove(n);
    }
}

geo::Mesh toOutputMesh(const Geometry& g)
{
    LILA_ZONE();

    Geometry r = realize(g);
    geo::Mesh m = r.mesh ? *r.mesh : geo::Mesh();
    if (r.heightfield && !r.heightfield->empty())
    {
        // Past this a terrain is drawn, not edited, as a mesh: Heightfield to Mesh gives the full grid when that's wanted.
        constexpr size_t kMaxPoints = size_t(4) << 20;
        const geo::Heightfield& h = *r.heightfield;
        geo::HeightfieldMeshOptions o;
        while (size_t((h.nx + o.step - 1) / o.step) * size_t((h.nz + o.step - 1) / o.step) > kMaxPoints)
            ++o.step;
        m.append(geo::heightfieldToMesh(h, o));
    }
    stripAnonymous(m);
    m.compact();
    return m;
}
} // namespace lila::gn
