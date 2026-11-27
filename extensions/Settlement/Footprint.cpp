// Settlement: footprints of what a settlement keeps clear of (meshes, curves, polygons grown by a margin), traced from a
// raster where exact offsets fold over.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

namespace settlement
{
namespace
{
double crossOf(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }

double pointSegment(Vec2 q, Vec2 a, Vec2 b)
{
    const Vec2 d = b - a;
    const double len2 = glm::dot(d, d);
    const double t = len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
    return glm::length(a + d * t - q);
}

bool segmentsTouch(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = crossOf(b - a, c - a), d2 = crossOf(b - a, d - a);
    const double d3 = crossOf(d - c, a - c), d4 = crossOf(d - c, b - c);
    if (((d1 > 0) != (d2 > 0) || d1 == 0 || d2 == 0) && ((d3 > 0) != (d4 > 0) || d3 == 0 || d4 == 0))
    {
        // Collinear segments only touch when they overlap.
        if (d1 == 0 && d2 == 0)
            return pointSegment(c, a, b) < 1e-9 || pointSegment(d, a, b) < 1e-9 || pointSegment(a, c, d) < 1e-9;
        return true;
    }
    return false;
}

/** Ramer-Douglas-Peucker over an open run, marking what to keep. */
void rdp(std::span<const Vec2> pts, size_t lo, size_t hi, double tol, std::vector<char>& keep)
{
    if (hi <= lo + 1)
        return;
    const Vec2 a = pts[lo], b = pts[hi];
    double worst = -1.0;
    size_t at = lo;
    for (size_t k = lo + 1; k < hi; ++k)
        if (const double d = pointSegment(pts[k], a, b); d > worst)
        {
            worst = d;
            at = k;
        }
    if (worst > tol)
    {
        keep[at] = 1;
        rdp(pts, lo, at, tol, keep);
        rdp(pts, at, hi, tol, keep);
    }
}

/** A closed loop simplified to within tol, split at its two furthest-apart points. */
std::vector<Vec2> simplifyLoop(const std::vector<Vec2>& loop, double tol)
{
    if (loop.size() < 4)
        return loop;
    size_t far = 0;
    double best = -1.0;
    for (size_t k = 0; k < loop.size(); ++k)
        if (const double d = glm::length(loop[k] - loop[0]); d > best)
        {
            best = d;
            far = k;
        }
    std::vector<Vec2> pts(loop.begin(), loop.end());
    pts.push_back(loop[0]);
    std::vector<char> keep(pts.size(), 0);
    keep[0] = keep[far] = keep.back() = 1;
    rdp(pts, 0, far, tol, keep);
    rdp(pts, far, pts.size() - 1, tol, keep);
    std::vector<Vec2> out;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        if (keep[k])
            out.push_back(pts[k]);
    return out;
}

/** The mitred outward offset, or empty when a corner overshoots or the result crosses itself. */
Polygon mitred(const Polygon& p, double margin)
{
    const size_t n = p.size();
    std::vector<Vec2> dir(n), out(n);
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 e = p.pts[(k + 1) % n] - p.pts[k];
        const double len = glm::length(e);
        dir[k] = len > 1e-12 ? e / len : Vec2(1.0, 0.0);
    }
    Polygon r;
    for (size_t k = 0; k < n; ++k)
    {
        const size_t j = (k + n - 1) % n;
        // Outward is to the right of a counter-clockwise side.
        const Vec2 nj(dir[j].y, -dir[j].x), nk(dir[k].y, -dir[k].x);
        const Vec2 pj = p.pts[j] + nj * margin, pk = p.pts[k] + nk * margin;
        const double den = crossOf(dir[j], dir[k]);
        Vec2 v = pk;
        if (std::abs(den) > 1e-9)
            v = pj + dir[j] * (crossOf(pk - pj, dir[k]) / den);
        if (glm::length(v - p.pts[k]) > 4.0 * margin)
            return {};
        r.pts.push_back(v);
    }
    r.tag = p.tag;
    if (selfIntersects(r.pts) || signedArea(r.pts) <= signedArea(p.pts))
        return {};
    return r;
}
} // namespace

double boundaryDistance(std::span<const Vec2> pts, Vec2 q)
{
    double best = std::numeric_limits<double>::max();
    for (size_t k = 0; k < pts.size(); ++k)
        best = std::min(best, pointSegment(q, pts[k], pts[(k + 1) % pts.size()]));
    return best;
}

bool segmentCrosses(std::span<const Vec2> pts, Vec2 a, Vec2 b)
{
    for (size_t k = 0; k < pts.size(); ++k)
        if (segmentsTouch(a, b, pts[k], pts[(k + 1) % pts.size()]))
            return true;
    return false;
}

double segmentDistance(std::span<const Vec2> pts, Vec2 a, Vec2 b)
{
    if (segmentCrosses(pts, a, b))
        return 0.0;
    double best = std::min(boundaryDistance(pts, a), boundaryDistance(pts, b));
    for (const Vec2& q : pts)
        best = std::min(best, pointSegment(q, a, b));
    return best;
}

