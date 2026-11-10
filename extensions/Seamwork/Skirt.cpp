#include "Skirt.h"

#include <lila/Boolean.h>
#include <lila/Mesh.h>


#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

namespace seamwork
{
using lila::geo::Index;

namespace
{
glm::dvec3 unitTo(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

/** One source surface: an object or the terrain, with exact projection onto it. */
struct Surface
{
    const Object* object = nullptr;
    const Ground* ground = nullptr;
    glm::dmat4 inv{1.0};
    glm::dmat3 normals{1.0};

    static Surface of(const Object& o)
    {
        Surface s;
        s.object = &o;
        s.inv = glm::inverse(o.world);
        s.normals = glm::transpose(glm::inverse(glm::dmat3(o.world)));
        return s;
    }
    static Surface of(const Ground& g)
    {
        Surface s;
        s.ground = &g;
        s.inv = glm::inverse(g.world);
        s.normals = glm::transpose(glm::inverse(glm::dmat3(g.world)));
        return s;
    }
    /** The closest point on the surface and its normal there (interpolated on terrain, the face's on a mesh). */
    bool project(const glm::dvec3& p, glm::dvec3& at, glm::dvec3& normal, double* distance = nullptr) const
    {
        const glm::dvec3 l(inv * glm::dvec4(p, 1.0));
        if (ground)
        {
            const Heightfield& h = *ground->field;
            const glm::dvec3 q(l.x, h.heightAt(l.x, l.z), l.z);
            at = glm::dvec3(ground->world * glm::dvec4(q, 1.0));
            normal = unitTo(normals * h.normalAt(l.x, l.z), {0, 1, 0});
            if (distance)
                *distance = glm::length(at - p);
            return true;
        }
        lila::geo::BvhClosest c;
        if (!object->bvh->closestPoint(l, c))
            return false;
        at = glm::dvec3(object->world * glm::dvec4(c.point, 1.0));
        normal = unitTo(normals * object->mesh->faceNormal(c.face), {0, 1, 0});
        if (distance)
            *distance = glm::length(at - p);
        return true;
    }
    double distance(const glm::dvec3& p) const
    {
        glm::dvec3 at, n;
        double d = 1e300;
        project(p, at, n, &d);
        return d;
    }
};

struct SkirtRow
{
    glm::dvec3 p, t, na, nb;
    double along = 0.0;
    /** The wall border takes na as its normal: rows fanning round a wall's edge, where the wall's own faces disagree. */
    bool roundWall = false;
};

std::vector<SkirtRow> skirtRows(const Curve& c, double spacing)
{
    std::vector<SkirtRow> rows;
    const size_t n = c.points.size();
    if (n < 2)
        return rows;
    const size_t segs = c.closed ? n : n - 1;
    std::vector<double> at(segs + 1, 0.0);
    for (size_t s = 0; s < segs; ++s)
        at[s + 1] = at[s] + glm::length(c.points[(s + 1) % n] - c.points[s]);
    const double total = at.back();
    if (total < 1e-9)
        return rows;
    // Keep every curve point (corners stay sharp) and fill long segments at spacing.
    for (size_t s = 0; s < segs; ++s)
    {
        const size_t i0 = s, i1 = (s + 1) % n;
        const double len = at[s + 1] - at[s];
        const int steps = std::max(1, int(std::ceil(len / std::max(0.5, spacing))));
        const bool lastSeg = s + 1 == segs;
        for (int k = 0; k < steps + (lastSeg && !c.closed ? 1 : 0); ++k)
        {
            const double t = double(k) / steps;
            SkirtRow r;
            r.p = c.points[i0] + (c.points[i1] - c.points[i0]) * t;
            r.t = unitTo(c.points[i1] - c.points[i0], {1, 0, 0});
            r.na = unitTo(c.normalA[i0] * (1.0 - t) + c.normalA[i1] * t, c.normalA[i0]);
            r.nb = unitTo(c.normalB[i0] * (1.0 - t) + c.normalB[i1] * t, c.normalB[i0]);
            r.along = at[s] + len * t;
            rows.push_back(r);
        }
    }
    return rows;
}

glm::dvec3 sideDir(const glm::dvec3& surface, const glm::dvec3& other, const glm::dvec3& t)
{
    glm::dvec3 d = other - surface * glm::dot(other, surface);
    if (glm::length(d) < 1e-9)
        d = glm::cross(surface, t);
    return glm::normalize(d);
}

glm::dvec3 slerpDir(const glm::dvec3& a, const glm::dvec3& b, double t)
{
    if (t <= 0.0)
        return a;
    if (t >= 1.0)
        return b;
    const double w = std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
    if (w < 1e-6 || w > kPi - 1e-6)
        return unitTo(a * (1.0 - t) + b * t, a);
    return glm::normalize(a * (std::sin((1.0 - t) * w) / std::sin(w)) + b * (std::sin(t * w) / std::sin(w)));
}

/** Vertices with positions, normals, side, uv and ground colour, and quads; becomes the mesh at the end. */
struct Builder
{
    std::vector<glm::dvec3> pts, nrm;
    std::vector<float> side;
    std::vector<glm::vec2> uv;
    std::vector<glm::vec3> color;
    std::vector<Index> sizes, idx;

    Index add(const glm::dvec3& p, const glm::dvec3& n, double s, glm::vec2 u, glm::vec3 c)
    {
        pts.push_back(p);
        nrm.push_back(n);
        side.push_back(float(s));
        uv.push_back(u);
        color.push_back(c);
        return Index(pts.size() - 1);
    }
    void quad(Index a, Index b, Index c, Index d)
    {
        sizes.push_back(4);
        idx.insert(idx.end(), {a, b, c, d});
    }
    /** A polygon with repeated points dropped; nothing when fewer than three remain. */
    void polygon(std::vector<Index> face)
    {
        face.erase(std::unique(face.begin(), face.end()), face.end());
        while (face.size() > 1 && face.front() == face.back())
            face.pop_back();
        if (face.size() < 3)
            return;
        sizes.push_back(Index(face.size()));
        idx.insert(idx.end(), face.begin(), face.end());
    }

    /** add, reusing the point already at this position, so pieces built apart weld. */
    Index shared(const glm::dvec3& p, const glm::dvec3& n, double s, glm::vec2 u, glm::vec3 c)
    {
        const auto key = std::make_tuple(std::llround(p.x * 1e5), std::llround(p.y * 1e5), std::llround(p.z * 1e5));
        if (const auto it = at.find(key); it != at.end())
            return it->second;
        return at[key] = add(p, n, s, u, c);
    }
    std::map<std::tuple<long long, long long, long long>, Index> at;
    /** Per face and point: a normal for that one corner, where faces meeting at a point need their own. */
    std::map<std::pair<Index, Index>, glm::dvec3> cornerNormal;

    Mesh build() const
    {
        Mesh m = Mesh::fromPolygons(pts, sizes, idx);
        if (m.numFaces() == 0)
            return m;
        auto n = m.cornerAttrs().getOrAdd<glm::vec3>("normal");
        auto u = m.cornerAttrs().getOrAdd<glm::vec2>("uv0");
        auto col = lila::geo::colors::add(m, "Col", glm::vec4(1.0f));
        for (Index c = 0; c < m.cornerCapacity(); ++c)
            if (m.cornerValid(c))
            {
                const Index p = m.cornerPoint(c);
                n[c] = glm::vec3(nrm[p]);
                u[c] = uv[p];
                col[c] = glm::vec4(color[p], side[p]);
            }
        for (const auto& [key, normal] : cornerNormal)
            if (key.first < m.numFaces())
                for (Index c : m.faceCorners(key.first))
                    if (m.cornerPoint(c) == key.second)
                        n[c] = glm::vec3(normal);
        auto sd = m.pointAttrs().getOrAdd<float>("side");
        for (Index p = 0; p < m.pointCapacity(); ++p)
            sd[p] = side[p];
        m.materialNames() = {"LilaSkirtBlend"};
        auto mat = m.faceAttrs().getOrAdd<int>("material");
        std::fill(mat.begin(), mat.end(), 0);
        return m;
    }
};

/** Ground colour under a point: the terrain's layer weights times their colours, grey without either. */
glm::vec3 groundColor(const Ground* g, const std::vector<std::string>& layers, const SkirtOptions& o, const glm::dvec3& p)
{
    if (!g || layers.empty() || o.layerColors.empty())
        return glm::vec3(0.5f);
    const glm::dvec3 l(glm::inverse(g->world) * glm::dvec4(p, 1.0));
    glm::dvec3 sum(0.0);
    double w = 0.0;
    for (const std::string& name : layers)
    {
        const auto it = o.layerColors.find(name);
        if (it == o.layerColors.end())
            continue;
        const double v = g->field->maskAt("layer_" + name, l.x, l.z);
        sum += it->second * v;
        w += v;
    }
    return w > 1e-6 ? glm::vec3(sum / w) : glm::vec3(0.5f);
}

struct ProfilePoint
{
    glm::dvec3 p, n;
    double side = 0.0;
};

/** The fillet profile across one row, wall border first: a cubic arc tangent to both surfaces, borders on them exactly. */
bool profileRow(const SkirtRow& r, const Surface& wall, const Surface& floor, const SkirtOptions& o, std::vector<ProfilePoint>& out)
{
    const int segs = std::max(2, o.segments);
    const glm::dvec3 w = sideDir(r.na, r.nb, r.t), g = sideDir(r.nb, r.na, r.t);
    const double theta = std::acos(std::clamp(glm::dot(w, g), -1.0, 1.0));
    const double half = std::clamp(theta * 0.5, glm::radians(3.0), glm::radians(87.0));
    double d = o.width, radius = o.width * std::tan(half);
    if (o.radius > 0.0)
    {
        radius = o.radius;
        d = std::clamp(o.radius / std::tan(half), 0.5, o.width * 4.0);
    }
    glm::dvec3 pw, nw, pg, ng;
    if (!wall.project(r.p + w * d, pw, nw) || !floor.project(r.p + g * d, pg, ng))
        return false;
    if (r.roundWall)
        nw = r.na;
    const glm::dvec3 ws = sideDir(nw, w, r.t), gs = unitTo(g - ng * glm::dot(g, ng), g);
    const glm::dvec3 wAlong = unitTo(w - nw * glm::dot(w, nw), ws);
    const double phi = kPi - 2.0 * half;
    const double k = 4.0 / 3.0 * std::tan(phi / 4.0) * radius;
    const glm::dvec3 b0 = pw, b1 = pw - wAlong * k, b2 = pg - gs * k, b3 = pg;
    out.clear();
    for (int j = 0; j <= segs; ++j)
    {
        const double s = double(j) / segs, u = 1.0 - s;
        const glm::dvec3 p = j == 0 ? pw : j == segs ? pg : b0 * (u * u * u) + b1 * (3 * u * u * s) + b2 * (3 * u * s * s) + b3 * (s * s * s);
        out.push_back({p, slerpDir(nw, ng, s), s});
    }
    return true;
}

/** Sweeps the fillet profile along one curve. Returns false (with why) when it can't. */
bool sweepCurve(Builder& b, const Curve& c, const Surface& wall, const Surface& floor, const Ground* ground, const std::vector<std::string>& layers,
                const SkirtOptions& o, std::string* why)
{
    const std::vector<SkirtRow> rows = skirtRows(c, o.spacing);
    if (rows.size() < 2)
    {
        *why = "too short";
        return false;
    }
    const int segs = std::max(2, o.segments);
    const Index base = Index(b.pts.size());
    std::vector<ProfilePoint> profile;
    for (const SkirtRow& r : rows)
    {
        if (!profileRow(r, wall, floor, o, profile))
        {
            *why = "no surface to land on";
            return false;
        }
        for (const ProfilePoint& q : profile)
            b.add(q.p, q.n, q.side, glm::vec2(float(r.along / 100.0), float(q.side)), groundColor(ground, layers, o, q.p));
    }
    const Index cols = Index(segs + 1);
    const size_t nr = rows.size();
    const size_t spans = c.closed && nr > 2 ? nr : nr - 1;
    // Face out of the wedge: compare the first quad's normal with the profile's normal there.
    const glm::dvec3 a0 = b.pts[base], a1 = b.pts[base + 1], c0 = b.pts[base + cols];
    const bool flip = glm::dot(glm::cross(a1 - a0, c0 - a0), b.nrm[base]) < 0.0;
    for (size_t k = 0; k < spans; ++k)
    {
        const Index r0 = base + Index(k) * cols, r1 = base + Index((k + 1) % nr) * cols;
        for (Index j = 0; j + 1 < cols; ++j)
        {
            if (flip)
                b.quad(r0 + j, r1 + j, r1 + j + 1, r0 + j + 1);
            else
                b.quad(r0 + j, r0 + j + 1, r1 + j + 1, r1 + j);
        }
    }
    return true;
}

/** A closed slab of the heightfield over cells [x0, x1) x [z0, z1), down to bottom, in the heightfield's space. */
Mesh slab(const Heightfield& h, int x0, int z0, int x1, int z1, double bottom)
{
    const int cx = x1 - x0 + 1, cz = z1 - z0 + 1;
    std::vector<glm::dvec3> pts;
    auto top = [&](int i, int j) { return Index(j * cx + i); };
    auto bot = [&](int i, int j) { return Index(cx * cz + j * cx + i); };
    for (int j = 0; j < cz; ++j)
        for (int i = 0; i < cx; ++i)
            pts.push_back(h.position(x0 + i, z0 + j));
    for (int j = 0; j < cz; ++j)
        for (int i = 0; i < cx; ++i)
        {
            glm::dvec3 p = h.position(x0 + i, z0 + j);
            p.y = bottom;
            pts.push_back(p);
        }
    std::vector<Index> sizes, idx;
    for (int j = 0; j + 1 < cz; ++j)
        for (int i = 0; i + 1 < cx; ++i)
        {
            sizes.push_back(4);
            idx.insert(idx.end(), {top(i, j), top(i, j + 1), top(i + 1, j + 1), top(i + 1, j)});
            sizes.push_back(4);
            idx.insert(idx.end(), {bot(i, j), bot(i + 1, j), bot(i + 1, j + 1), bot(i, j + 1)});
        }
    const glm::dvec3 centre = (h.position(x0, z0) + h.position(x1, z1)) * 0.5;
    auto side = [&](Index a, Index b) {
        const Index c = a + Index(cx * cz), d = b + Index(cx * cz);
        const glm::dvec3 nrm = glm::cross(pts[b] - pts[a], pts[c] - pts[a]);
        glm::dvec3 out = (pts[a] + pts[b]) * 0.5 - centre;
        out.y = 0.0;
        sizes.push_back(4);
        if (glm::dot(nrm, out) >= 0.0)
            idx.insert(idx.end(), {a, b, d, c});
        else
            idx.insert(idx.end(), {a, c, d, b});
    };
    for (int i = 0; i + 1 < cx; ++i)
    {
        side(top(i, 0), top(i + 1, 0));
        side(top(i, cz - 1), top(i + 1, cz - 1));
    }
    for (int j = 0; j + 1 < cz; ++j)
    {
        side(top(0, j), top(0, j + 1));
        side(top(cx - 1, j), top(cx - 1, j + 1));
    }
    return Mesh::fromPolygons(pts, sizes, idx);
}

/**
 * An outer corner of a wall on the ground (a box's vertical edge): the union's fillet tapers there, so the band is cut
 * back on two planes across the seam, la before the corner and lb after it, and a rolling-ball cap fills the wedge.
 */
struct OuterCorner
{
    glm::dvec3 c, tIn, tOut, nIn, nOut, pa{0.0}, pb{0.0};
    double la = 0.0, lb = 0.0;
    /** Signed distances to the cut planes: the cap's wedge is where fa > 0 and fb < 0. */
    double fa(const glm::dvec3& p) const { return glm::dot(p - pa, tIn); }
    double fb(const glm::dvec3& p) const { return glm::dot(p - pb, tOut); }
};

glm::dvec3 acrossUp(const glm::dvec3& v, const glm::dvec3& up) { return v - up * glm::dot(v, up); }

std::vector<OuterCorner> outerCorners(const Curve& c, const SkirtOptions& o)
{
    std::vector<OuterCorner> out;
    const size_t n = c.points.size();
    if (n < 3)
        return out;
    const glm::dvec3 up = unitTo(o.up, {0, 1, 0});
    std::vector<double> at(n, 0.0);
    for (size_t k = 1; k < n; ++k)
        at[k] = at[k - 1] + glm::length(c.points[k] - c.points[k - 1]);
    const double total = at[n - 1] + (c.closed ? glm::length(c.points[0] - c.points[n - 1]) : 0.0);
    std::vector<size_t> found;
    for (size_t k = c.closed ? 0 : 1; k < (c.closed ? n : n - 1); ++k)
    {
        const size_t prev = (k + n - 1) % n, next = (k + 1) % n;
        const glm::dvec3 tIn = unitTo(acrossUp(c.points[k] - c.points[prev], up), {}), tOut = unitTo(acrossUp(c.points[next] - c.points[k], up), {});
        if (glm::length(tIn) < 0.5 || glm::length(tOut) < 0.5 || glm::dot(tIn, tOut) > std::cos(glm::radians(30.0)))
            continue;
        const glm::dvec3 nIn = unitTo(acrossUp(c.normalA[prev], up), {}), nOut = unitTo(acrossUp(c.normalA[next], up), {});
        // Outer: the contact turns back toward the wall it came along, away from the ground in front of it.
        if (glm::length(nIn) < 0.5 || glm::length(nOut) < 0.5 || glm::dot(tOut, nIn) >= -0.2)
            continue;
        found.push_back(k);
        OuterCorner oc;
        oc.c = c.points[k];
        oc.tIn = tIn;
        oc.tOut = tOut;
        oc.nIn = nIn;
        oc.nOut = nOut;
        out.push_back(oc);
    }
    const double reach = 1.5 * std::max(o.width, o.radius);
    auto gap = [&](size_t a, size_t b) { return b >= a ? at[b] - at[a] : total - at[a] + at[b]; };
    for (size_t i = 0; i < out.size(); ++i)
    {
        const size_t k = found[i];
        double before = at[k], after = (c.closed ? total : at[n - 1]) - at[k];
        if (c.closed)
        {
            before = gap(found[(i + out.size() - 1) % out.size()], k) * 0.5;
            after = gap(k, found[(i + 1) % out.size()]) * 0.5;
            if (out.size() == 1)
                before = after = total * 0.5;
        }
        else
        {
            if (i > 0)
                before = (at[k] - at[found[i - 1]]) * 0.5;
            if (i + 1 < out.size())
                after = (at[found[i + 1]] - at[k]) * 0.5;
        }
        OuterCorner& oc = out[i];
        oc.la = std::min(reach, before);
        oc.lb = std::min(reach, after);
        oc.pa = oc.c - oc.tIn * oc.la;
        oc.pb = oc.c + oc.tOut * oc.lb;
    }
    return out;
}

struct BandPoint
{
    glm::dvec3 p, n;
    double side = 0.0;
};
using BandPoly = std::vector<BandPoint>;

/** The part of a polygon where f <= 0 (Sutherland-Hodgman), new points interpolated along the cut edges. */
template <typename F>
BandPoly clipPoly(const BandPoly& poly, F f)
{
    BandPoly out;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i)
    {
        const BandPoint& a = poly[i];
        const BandPoint& b = poly[(i + 1) % n];
        const double fa = f(a.p), fb = f(b.p);
        if (fa <= 0.0)
            out.push_back(a);
        if ((fa < 0.0 && fb > 0.0) || (fa > 0.0 && fb < 0.0))
        {
            const double t = fa / (fa - fb);
            out.push_back({a.p + (b.p - a.p) * t, unitTo(a.n + (b.n - a.n) * t, a.n), a.side + (b.side - a.side) * t});
        }
    }
    return out.size() >= 3 ? out : BandPoly{};
}

/** The polygons with each corner's wedge taken out; a piece that crosses both planes is split in two. */
std::vector<BandPoly> cutCorners(std::vector<BandPoly> polys, std::span<const OuterCorner> corners)
{
    for (const OuterCorner& oc : corners)
    {
        std::vector<BandPoly> next;
        for (const BandPoly& poly : polys)
        {
            if (BandPoly before = clipPoly(poly, [&](const glm::dvec3& p) { return oc.fa(p); }); !before.empty())
                next.push_back(std::move(before));
            const BandPoly past = clipPoly(poly, [&](const glm::dvec3& p) { return -oc.fa(p); });
            if (!past.empty())
                if (BandPoly beyond = clipPoly(past, [&](const glm::dvec3& p) { return -oc.fb(p); }); !beyond.empty())
                    next.push_back(std::move(beyond));
        }
        polys = std::move(next);
    }
    return polys;
}

/**
 * Faces from row a to row b (each wall border first) as quads where they match and a zipper of triangles where they
 * don't, wound like the quad (a[j], a[j + 1], b[j + 1], b[j]), or the reverse when flip.
 */
void stitchRows(Builder& b, const std::vector<Index>& ra, const std::vector<Index>& rb, bool flip)
{
    auto emit = [&](std::vector<Index> f) {
        if (flip)
            std::reverse(f.begin(), f.end());
        b.polygon(std::move(f));
    };
    if (ra.size() == rb.size())
    {
        for (size_t j = 0; j + 1 < ra.size(); ++j)
            emit({ra[j], ra[j + 1], rb[j + 1], rb[j]});
        return;
    }
    auto param = [&](const std::vector<Index>& r) {
        std::vector<double> t(r.size(), 0.0);
        for (size_t k = 1; k < r.size(); ++k)
            t[k] = t[k - 1] + glm::length(b.pts[r[k]] - b.pts[r[k - 1]]);
        for (double& v : t)
            v /= std::max(t.back(), 1e-12);
        return t;
    };
    const std::vector<double> ta = param(ra), tb = param(rb);
    size_t i = 0, j = 0;
    while (i + 1 < ra.size() || j + 1 < rb.size())
    {
        if (j + 1 >= rb.size() || (i + 1 < ra.size() && ta[i + 1] <= tb[j + 1]))
        {
            emit({ra[i], ra[i + 1], rb[j]});
            ++i;
        }
        else
        {
            emit({ra[i], rb[j + 1], rb[j]});
            ++j;
        }
    }
}

/**
 * The rolling-ball cap of one corner: straight rows from the cut before the corner, a fan turning the profile round
 * the corner, straight rows to the cut after it. Its end rows are the band's points on the cut planes, so it welds.
 */
void cornerCap(Builder& b, const OuterCorner& oc, const std::vector<Index>& endA, const std::vector<Index>& endB, const Surface& wall,
               const Surface& floor, const Ground* ground, const std::vector<std::string>& layers, const SkirtOptions& o)
{
    const glm::dvec3 up = unitTo(o.up, {0, 1, 0});
    auto groundNormal = [&](const glm::dvec3& p) {
        glm::dvec3 at, n;
        return floor.project(p, at, n) ? n : up;
    };
    std::vector<SkirtRow> rows;
    const double step = std::max(0.5, o.spacing);
    const int na = std::max(1, int(std::ceil(oc.la / step))), nb = std::max(1, int(std::ceil(oc.lb / step)));
    for (int k = 0; k < na; ++k)
    {
        const glm::dvec3 p = oc.pa + oc.tIn * (oc.la * k / na);
        rows.push_back({p, oc.tIn, oc.nIn, groundNormal(p)});
    }
    const double turn = std::acos(std::clamp(glm::dot(oc.nIn, oc.nOut), -1.0, 1.0));
    const int fan = std::max(2, int(std::ceil(glm::degrees(turn) / 12.0)));
    const glm::dvec3 nc = groundNormal(oc.c);
    for (int k = 0; k <= fan; ++k)
    {
        const double s = double(k) / fan;
        rows.push_back({oc.c, slerpDir(oc.tIn, oc.tOut, s), slerpDir(oc.nIn, oc.nOut, s), nc, 0.0, true});
    }
    for (int k = 1; k <= nb; ++k)
    {
        const glm::dvec3 p = oc.c + oc.tOut * (oc.lb * k / nb);
        rows.push_back({p, oc.tOut, oc.nOut, groundNormal(p)});
    }
    std::vector<std::vector<Index>> ids;
    std::vector<glm::dvec3> wallNormals;
    std::vector<ProfilePoint> profile;
    for (size_t r = 0; r < rows.size(); ++r)
    {
        const bool first = r == 0, last = r + 1 == rows.size();
        if ((first && endA.size() >= 2) || (last && endB.size() >= 2))
        {
            ids.push_back(first ? endA : endB);
            wallNormals.push_back(b.nrm[ids.back()[0]]);
            continue;
        }
        if (!profileRow(rows[r], wall, floor, o, profile))
            return;
        wallNormals.push_back(profile.front().n);
        std::vector<Index>& row = ids.emplace_back();
        for (const ProfilePoint& q : profile)
            row.push_back(b.shared(q.p, q.n, q.side, glm::vec2(0.0f, float(q.side)), groundColor(ground, layers, o, q.p)));
    }
    if (ids.size() < 3)
        return;
    // Face out of the wedge, like the band: the profile's normal against the first full row's quad.
    const std::vector<Index>& r1 = ids[1];
    const std::vector<Index>& r2 = ids[2];
    const glm::dvec3 a0 = b.pts[r1[0]], a1 = b.pts[r1[1]], c0 = b.pts[r2[0]] == a0 ? b.pts[r2[1]] : b.pts[r2[0]];
    const bool flip = glm::dot(glm::cross(a1 - a0, c0 - a0), b.nrm[r1[1]]) < 0.0;
    for (size_t r = 0; r + 1 < ids.size(); ++r)
    {
        const Index firstFace = Index(b.sizes.size());
        stitchRows(b, ids[r], ids[r + 1], flip);
        // The fan's rows all start at the wall's edge, one point: each face there takes its own rows' wall normal.
        for (Index f = firstFace; f < Index(b.sizes.size()); ++f)
        {
            b.cornerNormal[{f, ids[r + 1][0]}] = wallNormals[r + 1];
            b.cornerNormal[{f, ids[r][0]}] =
                ids[r][0] == ids[r + 1][0] ? unitTo(wallNormals[r] + wallNormals[r + 1], wallNormals[r]) : wallNormals[r];
        }
    }
}

/** The object's ground skirt by boolean union with a fillet, cut back to the fillet band, outer corners capped round. False (with why) when it fails. */
bool unionObject(Builder& b, int object, std::span<const Object> objects, const Ground* ground, int groundObject, const std::vector<std::string>& layers,
                 const std::vector<const Curve*>& curves, const SkirtOptions& o, std::string* why)
{
    const Object& obj = objects[size_t(object)];
    Mesh ground3;
    lila::scene::BooleanOperand floorOp;
    Surface floor;
    if (groundObject >= 0)
    {
        floorOp = {objects[size_t(groundObject)].mesh.get(), objects[size_t(groundObject)].world};
        floor = Surface::of(objects[size_t(groundObject)]);
    }
    else
    {
        // The terrain cropped round the object's footprint, closed into a slab.
        const Heightfield& h = *ground->field;
        const glm::dmat4 toHf = glm::inverse(ground->world) * obj.world;
        const lila::geo::Bounds lb = obj.bvh->bounds();
        glm::dvec3 lo(1e300), hi(-1e300);
        for (int k = 0; k < 8; ++k)
        {
            const glm::dvec3 c((k & 1) ? lb.max.x : lb.min.x, (k & 2) ? lb.max.y : lb.min.y, (k & 4) ? lb.max.z : lb.min.z);
            const glm::dvec3 q(toHf * glm::dvec4(c, 1.0));
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
        const double margin = o.width * 3.0 + h.cell * 2.0;
        const int x0 = std::clamp(int(std::floor((lo.x - margin - h.origin.x) / h.cell)), 0, h.nx - 2);
        const int z0 = std::clamp(int(std::floor((lo.z - margin - h.origin.y) / h.cell)), 0, h.nz - 2);
        const int x1 = std::clamp(int(std::ceil((hi.x + margin - h.origin.x) / h.cell)), x0 + 1, h.nx - 1);
        const int z1 = std::clamp(int(std::ceil((hi.z + margin - h.origin.y) / h.cell)), z0 + 1, h.nz - 1);
        const double bottom = std::min(lo.y, double(h.range().x)) - 200.0;
        ground3 = slab(h, x0, z0, x1, z1, bottom);
        floorOp = {&ground3, ground->world};
        floor = Surface::of(*ground);
    }
    const lila::scene::BooleanOperand ops[] = {floorOp, {obj.mesh.get(), obj.world}};
    lila::scene::BooleanOptions bo;
    bo.op = lila::scene::BooleanOp::Union;
    bo.filletWidth = o.width;
    bo.filletSegments = std::max(2, o.segments);
    const lila::scene::BooleanResult res = lila::scene::meshBoolean(ops, glm::dmat4(1.0), bo);
    if (!res.ok())
    {
        *why = "union failed: " + res.error;
        return false;
    }
    const Mesh& m = res.mesh;
    const Surface wall = Surface::of(obj);
    constexpr double kOn = 0.05;
    std::vector<double> dw(m.pointCapacity(), 0.0), dg(m.pointCapacity(), 0.0);
    std::vector<glm::dvec3> nw(m.pointCapacity()), ng(m.pointCapacity());
    std::vector<uint8_t> below(m.pointCapacity(), 0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 at;
            wall.project(m.position(p), at, nw[p], &dw[p]);
            floor.project(m.position(p), at, ng[p], &dg[p]);
            below[p] = glm::dot(m.position(p) - at, ng[p]) < -kOn;
        }
    // The fillet band: faces with a point off both source surfaces, none under the ground (the slab's sides and bottom).
    std::vector<Index> keep;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        bool off = false, under = false;
        m.forEachFaceCorner(f, [&](Index c) {
            const Index p = m.cornerPoint(c);
            off |= dw[p] > kOn && dg[p] > kOn;
            under |= below[p] != 0;
        });
        if (off && !under)
            keep.push_back(f);
    }
    if (keep.empty())
    {
        *why = "union made no fillet band";
        return false;
    }
    std::vector<glm::dvec3> smooth(m.pointCapacity(), glm::dvec3(0.0));
    for (Index f : keep)
    {
        const glm::dvec3 a = m.faceAreaVector(f);
        m.forEachFaceCorner(f, [&](Index c) { smooth[m.cornerPoint(c)] += a; });
    }
    std::vector<BandPoly> polys;
    for (Index f : keep)
    {
        BandPoly& poly = polys.emplace_back();
        m.forEachFaceCorner(f, [&](Index c) {
            const Index p = m.cornerPoint(c);
            const double side = dw[p] <= kOn ? 0.0 : dg[p] <= kOn ? 1.0 : std::clamp(dw[p] / std::max(1e-9, dw[p] + dg[p]), 0.0, 1.0);
            const glm::dvec3 n = dw[p] <= kOn ? nw[p] : dg[p] <= kOn ? ng[p] : unitTo(smooth[p], ng[p]);
            poly.push_back({m.position(p), n, side});
        });
    }
    std::vector<OuterCorner> corners;
    for (const Curve* c : curves)
        for (const OuterCorner& oc : outerCorners(*c, o))
            corners.push_back(oc);
    polys = cutCorners(std::move(polys), corners);
    // Points the cut made on a border take their surface's normal, not one interpolated along the edge.
    for (BandPoly& poly : polys)
        for (BandPoint& q : poly)
            if (glm::dvec3 at, n; (q.side <= 0.0 && wall.project(q.p, at, n)) || (q.side >= 1.0 && floor.project(q.p, at, n)))
                q.n = n;
    for (const BandPoly& poly : polys)
    {
        std::vector<Index> face;
        for (const BandPoint& q : poly)
            face.push_back(b.shared(q.p, q.n, q.side, glm::vec2(0.0f, float(q.side)), groundColor(ground, layers, o, q.p)));
        b.polygon(std::move(face));
    }
    // Each cap starts and ends on the band's points on its cut planes, wall border first.
    constexpr double kPlane = 1e-6;
    for (const OuterCorner& oc : corners)
    {
        std::vector<std::pair<double, Index>> onA, onB;
        for (const BandPoly& poly : polys)
            for (const BandPoint& q : poly)
            {
                if (std::abs(oc.fa(q.p)) < kPlane && oc.fb(q.p) < 0.0)
                    onA.push_back({q.side, b.shared(q.p, q.n, q.side, {}, {})});
                else if (std::abs(oc.fb(q.p)) < kPlane && oc.fa(q.p) > 0.0)
                    onB.push_back({q.side, b.shared(q.p, q.n, q.side, {}, {})});
            }
        auto row = [](std::vector<std::pair<double, Index>> v) {
            std::sort(v.begin(), v.end());
            std::vector<Index> out;
            for (const auto& [s, id] : v)
                if (out.empty() || out.back() != id)
                    out.push_back(id);
            return out;
        };
        cornerCap(b, oc, row(onA), row(onB), wall, floor, ground, layers, o);
    }
    return true;
}
} // namespace

const std::vector<std::string>& skirtMethodNames()
{
    static const std::vector<std::string> n = {"auto", "union", "sweep"};
    return n;
}

double maxTurn(const Curve& c)
{
    const size_t n = c.points.size();
    double worst = 0.0;
    const size_t first = c.closed ? 0 : 1, last = c.closed ? n : n - 1;
    for (size_t k = first; k < last; ++k)
    {
        const glm::dvec3 a = c.points[k] - c.points[(k + n - 1) % n], b = c.points[(k + 1) % n] - c.points[k];
        if (glm::length(a) < 1e-9 || glm::length(b) < 1e-9)
            continue;
        worst = std::max(worst, glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0))));
    }
    return worst;
}

