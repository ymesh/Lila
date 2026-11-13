#include "Seams.h"

#include <lila/Intersect.h>

#include <glm/gtc/matrix_transform.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <optional>
#include <climits>
#include <cmath>
#include <map>
#include <numbers>
#include <set>
#include <unordered_map>

namespace seamwork
{
using lila::geo::Index;
using lila::geo::IntersectionPolyline;
using lila::geo::kInvalid;

namespace
{

glm::dmat3 normalMatrix(const glm::dmat4& m) { return glm::transpose(glm::inverse(glm::dmat3(m))); }

glm::dvec3 xformPoint(const glm::dmat4& m, const glm::dvec3& p) { return glm::dvec3(m * glm::dvec4(p, 1.0)); }

glm::dvec3 safeNormalize(const glm::dvec3& v, const glm::dvec3& fallback)
{
    const double l = glm::length(v);
    return l > 1e-12 ? v / l : fallback;
}

/** World face normal of an object's face. */
glm::dvec3 faceNormalWorld(const Object& o, Index face, const glm::dvec3& fallback)
{
    if (face == kInvalid || !o.mesh->faceValid(face))
        return fallback;
    return safeNormalize(normalMatrix(o.world) * o.mesh->faceNormal(face), fallback);
}

struct Ray
{
    glm::dvec3 origin, dir;
};

/** A world ray in an object's space; its parameter stays the world distance when dir is unit. */
Ray toLocal(const glm::dmat4& inv, const glm::dvec3& origin, const glm::dvec3& dir)
{
    return {xformPoint(inv, origin), glm::dmat3(inv) * dir};
}

bool rayBox(const glm::dvec3& o, const glm::dvec3& d, double tMax, const glm::dvec3& lo, const glm::dvec3& hi)
{
    double t0 = 0.0, t1 = tMax;
    for (int k = 0; k < 3; ++k)
    {
        if (std::abs(d[k]) < 1e-15)
        {
            if (o[k] < lo[k] || o[k] > hi[k])
                return false;
            continue;
        }
        double a = (lo[k] - o[k]) / d[k], b = (hi[k] - o[k]) / d[k];
        if (a > b)
            std::swap(a, b);
        t0 = std::max(t0, a);
        t1 = std::min(t1, b);
        if (t0 > t1)
            return false;
    }
    return true;
}

/** An object's world bounds from its tree's corners. */
void worldBounds(const Object& o, glm::dvec3& lo, glm::dvec3& hi)
{
    const lila::geo::Bounds b = o.bvh->bounds();
    lo = glm::dvec3(1e300);
    hi = glm::dvec3(-1e300);
    for (int k = 0; k < 8; ++k)
    {
        const glm::dvec3 c((k & 1) ? b.max.x : b.min.x, (k & 2) ? b.max.y : b.min.y, (k & 4) ? b.max.z : b.min.z);
        const glm::dvec3 w = xformPoint(o.world, c);
        lo = glm::min(lo, w);
        hi = glm::max(hi, w);
    }
}

/** Shared per-object facts the passes need. */
struct Prepared
{
    std::vector<glm::dmat4> inv;
    std::vector<glm::dvec3> lo, hi;
    glm::dmat4 groundInv{1.0};
};

Prepared prepare(std::span<const Object> objects, const Ground* ground)
{
    Prepared p;
    p.inv.resize(objects.size());
    p.lo.resize(objects.size());
    p.hi.resize(objects.size());
    for (size_t i = 0; i < objects.size(); ++i)
    {
        p.inv[i] = glm::inverse(objects[i].world);
        worldBounds(objects[i], p.lo[i], p.hi[i]);
    }
    if (ground)
        p.groundInv = glm::inverse(ground->world);
    return p;
}

/** True when a ray from origin hits the heightfield within range (world). */
bool heightfieldBlocks(const Ground& g, const glm::dmat4& inv, const glm::dvec3& origin, const glm::dvec3& dir, double range)
{
    const Heightfield& h = *g.field;
    const glm::dvec3 o = xformPoint(inv, origin);
    const glm::dvec3 d = glm::dmat3(inv) * dir;
    const double dl = glm::length(d);
    if (dl < 1e-12)
        return false;
    const double step = h.cell * 0.5 / dl;
    for (double t = step; t <= range; t += step)
    {
        const glm::dvec3 q = o + d * t;
        if (!h.contains(q.x, q.z))
        {
            // Leaving the grid while climbing means nothing above blocks.
            if (d.y >= 0.0)
                return false;
            continue;
        }
        if (q.y < h.heightAt(q.x, q.z) - 1e-3)
            return true;
        // Above the highest sample and climbing: clear.
        if (d.y >= 0.0 && q.y > h.range().y)
            return false;
    }
    return false;
}

bool anyBlocks(std::span<const Object> objects, const Prepared& p, const Ground* ground, const glm::dvec3& origin, const glm::dvec3& dir, double range)
{
    for (size_t i = 0; i < objects.size(); ++i)
    {
        if (!rayBox(origin, dir, range, p.lo[i], p.hi[i]))
            continue;
        const Ray r = toLocal(p.inv[i], origin, dir);
        if (objects[i].bvh->occluded(r.origin, r.dir, range))
            return true;
    }
    return ground && heightfieldBlocks(*ground, p.groundInv, origin, dir, range);
}

/** The nearest object surface along a ray within range: its object and whether it was met from behind; nullopt for none. */
std::optional<std::pair<size_t, lila::geo::BvhHit>> nearestHit(std::span<const Object> objects, const Prepared& p, const glm::dvec3& origin,
                                                      const glm::dvec3& dir, double range, bool backfaces = true)
{
    std::optional<std::pair<size_t, lila::geo::BvhHit>> best;
    for (size_t i = 0; i < objects.size(); ++i)
    {
        if (!rayBox(origin, dir, range, p.lo[i], p.hi[i]))
            continue;
        const Ray r = toLocal(p.inv[i], origin, dir);
        lila::geo::BvhHit h;
        if (objects[i].bvh->raycast(r.origin, r.dir, h, best ? best->second.t : range, backfaces))
            best = {i, h};
    }
    return best;
}

/**
 * A crease point buried in another object where the two meet, as the bake's AO sees it: the surface met along n is a
 * back face of an object that also closes behind the point along -n. exit is just outside it on the nearer side.
 */
bool buriedAt(std::span<const Object> objects, const Prepared& p, const glm::dvec3& origin, const glm::dvec3& n, double range, double step,
              glm::dvec3& exit)
{
    const auto up = nearestHit(objects, p, origin, n, range);
    if (!up || up->second.frontFacing)
        return false;
    const size_t j = up->first;
    const Ray r = toLocal(p.inv[j], origin, -n);
    lila::geo::BvhHit down;
    if (!objects[j].bvh->raycast(r.origin, r.dir, down, range, true) || down.frontFacing)
        return false;
    exit = up->second.t <= down.t ? origin + n * (up->second.t + step) : origin - n * (down.t + step);
    return true;
}

/** n cosine-weighted directions around axis, the same every call. */
std::vector<glm::dvec3> hemisphere(const glm::dvec3& axis, int n)
{
    const glm::dvec3 w = safeNormalize(axis, {0, 1, 0});
    const glm::dvec3 helper = std::abs(w.y) < 0.9 ? glm::dvec3(0, 1, 0) : glm::dvec3(1, 0, 0);
    const glm::dvec3 u = glm::normalize(glm::cross(helper, w));
    const glm::dvec3 v = glm::cross(w, u);
    std::vector<glm::dvec3> out;
    out.reserve(size_t(std::max(n, 1)));
    const double golden = kPi * (3.0 - std::sqrt(5.0));
    for (int k = 0; k < n; ++k)
    {
        const double r = std::sqrt((k + 0.5) / n);
        const double phi = k * golden;
        const double x = r * std::cos(phi), y = r * std::sin(phi);
        const double z = std::sqrt(std::max(0.0, 1.0 - r * r));
        out.push_back(glm::normalize(u * x + v * y + w * z));
    }
    return out;
}

double wedgeAngle(const glm::dvec3& na, const glm::dvec3& nb)
{
    return 180.0 - glm::degrees(std::acos(std::clamp(glm::dot(na, nb), -1.0, 1.0)));
}

int heaviestLayer(const Heightfield& h, const std::vector<std::string>& layers, double x, double z)
{
    int best = -1;
    double w = 0.0;
    for (size_t k = 0; k < layers.size(); ++k)
        if (const double v = h.maskAt("layer_" + layers[k], x, z); v > w)
        {
            w = v;
            best = int(k);
        }
    return best;
}

/** A polyline split into runs of kept points (wrapping a closed one), each with at least two points. */
std::vector<std::vector<size_t>> keptRuns(size_t n, bool closed, const std::vector<uint8_t>& keep, bool* wholeClosed)
{
    std::vector<std::vector<size_t>> runs;
    *wholeClosed = false;
    if (std::all_of(keep.begin(), keep.end(), [](uint8_t k) { return k != 0; }))
    {
        std::vector<size_t> all(n);
        for (size_t i = 0; i < n; ++i)
            all[i] = i;
        *wholeClosed = closed;
        runs.push_back(std::move(all));
        return runs;
    }
    // Start just after a dropped point so a closed curve's run wraps once.
    size_t start = 0;
    if (closed)
        while (keep[start])
            ++start;
    std::vector<size_t> cur;
    for (size_t k = 0; k < n; ++k)
    {
        const size_t i = closed ? (start + k) % n : k;
        if (keep[i])
            cur.push_back(i);
        else if (!cur.empty())
        {
            if (cur.size() >= 2)
                runs.push_back(cur);
            cur.clear();
        }
    }
    if (cur.size() >= 2)
        runs.push_back(cur);
    return runs;
}

double polylineLength(const std::vector<glm::dvec3>& p, bool closed)
{
    double l = 0.0;
    for (size_t k = 0; k + 1 < p.size(); ++k)
        l += glm::length(p[k + 1] - p[k]);
    if (closed && p.size() > 2)
        l += glm::length(p.front() - p.back());
    return l;
}

/** Fills the per-point arrays every curve carries, given points and the two normals. */
void finishCurve(Curve& c, std::span<const Object> objects, const Prepared& prep, const Ground* ground, const std::vector<std::string>& layers,
                 const Options& o)
{
    const size_t n = c.points.size();
    c.angle.resize(n);
    c.upFacing.resize(n);
    c.exposure.resize(n);
    c.layer.assign(n, -1);
    if (c.depth.size() != n)
        c.depth.assign(n, 0.0);
    if (c.gap.size() != n)
        c.gap.assign(n, 0.0);
    const glm::dvec3 up = safeNormalize(o.up, {0, 1, 0});
    const auto dirs = hemisphere(up, std::max(1, o.exposureRays));
    tbb::parallel_for(size_t(0), n, [&](size_t k) {
        const glm::dvec3 na = c.normalA[k], nb = c.normalB[k];
        c.angle[k] = c.b == kNone ? 0.0 : wedgeAngle(na, nb);
        const glm::dvec3 bis = safeNormalize(na + nb, up);
        c.upFacing[k] = std::clamp(glm::dot(bis, up), 0.0, 1.0);
        if (o.exposureRays > 0)
        {
            const glm::dvec3 origin = c.points[k] + bis * 1.0 + up * 0.5;
            double open = 0.0;
            for (const glm::dvec3& d : dirs)
                if (!anyBlocks(objects, prep, ground, origin, d, o.exposureRange))
                    open += 1.0;
            c.exposure[k] = open / double(dirs.size());
        }
        else
            c.exposure[k] = 1.0;
        if (ground && !layers.empty())
        {
            const glm::dvec3 l = xformPoint(prep.groundInv, c.points[k]);
            c.layer[k] = heaviestLayer(*ground->field, layers, l.x, l.z);
        }
    });
}

/** Distance a goes into b below a crossing point (along -nb, from just inside a), cm. */
double depthAt(std::span<const Object> objects, const Prepared& prep, int a, int b, const glm::dvec3& p, const glm::dvec3& na, const glm::dvec3& nb)
{
    constexpr double kEps = 0.05;
    constexpr double kMax = 1e5;
    const Object& oa = objects[size_t(a)];
    const glm::dvec3 dir = -nb;
    const Ray ra = toLocal(prep.inv[size_t(a)], p - na * kEps, dir);
    lila::geo::BvhHit hit;
    if (!oa.bvh->raycast(ra.origin, ra.dir, hit, kMax))
        return 0.0;
    double depth = hit.t + kEps;
    if (b >= 0)
    {
        const Object& ob = objects[size_t(b)];
        const Ray rb = toLocal(prep.inv[size_t(b)], p - nb * kEps, dir);
        lila::geo::BvhHit hb;
        if (ob.bvh->raycast(rb.origin, rb.dir, hb, kMax))
            depth = std::min(depth, hb.t + kEps);
    }
    return depth;
}

/** Group id the intersection pass sees: grounds share one so they never meet; group 0 is the object's own. */
int intersectGroup(const Object& o, size_t index)
{
    if (o.ground)
        return INT_MIN / 2;
    return o.group != 0 ? -std::abs(o.group) - 1 : int(index) + 1;
}

using PairKey = std::pair<int, int>;
PairKey key(int a, int b) { return a < b ? PairKey{a, b} : PairKey{b, a}; }
} // namespace

const char* kindName(Kind k)
{
    switch (k)
    {
    case Kind::Ground:
        return "ground";
    case Kind::Mesh:
        return "mesh";
    case Kind::Crease:
        return "crease";
    }
    return "mesh";
}

bool parseKind(const std::string& name, Kind* out)
{
    for (Kind k : {Kind::Ground, Kind::Mesh, Kind::Crease})
        if (name == kindName(k))
        {
            *out = k;
            return true;
        }
    return false;
}

double Curve::length() const { return polylineLength(points, closed); }

double Result::totalLength() const
{
    double l = 0.0;
    for (const Curve& c : curves)
        l += c.length();
    return l;
}

std::vector<PairStats> Result::pairs() const
{
    std::map<std::tuple<int, int, int>, PairStats> m;
    for (const Curve& c : curves)
    {
        PairStats& s = m[{c.a, c.b, int(c.kind)}];
        s.a = c.a;
        s.b = c.b;
        s.kind = c.kind;
        ++s.curves;
        s.length += c.length();
    }
    std::vector<PairStats> out;
    for (auto& [k, v] : m)
        out.push_back(v);
    return out;
}

std::vector<glm::dvec3> pointNormals(const Mesh& m)
{
    std::vector<glm::dvec3> n(m.pointCapacity(), glm::dvec3(0.0));
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const glm::dvec3 a = m.faceAreaVector(f);
        m.forEachFaceCorner(f, [&](Index c) { n[m.cornerPoint(c)] += a; });
    }
    for (glm::dvec3& v : n)
        v = safeNormalize(v, {0, 1, 0});
    return n;
}

