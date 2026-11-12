#include "Skirt.h"

#include <lila/Boolean.h>
#include <lila/Curves.h>
#include <lila/Mesh.h>


#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
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
    /** For those rows, the wall's edge they fan round (upward): the wall border lies on it. */
    glm::dvec3 edge{0.0};
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
    // Square across the seam: on sloped ground the other surface's normal leans along it too.
    if (const glm::dvec3 along = t - surface * glm::dot(t, surface); glm::length(along) > 1e-9)
        d -= glm::normalize(along) * glm::dot(d, glm::normalize(along));
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
    {
        nw = r.na;
        // On sloped ground the profile leans along the seam, and the nearest face would flip from row to row.
        if (glm::length(r.edge) > 0.5)
            pw = r.p + r.edge * glm::dot(w * d, r.edge);
    }
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

/**
 * The ground round an object where the union's slab keeps no grid points: inside the object's outline on the ground (its
 * contact curves seen from above) and within reach of it, past the rings holeTop meshes there. The fillet's bevel clamps
 * its offset to the shortest edge it slides along, so grid points close to the seam would narrow the whole band.
 */
struct KeepOut
{
    struct Loop
    {
        std::vector<glm::dvec2> pts;
        /** Each segment's normal toward the ground, away from the object. */
        std::vector<glm::dvec2> out;
        bool closed = false;
        size_t segments() const { return closed ? pts.size() : pts.size() - 1; }
    };
    std::vector<Loop> loops;
    double reach = 0.0;
    double width = 0.0;

    double outlineDistance(const glm::dvec2& p) const
    {
        double d = 1e300;
        for (const Loop& l : loops)
            for (size_t i = 0; i < l.segments(); ++i)
            {
                const glm::dvec2 a = l.pts[i], ab = l.pts[(i + 1) % l.pts.size()] - a;
                const double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-12), 0.0, 1.0);
                d = std::min(d, glm::distance(p, a + ab * t));
            }
        return d;
    }
    /** Inside the footprint: an odd number of closed loops round the point, so a courtyard is ground. */
    bool inside(const glm::dvec2& p) const
    {
        bool in = false;
        for (const Loop& l : loops)
            if (l.closed)
                for (size_t i = 0, j = l.pts.size() - 1; i < l.pts.size(); j = i++)
                {
                    const glm::dvec2 a = l.pts[i], b = l.pts[j];
                    if ((a.y > p.y) != (b.y > p.y) && p.x < a.x + (b.x - a.x) * (p.y - a.y) / (b.y - a.y))
                        in = !in;
                }
        return in;
    }
    bool contains(const glm::dvec2& p) const { return !loops.empty() && (inside(p) || outlineDistance(p) < reach); }

    /** The object's contact curves through toSpace, seen from above (x, z), for a skirt width wide. */
    static KeepOut of(std::span<const Curve* const> curves, const glm::dmat4& toSpace, double width)
    {
        KeepOut k;
        k.reach = width * 1.5;
        k.width = width;
        const glm::dmat3 dirs(toSpace);
        std::vector<std::vector<glm::dvec2>> wallOuts;
        for (const Curve* c : curves)
        {
            Loop l;
            l.closed = c->closed;
            std::vector<glm::dvec2> wallOut;
            for (size_t i = 0; i < c->points.size(); ++i)
            {
                const glm::dvec3 q(toSpace * glm::dvec4(c->points[i], 1.0));
                if (!l.pts.empty() && glm::distance(glm::dvec2(q.x, q.z), l.pts.back()) < 1e-6)
                    continue;
                l.pts.push_back({q.x, q.z});
                const glm::dvec3 n = dirs * c->normalA[i];
                wallOut.push_back({n.x, n.z});
            }
            if (l.closed && l.pts.size() > 2 && glm::distance(l.pts.front(), l.pts.back()) < 1e-6)
            {
                l.pts.pop_back();
                wallOut.pop_back();
            }
            if (l.pts.size() < (l.closed ? 3u : 2u))
                continue;
            k.loops.push_back(std::move(l));
            wallOuts.push_back(std::move(wallOut));
        }
        for (size_t li = 0; li < k.loops.size(); ++li)
        {
            Loop& l = k.loops[li];
            for (size_t i = 0; i < l.segments(); ++i)
            {
                const size_t j = (i + 1) % l.pts.size();
                const glm::dvec2 a = l.pts[i], b = l.pts[j];
                const glm::dvec2 e = (b - a) / std::max(glm::distance(a, b), 1e-12);
                const glm::dvec2 n(e.y, -e.x);
                // A closed outline knows its own inside; an open one goes by the wall's normals.
                const bool flip = l.closed ? k.inside((a + b) * 0.5 + n * (width * 1e-3)) : glm::dot(n, wallOuts[li][i] + wallOuts[li][j]) < 0.0;
                l.out.push_back(flip ? -n : n);
            }
            const size_t segs = l.segments();
            for (size_t i = l.closed ? 0 : 1; i < (l.closed ? l.pts.size() : l.pts.size() - 1); ++i)
            {
                const glm::dvec2 n0 = l.out[(i + segs - 1) % segs], n1 = l.out[i % segs];
                if (glm::dot(n0, n1) > std::cos(glm::radians(30.0)))
                    continue;
                k.corners.push_back(l.pts[i]);
            }
        }
        return k;
    }

    /** Outline vertices that turn more than 30 degrees. */
    std::vector<glm::dvec2> corners;

    /**
     * Too near a corner for a slab edge to cross the seam: the bevel clamps its offset to the gap there (on a slope the
     * corner slides up the wall's edge and its neighbour across the seam; in a notch the corner's fillet meets both walls).
     */
    bool nearCorner(const glm::dvec2& p) const
    {
        for (const glm::dvec2& c : corners)
            if (glm::distance(p, c) < width * 1.25)
                return true;
        return false;
    }
};