Mask Mask::around(Vec2 lo, Vec2 hi, double cell, double pad)
{
    Mask m;
    m.cell = std::max(cell, 1e-3);
    m.origin = lo - Vec2(pad + 2.0 * m.cell);
    const Vec2 ext = hi - lo + Vec2(2.0 * pad + 4.0 * m.cell);
    m.w = std::max(1, int(std::ceil(ext.x / m.cell)));
    m.h = std::max(1, int(std::ceil(ext.y / m.cell)));
    m.on.assign(size_t(m.w) * size_t(m.h), 0);
    return m;
}

void Mask::fillTriangle(Vec2 a, Vec2 b, Vec2 c)
{
    const Vec2 lo = glm::min(a, glm::min(b, c)), hi = glm::max(a, glm::max(b, c));
    const int x0 = std::max(0, int(std::floor((lo.x - origin.x) / cell))), x1 = std::min(w - 1, int(std::floor((hi.x - origin.x) / cell)));
    const int y0 = std::max(0, int(std::floor((lo.y - origin.y) / cell))), y1 = std::min(h - 1, int(std::floor((hi.y - origin.y) / cell)));
    const double area2 = crossOf(b - a, c - a);
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const Vec2 q = origin + Vec2((x + 0.5) * cell, (y + 0.5) * cell);
            const double s0 = crossOf(b - a, q - a), s1 = crossOf(c - b, q - b), s2 = crossOf(a - c, q - c);
            const bool in = area2 >= 0.0 ? (s0 >= 0 && s1 >= 0 && s2 >= 0) : (s0 <= 0 && s1 <= 0 && s2 <= 0);
            if (in)
                on[size_t(y) * w + x] = 1;
        }
    // Slivers thinner than a cell still mark where they run.
    fillSegment(a, b);
    fillSegment(b, c);
    fillSegment(c, a);
}

void Mask::fillSegment(Vec2 a, Vec2 b)
{
    const int steps = std::max(1, int(std::ceil(glm::length(b - a) / (0.5 * cell))));
    for (int s = 0; s <= steps; ++s)
    {
        const Vec2 q = a + (b - a) * (double(s) / steps);
        const int x = int(std::floor((q.x - origin.x) / cell)), y = int(std::floor((q.y - origin.y) / cell));
        if (x >= 0 && y >= 0 && x < w && y < h)
            on[size_t(y) * w + x] = 1;
    }
}

void Mask::fillPolygon(std::span<const Vec2> pts)
{
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const int x0 = std::max(0, int(std::floor((lo.x - origin.x) / cell))), x1 = std::min(w - 1, int(std::floor((hi.x - origin.x) / cell)));
    const int y0 = std::max(0, int(std::floor((lo.y - origin.y) / cell))), y1 = std::min(h - 1, int(std::floor((hi.y - origin.y) / cell)));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (contains(pts, origin + Vec2((x + 0.5) * cell, (y + 0.5) * cell)))
                on[size_t(y) * w + x] = 1;
    for (size_t k = 0; k < pts.size(); ++k)
        fillSegment(pts[k], pts[(k + 1) % pts.size()]);
}

void Mask::dilate(double r)
{
    if (r <= 0.0)
        return;
    // Chamfer distance (in cells) to the nearest set cell, two passes.
    const float big = 1e9f, d1 = 1.0f, d2 = 1.41421356f;
    std::vector<float> d(on.size());
    for (size_t k = 0; k < on.size(); ++k)
        d[k] = on[k] ? 0.0f : big;
    auto at = [&](int x, int y) -> float& { return d[size_t(y) * w + x]; };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float& v = at(x, y);
            if (x > 0)
                v = std::min(v, at(x - 1, y) + d1);
            if (y > 0)
            {
                v = std::min(v, at(x, y - 1) + d1);
                if (x > 0)
                    v = std::min(v, at(x - 1, y - 1) + d2);
                if (x + 1 < w)
                    v = std::min(v, at(x + 1, y - 1) + d2);
            }
        }
    for (int y = h - 1; y >= 0; --y)
        for (int x = w - 1; x >= 0; --x)
        {
            float& v = at(x, y);
            if (x + 1 < w)
                v = std::min(v, at(x + 1, y) + d1);
            if (y + 1 < h)
            {
                v = std::min(v, at(x, y + 1) + d1);
                if (x + 1 < w)
                    v = std::min(v, at(x + 1, y + 1) + d2);
                if (x > 0)
                    v = std::min(v, at(x - 1, y + 1) + d2);
            }
        }
    const float limit = float(r / cell);
    for (size_t k = 0; k < on.size(); ++k)
        on[k] = d[k] <= limit ? 1 : 0;
}