double exposureAt(std::span<const Object> objects, const Ground* ground, const glm::dvec3& p, const glm::dvec3& up, int rays, double range)
{
    const Prepared prep = prepare(objects, ground);
    const auto dirs = hemisphere(up, std::max(1, rays));
    double open = 0.0;
    for (const glm::dvec3& d : dirs)
        if (!anyBlocks(objects, prep, ground, p, d, range))
            open += 1.0;
    return open / double(dirs.size());
}

Result findSeams(std::span<const Object> objects, const Ground* ground, const Options& o)
{
    Result r;
    const Prepared prep = prepare(objects, ground);
    const glm::dvec3 up = safeNormalize(o.up, {0, 1, 0});
    if (ground && ground->field)
        for (const auto& [name, values] : ground->field->masks)
            if (name.rfind("layer_", 0) == 0)
                r.layers.push_back(name.substr(6));

    std::set<PairKey> ignored;
    for (const auto& [a, b] : o.ignorePairs)
        ignored.insert(key(a, b));
    auto skipPair = [&](int a, int b) {
        if (ignored.contains(key(a, b)))
            return true;
        const bool ga = objects[size_t(a)].ground, gb = objects[size_t(b)].ground;
        if (ga && gb)
            return true;
        if ((ga || gb) ? !o.ground : !o.mesh)
            return true;
        return false;
    };

    // Exact crossings between objects (grounds included, as one group).
    std::vector<lila::geo::IntersectionInput> inputs;
    std::vector<int> inputObject;
    for (size_t i = 0; i < objects.size(); ++i)
    {
        const Object& ob = objects[i];
        if (ob.ignore || !ob.bvh || ob.bvh->empty())
            continue;
        inputs.push_back({ob.bvh.get(), ob.world, intersectGroup(ob, i)});
        inputObject.push_back(int(i));
    }
    std::vector<Curve> exact;
    if (o.ground || o.mesh)
    {
        for (const lila::geo::ObjectIntersection& oi : lila::geo::intersectObjects(inputs, o.weld))
        {
            int a = inputObject[oi.a], b = inputObject[oi.b];
            if (skipPair(a, b))
                continue;
            r.crossings += oi.result.segments;
            bool swap = objects[size_t(a)].ground;
            for (const IntersectionPolyline& pl : oi.result.lines)
            {
                Curve c;
                c.kind = (objects[size_t(a)].ground || objects[size_t(b)].ground) ? Kind::Ground : Kind::Mesh;
                c.a = swap ? b : a;
                c.b = swap ? a : b;
                c.closed = pl.closed;
                c.points = pl.points;
                const auto& fa = swap ? pl.facesB : pl.facesA;
                const auto& fb = swap ? pl.facesA : pl.facesB;
                const size_t segs = fa.size();
                for (size_t k = 0; k < c.points.size(); ++k)
                {
                    const size_t s = segs == 0 ? 0 : std::min(k, segs - 1);
                    c.normalA.push_back(faceNormalWorld(objects[size_t(c.a)], segs ? fa[s] : kInvalid, up));
                    c.normalB.push_back(faceNormalWorld(objects[size_t(c.b)], segs ? fb[s] : kInvalid, up));
                }
                exact.push_back(std::move(c));
            }
        }
    }

    // Exact crossings with the terrain, one object per task.
    std::vector<size_t> terrainObjects;
    if (ground && ground->field && !ground->field->empty() && o.ground)
        for (size_t i = 0; i < objects.size(); ++i)
            if (!objects[i].ignore && !objects[i].ground && objects[i].bvh && !objects[i].bvh->empty())
                terrainObjects.push_back(i);
    const glm::dmat3 groundN = ground ? normalMatrix(ground->world) : glm::dmat3(1.0);
    auto groundNormal = [&](const glm::dvec3& local) {
        return safeNormalize(groundN * ground->field->normalAt(local.x, local.z), up);
    };
    {
        std::vector<std::vector<Curve>> per(terrainObjects.size());
        std::vector<size_t> segCount(terrainObjects.size(), 0);
        tbb::parallel_for(size_t(0), terrainObjects.size(), [&](size_t t) {
            const size_t i = terrainObjects[t];
            const Object& ob = objects[i];
            const auto mi = lila::geo::intersectHeightfield(*ob.bvh, prep.groundInv * ob.world, *ground->field, o.weld);
            segCount[t] = mi.segments;
            for (const IntersectionPolyline& pl : mi.lines)
            {
                Curve c;
                c.kind = Kind::Ground;
                c.a = int(i);
                c.b = kTerrain;
                c.closed = pl.closed;
                const size_t segs = pl.facesA.size();
                for (size_t k = 0; k < pl.points.size(); ++k)
                {
                    c.points.push_back(xformPoint(ground->world, pl.points[k]));
                    const size_t s = segs == 0 ? 0 : std::min(k, segs - 1);
                    c.normalA.push_back(faceNormalWorld(ob, segs ? pl.facesA[s] : kInvalid, up));
                    c.normalB.push_back(groundNormal(pl.points[k]));
                }
                per[t].push_back(std::move(c));
            }
        });
        for (size_t t = 0; t < per.size(); ++t)
        {
            r.crossings += segCount[t];
            for (Curve& c : per[t])
                exact.push_back(std::move(c));
        }
    }

    // Depth below each crossing point.
    for (Curve& c : exact)
    {
        c.depth.assign(c.points.size(), 0.0);
        c.gap.assign(c.points.size(), 0.0);
    }
    tbb::parallel_for(size_t(0), exact.size(), [&](size_t ci) {
        Curve& c = exact[ci];
        for (size_t k = 0; k < c.points.size(); ++k)
            c.depth[k] = depthAt(objects, prep, c.a, c.b, c.points[k], c.normalA[k], c.normalB[k]);
    });

    // Near contacts: the upper object lowered by gap crosses the lower surface exactly where the real one is within gap.
    std::vector<Curve> nearCurves;
    if (o.nearContact && o.gap > 0.0)
    {
        std::map<PairKey, std::vector<std::pair<glm::dvec3, glm::dvec3>>> exactSegs;
        for (const Curve& c : exact)
        {
            auto& v = exactSegs[key(c.a, c.b)];
            for (size_t k = 0; k + 1 < c.points.size(); ++k)
                v.emplace_back(c.points[k], c.points[k + 1]);
            if (c.closed && c.points.size() > 2)
                v.emplace_back(c.points.back(), c.points.front());
            if (c.points.size() == 1)
                v.emplace_back(c.points[0], c.points[0]);
        }
        std::map<PairKey, SegmentIndex> exactIndex;
        for (auto& [k, v] : exactSegs)
            exactIndex.emplace(k, SegmentIndex(std::move(v)));

        struct Task
        {
            int upper;
            int lower; // object, or kTerrain
        };
        std::vector<Task> tasks;
        for (size_t i = 0; i < objects.size(); ++i)
        {
            const Object& u = objects[i];
            if (u.ignore || !u.bvh || u.bvh->empty())
                continue;
            if (!u.ground && o.ground && ground && ground->field && !ground->field->empty())
                tasks.push_back({int(i), kTerrain});
            for (size_t j = 0; j < objects.size(); ++j)
            {
                if (j == i || objects[j].ignore || !objects[j].bvh || objects[j].bvh->empty())
                    continue;
                if (u.ground || skipPair(int(i), int(j)) || intersectGroup(u, i) == intersectGroup(objects[j], j))
                    continue;
                bool apart = false;
                for (int k = 0; k < 3; ++k)
                    apart |= prep.lo[i][k] > prep.hi[j][k] + o.gap || prep.lo[j][k] > prep.hi[i][k] + o.gap;
                if (!apart)
                    tasks.push_back({int(i), int(j)});
            }
        }
        std::vector<std::vector<Curve>> per(tasks.size());
        const glm::dmat4 lower = glm::translate(glm::dmat4(1.0), -up * o.gap);
        tbb::parallel_for(size_t(0), tasks.size(), [&](size_t t) {
            const Task task = tasks[t];
            const Object& u = objects[size_t(task.upper)];
            lila::geo::MeshIntersection mi;
            glm::dmat4 toWorld(1.0);
            if (task.lower == kTerrain)
            {
                mi = lila::geo::intersectHeightfield(*u.bvh, prep.groundInv * lower * u.world, *ground->field, o.weld);
                toWorld = ground->world;
            }
            else
            {
                const Object& l = objects[size_t(task.lower)];
                mi = lila::geo::intersectMeshes(*l.bvh, *u.bvh, prep.inv[size_t(task.lower)] * lower * u.world, o.weld);
                toWorld = l.world;
            }
            const int a = task.upper, b = task.lower;
            const Kind kind = (b == kTerrain || objects[size_t(b)].ground) ? Kind::Ground : Kind::Mesh;
            // Ground joins keep the object first; mesh joins keep the lower surface first, where the points lie.
            const bool upperFirst = kind == Kind::Ground;
            const SegmentIndex* nearby = nullptr;
            if (auto it = exactIndex.find(key(a, b)); it != exactIndex.end())
                nearby = &it->second;
            for (const IntersectionPolyline& pl : mi.lines)
            {
                const size_t n = pl.points.size();
                std::vector<glm::dvec3> pts(n), nUpper(n), nLower(n);
                std::vector<double> gaps(n);
                std::vector<uint8_t> keep(n, 0);
                const size_t segs = pl.facesA.size();
                for (size_t k = 0; k < n; ++k)
                {
                    pts[k] = xformPoint(toWorld, pl.points[k]);
                    const size_t s = segs == 0 ? 0 : std::min(k, segs - 1);
                    if (b == kTerrain)
                        nLower[k] = groundNormal(pl.points[k]);
                    else
                        nLower[k] = faceNormalWorld(objects[size_t(b)], segs ? pl.facesA[s] : kInvalid, up);
                    lila::geo::BvhClosest cl;
                    if (!u.bvh->closestPoint(xformPoint(prep.inv[size_t(a)], pts[k]), cl))
                        continue;
                    const glm::dvec3 cw = xformPoint(u.world, cl.point);
                    gaps[k] = glm::length(cw - pts[k]);
                    nUpper[k] = faceNormalWorld(u, cl.face, -up);
                    const bool crossing = gaps[k] < 0.01;
                    const bool byExact = nearby && nearby->distance(pts[k], 2.0 * o.gap + 0.01) < 2.0 * o.gap;
                    keep[k] = !crossing && !byExact && gaps[k] <= o.gap * 1.05 + 1e-6;
                }
                bool whole = false;
                for (const std::vector<size_t>& run : keptRuns(n, pl.closed, keep, &whole))
                {
                    Curve c;
                    c.kind = kind;
                    c.nearContact = true;
                    c.closed = whole;
                    c.a = upperFirst ? a : b;
                    c.b = upperFirst ? b : a;
                    for (size_t i : run)
                    {
                        c.points.push_back(pts[i]);
                        c.normalA.push_back(upperFirst ? nUpper[i] : nLower[i]);
                        c.normalB.push_back(upperFirst ? nLower[i] : nUpper[i]);
                        c.gap.push_back(gaps[i]);
                        c.depth.push_back(0.0);
                    }
                    per[t].push_back(std::move(c));
                }
            }
        });
        for (auto& v : per)
            for (Curve& c : v)
                nearCurves.push_back(std::move(c));
    }

    for (Curve& c : exact)
        if (c.length() >= o.minLength)
            r.curves.push_back(std::move(c));
    for (Curve& c : nearCurves)
        if (c.length() >= o.minLength)
            r.curves.push_back(std::move(c));

    // Creases: short-range occlusion per point; its iso-line away from the other joins becomes a crease curve.
    if (o.crease)
    {
        r.crease.resize(objects.size());
        auto creaseTarget = [&](const Object& ob) { return !ob.ignore && !ob.ground && ob.bvh && !ob.bvh->empty(); };
        // World position and normal per point; invalid points keep a zero normal and stay at 0.
        std::vector<std::vector<glm::dvec3>> cpts(objects.size()), cnrm(objects.size());
        for (size_t i = 0; i < objects.size(); ++i)
        {
            const Object& ob = objects[i];
            if (!creaseTarget(ob))
                continue;
            const Mesh& m = *ob.mesh;
            const std::vector<glm::dvec3> pn = pointNormals(m);
            const glm::dmat3 nm = normalMatrix(ob.world);
            cpts[i].assign(m.pointCapacity(), glm::dvec3(0.0));
            cnrm[i].assign(m.pointCapacity(), glm::dvec3(0.0));
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p))
                {
                    cpts[i][p] = xformPoint(ob.world, m.position(p));
                    cnrm[i][p] = safeNormalize(nm * pn[p], up);
                }
        }
        std::string gpu;
        if (o.occluded)
            r.creaseBackend = "custom";
        else if (o.creaseBackend != "cpu" && std::any_of(cpts.begin(), cpts.end(), [](const auto& v) { return !v.empty(); }))
        {
            std::string note;
            const bool reference = o.creaseBackend == "bakeCpu";
            gpu = bakeCrease(objects, ground, cpts, cnrm, o, r.crease, note, reference ? "cpu" : "gpu");
            if (reference && !gpu.empty())
                gpu = "bakeCpu";
            if (gpu.empty())
            {
                if (o.creaseBackend == "gpu")
                    throw std::runtime_error("crease on the GPU: " + note);
                r.warnings.push_back("contact creases traced on the CPU: " + note);
            }
        }
        if (!o.occluded)
            r.creaseBackend = gpu.empty() ? "cpu" : gpu;
        const SegmentIndex joins(r);
        // A buried point leaves its cover by the bake AO's own step: 1e-4 of the objects' extent.
        glm::dvec3 sceneLo(std::numeric_limits<double>::max()), sceneHi(std::numeric_limits<double>::lowest());
        for (size_t i = 0; i < objects.size(); ++i)
            if (!objects[i].ignore && objects[i].mesh)
            {
                sceneLo = glm::min(sceneLo, prep.lo[i]);
                sceneHi = glm::max(sceneHi, prep.hi[i]);
            }
        const double lift = sceneLo.x <= sceneHi.x ? std::max(1e-4 * glm::length(sceneHi - sceneLo), 1e-4) : 1e-4;
        for (size_t i = 0; i < objects.size(); ++i)
        {
            const Object& ob = objects[i];
            if (!creaseTarget(ob))
                continue;
            const Mesh& m = *ob.mesh;
            std::vector<float>& values = r.crease[i];
            if (gpu.empty())
            {
                values.assign(m.pointCapacity(), 0.0f);
                tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
                    if (!m.pointValid(p))
                        return;
                    const glm::dvec3 n = cnrm[i][p];
                    const glm::dvec3 origin = cpts[i][p] + n * 0.05;
                    int blocked = 0, backs = 0;
                    const auto dirs = hemisphere(n, std::max(4, o.creaseRays));
                    for (const glm::dvec3& d : dirs)
                    {
                        if (o.occluded)
                        {
                            blocked += o.occluded(origin, d, o.creaseRange) ? 1 : 0;
                            continue;
                        }
                        if (const auto h = nearestHit(objects, prep, origin, d, o.creaseRange))
                        {
                            ++blocked;
                            backs += h->second.frontFacing ? 0 : 1;
                        }
                        else if (ground && heightfieldBlocks(*ground, prep.groundInv, origin, d, o.creaseRange))
                            ++blocked;
                    }
                    // Resting on or sunk into another object: shaded from where it leaves that object, front faces only.
                    glm::dvec3 exit;
                    if (!o.occluded && backs * 4 >= int(dirs.size()) * 3 && buriedAt(objects, prep, cpts[i][p] + n * lift, n, o.creaseRange, lift, exit))
                    {
                        blocked = 0;
                        for (const glm::dvec3& d : dirs)
                            blocked += nearestHit(objects, prep, exit, d, o.creaseRange, false) ||
                                               (ground && heightfieldBlocks(*ground, prep.groundInv, exit, d, o.creaseRange))
                                           ? 1
                                           : 0;
                    }
                    values[p] = float(double(blocked) / double(dirs.size()));
                });
            }
            std::vector<uint8_t> closedFlags;
            const auto lines = isoLines(m, *ob.bvh, ob.world, values, o.creaseThreshold, &closedFlags);
            for (size_t li = 0; li < lines.size(); ++li)
            {
                const auto& line = lines[li];
                std::vector<uint8_t> keep(line.size());
                for (size_t k = 0; k < line.size(); ++k)
                    keep[k] = joins.empty() || joins.distance(line[k], o.creaseRange * 1.5) >= o.creaseRange * 1.5;
                bool whole = false;
                for (const std::vector<size_t>& run : keptRuns(line.size(), closedFlags[li] != 0, keep, &whole))
                {
                    Curve c;
                    c.kind = Kind::Crease;
                    c.a = int(i);
                    c.b = kNone;
                    c.closed = whole;
                    for (size_t k : run)
                    {
                        c.points.push_back(line[k]);
                        lila::geo::BvhClosest cl;
                        glm::dvec3 nrm = up;
                        if (ob.bvh->closestPoint(xformPoint(prep.inv[i], line[k]), cl))
                            nrm = faceNormalWorld(ob, cl.face, up);
                        c.normalA.push_back(nrm);
                        c.normalB.push_back(nrm);
                    }
                    if (c.length() >= o.minLength)
                        r.curves.push_back(std::move(c));
                }
            }
        }
    }

    for (Curve& c : r.curves)
        finishCurve(c, objects, prep, ground, r.layers, o);
    return r;
}