/** A ring round one loop: points in loop order, each with the sample it came from (-1 for a mitre) and that sample's outline point. */
struct Ring
{
    std::vector<glm::dvec2> pts;
    std::vector<int> sample;
    std::vector<glm::dvec2> base;
};

/** A point on a loop's outline, evenly spaced along each straight run, with the run's ground normal. */
struct OutlineSample
{
    glm::dvec2 p, n;
    /** Starts a run: an outline vertex that turns, whose normal before it is nPrev. */
    bool vertex = false;
    glm::dvec2 nPrev{0.0};
};

/**
 * The loop resampled at most step apart: its turning vertices (the wall's edges) kept, the straight runs between them
 * evenly spaced, so short pieces of the contact curve (where it crosses terrain cells) leave no short slab edges.
 */
std::vector<OutlineSample> outlineSamples(const KeepOut::Loop& l, double step)
{
    const size_t n = l.pts.size();
    std::vector<size_t> turns;
    for (size_t i = 0; i < n; ++i)
        if (glm::dot(l.out[(i + n - 1) % n], l.out[i]) < std::cos(glm::radians(0.5)))
            turns.push_back(i);
    if (turns.empty())
        turns.push_back(0);
    std::vector<OutlineSample> out;
    for (size_t r = 0; r < turns.size(); ++r)
    {
        const size_t a = turns[r], b = turns[(r + 1) % turns.size()];
        std::vector<size_t> run = {a};
        for (size_t i = (a + 1) % n; run.size() <= n; i = (i + 1) % n)
        {
            run.push_back(i);
            if (i == b)
                break;
        }
        std::vector<double> at(run.size(), 0.0);
        for (size_t k = 1; k < run.size(); ++k)
            at[k] = at[k - 1] + glm::distance(l.pts[run[k - 1]], l.pts[run[k]]);
        const int steps = std::max(1, int(std::ceil(at.back() / step)));
        size_t seg = 0;
        for (int k = 0; k < steps; ++k)
        {
            const double s = at.back() * k / steps;
            while (seg + 2 < run.size() && at[seg + 1] < s)
                ++seg;
            const double t = (s - at[seg]) / std::max(at[seg + 1] - at[seg], 1e-12);
            OutlineSample q;
            q.p = glm::mix(l.pts[run[seg]], l.pts[run[seg + 1]], t);
            q.n = l.out[run[seg]];
            q.vertex = k == 0;
            q.nPrev = l.out[(a + n - 1) % n];
            out.push_back(q);
        }
    }
    return out;
}

/** 2D segments ab and cd cross (touching ends don't count). */
bool segmentsCross(const glm::dvec2& a, const glm::dvec2& b, const glm::dvec2& c, const glm::dvec2& d)
{
    auto side = [](const glm::dvec2& o, const glm::dvec2& p, const glm::dvec2& q) { return (p.x - o.x) * (q.y - o.y) - (p.y - o.y) * (q.x - o.x); };
    const double d1 = side(c, d, a), d2 = side(c, d, b), d3 = side(a, b, c), d4 = side(a, b, d);
    return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
}

/**
 * The outline offset by d onto the ground, mitred at every turn, without the points nearer the outline than d (an inner
 * corner's overlap). False when the ring isn't a simple loop clear of the outline, as round a notch narrower than 2d.
 */