std::vector<Polygon> Mask::outlines(double tolerance) const
{
    LILA_ZONE();

    auto set = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && on[size_t(y) * w + x]; };
    // Cell sides between a set cell and an empty one, directed with the set cell on the left.
    struct Side
    {
        int x0, y0, x1, y1;
    };
    std::vector<Side> sides;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            if (!set(x, y))
                continue;
            if (!set(x, y - 1))
                sides.push_back({x, y, x + 1, y});
            if (!set(x + 1, y))
                sides.push_back({x + 1, y, x + 1, y + 1});
            if (!set(x, y + 1))
                sides.push_back({x + 1, y + 1, x, y + 1});
            if (!set(x - 1, y))
                sides.push_back({x, y + 1, x, y});
        }
    auto vkey = [&](int x, int y) { return int64_t(y) * int64_t(w + 2) + x; };
    std::unordered_map<int64_t, std::vector<int>> from;
    for (size_t k = 0; k < sides.size(); ++k)
        from[vkey(sides[k].x0, sides[k].y0)].push_back(int(k));
    std::vector<char> used(sides.size(), 0);
    std::vector<Polygon> out;
    for (size_t s = 0; s < sides.size(); ++s)
    {
        if (used[s])
            continue;
        std::vector<Vec2> loop;
        int cur = int(s);
        while (cur >= 0 && !used[size_t(cur)])
        {
            used[size_t(cur)] = 1;
            const Side& c = sides[size_t(cur)];
            loop.push_back(origin + Vec2(c.x0 * cell, c.y0 * cell));
            const auto& next = from[vkey(c.x1, c.y1)];
            // Where two cells touch only at a corner, keep them apart: take the sharpest left turn.
            int pick = -1;
            double bestTurn = -2.0;
            const Vec2 din(c.x1 - c.x0, c.y1 - c.y0);
            for (int n : next)
            {
                if (used[size_t(n)])
                    continue;
                const Side& o = sides[size_t(n)];
                const Vec2 dout(o.x1 - o.x0, o.y1 - o.y0);
                const double turn = crossOf(din, dout) + 0.5 * glm::dot(din, dout);
                if (turn > bestTurn)
                {
                    bestTurn = turn;
                    pick = n;
                }
            }
            cur = pick;
        }
        if (loop.size() < 4 || signedArea(loop) <= 0.0)
            continue; // a hole: filled
        Polygon p = makePolygon(simplifyLoop(loop, tolerance > 0.0 ? tolerance : 0.75 * cell));
        if (!p.pts.empty())
            out.push_back(std::move(p));
    }
    return out;
}

Polygon grow(const Polygon& p, double margin)
{
    if (p.pts.empty() || margin <= 0.0)
        return p;
    if (Polygon m = mitred(p, margin); !m.pts.empty())
        return m;
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : p.pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const double ext = std::max(hi.x - lo.x, hi.y - lo.y) + 2.0 * margin;
    Mask m = Mask::around(lo, hi, std::max(margin / 4.0, ext / 1024.0), margin);
    m.fillPolygon(p.pts);
    m.dilate(margin);
    std::vector<Polygon> loops = m.outlines();
    Polygon best;
    for (Polygon& q : loops)
        if (area(q) > area(best))
            best = std::move(q);
    return best;
}

std::vector<Polygon> meshFootprint(const geo::Mesh& mesh, double margin, double cell)
{
    LILA_ZONE();

    const geo::Bounds b = mesh.bounds();
    if (!b.valid() || mesh.numFaces() == 0)
        return {};
    const Vec2 lo(b.min.x, b.min.z), hi(b.max.x, b.max.z);
    const double ext = std::max(hi.x - lo.x, hi.y - lo.y) + 2.0 * margin;
    if (cell <= 0.0)
        cell = std::clamp(ext / 512.0, 10.0, 1000.0);
    cell = std::max(cell, ext / 2048.0);
    Mask m = Mask::around(lo, hi, cell, margin);
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = mesh.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            const glm::dvec3 a = mesh.position(pts[0]), c1 = mesh.position(pts[k]), c2 = mesh.position(pts[k + 1]);
            m.fillTriangle({a.x, a.z}, {c1.x, c1.z}, {c2.x, c2.z});
        }
    }
    m.dilate(margin + 0.5 * cell);
    return m.outlines();
}

std::vector<Polygon> lineFootprint(std::span<const std::vector<Vec2>> lines, double margin, double cell)
{
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const std::vector<Vec2>& l : lines)
        for (const Vec2& q : l)
        {
            lo = glm::min(lo, q);
            hi = glm::max(hi, q);
        }
    if (lo.x > hi.x)
        return {};
    margin = std::max(margin, 1.0);
    const double ext = std::max(hi.x - lo.x, hi.y - lo.y) + 2.0 * margin;
    if (cell <= 0.0)
        cell = std::clamp(std::min(margin / 3.0, ext / 512.0), 5.0, 1000.0);
    cell = std::max(cell, ext / 2048.0);
    Mask m = Mask::around(lo, hi, cell, margin);
    for (const std::vector<Vec2>& l : lines)
        for (size_t k = 0; k + 1 < l.size(); ++k)
            m.fillSegment(l[k], l[k + 1]);
    m.dilate(margin);
    return m.outlines();
}
} // namespace settlement