// ------------------------------------------------------------------------------------------------ bands

const std::vector<std::string>& falloffNames()
{
    static const std::vector<std::string> names = {"linear", "smooth", "sharp", "root", "custom"};
    return names;
}

namespace
{
uint32_t hash3(int x, int y, int z, uint32_t seed)
{
    uint32_t h = seed * 0x9E3779B9u ^ uint32_t(x) * 0x85EBCA6Bu ^ uint32_t(y) * 0xC2B2AE35u ^ uint32_t(z) * 0x27D4EB2Fu;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    h *= 0x297A2D39u;
    h ^= h >> 15;
    return h;
}

glm::dvec3 gradient(int x, int y, int z, uint32_t seed)
{
    const uint32_t h = hash3(x, y, z, seed);
    const double theta = (h & 0xFFFF) / 65535.0 * 2.0 * kPi;
    const double u = ((h >> 16) & 0xFFFF) / 65535.0 * 2.0 - 1.0;
    const double s = std::sqrt(std::max(0.0, 1.0 - u * u));
    return {s * std::cos(theta), u, s * std::sin(theta)};
}

double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

std::vector<glm::dvec2> parseProfile(const std::string& text)
{
    std::vector<glm::dvec2> out;
    size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == ','))
            ++i;
        if (i >= text.size())
            break;
        char* end = nullptr;
        const double d = std::strtod(text.c_str() + i, &end);
        if (end == text.c_str() + i || *end != ':')
            break;
        const char* wStart = end + 1;
        const double w = std::strtod(wStart, &end);
        if (end == wStart)
            break;
        out.push_back({d, w});
        i = size_t(end - text.c_str());
    }
    std::sort(out.begin(), out.end(), [](const glm::dvec2& a, const glm::dvec2& b) { return a.x < b.x; });
    return out;
}
} // namespace