bool offsetRing(const KeepOut& keep, const std::vector<OutlineSample>& samples, double d, double step, Ring& out)
{
    out = {};
    auto take = [&](const glm::dvec2& p, int sample, const glm::dvec2& base) {
        if (keep.inside(p) || keep.outlineDistance(p) < d * 0.96)
            return;
        if (!out.pts.empty() && glm::distance(out.pts.back(), p) < step * 0.2)
            return;
        out.pts.push_back(p);
        out.sample.push_back(sample);
        out.base.push_back(base);
    };
    for (size_t i = 0; i < samples.size(); ++i)
    {
        const OutlineSample& q = samples[i];
        if (q.vertex)
        {
            const glm::dvec2 n0 = q.nPrev, n1 = q.n;
            // Where the two offset lines meet; a hairpin's would run off, so it gets three points round it.
            if (const double c = glm::dot(n0, n1); c > -0.4)
                take(q.p + (n0 + n1) * (d / (1.0 + c)), -1, q.p);
            else
            {
                take(q.p + n0 * d, -1, q.p);
                take(q.p + glm::normalize(n0 + n1 + glm::dvec2(1e-9, 0.0)) * d * 1.5, -1, q.p);
                take(q.p + n1 * d, -1, q.p);
            }
        }
        take(q.p + q.n * d, int(i), q.p);
    }
    while (out.pts.size() > 1 && glm::distance(out.pts.back(), out.pts.front()) < step * 0.2)
    {
        out.pts.pop_back();
        out.sample.pop_back();
        out.base.pop_back();
    }
    const size_t m = out.pts.size();
    if (m < 3)
        return false;
    std::vector<double> along(m + 1, 0.0);
    for (size_t i = 0; i < m; ++i)
        along[i + 1] = along[i] + glm::distance(out.pts[i], out.pts[(i + 1) % m]);
    for (size_t i = 0; i < m; ++i)
    {
        const glm::dvec2 a = out.pts[i], b = out.pts[(i + 1) % m];
        if (keep.outlineDistance((a + b) * 0.5) < d * 0.9)
            return false;
        // Clear of itself too, as across a notch.
        for (size_t j = 0; j < m; ++j)
        {
            auto gapTo = [&](double s) {
                const double g = std::abs(along[i] - s);
                return std::min(g, along[m] - g);
            };
            if (std::min(gapTo(along[j]), gapTo(along[j + 1])) < d * 3.0)
                continue;
            const glm::dvec2 c = out.pts[j], cd = out.pts[(j + 1) % m] - c;
            const double t = std::clamp(glm::dot(a - c, cd) / std::max(glm::dot(cd, cd), 1e-12), 0.0, 1.0);
            if (glm::distance(a, c + cd * t) < d * 0.5)
                return false;
        }
        for (size_t j = i + 2; j < m; ++j)
            if ((j + 1) % m != i && segmentsCross(a, b, out.pts[j], out.pts[(j + 1) % m]))
                return false;
        for (const KeepOut::Loop& o : keep.loops)
            for (size_t j = 0; j < o.segments(); ++j)
                if (segmentsCross(a, b, o.pts[j], o.pts[(j + 1) % o.pts.size()]))
                    return false;
    }
    return true;
}

/**
 * The outline itself as a ring of slab points, so the seam runs along slab edges and no slab face crosses it at a slant:
 * the wall's edges and the samples, less the samples near a corner, where a seam point close to the corner clamps the bevel.
 */
Ring outlineRing(const KeepOut& keep, const std::vector<OutlineSample>& samples)
{
    Ring out;
    for (size_t i = 0; i < samples.size(); ++i)
        if (samples[i].vertex || !keep.nearCorner(samples[i].p))
        {
            out.pts.push_back(samples[i].p);
            out.sample.push_back(int(i));
            out.base.push_back(samples[i].p);
        }
    return out;
}

/** Appends a face, wound to face up. */
void upFace(const std::vector<glm::dvec3>& pts, std::vector<Index>& sizes, std::vector<Index>& idx, std::vector<Index> v)
{
    glm::dvec3 n(0.0);
    for (size_t i = 0; i < v.size(); ++i)
        n += glm::cross(pts[v[i]], pts[v[(i + 1) % v.size()]]);
    if (n.y < 0.0)
        std::reverse(v.begin(), v.end());
    sizes.push_back(Index(v.size()));
    idx.insert(idx.end(), v.begin(), v.end());
}

/** Rings round every loop: on the outline and 1.25 x the width out, with the reach the slab's hole needs to clear the outer one. */
struct HoleRings
{
    std::vector<Ring> inner, outer;
    double reach = 0.0;
};

/** The rings for keep's loops, or false with why when they don't fit. */
bool holeRings(const KeepOut& keep, HoleRings& out, std::string* why)
{
    const double w = keep.width;
    out = {};
    for (const KeepOut::Loop& l : keep.loops)
    {
        if (!l.closed)
        {
            *why = "the contact runs off the ground";
            return false;
        }
        const std::vector<OutlineSample> samples = outlineSamples(l, w);
        Ring outer;
        if (!offsetRing(keep, samples, w * 1.25, w, outer))
        {
            *why = "the ground round it is narrower than 2.5 x the width somewhere";
            return false;
        }
        for (const glm::dvec2& p : outer.pts)
            out.reach = std::max(out.reach, keep.outlineDistance(p) + w * 0.25);
        out.outer.push_back(std::move(outer));
        out.inner.push_back(outlineRing(keep, samples));
    }
    out.reach = std::max(out.reach, w * 1.5);
    return !out.outer.empty();
}

/**
 * Fills the slab's hole round the object (rim: each hole edge's end to its start). Between each loop's rings, faces
 * square across the seam (from samples away from corners) and one polygon round each corner, so every slab edge across
 * the seam reaches past the fillet; Delaunay triangles between the outer rings and the rim, and inside the inner rings.
 * False when the triangulation can't keep the rings.
 */