SkirtResult buildSkirt(const Result& r, std::span<const Object> objects, const Ground* ground, const SkirtOptions& o)
{
    SkirtResult out;
    Builder b;
    // Objects to union, each once, with the curves that chose it.
    std::map<int, std::vector<size_t>> unions;
    std::map<int, int> unionGround;
    for (size_t ci = 0; ci < r.curves.size(); ++ci)
    {
        const Curve& c = r.curves[ci];
        if (!(o.kinds & (1 << int(c.kind))) || c.b == kNone || c.a < 0 || (o.object >= 0 && c.a != o.object))
            continue;
        if (c.b == kTerrain && !ground)
            continue;
        const double turn = maxTurn(c);
        SkirtMethod m = o.method;
        std::string reason;
        if (m == SkirtMethod::Auto)
        {
            m = turn <= o.straightAngle ? SkirtMethod::Sweep : SkirtMethod::Union;
            reason = m == SkirtMethod::Sweep ? "turns at most " + std::to_string(int(std::round(turn))) + " deg"
                                             : "turns " + std::to_string(int(std::round(turn))) + " deg, past " +
                                                   std::to_string(int(o.straightAngle)) + " deg";
        }
        else
            reason = "asked";
        if (m == SkirtMethod::Union)
        {
            unions[c.a].push_back(ci);
            unionGround[c.a] = c.b;
            out.curves.push_back({ci, "union", reason});
            continue;
        }
        const Surface wall = Surface::of(objects[size_t(c.a)]);
        const Surface floor = c.b == kTerrain ? Surface::of(*ground) : Surface::of(objects[size_t(c.b)]);
        std::string why;
        if (sweepCurve(b, c, wall, floor, ground, r.layers, o, &why))
            out.curves.push_back({ci, "sweep", reason});
        else
            out.curves.push_back({ci, "none", "sweep failed: " + why});
    }
    for (const auto& [object, curves] : unions)
    {
        std::string why;
        const int g = unionGround[object];
        std::vector<const Curve*> own;
        for (size_t ci : curves)
            own.push_back(&r.curves[ci]);
        if (unionObject(b, object, objects, ground, g >= 0 ? g : -1, r.layers, own, o, &why))
            continue;
        // Say what was used instead and why.
        for (size_t ci : curves)
            for (SkirtCurve& sc : out.curves)
                if (sc.curve == ci)
                {
                    const Curve& c = r.curves[ci];
                    const Surface wall = Surface::of(objects[size_t(c.a)]);
                    const Surface floor = c.b == kTerrain ? Surface::of(*ground) : Surface::of(objects[size_t(c.b)]);
                    std::string sweepWhy;
                    const bool ok = sweepCurve(b, c, wall, floor, ground, r.layers, o, &sweepWhy);
                    sc.method = ok ? "sweep" : "none";
                    sc.reason = why + (ok ? "; swept instead" : "; sweep failed too: " + sweepWhy);
                }
        out.warnings.push_back(objects[size_t(object)].name + ": " + why);
    }
    out.mesh = b.build();
    return out;
}
} // namespace seamwork