double noise3(const glm::dvec3& p, uint32_t seed)
{
    const glm::dvec3 f = glm::floor(p);
    const glm::ivec3 i(f);
    const glm::dvec3 t = p - f;
    double v[8];
    for (int k = 0; k < 8; ++k)
    {
        const glm::ivec3 c = i + glm::ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
        v[k] = glm::dot(gradient(c.x, c.y, c.z, seed), t - glm::dvec3(c - i));
    }
    const double u = fade(t.x), w = fade(t.y), s = fade(t.z);
    const double x00 = v[0] + (v[1] - v[0]) * u, x10 = v[2] + (v[3] - v[2]) * u;
    const double x01 = v[4] + (v[5] - v[4]) * u, x11 = v[6] + (v[7] - v[6]) * u;
    const double y0 = x00 + (x10 - x00) * w, y1 = x01 + (x11 - x01) * w;
    return std::clamp((y0 + (y1 - y0) * s) * 1.6, -1.0, 1.0);
}

double Band::weight(double distance, const glm::dvec3& pos) const
{
    double w = width;
    if (noise > 0.0 && noiseScale > 0.0)
        w *= std::max(0.0, 1.0 + noise * noise3(pos / noiseScale, seed));
    if (w <= 1e-9)
        return distance <= 0.0 ? 1.0 : 0.0;
    const double t = std::clamp(distance / w, 0.0, 1.0);
    switch (falloff)
    {
    case Falloff::Linear:
        return 1.0 - t;
    case Falloff::Smooth:
        return 1.0 - t * t * (3.0 - 2.0 * t);
    case Falloff::Sharp:
        return (1.0 - t) * (1.0 - t);
    case Falloff::Root:
        return std::sqrt(1.0 - t);
    case Falloff::Custom:
    {
        const auto pts = parseProfile(curve);
        if (pts.empty())
            return 1.0 - t;
        if (t <= pts.front().x)
            return std::clamp(pts.front().y, 0.0, 1.0);
        for (size_t k = 1; k < pts.size(); ++k)
            if (t <= pts[k].x)
            {
                const double s = (t - pts[k - 1].x) / std::max(1e-12, pts[k].x - pts[k - 1].x);
                return std::clamp(pts[k - 1].y + (pts[k].y - pts[k - 1].y) * s, 0.0, 1.0);
            }
        return std::clamp(pts.back().y, 0.0, 1.0);
    }
    }
    return 1.0 - t;
}