bool holeTop(const Heightfield& h, const HoleRings& rings, const KeepOut& keep, const std::map<Index, Index>& rim, std::vector<glm::dvec3>& pts,
             std::vector<Index>& sizes, std::vector<Index>& idx)
{
    auto add = [&](const glm::dvec2& p) {
        pts.push_back({p.x, h.heightAt(p.x, p.y), p.y});
        return Index(pts.size() - 1);
    };
    std::vector<std::vector<Index>> outerIds, innerIds;
    for (const Ring& r : rings.outer)
    {
        std::vector<Index>& ids = outerIds.emplace_back();
        for (const glm::dvec2& p : r.pts)
            ids.push_back(add(p));
    }
    for (const Ring& r : rings.inner)
    {
        std::vector<Index>& ids = innerIds.emplace_back();
        for (const glm::dvec2& p : r.pts)
            ids.push_back(add(p));
    }
    const size_t firstFace = sizes.size(), firstIdx = idx.size();
    // Delaunay inside closed loops of slab points: odd crossings in, so the rim's hole and islands work out.
    auto fill = [&](const std::vector<std::pair<Index, Index>>& loopEdges) {
        std::vector<glm::dvec2> flat;
        std::vector<Index> ids;
        std::map<Index, int> slot;
        auto slotOf = [&](Index p) {
            const auto [it, fresh] = slot.try_emplace(p, int(flat.size()));
            if (fresh)
            {
                flat.push_back({pts[p].x, pts[p].z});
                ids.push_back(p);
            }
            return it->second;
        };
        std::vector<std::pair<int, int>> segments;
        for (const auto& [a, b] : loopEdges)
            segments.push_back({slotOf(a), slotOf(b)});
        const lila::curve::CdtResult t = lila::curve::constrainedDelaunay(flat, segments, true);
        if (t.lostSegments > 0)
            return false;
        for (const std::array<int, 3>& tri : t.triangles)
            upFace(pts, sizes, idx, {ids[size_t(tri[0])], ids[size_t(tri[1])], ids[size_t(tri[2])]});
        return true;
    };
    std::vector<std::pair<Index, Index>> outside(rim.begin(), rim.end()), core;
    for (const std::vector<Index>& ids : outerIds)
        for (size_t i = 0; i < ids.size(); ++i)
            outside.push_back({ids[i], ids[(i + 1) % ids.size()]});
    for (const std::vector<Index>& ids : innerIds)
        for (size_t i = 0; i < ids.size(); ++i)
            core.push_back({ids[i], ids[(i + 1) % ids.size()]});
    auto undo = [&] {
        sizes.resize(firstFace);
        idx.resize(firstIdx);
        return false;
    };
    if (!fill(outside) || !fill(core))
        return undo();
    for (size_t li = 0; li < rings.outer.size(); ++li)
    {
        const Ring& in = rings.inner[li];
        const Ring& out = rings.outer[li];
        // Samples on both rings away from corners cross the seam square.
        std::vector<std::pair<size_t, size_t>> pairs;
        std::map<int, size_t> outerOf;
        for (size_t j = 0; j < out.pts.size(); ++j)
            if (out.sample[j] >= 0)
                outerOf[out.sample[j]] = j;
        for (size_t i = 0; i < in.pts.size(); ++i)
            if (const auto it = outerOf.find(in.sample[i]); in.sample[i] >= 0 && it != outerOf.end() && !keep.nearCorner(in.base[i]))
                pairs.push_back({i, it->second});
        if (pairs.size() < 2)
            return undo();
        const size_t ni = in.pts.size(), no = out.pts.size();
        for (size_t k = 0; k < pairs.size(); ++k)
        {
            const auto [i0, o0] = pairs[k];
            const auto [i1, o1] = pairs[(k + 1) % pairs.size()];
            std::vector<Index> face;
            for (size_t i = i0;; i = (i + 1) % ni)
            {
                face.push_back(innerIds[li][i]);
                if (i == i1)
                    break;
            }
            for (size_t o = o1;; o = (o + no - 1) % no)
            {
                face.push_back(outerIds[li][o]);
                if (o == o0)
                    break;
            }
            upFace(pts, sizes, idx, std::move(face));
        }
    }
    return true;
}

/**
 * A closed slab of the heightfield over cells [x0, x1) x [z0, z1), down to bottom, in the heightfield's space, and its
 * top faces alone in top. Cells touching keep's zone are left out and the hole is meshed by holeTop; where its rings
 * don't fit, the grid stays whole and note says why.
 */
