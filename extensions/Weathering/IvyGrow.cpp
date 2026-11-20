#include "IvyGrow.h"

#include "Pieces.h"

#include <lila/GeometryNodes.h>
#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>

namespace weathering::ivy
{
namespace
{
namespace geo = lila::geo;
namespace gn = lila::gn;
using D3 = glm::dvec3;

constexpr D3 kUp{0.0, 1.0, 0.0};
constexpr double kTau = 2.0 * std::numbers::pi;

double ivyHash(uint32_t seed, uint32_t a, uint32_t b = 0) { return unitHash(seed ^ 0x1F7A3C5Du, a, b); }

D3 safeNormalize(const D3& v, const D3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

/** Any unit vector at right angles to n. */
D3 anyPerpendicular(const D3& n) { return safeNormalize(glm::cross(n, std::abs(n.y) < 0.9 ? kUp : D3(1, 0, 0)), D3(1, 0, 0)); }

struct Grid
{
    double cell = 1.0;
    std::unordered_map<uint64_t, std::vector<uint32_t>> cells;

    static uint64_t key(int64_t x, int64_t y, int64_t z)
    {
        return (uint64_t(x & 0x1FFFFF) << 42) | (uint64_t(y & 0x1FFFFF) << 21) | uint64_t(z & 0x1FFFFF);
    }
    glm::i64vec3 at(const D3& p) const { return {int64_t(std::floor(p.x / cell)), int64_t(std::floor(p.y / cell)), int64_t(std::floor(p.z / cell))}; }
    void add(const D3& p, uint32_t i)
    {
        const auto c = at(p);
        cells[key(c.x, c.y, c.z)].push_back(i);
    }
    template <class F> void around(const D3& p, F&& f) const
    {
        const auto c = at(p);
        for (int64_t x = c.x - 1; x <= c.x + 1; ++x)
            for (int64_t y = c.y - 1; y <= c.y + 1; ++y)
                for (int64_t z = c.z - 1; z <= c.z + 1; ++z)
                    if (auto it = cells.find(key(x, y, z)); it != cells.end())
                        for (uint32_t i : it->second)
                            f(i);
    }
};

/** The surface with its tree, for closest points and rays. */
struct Surface
{
    const geo::Mesh& mesh;
    geo::MeshBvh bvh;
    double offset = 0.5; // where clinging stems sit off the surface

    explicit Surface(const geo::Mesh& m) : mesh(m), bvh(m) {}

    /** Moves p onto the surface plus the offset; n gets the surface normal. False when nothing is within reach. */
    bool snap(D3& p, D3& n, double reach = std::numeric_limits<double>::max()) const
    {
        geo::BvhClosest c;
        if (!bvh.closestPoint(p, c, reach) || c.face == geo::kInvalid)
            return false;
        const D3 fn = mesh.faceNormal(c.face);
        const D3 away = p - c.point;
        const double d = glm::length(away);
        // Off a corner the direction to the point is the smooth normal; inside the mesh, the face's.
        n = d > offset * 0.25 && glm::dot(away, fn) > 0.0 ? away / d : fn;
        p = c.point + n * offset;
        return true;
    }

    double distance(const D3& p, double reach = std::numeric_limits<double>::max()) const
    {
        geo::BvhClosest c;
        return bvh.closestPoint(p, c, reach) ? c.distance : std::numeric_limits<double>::max();
    }
};

struct Sample
{
    D3 p;
    D3 n;
};

/** Area-weighted points on the given triangles, about one per spacing squared. */
std::vector<Sample> scatterOnTriangles(const Surface& s, std::span<const geo::Index> tris, double spacing, uint32_t seed, size_t cap)
{
    const geo::Triangulation& t = s.bvh.triangles();
    double area = 0.0;
    for (geo::Index k : tris)
    {
        D3 a, b, c;
        s.bvh.trianglePoints(k, a, b, c);
        area += 0.5 * glm::length(glm::cross(b - a, c - a));
    }
    double cellArea = spacing * spacing;
    if (area / cellArea > double(cap))
        cellArea = area / double(cap);
    std::vector<Sample> out;
    for (geo::Index k : tris)
    {
        D3 a, b, c;
        s.bvh.trianglePoints(k, a, b, c);
        const D3 cr = glm::cross(b - a, c - a);
        const double triArea = 0.5 * glm::length(cr);
        if (triArea < 1e-12)
            continue;
        const D3 n = s.mesh.faceNormal(t.faces[k]);
        const double expected = triArea / cellArea;
        int count = int(expected);
        if (ivyHash(seed, k, 1) < expected - count)
            ++count;
        for (int i = 0; i < count; ++i)
        {
            double u = ivyHash(seed, k, 100 + uint32_t(i) * 2), v = ivyHash(seed, k, 101 + uint32_t(i) * 2);
            if (u + v > 1.0)
                u = 1.0 - u, v = 1.0 - v;
            out.push_back({a + (b - a) * u + (c - a) * v, n});
        }
    }
    return out;
}

std::vector<geo::Index> allTriangles(const Surface& s)
{
    std::vector<geo::Index> all(s.bvh.triangleCount());
    for (geo::Index k = 0; k < all.size(); ++k)
        all[k] = k;
    return all;
}

double polyLength(std::span<const D3> pts)
{
    double l = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
        l += glm::distance(pts[k - 1], pts[k]);
    return l;
}

/** The polyline resampled every spacing (ends kept). */
std::vector<D3> resample(std::span<const D3> pts, double spacing)
{
    std::vector<D3> out;
    if (pts.empty())
        return out;
    out.push_back(pts.front());
    double carry = 0.0;
    for (size_t k = 1; k < pts.size(); ++k)
    {
        const D3 a = pts[k - 1], b = pts[k];
        const double seg = glm::distance(a, b);
        double at = spacing - carry;
        while (at < seg)
        {
            out.push_back(glm::mix(a, b, at / seg));
            at += spacing;
        }
        carry = seg - (at - spacing);
    }
    if (glm::distance(out.back(), pts.back()) > spacing * 0.25)
        out.push_back(pts.back());
    return out;
}

/** Runs the geometry-nodes Space Colonization node: branches from roots toward attractors. */
gn::CurveSet spaceColonize(std::span<const D3> roots, std::span<const D3> attractors, double step, double influence, double kill, int iterations,
                           double tipRadius)
{
    const gn::NodeDecl* d = gn::findDecl("SpaceColonization");
    if (!d || !d->exec)
        throw std::runtime_error("ivy: the Space Colonization geometry node is missing");
    auto points = [](std::span<const D3> pts) {
        geo::Mesh m;
        for (const D3& p : pts)
            m.addPoint(p);
        return gn::pointGeometry(std::move(m));
    };
    gn::Group group;
    gn::GNode node;
    node.name = "ivySpaceColonization";
    node.type = d->type;
    node.values = {{"step", step}, {"influence", influence}, {"killDistance", kill}, {"iterations", iterations}, {"tipRadius", tipRadius}, {"exponent", 2.2}};
    const gn::Sockets& sockets = d->sockets;
    gn::ExecEnv env;
    gn::ExecContext c(group, node, *d, sockets, env);
    c.inputs.resize(sockets.inputs.size());
    c.outputs.resize(sockets.outputs.size());
    for (size_t k = 0; k < sockets.inputs.size(); ++k)
    {
        const gn::SocketDecl& sd = sockets.inputs[k];
        gn::Value v;
        if (sd.id == "attractors")
            v.geometry = points(attractors);
        else if (sd.id == "roots")
            v.geometry = points(roots);
        else
            v = gn::valueFromJson(sd.type, node.values.contains(sd.id) ? node.values[sd.id] : sd.defaultValue);
        c.inputs[k].push_back(std::move(v));
    }
    for (size_t k = 0; k < sockets.outputs.size(); ++k)
        c.outputs[k] = gn::defaultValue(sockets.outputs[k].type);
    d->exec(c);
    const int out = sockets.outputIndex("curve");
    if (out < 0 || !c.outputs[size_t(out)].geometry || !c.outputs[size_t(out)].geometry->curves)
        return {};
    return *c.outputs[size_t(out)].geometry->curves;
}

uint64_t posKey(const D3& p)
{
    const double q = 1e-6;
    const auto h = [](double v) { return uint64_t(std::llround(v)); };
    return h(p.x / q) * 0x9E3779B97F4A7C15ull ^ h(p.y / q) * 0xC2B2AE3D27D4EB4Full ^ h(p.z / q) * 0x165667B19E3779F9ull;
}

struct Raw
{
    std::vector<D3> pts;
    std::vector<double> radius;
    int parent = -1;
    int joint = -1; // index in the parent
    int root = -1;  // which root it grew from
};

/** The colonized curves as strands that know their parent and joint. */
std::vector<Raw> strandsOf(const gn::CurveSet& cs, std::span<const D3> roots)
{
    std::unordered_map<uint64_t, std::pair<int, int>> where;
    std::unordered_map<uint64_t, int> rootOf;
    for (size_t r = 0; r < roots.size(); ++r)
        rootOf.emplace(posKey(roots[r]), int(r));
    const geo::Attribute* ra = cs.pointAttrs.find("radius");
    std::vector<Raw> out;
    for (size_t c = 0; c < cs.numCurves(); ++c)
    {
        const auto pts = cs.curvePoints(c);
        if (pts.size() < 2)
            continue;
        Raw s;
        s.pts.assign(pts.begin(), pts.end());
        for (size_t k = 0; k < pts.size(); ++k)
        {
            double r = 0.0;
            if (ra && ra->type() == geo::AttrType::Float)
                r = ra->values<float>()[cs.offsets[c] + k];
            s.radius.push_back(r);
        }
        if (auto it = where.find(posKey(pts[0])); it != where.end())
        {
            s.parent = it->second.first;
            s.joint = it->second.second;
            s.root = out[size_t(s.parent)].root;
        }
        else if (auto r = rootOf.find(posKey(pts[0])); r != rootOf.end())
            s.root = r->second;
        else
            s.root = 0;
        const int id = int(out.size());
        for (size_t k = 0; k < pts.size(); ++k)
            where.emplace(posKey(pts[k]), std::pair<int, int>{id, int(k)});
        out.push_back(std::move(s));
    }
    return out;
}

/** Smooth noise in about [-1, 1] along a strand. */
double wander(double t, double ph0, double ph1) { return 0.65 * std::sin(t * 1.0 + ph0) + 0.35 * std::sin(t * 2.7 + ph1); }

/** Catmull-Rom midpoints between every pair: originals stay at even indices. */
std::vector<D3> subdivide(const std::vector<D3>& p)
{
    if (p.size() < 2)
        return p;
    std::vector<D3> out;
    for (size_t k = 0; k + 1 < p.size(); ++k)
    {
        const D3 p0 = p[k == 0 ? 0 : k - 1], p1 = p[k], p2 = p[k + 1], p3 = p[std::min(k + 2, p.size() - 1)];
        out.push_back(p1);
        out.push_back(0.5625 * (p1 + p2) - 0.0625 * (p0 + p3));
    }
    out.push_back(p.back());
    return out;
}

/** A piece moved into a unit frame: fitted by its bounds, its stem end at the origin. */
geo::Mesh fitPiece(const geo::Mesh& m, bool stretch)
{
    geo::Mesh out = m;
    const geo::Bounds b = m.bounds();
    if (!b.valid())
        return out;
    const D3 size = glm::max(b.size(), D3(1e-9));
    const double longest = std::max({size.x, size.y, size.z});
    for (geo::Index p = 0; p < out.pointCapacity(); ++p)
        if (out.pointValid(p))
        {
            D3 q = out.position(p);
            q = D3(q.x - (b.min.x + b.max.x) * 0.5, q.y - (b.min.y + b.max.y) * 0.5, q.z - b.min.z);
            out.setPosition(p, stretch ? q / size : q / longest);
        }
    return out;
}

struct PlacedPiece
{
    geo::Mesh mesh;
    double weight = 1.0;
};

std::vector<PlacedPiece> fitAll(const std::vector<Piece>& pieces, bool stretch)
{
    std::vector<PlacedPiece> out;
    for (const Piece& p : pieces)
        if (p.mesh.numFaces() > 0 && p.weight > 0.0)
            out.push_back({fitPiece(p.mesh, stretch), p.weight});
    return out;
}

size_t pickWeighted(const std::vector<PlacedPiece>& pieces, double u)
{
    double total = 0.0;
    for (const PlacedPiece& p : pieces)
        total += p.weight;
    double t = std::clamp(u, 0.0, 0.999999) * total;
    for (size_t k = 0; k < pieces.size(); ++k)
    {
        t -= pieces[k].weight;
        if (t < 0.0)
            return k;
    }
    return pieces.size() - 1;
}

/** Appends a placed copy of a piece; its own slots kept, bare faces given slot. Returns the first new point. */
geo::Index appendPiece(geo::Mesh& out, const geo::Mesh& piece, const glm::dmat4& xf, const char* slot)
{
    geo::Mesh m = piece;
    for (geo::Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.setPosition(p, D3(xf * glm::dvec4(m.position(p), 1.0)));
    if (m.materialNames().empty())
        lila::scene::materials::assign(m, slot, {});
    return out.append(m);
}

/** The point, normal and direction at a distance along a strand. */
struct AlongStrand
{
    D3 p, n, t;
    double radius = 0.0;
    bool hang = false;
};

AlongStrand alongStrand(const Strand& s, double g)
{
    const size_t n = s.points.size();
    size_t k = 1;
    while (k + 1 < n && s.growth[k] < g)
        ++k;
    const double g0 = s.growth[k - 1], g1 = s.growth[k];
    const double f = g1 > g0 ? std::clamp((g - g0) / (g1 - g0), 0.0, 1.0) : 0.0;
    AlongStrand a;
    a.p = glm::mix(s.points[k - 1], s.points[k], f);
    a.n = safeNormalize(glm::mix(s.normals[k - 1], s.normals[k], f), s.normals[k]);
    a.t = safeNormalize(s.points[k] - s.points[k - 1], kUp);
    a.radius = glm::mix(s.radius[k - 1], s.radius[k], f);
    a.hang = s.hang[k - 1] && s.hang[k];
    return a;
}

/** Farthest-point picks from candidates, the first one chosen by the seed. */
std::vector<D3> spreadPicks(const std::vector<D3>& cands, int count, uint32_t seed)
{
    std::vector<D3> out;
    if (cands.empty() || count <= 0)
        return out;
    std::vector<double> best(cands.size(), std::numeric_limits<double>::max());
    size_t pick = size_t(ivyHash(seed, 77) * double(cands.size())) % cands.size();
    for (int k = 0; k < count && k < int(cands.size()); ++k)
    {
        out.push_back(cands[pick]);
        size_t next = 0;
        double far = -1.0;
        for (size_t i = 0; i < cands.size(); ++i)
        {
            best[i] = std::min(best[i], glm::distance(cands[i], cands[pick]));
            if (best[i] > far)
            {
                far = best[i];
                next = i;
            }
        }
        if (far <= 1e-9)
            break;
        pick = next;
    }
    return out;
}

/** A leaf outline in the card layout: x across, z along from the stem end, y up from the face. */
geo::Mesh outlineCard(const std::vector<glm::dvec2>& outline, glm::dvec2 centre, double fold, double thickness)
{
    geo::Mesh m;
    double halfWidth = 1e-9, len = 1e-9;
    for (const glm::dvec2& q : outline)
    {
        halfWidth = std::max(halfWidth, std::abs(q.x));
        len = std::max(len, q.y);
    }
    auto lift = [&](const glm::dvec2& q) { return std::abs(q.x) * fold; };
    std::vector<geo::Index> top, bottom;
    const geo::Index c = m.addPoint(D3(centre.x, lift(centre) + thickness * 0.5, centre.y));
    for (const glm::dvec2& q : outline)
        top.push_back(m.addPoint(D3(q.x, lift(q) + thickness * 0.5, q.y)));
    std::vector<glm::vec2> uvs;
    std::vector<std::array<geo::Index, 3>> faces;
    const size_t n = outline.size();
    for (size_t k = 0; k < n; ++k)
        faces.push_back({c, top[(k + 1) % n], top[k]});
    geo::Index cb = geo::kInvalid;
    if (thickness > 0.0)
    {
        cb = m.addPoint(D3(centre.x, lift(centre) - thickness * 0.5, centre.y));
        for (const glm::dvec2& q : outline)
            bottom.push_back(m.addPoint(D3(q.x, lift(q) - thickness * 0.5, q.y)));
        for (size_t k = 0; k < n; ++k)
            faces.push_back({cb, bottom[k], bottom[(k + 1) % n]});
    }
    for (const auto& f : faces)
        m.addFace({f[0], f[1], f[2]});
    if (thickness > 0.0)
        for (size_t k = 0; k < n; ++k)
            m.addFace({top[k], top[(k + 1) % n], bottom[(k + 1) % n], bottom[k]});
    // Wound so the top faces +Y.
    if (m.faceNormal(0).y < 0.0)
        for (geo::Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.flipFace(f);
    auto uv = m.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
    for (geo::Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            for (geo::Index cc : m.faceCorners(f))
            {
                const D3 q = m.position(m.cornerPoint(cc));
                uv[cc] = glm::vec2(float(q.x / (2.0 * halfWidth) + 0.5), float(q.z / len));
            }
    return m;
}
} // namespace

geo::Mesh leafCard(bool toon, int variant)
{
    std::vector<glm::dvec2> half;
    if (toon)
        // A plump teardrop: few points, a clean silhouette.
        half = variant == 0 ? std::vector<glm::dvec2>{{0.0, 0.0}, {0.22, 0.06}, {0.4, 0.24}, {0.44, 0.46}, {0.36, 0.7}, {0.18, 0.9}}
                            : std::vector<glm::dvec2>{{0.0, 0.0}, {0.3, 0.1}, {0.48, 0.32}, {0.46, 0.56}, {0.3, 0.78}, {0.12, 0.94}};
    else if (variant == 0)
        // Five-lobed ivy.
        half = {{0.0, 0.08}, {0.18, 0.0}, {0.42, 0.08}, {0.52, 0.22}, {0.36, 0.32}, {0.42, 0.5}, {0.22, 0.58}};
    else
        // Broad heart, for vines.
        half = {{0.0, 0.1}, {0.2, 0.0}, {0.4, 0.06}, {0.5, 0.24}, {0.46, 0.46}, {0.32, 0.66}, {0.14, 0.84}};
    std::vector<glm::dvec2> outline;
    for (const glm::dvec2& q : half)
        outline.push_back(q);
    outline.push_back({0.0, 1.0});
    for (auto it = half.rbegin(); it != half.rend(); ++it)
        if (it->x > 1e-9)
            outline.push_back({-it->x, it->y});
    const double size = toon ? 12.0 : 10.0;
    for (glm::dvec2& q : outline)
        q *= size;
    return outlineCard(outline, glm::dvec2(0.0, size * 0.42), toon ? 0.08 : 0.18, toon ? size * 0.08 : 0.0);
}

geo::Mesh flowerCard(bool toon)
{
    std::vector<glm::dvec2> outline;
    const int petals = 5;
    const int per = toon ? 3 : 6;
    const double size = 4.0;
    for (int k = 0; k < petals * per; ++k)
    {
        const double a = kTau * double(k) / double(petals * per);
        const double r = size * 0.5 * (toon ? 0.7 + 0.3 * std::abs(std::cos(a * petals * 0.5)) : 0.45 + 0.55 * std::pow(std::abs(std::cos(a * petals * 0.5)), 0.7));
        outline.push_back({std::sin(a) * r, size * 0.5 + std::cos(a) * r});
    }
    return outlineCard(outline, glm::dvec2(0.0, size * 0.5), 0.0, toon ? 0.4 : 0.0);
}

geo::Mesh mossTuft(bool toon)
{
    if (toon)
    {
        // A soft low dome.
        geo::Mesh m;
        const int ring = 8;
        std::vector<geo::Index> lo, hi;
        for (int k = 0; k < ring; ++k)
        {
            const double a = kTau * k / ring;
            lo.push_back(m.addPoint(D3(std::cos(a) * 3.0, 0.0, 3.0 + std::sin(a) * 3.0)));
            hi.push_back(m.addPoint(D3(std::cos(a) * 2.0, 1.6, 3.0 + std::sin(a) * 2.0)));
        }
        const geo::Index top = m.addPoint(D3(0.0, 2.2, 3.0));
        for (int k = 0; k < ring; ++k)
        {
            const int j = (k + 1) % ring;
            m.addFace({lo[size_t(k)], hi[size_t(k)], hi[size_t(j)], lo[size_t(j)]});
            m.addFace({hi[size_t(k)], top, hi[size_t(j)]});
        }
        if (m.faceNormal(0).y < -0.5 || glm::dot(m.faceNormal(0), m.faceCentroid(0) - D3(0, 1, 3)) < 0.0)
            for (geo::Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f))
                    m.flipFace(f);
        return m;
    }
    // A fan of fine blades.
    std::vector<glm::dvec2> outline;
    const int blades = 7;
    for (int k = 0; k < blades; ++k)
    {
        const double a = -0.9 + 1.8 * k / (blades - 1);
        outline.push_back({std::sin(a) * 3.0, std::cos(a) * 3.0});
        const double b = a + 0.9 / (blades - 1);
        if (k + 1 < blades)
            outline.push_back({std::sin(b) * 1.2, std::cos(b) * 1.2});
    }
    outline.insert(outline.begin(), glm::dvec2(0.0, 0.0));
    return outlineCard(outline, glm::dvec2(0.0, 0.6), 0.3, 0.0);
}

std::vector<D3> groundSeeds(const geo::Mesh& surface, int count, uint32_t seed, const double* groundY)
{
    LILA_ZONE();
    if (surface.numFaces() == 0 || count <= 0)
        return {};
    Surface s(surface);
    const geo::Bounds b = surface.bounds();
    const double floor = groundY ? *groundY : b.min.y;
    const double band = std::max(4.0, b.size().y * 0.04);
    std::vector<geo::Index> tris;
    const geo::Triangulation& t = s.bvh.triangles();
    for (geo::Index k = 0; k < s.bvh.triangleCount(); ++k)
    {
        D3 a, bb, c;
        s.bvh.trianglePoints(k, a, bb, c);
        // Faces that rise from the ground band, not the ground's own edges.
        if (std::min({a.y, bb.y, c.y}) > floor + band || std::max({a.y, bb.y, c.y}) < floor + band)
            continue;
        if (std::abs(surface.faceNormal(t.faces[k]).y) < 0.6)
            tris.push_back(k);
    }
    const double spacing = std::max(1.0, band * 0.5);
    std::vector<D3> cands;
    for (const Sample& sm : scatterOnTriangles(s, tris, spacing, seed + 11, 20000))
        if (sm.p.y <= floor + band && sm.p.y >= floor - band)
            cands.push_back(sm.p);
    return spreadPicks(cands, count, seed);
}

std::vector<D3> maskSeeds(const geo::Mesh& surface, std::span<const float> pointWeight, int count, uint32_t seed)
{
    LILA_ZONE();
    if (surface.numFaces() == 0 || count <= 0 || pointWeight.size() < surface.pointCapacity())
        return {};
    Surface s(surface);
    const geo::Triangulation& t = s.bvh.triangles();
    std::vector<geo::Index> tris;
    for (geo::Index k = 0; k < s.bvh.triangleCount(); ++k)
    {
        float w = 0.0f;
        for (int c = 0; c < 3; ++c)
            w = std::max(w, pointWeight[surface.cornerPoint(t.corners[size_t(k) * 3 + size_t(c)])]);
        if (w > 0.05f)
            tris.push_back(k);
    }
    const geo::Bounds b = surface.bounds();
    const double spacing = std::max(1.0, glm::length(b.size()) / 200.0);
    std::vector<D3> cands;
    uint32_t i = 0;
    for (const Sample& sm : scatterOnTriangles(s, tris, spacing, seed + 13, 20000))
    {
        geo::BvhClosest c;
        if (!s.bvh.closestPoint(sm.p, c, spacing))
            continue;
        const geo::Index* cn = &t.corners[size_t(c.triangle) * 3];
        const double wv = pointWeight[surface.cornerPoint(cn[0])] * c.bary.x + pointWeight[surface.cornerPoint(cn[1])] * c.bary.y +
                          pointWeight[surface.cornerPoint(cn[2])] * c.bary.z;
        if (ivyHash(seed, 31, i++) < wv)
            cands.push_back(sm.p);
    }
    return spreadPicks(cands, count, seed);
}

double meanCurvature(const Result& r)
{
    double turn = 0.0, len = 0.0;
    for (const Strand& s : r.strands)
        for (size_t k = 1; k + 1 < s.points.size(); ++k)
        {
            if (s.hang[k - 1] || s.hang[k] || s.hang[k + 1])
                continue;
            const D3 a = s.points[k] - s.points[k - 1], b = s.points[k + 1] - s.points[k];
            const double la = glm::length(a), lb = glm::length(b);
            if (la < 1e-9 || lb < 1e-9)
                continue;
            turn += std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0));
            len += la;
        }
    return len > 0.0 ? turn / len : 0.0;
}

Result grow(const geo::Mesh& surfaceMesh, std::span<const D3> seedsIn, std::span<const std::vector<D3>> strokesIn, const Params& p, const Pieces& pieces)
{
    LILA_ZONE();
    Result r;
    if (surfaceMesh.numFaces() == 0)
    {
        r.warnings.push_back("no surface to grow on");
        return r;
    }
    Surface s(surfaceMesh);
    const double length = std::max(1.0, p.length);
    const double step = p.step > 0.0 ? p.step : std::clamp(std::max(length / 50.0, p.leafSize * 0.5), 0.5, 40.0);
    const double adhesion = std::max(0.01, p.adhesion);
    s.offset = std::clamp(p.stemRadius, 0.01, adhesion * 0.5);
    const double smooth = std::clamp(p.smoothness, 0.0, 1.0);
    const double branch = std::clamp(p.branchiness, 0.0, 1.0);
    const uint32_t seed = p.seed;

    // Roots: seeds and stroke starts, on the surface.
    std::vector<D3> roots;
    std::vector<double> budget;
    for (const D3& q : seedsIn)
    {
        D3 at = q, n;
        if (!s.snap(at, n))
            continue;
        roots.push_back(at);
        budget.push_back(length);
    }
    std::vector<std::vector<D3>> strokes;
    for (const std::vector<D3>& st : strokesIn)
    {
        std::vector<D3> line = resample(st, step * 0.5);
        std::vector<D3> on;
        for (D3 q : line)
        {
            D3 n;
            if (s.snap(q, n))
                on.push_back(q);
        }
        if (on.size() < 2)
            continue;
        roots.push_back(on.front());
        budget.push_back(std::max(length, polyLength(on) * 1.15));
        strokes.push_back(std::move(on));
    }
    if (roots.empty())
    {
        r.warnings.push_back("no seed points or strokes on the surface");
        return r;
    }
    const size_t seedCount = roots.size() - strokes.size();
    const double maxBudget = *std::max_element(budget.begin(), budget.end());

    // Attractors: surface samples a seed can reach over the surface, thinned toward up for climbing plants, plus a band
    // round each stroke.
    const double spacing = step * (2.2 - 1.2 * branch);
    std::vector<D3> attractors;
    {
        LILA_ZONE_N("ivy attractors");
        geo::Bounds box;
        for (const D3& q : roots)
            box.extend(q);
        box.min -= D3(maxBudget);
        box.max += D3(maxBudget);
        const std::vector<geo::Index> tris = s.bvh.overlapping(box);
        std::vector<Sample> samples = scatterOnTriangles(s, tris, spacing, seed + 3, 40000);
        if (seedCount > 0)
        {
            // Reach over the surface: shortest hops between nearby samples whose midpoint stays on the surface.
            const size_t n = samples.size();
            std::vector<D3> pos(n + seedCount);
            for (size_t k = 0; k < n; ++k)
                pos[k] = samples[k].p;
            for (size_t k = 0; k < seedCount; ++k)
                pos[n + k] = roots[k];
            const double hop = spacing * 1.9;
            Grid grid{hop, {}};
            for (size_t k = 0; k < pos.size(); ++k)
                grid.add(pos[k], uint32_t(k));
            std::vector<double> dist(pos.size(), std::numeric_limits<double>::max());
            std::vector<int> from(pos.size(), -1);
            using QItem = std::pair<double, uint32_t>;
            std::vector<QItem> heap;
            for (size_t k = 0; k < seedCount; ++k)
            {
                dist[n + k] = 0.0;
                from[n + k] = int(k);
                heap.push_back({0.0, uint32_t(n + k)});
            }
            std::make_heap(heap.begin(), heap.end(), std::greater<>());
            while (!heap.empty())
            {
                std::pop_heap(heap.begin(), heap.end(), std::greater<>());
                const auto [d, i] = heap.back();
                heap.pop_back();
                if (d > dist[i] || d > length)
                    continue;
                grid.around(pos[i], [&](uint32_t j) {
                    if (j == i)
                        return;
                    const double e = glm::distance(pos[i], pos[j]);
                    const double nd = d + e;
                    if (e > hop || nd >= dist[j] || nd > length)
                        return;
                    if (s.distance((pos[i] + pos[j]) * 0.5, spacing) > spacing * 0.6)
                        return;
                    dist[j] = nd;
                    from[j] = from[i];
                    heap.push_back({nd, j});
                    std::push_heap(heap.begin(), heap.end(), std::greater<>());
                });
            }
            const double climb = std::clamp(p.climb, 0.0, 1.0);
            for (size_t k = 0; k < n; ++k)
            {
                if (from[k] < 0)
                    continue;
                const double rel = dist[k] > 1e-9 ? (pos[k].y - roots[size_t(from[k])].y) / dist[k] : 1.0;
                const double keep = std::clamp(1.0 - climb * (0.8 - rel), 0.0, 1.0) * (0.45 + 0.55 * branch);
                if (ivyHash(seed, 5, uint32_t(k)) < keep)
                    attractors.push_back(samples[k].p + samples[k].n * s.offset);
            }
        }
        const double width = std::max(p.leafSize * 1.5, step * 3.0);
        for (size_t k = 0; k < strokes.size(); ++k)
        {
            for (const D3& q : strokes[k])
                attractors.push_back(q);
            Grid sg{width, {}};
            for (size_t i = 0; i < strokes[k].size(); ++i)
                sg.add(strokes[k][i], uint32_t(i));
            for (size_t i = 0; i < samples.size(); ++i)
            {
                bool near = false;
                sg.around(samples[i].p, [&](uint32_t j) { near = near || glm::distance(samples[i].p, strokes[k][j]) < width; });
                if (near && ivyHash(seed, 6 + uint32_t(k), uint32_t(i)) < 0.25 + 0.5 * branch)
                    attractors.push_back(samples[i].p + samples[i].n * s.offset);
            }
        }
    }
    r.attractors = attractors.size();

    // Branching: the Space Colonization node.
    gn::CurveSet cs;
    {
        LILA_ZONE_N("ivy space colonization");
        const double influence = spacing * 2.5;
        const double kill = std::max(step * 1.6, spacing * (1.15 - 0.4 * branch));
        const int iterations = int(std::ceil(maxBudget / step)) + 2;
        cs = spaceColonize(roots, attractors, step, influence, kill, iterations, std::max(0.01, p.stemRadius * 0.35));
    }
    std::vector<Raw> raw = strandsOf(cs, roots);

    // Shape each strand, parents first: wander, onto the surface, smoothing.
    const int smoothPasses = int(std::lround(smooth * smooth * 30.0));
    const double wanderAmp = (1.0 - smooth) * step * 0.45;
    const double wanderFreq = 1.0 / (step * (2.5 + 6.0 * smooth));
    std::vector<int> keep(raw.size(), 1);
    for (size_t k = 0; k < raw.size(); ++k)
    {
        Raw& st = raw[k];
        if (st.parent >= 0 && !keep[size_t(st.parent)])
        {
            keep[k] = 0;
            continue;
        }
        std::vector<D3> pts = st.pts;
        std::vector<D3> nrm(pts.size(), kUp);
        if (st.parent >= 0)
            pts[0] = raw[size_t(st.parent)].pts[size_t(st.joint)];
        const double ph0 = ivyHash(seed, 21, uint32_t(k)) * kTau, ph1 = ivyHash(seed, 22, uint32_t(k)) * kTau;
        double along = 0.0;
        for (size_t i = 0; i < pts.size(); ++i)
        {
            D3 n;
            D3 q = pts[i];
            s.snap(q, n);
            nrm[i] = n;
            if (i > 0)
                along += glm::distance(st.pts[i], st.pts[i - 1]);
            if (i == 0 || wanderAmp <= 0.0)
                continue;
            const D3 t = safeNormalize(st.pts[std::min(i + 1, pts.size() - 1)] - st.pts[i - 1], kUp);
            const D3 side = safeNormalize(glm::cross(t, n), anyPerpendicular(n));
            const double ramp = std::min(1.0, along / (step * 2.0));
            pts[i] += side * (wanderAmp * ramp * wander(along * wanderFreq * kTau, ph0, ph1));
        }
        for (int pass = 0; pass <= smoothPasses; ++pass)
        {
            if (pass > 0)
            {
                std::vector<D3> prev = pts;
                for (size_t i = 1; i + 1 < pts.size(); ++i)
                    pts[i] = glm::mix(prev[i], (prev[i - 1] + prev[i + 1]) * 0.5, 0.5);
            }
            if (pass % 5 == 0 || pass == smoothPasses)
                for (size_t i = 1; i < pts.size(); ++i)
                    s.snap(pts[i], nrm[i]);
        }
        st.pts = std::move(pts);
        Strand out;
        out.points = st.pts;
        out.normals = nrm;
        out.radius = st.radius;
        out.parent = st.parent;
        r.strands.push_back(std::move(out));
    }

    // Smooth arcs for stylized looks: twice the points, the originals kept so joints stay put.
    if (smooth > 0.3)
        for (size_t k = 0; k < r.strands.size(); ++k)
        {
            Strand& st = r.strands[k];
            const std::vector<D3> sub = subdivide(st.points);
            std::vector<D3> nrm(sub.size());
            std::vector<double> rad(sub.size());
            std::vector<D3> pts = sub;
            for (size_t i = 0; i < sub.size(); ++i)
            {
                if (i % 2 == 0)
                {
                    pts[i] = st.points[i / 2];
                    nrm[i] = st.normals[i / 2];
                    rad[i] = st.radius[i / 2];
                    continue;
                }
                s.snap(pts[i], nrm[i]);
                rad[i] = 0.5 * (st.radius[i / 2] + st.radius[i / 2 + 1]);
            }
            st.points = std::move(pts);
            st.normals = std::move(nrm);
            st.radius = std::move(rad);
            raw[k].joint *= 2;
        }

    // Growth along the paths, trimmed to each root's budget; strands whose joint was cut go too.
    {
        std::vector<int> newIndex(r.strands.size(), -1);
        std::vector<Strand> kept;
        size_t rawIdx = 0;
        for (size_t k = 0; k < raw.size(); ++k)
        {
            if (!keep[k])
                continue;
            Strand st = std::move(r.strands[rawIdx++]);
            const Raw& rw = raw[k];
            double g0 = 0.0;
            int parent = -1;
            if (rw.parent >= 0)
            {
                parent = newIndex[size_t(rw.parent)];
                if (parent < 0 || size_t(rw.joint) >= kept[size_t(parent)].points.size())
                    continue;
                g0 = kept[size_t(parent)].growth[size_t(rw.joint)];
                st.points[0] = kept[size_t(parent)].points[size_t(rw.joint)];
                st.normals[0] = kept[size_t(parent)].normals[size_t(rw.joint)];
            }
            const double limit = budget[size_t(std::clamp(rw.root, 0, int(budget.size()) - 1))];
            st.growth.assign(1, g0);
            for (size_t i = 1; i < st.points.size(); ++i)
                st.growth.push_back(st.growth.back() + glm::distance(st.points[i - 1], st.points[i]));
            size_t end = st.points.size();
            for (size_t i = 1; i < st.points.size(); ++i)
                if (st.growth[i] > limit)
                {
                    end = i;
                    break;
                }
            if (end < st.points.size())
            {
                const double f = (limit - st.growth[end - 1]) / std::max(1e-9, st.growth[end] - st.growth[end - 1]);
                st.points[end] = glm::mix(st.points[end - 1], st.points[end], f);
                D3 n = st.normals[end];
                s.snap(st.points[end], n);
                st.normals[end] = n;
                st.growth[end] = st.growth[end - 1] + glm::distance(st.points[end - 1], st.points[end]);
                if (st.growth[end] > limit)
                    st.points[end] = st.points[end - 1], st.growth[end] = st.growth[end - 1];
                ++end;
                st.points.resize(end);
                st.normals.resize(end);
                st.radius.resize(end);
                st.growth.resize(end);
            }
            if (st.points.size() < 2)
                continue;
            st.parent = parent;
            st.hang.assign(st.points.size(), 0);
            for (double& rad : st.radius)
                rad = std::clamp(rad, p.stemRadius * 0.3, p.stemRadius);
            newIndex[k] = int(kept.size());
            kept.push_back(std::move(st));
        }
        r.strands = std::move(kept);
    }

    // Hanging: over ledges and from overhangs, as gravity pulls.
    const double gravity = std::clamp(p.gravity, 0.0, 1.0);
    if (gravity > 0.0)
    {
        LILA_ZONE_N("ivy hangs");
        const D3 down(0.0, -1.0, 0.0);
        const size_t climbing = r.strands.size();
        for (size_t k = 0; k < climbing; ++k)
        {
            const size_t n = r.strands[k].points.size();
            for (size_t i = 1; i < n; i += 2)
            {
                const Strand& st = r.strands[k];
                const D3 at = st.points[i], nrm = st.normals[i];
                const double left = length - st.growth[i];
                if (left < step * 2.0)
                    continue;
                const bool tip = i + 2 >= n;
                D3 dir = down;
                bool overhang = nrm.y < -0.35;
                if (overhang)
                    dir = down;
                else if (nrm.y > 0.5)
                {
                    bool found = false;
                    const double a0 = ivyHash(seed, 41, uint32_t(k * 7919 + i)) * kTau;
                    for (int d = 0; d < 8 && !found; ++d)
                    {
                        const double a = a0 + kTau * d / 8.0;
                        const D3 out(std::cos(a), 0.0, std::sin(a));
                        const D3 q = at + out * (step * 1.2);
                        geo::BvhHit hit;
                        if (!s.bvh.raycast(q, down, hit, step * 2.5) && s.distance(q, step * 0.8) > step * 0.8)
                        {
                            dir = safeNormalize(out + down * 0.2, down);
                            found = true;
                        }
                    }
                    if (!found)
                        continue;
                }
                else
                    continue;
                const double chance = gravity * (overhang ? 0.2 : 0.45) * (tip ? 2.0 : 1.0);
                if (ivyHash(seed, 43, uint32_t(k * 7919 + i)) >= chance)
                    continue;
                Strand h;
                h.parent = int(k);
                const double total = left * (0.35 + 0.65 * ivyHash(seed, 44, uint32_t(k * 7919 + i))) * (0.5 + 0.5 * gravity);
                const double hs = step * 0.6;
                const D3 face = safeNormalize(D3(dir.x, 0.0, dir.z), safeNormalize(D3(nrm.x, 0.0, nrm.z), D3(1, 0, 0)));
                const D3 side = safeNormalize(glm::cross(kUp, face), D3(1, 0, 0));
                const double ph = ivyHash(seed, 45, uint32_t(k * 7919 + i)) * kTau;
                D3 pos = at;
                double g = st.growth[i];
                h.points.push_back(pos);
                h.normals.push_back(nrm);
                h.radius.push_back(st.radius[i]);
                h.growth.push_back(g);
                h.hang.push_back(0);
                double done = 0.0;
                while (done + hs <= total && g + hs <= length)
                {
                    dir = safeNormalize(dir + down * (0.15 + 0.35 * gravity) + side * ((1.0 - smooth) * 0.12 * std::sin(done / (step * 2.0) + ph)), down);
                    D3 q = pos + dir * hs;
                    D3 cn;
                    D3 probe = q;
                    bool landed = false;
                    if (s.distance(q, s.offset) < s.offset && s.snap(probe, cn))
                    {
                        // Slides along what it meets; anything floor-like ends it.
                        if (cn.y > 0.6)
                            landed = true;
                        q = probe;
                    }
                    if (landed)
                        break;
                    const double seg = glm::distance(pos, q);
                    g += seg;
                    done += seg;
                    pos = q;
                    h.points.push_back(pos);
                    h.normals.push_back(face);
                    h.radius.push_back(std::max(p.stemRadius * 0.3, st.radius[i] * 0.7));
                    h.growth.push_back(g);
                    h.hang.push_back(1);
                }
                if (h.points.size() >= 3)
                    r.strands.push_back(std::move(h));
            }
        }
    }

    // The mesh: stems, then leaves and flowers.
    geo::Mesh& out = r.mesh;
    std::vector<glm::vec4> wind;
    std::vector<glm::vec2> pointUv;
    {
        LILA_ZONE_N("ivy stems");
        const std::vector<PlacedPiece> stemPieces = fitAll(pieces.stems, true);
        std::vector<glm::dvec2> ring;
        if (p.profile == Profile::Custom && p.profilePoints.size() >= 3)
        {
            double big = 1e-9;
            glm::dvec2 c(0.0);
            for (const glm::dvec2& q : p.profilePoints)
                c += q;
            c /= double(p.profilePoints.size());
            for (const glm::dvec2& q : p.profilePoints)
                big = std::max(big, glm::length(q - c));
            for (const glm::dvec2& q : p.profilePoints)
                ring.push_back((q - c) / big);
        }
        else if (p.profile == Profile::Ribbon)
            ring = {{-1.6, 0.0}, {1.6, 0.0}};
        else
        {
            const int sides = std::clamp(p.sides, 3, 16);
            for (int k = 0; k < sides; ++k)
                ring.push_back({std::cos(kTau * k / sides), std::sin(kTau * k / sides)});
        }
        const bool closed = ring.size() >= 3;
        for (size_t si = 0; si < r.strands.size(); ++si)
        {
            const Strand& st = r.strands[si];
            const size_t n = st.points.size();
            std::vector<D3> sides(n), ups(n);
            D3 prevSide = anyPerpendicular(st.normals[0]);
            for (size_t i = 0; i < n; ++i)
            {
                const D3 t = safeNormalize(st.points[std::min(i + 1, n - 1)] - st.points[i == 0 ? 0 : i - 1], kUp);
                D3 side = glm::cross(t, st.normals[i]);
                side = glm::length(side) > 1e-6 ? glm::normalize(side) : prevSide;
                if (glm::dot(side, prevSide) < 0.0 && st.hang[i])
                    side = -side;
                prevSide = side;
                sides[i] = side;
                ups[i] = safeNormalize(glm::cross(side, t), st.normals[i]);
            }
            const double hangStart = st.hang.empty() || !st.hang.back() ? 0.0 : st.growth[0];
            auto stemWind = [&](size_t i) {
                const double depth = st.hang[i] ? std::min(1.0, (st.growth[i] - hangStart) / 60.0) * 0.8 : 0.0;
                return glm::vec4(float(depth), float(ivyHash(seed, 51, uint32_t(si))), 0.0f, 1.0f);
            };
            if (!stemPieces.empty())
            {
                for (size_t i = 0; i + 1 < n; ++i)
                {
                    const D3 a = st.points[i], b = st.points[i + 1];
                    const double len = glm::distance(a, b);
                    if (len < 1e-6)
                        continue;
                    const D3 z = (b - a) / len, y = ups[i], x = safeNormalize(glm::cross(y, z), sides[i]);
                    const double w = 2.0 * st.radius[i];
                    glm::dmat4 xf(1.0);
                    xf[0] = glm::dvec4(x * w, 0.0);
                    xf[1] = glm::dvec4(glm::cross(z, x) * w, 0.0);
                    xf[2] = glm::dvec4(z * len, 0.0);
                    xf[3] = glm::dvec4(a, 1.0);
                    const PlacedPiece& pc = stemPieces[pickWeighted(stemPieces, ivyHash(seed, 52, uint32_t(si * 100003 + i)))];
                    const geo::Index first = appendPiece(out, pc.mesh, xf, kStemSlot);
                    wind.resize(out.pointCapacity(), stemWind(i));
                    pointUv.resize(out.pointCapacity(), glm::vec2(-1.0f));
                    (void)first;
                }
                continue;
            }
            const size_t cols = closed ? ring.size() + 1 : ring.size();
            std::vector<geo::Index> prevRow;
            double v = 0.0;
            for (size_t i = 0; i < n; ++i)
            {
                if (i > 0)
                    v += glm::distance(st.points[i - 1], st.points[i]) / std::max(1e-6, kTau * std::max(st.radius[i], 1e-3));
                std::vector<geo::Index> row;
                for (size_t c = 0; c < cols; ++c)
                {
                    const glm::dvec2 q = ring[c % ring.size()];
                    const double rad = st.radius[i];
                    // A ribbon lies on the surface instead of standing round the centre line.
                    const D3 lift = p.profile == Profile::Ribbon && !st.hang[i] ? -ups[i] * (s.offset * 0.8) : D3(0.0);
                    row.push_back(out.addPoint(st.points[i] + lift + sides[i] * (q.x * rad) + ups[i] * (q.y * rad)));
                    wind.push_back(stemWind(i));
                    pointUv.push_back(glm::vec2(float(c) / float(std::max<size_t>(1, cols - 1)), float(v)));
                }
                if (!prevRow.empty())
                    for (size_t c = 0; c + 1 < cols; ++c)
                        out.addFace({prevRow[c], prevRow[c + 1], row[c + 1], row[c]});
                prevRow = std::move(row);
            }
        }
        if (out.numFaces() > 0 && stemPieces.empty())
            lila::scene::materials::assign(out, kStemSlot, {});
    }
    // Swept stems face outward: the ring runs side then up, which winds the quads inward for a round profile.
    if (pieces.stems.empty() && p.profile != Profile::Ribbon)
        for (geo::Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
                out.flipFace(f);
    if (p.profile == Profile::Ribbon && pieces.stems.empty())
    {
        for (geo::Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
            {
                const D3 c = out.faceCentroid(f);
                D3 n, q = c;
                if (s.snap(q, n, adhesion * 2.0) && glm::dot(out.faceNormal(f), n) < 0.0)
                    out.flipFace(f);
            }
    }

    auto place = [&](const std::vector<PlacedPiece>& set, double perMetre, double size, double jitter, const char* slot, uint32_t salt, bool flower,
                     size_t& count) {
        if (set.empty() || perMetre <= 0.0 || size <= 0.0)
            return;
        const double gap = 100.0 / perMetre;
        constexpr size_t kMax = 200000;
        for (size_t si = 0; si < r.strands.size() && count < kMax; ++si)
        {
            const Strand& st = r.strands[si];
            if (st.points.size() < 2)
                continue;
            const double gEnd = st.growth.back();
            double g = st.growth.front() + gap * ivyHash(seed, salt, uint32_t(si));
            uint32_t k = 0;
            while (g < gEnd && count < kMax)
            {
                const uint32_t key = uint32_t(si) * 7919u + k;
                const AlongStrand a = alongStrand(st, g);
                const double h0 = ivyHash(seed, salt + 1, key), h1 = ivyHash(seed, salt + 2, key), h2 = ivyHash(seed, salt + 3, key);
                const double alt = (k % 2) ? 1.0 : -1.0;
                const D3 n = a.n;
                const D3 side = safeNormalize(glm::cross(a.t, n), anyPerpendicular(n));
                const D3 upOnSurface = safeNormalize(kUp - n * glm::dot(kUp, n), a.t);
                D3 along;
                LeafOrient orient = flower ? LeafOrient::Up : p.leafOrient;
                switch (orient)
                {
                case LeafOrient::Up: along = upOnSurface; break;
                case LeafOrient::Stem: along = a.t; break;
                case LeafOrient::Random:
                {
                    const double ang = kTau * h0;
                    along = a.t * std::cos(ang) + side * std::sin(ang);
                    break;
                }
                default: along = safeNormalize(upOnSurface * 0.7 + side * (alt * 0.75) + a.t * 0.2, upOnSurface); break;
                }
                along = safeNormalize(along + n * (flower ? 0.1 : 0.35), n);
                D3 face = safeNormalize(n - along * glm::dot(n, along), anyPerpendicular(along));
                // A little twist about the leaf's own axis.
                const double twist = (h1 - 0.5) * 0.6;
                const D3 across = glm::cross(face, along);
                face = safeNormalize(face * std::cos(twist) + across * std::sin(twist), face);
                const double toTip = gEnd - g;
                const double taper = flower ? 1.0 : 0.5 + 0.5 * std::clamp(toTip / (size * 3.0), 0.0, 1.0);
                const double sc = size * std::max(0.2, 1.0 + jitter * (2.0 * h2 - 1.0)) * taper;
                const D3 x = glm::cross(face, along);
                glm::dmat4 xf(1.0);
                xf[0] = glm::dvec4(x * sc, 0.0);
                xf[1] = glm::dvec4(face * sc, 0.0);
                xf[2] = glm::dvec4(along * sc, 0.0);
                xf[3] = glm::dvec4(a.p + side * (alt * a.radius) + n * (a.radius * 0.5), 1.0);
                const PlacedPiece& pc = set[pickWeighted(set, ivyHash(seed, salt + 4, key))];
                const geo::Index first = appendPiece(out, pc.mesh, xf, slot);
                const float phase = float(ivyHash(seed, salt + 5, key));
                for (geo::Index q = first; q < out.pointCapacity(); ++q)
                {
                    const double alongLeaf = q - first < pc.mesh.pointCapacity() && pc.mesh.pointValid(q - first) ? pc.mesh.position(q - first).z : 0.5;
                    const float sway = float(std::clamp(0.35 + 0.65 * alongLeaf + (a.hang ? 0.3 : 0.0), 0.0, 1.0));
                    wind.push_back(glm::vec4(sway, phase, 1.0f, 1.0f));
                    pointUv.push_back(glm::vec2(-1.0f));
                }
                ++count;
                ++k;
                g += gap * (0.6 + 0.8 * ivyHash(seed, salt + 6, key));
            }
        }
    };
    {
        LILA_ZONE_N("ivy leaves");
        const std::vector<PlacedPiece> leaves = fitAll(pieces.leaves, false);
        const std::vector<PlacedPiece> flowers = fitAll(pieces.flowers, false);
        place(leaves, p.leafDensity, p.leafSize, std::clamp(p.leafSizeJitter, 0.0, 1.0), kLeafSlot, 600, false, r.leaves);
        place(flowers, p.flowerDensity, p.flowerSize, 0.2, kFlowerSlot, 700, true, r.flowers);
        if (p.leafDensity > 0.0 && leaves.empty() && !pieces.leaves.empty())
            r.warnings.push_back("the leaf pieces have no faces");
    }

    // Stem UVs and the wind colours.
    {
        auto uv = out.cornerAttrs().getOrAdd<glm::vec2>(geo::attr::uvName(0));
        for (geo::Index f = 0; f < out.faceCapacity(); ++f)
            if (out.faceValid(f))
                for (geo::Index c : out.faceCorners(f))
                {
                    const geo::Index q = out.cornerPoint(c);
                    if (q < pointUv.size() && pointUv[q].x >= 0.0f)
                        uv[c] = pointUv[q];
                }
        wind.resize(out.pointCapacity(), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        geo::colors::add(out, kWindSet, glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
        geo::colors::setFromPoints(out, kWindSet, wind);
        geo::colors::move(out, kWindSet, 0);
        geo::colors::setData(out, kWindSet, true);
    }
    if (r.leaves > 150000)
        r.warnings.push_back("over 150000 leaves; lower the leaf density for a lighter mesh");
    return r;
}
} // namespace weathering::ivy