// ------------------------------------------------------------------------------------------------ segment index

SegmentIndex::SegmentIndex(const Result& r, const std::function<bool(const Curve&)>& filter)
{
    for (size_t ci = 0; ci < r.curves.size(); ++ci)
    {
        const Curve& c = r.curves[ci];
        if (filter && !filter(c))
            continue;
        const size_t n = c.points.size();
        if (n == 1)
        {
            mSegs.emplace_back(c.points[0], c.points[0]);
            mOrigin.emplace_back(ci, 0);
        }
        for (size_t k = 0; k + 1 < n; ++k)
        {
            mSegs.emplace_back(c.points[k], c.points[k + 1]);
            mOrigin.emplace_back(ci, k);
        }
        if (c.closed && n > 2)
        {
            mSegs.emplace_back(c.points.back(), c.points.front());
            mOrigin.emplace_back(ci, n - 1);
        }
    }
    build();
}

SegmentIndex::SegmentIndex(std::vector<std::pair<glm::dvec3, glm::dvec3>> segments) : mSegs(std::move(segments))
{
    mOrigin.resize(mSegs.size());
    for (size_t k = 0; k < mSegs.size(); ++k)
        mOrigin[k] = {0, k};
    build();
}

void SegmentIndex::build()
{
    if (mSegs.empty())
        return;
    glm::dvec3 lo(1e300), hi(-1e300);
    double total = 0.0;
    for (const auto& [a, b] : mSegs)
    {
        lo = glm::min(lo, glm::min(a, b));
        hi = glm::max(hi, glm::max(a, b));
        total += glm::length(b - a);
    }
    const glm::dvec3 ext = hi - lo;
    const double longest = std::max({ext.x, ext.y, ext.z, 1.0});
    mCell = std::max({total / double(mSegs.size()), longest / 64.0, 1.0});
    mMin = lo - glm::dvec3(mCell * 0.5);
    mDims = glm::ivec3(glm::floor((hi - mMin) / mCell)) + glm::ivec3(1);
    const size_t cells = size_t(mDims.x) * size_t(mDims.y) * size_t(mDims.z);
    std::vector<std::vector<uint32_t>> buckets(cells);
    for (size_t s = 0; s < mSegs.size(); ++s)
    {
        const auto& [a, b] = mSegs[s];
        const glm::ivec3 c0 = glm::clamp(glm::ivec3(glm::floor((glm::min(a, b) - mMin) / mCell)), glm::ivec3(0), mDims - 1);
        const glm::ivec3 c1 = glm::clamp(glm::ivec3(glm::floor((glm::max(a, b) - mMin) / mCell)), glm::ivec3(0), mDims - 1);
        for (int z = c0.z; z <= c1.z; ++z)
            for (int y = c0.y; y <= c1.y; ++y)
                for (int x = c0.x; x <= c1.x; ++x)
                    buckets[(size_t(z) * size_t(mDims.y) + size_t(y)) * size_t(mDims.x) + size_t(x)].push_back(uint32_t(s));
    }
    mCellStart.assign(cells + 1, 0);
    for (size_t c = 0; c < cells; ++c)
        mCellStart[c + 1] = mCellStart[c] + uint32_t(buckets[c].size());
    mCellSegs.reserve(mCellStart.back());
    for (auto& b : buckets)
        mCellSegs.insert(mCellSegs.end(), b.begin(), b.end());
}