Mesh slab(const Heightfield& h, int x0, int z0, int x1, int z1, double bottom, KeepOut keep, Mesh& top, std::string* note)
{
    const int cx = x1 - x0 + 1, cz = z1 - z0 + 1;
    std::vector<glm::dvec3> pts;
    auto up = [&](int i, int j) { return Index(j * cx + i); };
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
    auto onOutline = [&](Index a, Index b) {
        const int ia = int(a) % cx, ja = int(a) / cx, ib = int(b) % cx, jb = int(b) / cx;
        return (ia == ib && (ia == 0 || ia == cx - 1)) || (ja == jb && (ja == 0 || ja == cz - 1));
    };
    // The grid's top, without the cells touching keep's zone when holed; false when the hole can't be meshed.
    const size_t gridPoints = pts.size();
    auto gridTop = [&](const HoleRings* rings) {
        sizes.clear();
        idx.clear();
        pts.resize(gridPoints);
        std::vector<uint8_t> out(size_t(cx) * size_t(cz), 0);
        if (rings)
            for (int j = 1; j + 1 < cz; ++j)
                for (int i = 1; i + 1 < cx; ++i)
                    out[size_t(up(i, j))] = keep.contains(h.samplePos(x0 + i, z0 + j));
        // Each kept top face's edges, to find the hole's rim: an edge with no twin that isn't on the slab's outline.
        std::map<std::pair<Index, Index>, int> edges;
        for (int j = 0; j + 1 < cz; ++j)
            for (int i = 0; i + 1 < cx; ++i)
            {
                const Index q[4] = {up(i, j), up(i, j + 1), up(i + 1, j + 1), up(i + 1, j)};
                if (out[size_t(q[0])] || out[size_t(q[1])] || out[size_t(q[2])] || out[size_t(q[3])])
                    continue;
                sizes.push_back(4);
                idx.insert(idx.end(), std::begin(q), std::end(q));
                for (int k = 0; k < 4; ++k)
                    ++edges[{q[k], q[(k + 1) % 4]}];
            }
        std::map<Index, Index> rim;
        for (const auto& [e, n] : edges)
            if (!edges.count({e.second, e.first}) && !onOutline(e.first, e.second))
                rim[e.second] = e.first;
        return !rings || (!rim.empty() && holeTop(h, *rings, keep, rim, pts, sizes, idx));
    };
    HoleRings rings;
    std::string why;
    if (!holeRings(keep, rings, &why))
        *note = "slab hole not cut: " + why + ", so the fillet can come out narrower";
    else
    {
        keep.reach = rings.reach;
        if (!gridTop(&rings))
            *note = "slab hole not cut: its rings didn't triangulate, so the fillet can come out narrower";
        else
            note->clear();
    }
    if (!note->empty())
        gridTop(nullptr);
    top = Mesh::fromPolygons(pts, sizes, idx);
    for (int j = 0; j + 1 < cz; ++j)
        for (int i = 0; i + 1 < cx; ++i)
        {
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
        side(up(i, 0), up(i + 1, 0));
        side(up(i, cz - 1), up(i + 1, cz - 1));
    }
    for (int j = 0; j + 1 < cz; ++j)
    {
        side(up(0, j), up(0, j + 1));
        side(up(cx - 1, j), up(cx - 1, j + 1));
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
    /** The wall's edge at the corner, upward. */
    glm::dvec3 edge{0.0, 1.0, 0.0};
    double la = 0.0, lb = 0.0;
    const Curve* curve = nullptr;
    /** The corner's distance along its curve, and how far past the cut planes a point may lie and still be this corner's. */
    double at = 0.0, margin = 0.0;
    /** Signed distances to the cut planes: the cap's wedge is where fa > 0 and fb < 0. */
    double fa(const glm::dvec3& p) const { return glm::dot(p - pa, tIn); }
    double fb(const glm::dvec3& p) const { return glm::dot(p - pb, tOut); }
    /** The point on the seam s along it from the corner (negative before it). */
    glm::dvec3 seamAt(double s, glm::dvec3* tangent = nullptr) const;
    /** The band near p belongs to this corner: its closest seam point lies between the cut planes, give or take margin. */
    bool owns(const glm::dvec3& p) const;
};

/** Each point's distance along the curve, and the whole length (closing segment included when closed). */
std::vector<double> arcLengths(const Curve& c, double* total)
{
    const size_t n = c.points.size();
    std::vector<double> at(n, 0.0);
    for (size_t k = 1; k < n; ++k)
        at[k] = at[k - 1] + glm::length(c.points[k] - c.points[k - 1]);
    *total = (n ? at[n - 1] : 0.0) + (c.closed && n > 1 ? glm::length(c.points[0] - c.points[n - 1]) : 0.0);
    return at;
}

glm::dvec3 OuterCorner::seamAt(double s, glm::dvec3* tangent) const
{
    double total;
    const std::vector<double> arc = arcLengths(*curve, &total);
    const size_t n = arc.size();
    double u = this->at + s;
    if (curve->closed)
        u -= std::floor(u / total) * total;
    else
        u = std::clamp(u, 0.0, arc[n - 1]);
    for (size_t k = 0; k < (curve->closed ? n : n - 1); ++k)
    {
        const double end = k + 1 < n ? arc[k + 1] : total;
        if (u <= end)
        {
            const glm::dvec3 a = curve->points[k], b = curve->points[(k + 1) % n];
            if (tangent)
                *tangent = unitTo(b - a, tIn);
            return glm::mix(a, b, (u - arc[k]) / std::max(end - arc[k], 1e-12));
        }
    }
    if (tangent)
        *tangent = tOut;
    return curve->points[curve->closed ? 0 : n - 1];
}

bool OuterCorner::owns(const glm::dvec3& p) const
{
    double total;
    const std::vector<double> arc = arcLengths(*curve, &total);
    const size_t n = arc.size();
    double best = 1e300, where = 0.0;
    for (size_t k = 0; k < (curve->closed ? n : n - 1); ++k)
    {
        const glm::dvec3 a = curve->points[k], ab = curve->points[(k + 1) % n] - a;
        const double t = std::clamp(glm::dot(p - a, ab) / std::max(glm::dot(ab, ab), 1e-12), 0.0, 1.0);
        if (const double d = glm::distance(p, a + ab * t); d < best)
        {
            best = d;
            where = arc[k] + t * glm::length(ab);
        }
    }
    double s = where - this->at;
    if (curve->closed)
        s -= std::round(s / total) * total;
    return s > -la - margin && s < lb + margin;
}

glm::dvec3 acrossUp(const glm::dvec3& v, const glm::dvec3& up) { return v - up * glm::dot(v, up); }

std::vector<OuterCorner> outerCorners(const Curve& c, const SkirtOptions& o)
{
    std::vector<OuterCorner> out;
    const size_t n = c.points.size();
    if (n < 3)
        return out;
    const glm::dvec3 up = unitTo(o.up, {0, 1, 0});
    double total;
    const std::vector<double> arc = arcLengths(c, &total);
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
        oc.edge = unitTo(glm::cross(c.normalA[prev], c.normalA[next]), up);
        if (glm::dot(oc.edge, up) < 0.0)
            oc.edge = -oc.edge;
        oc.curve = &c;
        oc.at = arc[k];
        oc.margin = o.width;
        out.push_back(oc);
    }
    const double reach = 1.5 * std::max(o.width, o.radius);
    auto gap = [&](size_t a, size_t b) { return b >= a ? arc[b] - arc[a] : total - arc[a] + arc[b]; };
    for (size_t i = 0; i < out.size(); ++i)
    {
        const size_t k = found[i];
        double before = arc[k], after = (c.closed ? total : arc[n - 1]) - arc[k];
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
                before = (arc[k] - arc[found[i - 1]]) * 0.5;
            if (i + 1 < out.size())
                after = (arc[found[i + 1]] - arc[k]) * 0.5;
        }
        OuterCorner& oc = out[i];
        oc.la = std::min(reach, before);
        oc.lb = std::min(reach, after);
        oc.pa = oc.seamAt(-oc.la);
        oc.pb = oc.seamAt(oc.lb);
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
    // Points lying on the plane can leave a sliver of nothing but plane points.
    glm::dvec3 area(0.0);
    for (size_t i = 0; i < out.size(); ++i)
        area += glm::cross(out[i].p, out[(i + 1) % out.size()].p);
    return out.size() >= 3 && glm::length(area) > 1e-8 ? out : BandPoly{};
}