double SegmentIndex::distance(const glm::dvec3& p, double maxDistance, glm::dvec3* closest, size_t* segment) const
{
    double best = maxDistance;
    if (mSegs.empty())
        return best;
    const glm::ivec3 home = glm::ivec3(glm::floor((p - mMin) / mCell));
    const int maxRing = int(std::ceil(maxDistance / mCell)) + 1;
    const int gridSpan = std::max({mDims.x, mDims.y, mDims.z}) + std::max({std::abs(home.x), std::abs(home.y), std::abs(home.z)});
    auto test = [&](int x, int y, int z) {
        if (x < 0 || y < 0 || z < 0 || x >= mDims.x || y >= mDims.y || z >= mDims.z)
            return;
        const size_t c = (size_t(z) * size_t(mDims.y) + size_t(y)) * size_t(mDims.x) + size_t(x);
        for (uint32_t i = mCellStart[c]; i < mCellStart[c + 1]; ++i)
        {
            const auto& [a, b] = mSegs[mCellSegs[i]];
            const glm::dvec3 ab = b - a;
            const double len2 = glm::dot(ab, ab);
            const double t = len2 > 1e-24 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
            const glm::dvec3 q = a + ab * t;
            const double d = glm::length(p - q);
            if (d < best)
            {
                best = d;
                if (closest)
                    *closest = q;
                if (segment)
                    *segment = mCellSegs[i];
            }
        }
    };
    for (int r = 0; r <= std::min(maxRing, gridSpan); ++r)
    {
        // Anything in ring r is at least (r - 1) cells away.
        if (r > 1 && double(r - 1) * mCell > best)
            break;
        for (int z = home.z - r; z <= home.z + r; ++z)
            for (int y = home.y - r; y <= home.y + r; ++y)
            {
                const bool face = std::abs(z - home.z) == r || std::abs(y - home.y) == r;
                if (face)
                    for (int x = home.x - r; x <= home.x + r; ++x)
                        test(x, y, z);
                else
                {
                    test(home.x - r, y, z);
                    if (r > 0)
                        test(home.x + r, y, z);
                }
            }
    }
    return best;
}