/** q moved onto the ground, with the ground's normal there. */
BandPoint onGround(const Surface& floor, BandPoint q)
{
    if (glm::dvec3 at, n; floor.project(q.p, at, n))
        q = {at, n, 1.0};
    return q;
}

/** Points on the ground between a and b, in order from a: halving the chord wherever its middle is off the ground. */
void splitOnGround(const Surface& floor, const BandPoint& a, const BandPoint& b, BandPoly& out)
{
    const glm::dvec3 mid = (a.p + b.p) * 0.5;
    const BandPoint m = onGround(floor, {mid, a.n, 1.0});
    if (glm::distance(m.p, mid) < 0.004 || glm::distance(a.p, b.p) < 0.5)
        return;
    splitOnGround(floor, a, m, out);
    out.push_back(m);
    splitOnGround(floor, m, b, out);
}

/** Splits b's faces from firstFace on along their ground-border edges where the ground bends away, as groundBorder does. */
void groundEdges(Builder& b, size_t firstFace, const Surface& floor, const std::function<glm::vec3(const glm::dvec3&)>& color)
{
    size_t at = 0;
    for (size_t f = 0; f < firstFace; ++f)
        at += b.sizes[f];
    std::vector<Index> sizes(b.sizes.begin(), b.sizes.begin() + std::ptrdiff_t(firstFace));
    std::vector<Index> idx(b.idx.begin(), b.idx.begin() + std::ptrdiff_t(at));
    for (size_t f = firstFace; f < b.sizes.size(); ++f)
    {
        const size_t n = b.sizes[f];
        const size_t start = idx.size();
        for (size_t k = 0; k < n; ++k)
        {
            const Index i0 = b.idx[at + k], i1 = b.idx[at + (k + 1) % n];
            idx.push_back(i0);
            if (b.side[i0] < 1.0f || b.side[i1] < 1.0f)
                continue;
            const bool fwd = std::tie(b.pts[i0].x, b.pts[i0].y, b.pts[i0].z) < std::tie(b.pts[i1].x, b.pts[i1].y, b.pts[i1].z);
            const Index lo = fwd ? i0 : i1, hi = fwd ? i1 : i0;
            BandPoly between;
            splitOnGround(floor, {b.pts[lo], b.nrm[lo], 1.0}, {b.pts[hi], b.nrm[hi], 1.0}, between);
            if (!fwd)
                std::reverse(between.begin(), between.end());
            for (const BandPoint& q : between)
                idx.push_back(b.shared(q.p, q.n, 1.0, glm::vec2(0.0f, 1.0f), color(q.p)));
        }
        sizes.push_back(Index(idx.size() - start));
        at += n;
    }
    b.sizes = std::move(sizes);
    b.idx = std::move(idx);
}

/**
 * Moves the band's ground border onto the true ground: the slab's faces are chords of it. Border edges where the ground
 * bends away between their ends get points along them, so the border follows it too.
 */
void groundBorder(std::vector<BandPoly>& polys, const Surface& floor)
{
    for (BandPoly& poly : polys)
    {
        BandPoly fine;
        for (size_t i = 0; i < poly.size(); ++i)
        {
            const BandPoint& a = poly[i];
            const BandPoint& c = poly[(i + 1) % poly.size()];
            if (a.side < 1.0)
            {
                fine.push_back(a);
                continue;
            }
            fine.push_back(onGround(floor, a));
            if (c.side < 1.0)
                continue;
            // Both faces on an edge split it alike: from its lesser end, so they weld.
            const bool fwd = std::tie(a.p.x, a.p.y, a.p.z) < std::tie(c.p.x, c.p.y, c.p.z);
            BandPoly between;
            splitOnGround(floor, onGround(floor, fwd ? a : c), onGround(floor, fwd ? c : a), between);
            if (!fwd)
                std::reverse(between.begin(), between.end());
            fine.insert(fine.end(), between.begin(), between.end());
        }
        poly = std::move(fine);
    }
}

/** The polygons with each corner's wedge taken out; a piece that crosses both planes is split in two. */
std::vector<BandPoly> cutCorners(std::vector<BandPoly> polys, std::span<const OuterCorner> corners)
{
    for (const OuterCorner& oc : corners)
    {
        std::vector<BandPoly> next;
        for (BandPoly& poly : polys)
        {
            // The cut planes run on past the corner, across other walls' bands.
            if (std::none_of(poly.begin(), poly.end(), [&](const BandPoint& q) { return oc.owns(q.p); }))
            {
                next.push_back(std::move(poly));
                continue;
            }
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
        glm::dvec3 t;
        const glm::dvec3 p = oc.seamAt(-oc.la * (na - k) / na, &t);
        rows.push_back({p, t, oc.nIn, groundNormal(p)});
    }
    const double turn = std::acos(std::clamp(glm::dot(oc.nIn, oc.nOut), -1.0, 1.0));
    const int fan = std::max(2, int(std::ceil(glm::degrees(turn) / 12.0)));
    const glm::dvec3 nc = groundNormal(oc.c);
    glm::dvec3 t0, t1;
    oc.seamAt(-1e-6, &t0);
    oc.seamAt(1e-6, &t1);
    for (int k = 0; k <= fan; ++k)
    {
        const double s = double(k) / fan;
        rows.push_back({oc.c, slerpDir(t0, t1, s), slerpDir(oc.nIn, oc.nOut, s), nc, 0.0, true, oc.edge});
    }
    for (int k = 1; k <= nb; ++k)
    {
        glm::dvec3 t;
        const glm::dvec3 p = oc.seamAt(oc.lb * k / nb, &t);
        rows.push_back({p, t, oc.nOut, groundNormal(p)});
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

/**
 * A ground mesh seen from above round the object's footprint, resampled at a third of the skirt width and closed into a
 * world-space slab. The union needs a closed solid with points near the seam, which an open floor or one huge face is not.
 */
bool meshFloorSlab(const Object& floorObj, const Object& obj, std::span<const Curve* const> curves, const SkirtOptions& o, Mesh& out, Mesh& top,
                   std::string* note, std::string* why)
{
    const lila::geo::Bounds lb = obj.bvh->bounds();
    glm::dvec3 lo(1e300), hi(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 c((k & 1) ? lb.max.x : lb.min.x, (k & 2) ? lb.max.y : lb.min.y, (k & 4) ? lb.max.z : lb.min.z);
        const glm::dvec3 q(obj.world * glm::dvec4(c, 1.0));
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const double margin = o.width * 3.0;
    const double ex = hi.x - lo.x + 2.0 * margin, ez = hi.z - lo.z + 2.0 * margin;
    const double cell = std::max(o.width / 3.0, std::max(ex, ez) / 256.0);
    const int nx = int(std::ceil(ex / cell)) + 1, nz = int(std::ceil(ez / cell)) + 1;
    const glm::dvec2 origin(lo.x - margin, lo.z - margin);

    Mesh world = *floorObj.mesh;
    for (glm::dvec3& p : world.positions())
        p = glm::dvec3(floorObj.world * glm::dvec4(p, 1.0));
    const double fill = world.bounds().min.y;
    size_t covered = 0;
    const Heightfield h = lila::geo::meshToHeightfield(world, nx, nz, cell, origin, float(fill), &covered);
    if (covered == 0)
    {
        *why = "the ground mesh doesn't reach under the object";
        return false;
    }
    out = slab(h, 0, 0, nx - 1, nz - 1, std::min(lo.y, double(h.range().x)) - 200.0, KeepOut::of(curves, glm::dmat4(1.0), o.width), top, note);
    return true;
}

/**
 * The object's ground skirt by boolean union with a fillet, cut back to the fillet band, outer corners capped round.
 * False (with why) when it fails; note says what it did short of the full width.
 */
bool unionObject(Builder& b, int object, std::span<const Object> objects, const Ground* ground, int groundObject, const std::vector<std::string>& layers,
                 const std::vector<const Curve*>& curves, const SkirtOptions& o, std::string* note, std::string* why)
{
    const Object& obj = objects[size_t(object)];
    Mesh ground3;
    lila::scene::BooleanOperand floorOp;
    Surface floor;
    // The slab's top as built: the union's points lie on it exactly, where the true ground can bend away between its points.
    Object slabTop;
    auto top = std::make_shared<Mesh>();
    if (groundObject >= 0)
    {
        if (!meshFloorSlab(objects[size_t(groundObject)], obj, curves, o, ground3, *top, note, why))
            return false;
        floorOp = {&ground3, glm::dmat4(1.0)};
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
        ground3 = slab(h, x0, z0, x1, z1, bottom, KeepOut::of(curves, glm::inverse(ground->world), o.width), *top, note);
        floorOp = {&ground3, ground->world};
        floor = Surface::of(*ground);
        slabTop.world = ground->world;
    }
    slabTop.bvh = std::make_shared<const MeshBvh>(*top);
    slabTop.mesh = std::move(top);
    const Surface onSlab = Surface::of(slabTop);
    const lila::scene::BooleanOperand ops[] = {floorOp, {obj.mesh.get(), obj.world}};
    lila::scene::BooleanOptions bo;
    bo.op = lila::scene::BooleanOp::Union;
    bo.filletWidth = o.width;
    bo.filletSegments = std::max(2, o.segments);
    bo.markFillet = true;
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
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            glm::dvec3 at, up;
            wall.project(m.position(p), at, nw[p], &dw[p]);
            onSlab.project(m.position(p), at, up, &dg[p]);
            floor.project(m.position(p), at, ng[p]);
        }
    // The fillet band: the boolean's fillet faces.
    const auto fillet = m.faceAttrs().get<int32_t>(lila::scene::kFilletFaceAttr);
    std::vector<Index> keep;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f) && f < fillet.size() && fillet[f] == 1)
            keep.push_back(f);
    if (keep.empty())
    {
        *why = "union made no fillet band";
        return false;
    }
    // Its ground border: the band's open edges off the wall. On bent ground the bevel's points can sit a little off the slab.
    std::vector<int> edgeUse(m.edgeCapacity(), 0);
    for (Index f : keep)
        m.forEachFaceCorner(f, [&](Index c) { ++edgeUse[m.cornerEdge(c)]; });
    std::vector<uint8_t> onGround(m.pointCapacity(), 0);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && edgeUse[e] == 1)
            for (int k = 0; k < 2; ++k)
                if (const Index p = m.edgePoint(e, k); dw[p] > kOn)
                    onGround[p] = 1;
    std::vector<glm::dvec3> smooth(m.pointCapacity(), glm::dvec3(0.0));
    for (Index f : keep)
    {
        const glm::dvec3 a = m.faceAreaVector(f);
        m.forEachFaceCorner(f, [&](Index c) { smooth[m.cornerPoint(c)] += a; });
    }
    auto bandPoint = [&](Index p) {
        const double side = dw[p] <= kOn ? 0.0 : onGround[p] ? 1.0 : std::clamp(dw[p] / std::max(1e-9, dw[p] + dg[p]), 0.0, 0.999);
        const glm::dvec3 n = dw[p] <= kOn ? nw[p] : onGround[p] ? ng[p] : unitTo(smooth[p], ng[p]);
        return BandPoint{m.position(p), n, side};
    };
    // Convex pieces only: clipping a bent face (a strip round a corner) on a plane would bridge its arms.
    auto convex = [&](Index f) {
        const glm::dvec3 n = m.faceAreaVector(f);
        const std::vector<Index> cs = m.faceCorners(f);
        for (size_t i = 0; i < cs.size(); ++i)
        {
            const glm::dvec3 a = m.position(m.cornerPoint(cs[(i + cs.size() - 1) % cs.size()])), p = m.position(m.cornerPoint(cs[i])),
                             c = m.position(m.cornerPoint(cs[(i + 1) % cs.size()]));
            if (glm::dot(glm::cross(p - a, c - p), n) < -1e-9 * glm::dot(n, n))
                return false;
        }
        return true;
    };
    std::vector<BandPoly> polys;
    std::vector<Index> tris;
    for (Index f : keep)
    {
        if (convex(f))
        {
            BandPoly& poly = polys.emplace_back();
            m.forEachFaceCorner(f, [&](Index c) { poly.push_back(bandPoint(m.cornerPoint(c))); });
            continue;
        }
        tris.clear();
        lila::geo::triangulateFace(m, f, tris);
        for (size_t t = 0; t + 2 < tris.size(); t += 3)
            polys.push_back({bandPoint(m.cornerPoint(tris[t])), bandPoint(m.cornerPoint(tris[t + 1])), bandPoint(m.cornerPoint(tris[t + 2]))});
    }
    std::vector<OuterCorner> corners;
    for (const Curve* c : curves)
        for (const OuterCorner& oc : outerCorners(*c, o))
            corners.push_back(oc);
    groundBorder(polys, floor);
    polys = cutCorners(std::move(polys), corners);
    // Points the cut made on a border take their surface's normal, not one interpolated along the edge, and sit on the ground.
    for (BandPoly& poly : polys)
        for (BandPoint& q : poly)
        {
            glm::dvec3 at, n;
            if (q.side <= 0.0 && wall.project(q.p, at, n))
                q.n = n;
            else if (q.side >= 1.0 && floor.project(q.p, at, n))
                q = {at, n, 1.0};
        }
    for (const BandPoly& poly : polys)
    {
        std::vector<Index> face;
        for (const BandPoint& q : poly)
            face.push_back(b.shared(q.p, q.n, q.side, glm::vec2(0.0f, float(q.side)), groundColor(ground, layers, o, q.p)));
        b.polygon(std::move(face));
    }
    const size_t capFaces = b.sizes.size();
    // Each cap starts and ends on the band's points on its cut planes, wall border first.
    constexpr double kPlane = 1e-6;
    for (const OuterCorner& oc : corners)
    {
        std::vector<std::pair<double, Index>> onA, onB;
        for (const BandPoly& poly : polys)
            for (const BandPoint& q : poly)
            {
                if (!oc.owns(q.p))
                    continue;
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
    groundEdges(b, capFaces, floor, [&](const glm::dvec3& p) { return groundColor(ground, layers, o, p); });
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
        std::string note;
        if (unionObject(b, object, objects, ground, g >= 0 ? g : -1, r.layers, own, o, &note, &why))
        {
            if (!note.empty())
            {
                for (size_t ci : curves)
                    for (SkirtCurve& sc : out.curves)
                        if (sc.curve == ci)
                            sc.reason += "; " + note;
                out.warnings.push_back(objects[size_t(object)].name + ": " + note);
            }
            continue;
        }
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