std::vector<float> distanceField(const Object& obj, int objectIndex, const Result& r, double maxDistance)
{
    const Mesh& m = *obj.mesh;
    std::vector<float> out(m.pointCapacity(), float(maxDistance));
    const SegmentIndex idx(r, [&](const Curve& c) { return c.a == objectIndex || c.b == objectIndex; });
    if (idx.empty())
        return out;
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (m.pointValid(p))
            out[p] = float(idx.distance(xformPoint(obj.world, m.position(p)), maxDistance));
    });
    return out;
}

void contactMask(Heightfield& h, const glm::dmat4& world, const Result& r, const Band& band, const std::string& mask)
{
    std::vector<float>& values = h.mask(mask);
    std::fill(values.begin(), values.end(), 0.0f);
    const SegmentIndex idx(r, [](const Curve& c) { return c.kind == Kind::Ground; });
    if (idx.empty())
        return;
    const double reach = band.width * (1.0 + std::max(0.0, band.noise)) + 1e-6;
    tbb::parallel_for(0, h.nz, [&](int z) {
        for (int x = 0; x < h.nx; ++x)
        {
            const glm::dvec3 w = xformPoint(world, h.position(x, z));
            const double d = idx.distance(w, reach);
            values[h.index(x, z)] = d >= reach ? 0.0f : float(band.weight(d, w));
        }
    });
}

// ------------------------------------------------------------------------------------------------ points

std::vector<SeamPoint> samplePoints(const Result& r, double spacing)
{
    std::vector<SeamPoint> out;
    spacing = std::max(spacing, 0.01);
    for (size_t ci = 0; ci < r.curves.size(); ++ci)
    {
        const Curve& c = r.curves[ci];
        const size_t n = c.points.size();
        if (n < 2)
            continue;
        const size_t segs = c.closed ? n : n - 1;
        const double total = c.length();
        double next = std::min(spacing * 0.5, total * 0.5);
        double at = 0.0;
        for (size_t s = 0; s < segs; ++s)
        {
            const size_t i0 = s, i1 = (s + 1) % n;
            const double len = glm::length(c.points[i1] - c.points[i0]);
            while (next <= at + len + 1e-9 && len > 1e-12)
            {
                const double t = std::clamp((next - at) / len, 0.0, 1.0);
                SeamPoint p;
                auto lerp = [t](double a, double b) { return a + (b - a) * t; };
                p.position = c.points[i0] + (c.points[i1] - c.points[i0]) * t;
                p.tangent = (c.points[i1] - c.points[i0]) / len;
                p.normalA = safeNormalize(c.normalA[i0] * (1.0 - t) + c.normalA[i1] * t, c.normalA[i0]);
                p.normalB = safeNormalize(c.normalB[i0] * (1.0 - t) + c.normalB[i1] * t, c.normalB[i0]);
                p.angle = lerp(c.angle[i0], c.angle[i1]);
                p.depth = lerp(c.depth[i0], c.depth[i1]);
                p.gap = lerp(c.gap[i0], c.gap[i1]);
                p.upFacing = lerp(c.upFacing[i0], c.upFacing[i1]);
                p.exposure = lerp(c.exposure[i0], c.exposure[i1]);
                p.layer = t < 0.5 ? c.layer[i0] : c.layer[i1];
                p.a = c.a;
                p.b = c.b;
                p.kind = c.kind;
                p.curve = ci;
                p.along = next;
                out.push_back(p);
                next += spacing;
            }
            at += len;
        }
    }
    return out;
}

glm::dvec3 bisector(const SeamPoint& p, const glm::dvec3& up) { return safeNormalize(p.normalA + p.normalB, up); }

// ------------------------------------------------------------------------------------------------ iso-lines

std::vector<std::vector<glm::dvec3>> isoLines(const Mesh& m, const MeshBvh& bvh, const glm::dmat4& world, std::span<const float> values, double level,
                                              std::vector<uint8_t>* closedOut)
{
    const auto& tris = bvh.triangles();
    using EdgeKey = std::pair<Index, Index>;
    struct Seg
    {
        EdgeKey e0, e1;
        glm::dvec3 p0, p1;
    };
    std::vector<Seg> segs;
    auto edgePoint = [&](Index a, Index b) {
        const double va = values[a], vb = values[b];
        const double t = std::abs(vb - va) > 1e-12 ? (level - va) / (vb - va) : 0.5;
        return xformPoint(world, m.position(a) + (m.position(b) - m.position(a)) * std::clamp(t, 0.0, 1.0));
    };
    for (Index t = 0; t < tris.triangleCount(); ++t)
    {
        Index p[3];
        for (int k = 0; k < 3; ++k)
            p[k] = m.cornerPoint(tris.corners[size_t(t) * 3 + size_t(k)]);
        std::vector<std::pair<EdgeKey, glm::dvec3>> cross;
        for (int k = 0; k < 3; ++k)
        {
            const Index a = p[k], b = p[(k + 1) % 3];
            const bool ia = values[a] >= level, ib = values[b] >= level;
            if (ia != ib)
                cross.push_back({{std::min(a, b), std::max(a, b)}, edgePoint(a, b)});
        }
        if (cross.size() == 2)
            segs.push_back({cross[0].first, cross[1].first, cross[0].second, cross[1].second});
    }
    std::map<EdgeKey, std::vector<size_t>> byEdge;
    for (size_t s = 0; s < segs.size(); ++s)
    {
        byEdge[segs[s].e0].push_back(s);
        byEdge[segs[s].e1].push_back(s);
    }
    std::vector<uint8_t> used(segs.size(), 0);
    std::vector<std::vector<glm::dvec3>> out;
    if (closedOut)
        closedOut->clear();
    auto other = [&](EdgeKey e, size_t s) -> size_t {
        for (size_t t : byEdge[e])
            if (t != s && !used[t])
                return t;
        return SIZE_MAX;
    };
    for (size_t s0 = 0; s0 < segs.size(); ++s0)
    {
        if (used[s0])
            continue;
        used[s0] = 1;
        std::vector<glm::dvec3> fwd{segs[s0].p0, segs[s0].p1};
        std::vector<glm::dvec3> back;
        bool closed = false;
        // Walk forward from e1, then backward from e0.
        for (int dir = 0; dir < 2; ++dir)
        {
            EdgeKey at = dir == 0 ? segs[s0].e1 : segs[s0].e0;
            size_t cur = s0;
            while (true)
            {
                const size_t nx = other(at, cur);
                if (nx == SIZE_MAX)
                {
                    if (dir == 0)
                        for (size_t t : byEdge[at])
                            if (t == s0 && cur != s0)
                                closed = true;
                    break;
                }
                used[nx] = 1;
                const bool flip = segs[nx].e0 != at;
                const EdgeKey nextEdge = flip ? segs[nx].e0 : segs[nx].e1;
                const glm::dvec3 np = flip ? segs[nx].p0 : segs[nx].p1;
                if (nextEdge == (dir == 0 ? segs[s0].e0 : segs[s0].e1))
                {
                    closed = true;
                    break;
                }
                (dir == 0 ? fwd : back).push_back(np);
                at = nextEdge;
                cur = nx;
            }
            if (closed)
                break;
        }
        std::reverse(back.begin(), back.end());
        back.insert(back.end(), fwd.begin(), fwd.end());
        out.push_back(std::move(back));
        if (closedOut)
            closedOut->push_back(closed ? 1 : 0);
    }
    return out;
}
} // namespace seamwork
