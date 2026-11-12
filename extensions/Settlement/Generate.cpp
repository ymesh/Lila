// Settlement: streets grown or laid out over the ground, then blocks, lots and building envelopes inside them.

#include "SettlementCore.h"
#include "Parcels.h"
#include "SettlementTerrain.h"
#include "Generators.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>
#include <stdexcept>

namespace settlement
{
double bankDistance(Vec2 q, std::span<const WaterLine> water)
{
    double best = std::numeric_limits<double>::max();
    for (const WaterLine& w : water)
        for (size_t k = 0; k + 1 < w.pts.size(); ++k)
        {
            const Vec2 a = w.pts[k], d = w.pts[k + 1] - a;
            const double len2 = glm::dot(d, d);
            const double t = len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, glm::length(a + d * t - q) - 0.5 * w.width);
        }
    return best;
}

namespace
{
/** Stable random numbers: the same seed and stream give the same sequence on every machine. */
struct Rng
{
    uint64_t base;
    uint64_t n = 0;
    Rng(uint32_t seed, uint64_t stream) : base(hash64(hash64(seed) ^ (stream * 0x9E3779B97F4A7C15ull))) {}
    double next() { return unitRandom(base + n++); }
    double range(double lo, double hi) { return lo + (hi - lo) * next(); }
};

Vec2 rotate(Vec2 d, double radians)
{
    const double c = std::cos(radians), s = std::sin(radians);
    return {d.x * c - d.y * s, d.x * s + d.y * c};
}

/** Where the ray from o along d leaves the polygon, or o when it never does. */
Vec2 exitPoint(const Polygon& poly, Vec2 o, Vec2 d)
{
    double best = std::numeric_limits<double>::max();
    const size_t n = poly.pts.size();
    for (size_t k = 0; k < n; ++k)
    {
        const Vec2 a = poly.pts[k], b = poly.pts[(k + 1) % n];
        const Vec2 e = b - a;
        const double den = cross(d, e);
        if (std::abs(den) <= 1e-9 * glm::length(d) * glm::length(e))
            continue;
        const double t = cross(a - o, e) / den, u = cross(a - o, d) / den;
        if (t > 1e-6 && u >= 0.0 && u <= 1.0)
            best = std::min(best, t);
    }
    return best < std::numeric_limits<double>::max() ? o + d * best : o;
}

/** Parameters along a-b where it crosses a circle. */
void circleCrossings(Vec2 a, Vec2 b, Vec2 c, double r, std::vector<double>& out)
{
    if (r <= 0.0)
        return;
    const Vec2 d = b - a, f = a - c;
    const double A = glm::dot(d, d), B = 2.0 * glm::dot(f, d), C = glm::dot(f, f) - r * r;
    const double disc = B * B - 4 * A * C;
    if (A < 1e-12 || disc < 0.0)
        return;
    const double s = std::sqrt(disc);
    for (double t : {(-B - s) / (2 * A), (-B + s) / (2 * A)})
        if (t > 0.0 && t < 1.0)
            out.push_back(t);
}

/** Parameters along a-b where it crosses the polygon's sides. */
void polygonCrossings(Vec2 a, Vec2 b, std::span<const Vec2> poly, std::vector<double>& out)
{
    const size_t n = poly.size();
    for (size_t j = 0; j < n; ++j)
    {
        const Vec2 p = poly[j], q = poly[(j + 1) % n];
        const double den = cross(b - a, q - p);
        if (std::abs(den) <= 1e-9 * glm::length(b - a) * glm::length(q - p))
            continue;
        const double t = cross(p - a, q - p) / den, u = cross(p - a, b - a) / den;
        if (t > 0.0 && t < 1.0 && u >= 0.0 && u <= 1.0)
            out.push_back(t);
    }
}

bool insideAny(std::span<const Polygon> polys, Vec2 q)
{
    for (const Polygon& p : polys)
        if (contains(p.pts, q))
            return true;
    return false;
}

/** The parts of a polyline inside the outline, outside the holes and between the two radii around c (0 for no limit). */
std::vector<std::vector<Vec2>> clipPolyline(std::span<const Vec2> pts, const Polygon& outline, Vec2 c, double inner, double outer,
                                            std::span<const Polygon> holes = {})
{
    std::vector<std::vector<Vec2>> runs;
    std::vector<Vec2> run;
    auto keep = [&](Vec2 q) {
        const double d = glm::length(q - c);
        return contains(outline.pts, q) && (inner <= 0.0 || d >= inner) && (outer <= 0.0 || d <= outer) && !insideAny(holes, q);
    };
    for (size_t k = 0; k + 1 < pts.size(); ++k)
    {
        const Vec2 a = pts[k], b = pts[k + 1];
        std::vector<double> ts = {0.0, 1.0};
        polygonCrossings(a, b, outline.pts, ts);
        for (const Polygon& h : holes)
            polygonCrossings(a, b, h.pts, ts);
        circleCrossings(a, b, c, inner, ts);
        circleCrossings(a, b, c, outer, ts);
        std::sort(ts.begin(), ts.end());
        for (size_t i = 0; i + 1 < ts.size(); ++i)
        {
            if (ts[i + 1] - ts[i] < 1e-9)
                continue;
            const Vec2 p0 = a + (b - a) * ts[i], p1 = a + (b - a) * ts[i + 1];
            if (keep((p0 + p1) * 0.5))
            {
                if (run.empty())
                    run.push_back(p0);
                run.push_back(p1);
            }
            else if (!run.empty())
            {
                runs.push_back(std::move(run));
                run.clear();
            }
        }
    }
    if (run.size() >= 2)
        runs.push_back(std::move(run));
    return runs;
}

/** Splits a run where the ground is steeper than maxGrade, keeping the climbable parts. */
std::vector<std::vector<Vec2>> climbable(const std::vector<Vec2>& run, const Ground& g, double maxGrade, double step)
{
    if (g.isPlane())
        return {run};
    std::vector<std::vector<Vec2>> out;
    std::vector<Vec2> cur;
    for (size_t k = 0; k + 1 < run.size(); ++k)
    {
        const Vec2 a = run[k], b = run[k + 1];
        const int n = std::max(1, int(glm::length(b - a) / step));
        for (int i = 0; i < n; ++i)
        {
            const Vec2 p = a + (b - a) * (double(i) / n), q = a + (b - a) * (double(i + 1) / n);
            if (g.gradeAlong(p, q) <= maxGrade)
            {
                if (cur.empty())
                    cur.push_back(p);
                cur.push_back(q);
            }
            else if (!cur.empty())
            {
                if (cur.size() >= 2)
                    out.push_back(std::move(cur));
                cur.clear();
            }
        }
    }
    if (cur.size() >= 2)
        out.push_back(std::move(cur));
    return out;
}

double polylineLength(std::span<const Vec2> pts)
{
    double len = 0.0;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        len += glm::length(pts[k + 1] - pts[k]);
    return len;
}

/** The run with a gap at every water crossing: only arterials bridge rivers. */
std::vector<std::vector<Vec2>> cutAtWater(const std::vector<Vec2>& run, std::span<const WaterLine> water)
{
    if (water.empty() || run.size() < 2)
        return {run};
    // Crossings as arc length along the run, each with the half gap it needs.
    std::vector<std::pair<double, double>> gaps;
    double at = 0.0;
    for (size_t k = 0; k + 1 < run.size(); ++k)
    {
        const Vec2 a = run[k], b = run[k + 1];
        const double len = glm::length(b - a);
        for (const WaterLine& w : water)
            for (size_t j = 0; j + 1 < w.pts.size(); ++j)
            {
                const Vec2 c = w.pts[j], d = w.pts[j + 1];
                const double den = cross(b - a, d - c);
                if (std::abs(den) <= 1e-9 * len * glm::length(d - c))
                    continue;
                const double t = cross(c - a, d - c) / den, u = cross(c - a, b - a) / den;
                if (t >= 0.0 && t <= 1.0 && u >= 0.0 && u <= 1.0)
                    gaps.push_back({at + t * len, 0.5 * w.width + 300.0});
            }
        at += len;
    }
    if (gaps.empty())
        return {run};
    std::vector<std::pair<double, double>> keep = {{0.0, at}};
    for (const auto& [c, half] : gaps)
    {
        std::vector<std::pair<double, double>> next;
        for (const auto& [lo, hi] : keep)
        {
            if (c - half > lo)
                next.push_back({lo, std::min(hi, c - half)});
            if (c + half < hi)
                next.push_back({std::max(lo, c + half), hi});
        }
        keep = std::move(next);
    }
    auto pointAt = [&](double d) {
        double acc = 0.0;
        for (size_t k = 0; k + 1 < run.size(); ++k)
        {
            const double len = glm::length(run[k + 1] - run[k]);
            if (acc + len >= d || k + 2 == run.size())
                return run[k] + (run[k + 1] - run[k]) * (len > 1e-12 ? std::clamp((d - acc) / len, 0.0, 1.0) : 0.0);
            acc += len;
        }
        return run.back();
    };
    std::vector<std::vector<Vec2>> out;
    for (const auto& [lo, hi] : keep)
    {
        if (hi - lo < 100.0)
            continue;
        std::vector<Vec2> piece = {pointAt(lo)};
        double acc = 0.0;
        for (size_t k = 0; k + 1 < run.size(); ++k)
        {
            acc += glm::length(run[k + 1] - run[k]);
            if (acc > lo && acc < hi)
                piece.push_back(run[k + 1]);
        }
        piece.push_back(pointAt(hi));
        out.push_back(std::move(piece));
    }
    return out;
}

/** Ramer-Douglas-Peucker. */
void simplify(std::span<const Vec2> pts, size_t lo, size_t hi, double tol, std::vector<char>& keep)
{
    if (hi <= lo + 1)
        return;
    const Vec2 a = pts[lo], b = pts[hi];
    double worst = -1.0;
    size_t at = lo;
    for (size_t k = lo + 1; k < hi; ++k)
    {
        const Vec2 d = b - a;
        const double len = glm::length(d);
        const double dist = len > 1e-9 ? std::abs(cross(d, pts[k] - a)) / len : glm::length(pts[k] - a);
        if (dist > worst)
        {
            worst = dist;
            at = k;
        }
    }
    if (worst > tol)
    {
        keep[at] = 1;
        simplify(pts, lo, at, tol, keep);
        simplify(pts, at, hi, tol, keep);
    }
}

std::vector<Vec2> chaikin(std::vector<Vec2> pts, int iterations)
{
    for (int it = 0; it < iterations && pts.size() > 2; ++it)
    {
        std::vector<Vec2> out = {pts.front()};
        for (size_t k = 0; k + 1 < pts.size(); ++k)
        {
            const Vec2 a = pts[k], b = pts[k + 1];
            if (k > 0)
                out.push_back(a * 0.75 + b * 0.25);
            if (k + 2 < pts.size())
                out.push_back(a * 0.25 + b * 0.75);
        }
        out.push_back(pts.back());
        pts = std::move(out);
    }
    return pts;
}

double settlementArea(const Layout& l, double limit) { return limit > 0.0 ? kPi * limit * limit : area(l.outline); }

/** How built up the settlement is at p: 1 at the centre, thinning outward. */
double density(const Layout& l, const Profile& p, Vec2 q, double radius)
{
    const double d = glm::length(q - l.center) / std::max(radius, 1.0);
    return std::exp(-p.falloff * d * d);
}

double classWeight(StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return 3.0;
    case StreetClass::Collector: return 2.0;
    case StreetClass::Local: return 1.0;
    case StreetClass::Alley: return 0.3;
    case StreetClass::Boundary:
    case StreetClass::Water: return 0.0;
    }
    return 1.0;
}

StreetClass childClass(StreetClass parent, double r)
{
    switch (parent)
    {
    case StreetClass::Arterial: return StreetClass::Collector;
    case StreetClass::Collector: return StreetClass::Local;
    default: return r < 0.2 ? StreetClass::Alley : StreetClass::Local;
    }
}

double totalStreetLength(const StreetGraph& g)
{
    double len = 0.0;
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)))
            len += g.length(int(e));
    return len;
}

/** Points where arterials leave: evenly spread round the centre, jittered, on the outline or a ring. */
std::vector<Vec2> gatePoints(const Layout& l, const Profile& p, Rng& rng, double limit)
{
    std::vector<Vec2> out;
    const int n = std::max(1, p.pattern == Pattern::Linear ? 2 : p.gates);
    const double base = rng.range(0.0, 2.0 * kPi);
    for (int i = 0; i < n; ++i)
    {
        const double a = base + 2.0 * kPi * i / n + (p.pattern == Pattern::Linear ? 0.0 : rng.range(-0.35, 0.35) * kPi / n);
        const Vec2 d(std::cos(a), std::sin(a));
        out.push_back(limit > 0.0 ? l.center + d * limit : exitPoint(l.outline, l.center, d));
    }
    return out;
}

/** A square of streets round the centre, the market place. Empty when it does not fit inside the outline. */
std::vector<Vec2> addPlaza(Layout& l, const Profile& p, double angle)
{
    const double half = std::max(1.25 * p.lotDepth, 2.0 * p.widthCollector);
    std::vector<Vec2> sq;
    for (int k = 0; k < 4; ++k)
        sq.push_back(l.center + rotate(Vec2(k == 0 || k == 3 ? -half : half, k < 2 ? -half : half), angle));
    for (size_t k = 0; k < sq.size(); ++k)
        if (!contains(l.outline.pts, sq[k]) || crossesExclusion(l, sq[k], sq[(k + 1) % sq.size()]))
            return {};
    l.graph.addPolyline(sq, StreetClass::Collector, 0, true);
    return sq;
}
bool onWater(const StreetGraph& g, int node)
{
    for (int e : g.incident(node))
        if (g.edges[size_t(e)].cls == StreetClass::Water)
            return true;
    return false;
}

/**
 * Street networks cut off from the one at the centre (pieces of a passing road, streets stopped by a river) are linked
 * to it by the shortest lane, or dropped when they are scraps.
 */
void joinComponents(Layout& l, const Ground& g)
{
    StreetGraph& graph = l.graph;
    const Profile& p = l.profile;
    std::vector<int> parent(graph.nodes.size());
    for (size_t k = 0; k < parent.size(); ++k)
        parent[k] = int(k);
    auto root = [&](int x) {
        while (parent[size_t(x)] != x)
            x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        return x;
    };
    for (size_t e = 0; e < graph.edges.size(); ++e)
        if (graph.isStreet(int(e)))
            parent[size_t(root(graph.edges[e].a))] = root(graph.edges[e].b);
    int centre = -1;
    double best = std::numeric_limits<double>::max();
    std::map<int, std::vector<int>> members;
    for (size_t n = 0; n < graph.nodes.size(); ++n)
    {
        if (graph.degree(int(n)) == 0)
            continue;
        members[root(int(n))].push_back(int(n));
        const double d = glm::length(graph.nodes[n].p - l.center);
        if (d < best)
        {
            best = d;
            centre = int(n);
        }
    }
    if (centre < 0)
        return;
    const int main = root(centre);
    const std::vector<int> mainNodes = members[main];
    for (const auto& [r, nodes] : members)
    {
        if (r == main)
            continue;
        double gap = std::numeric_limits<double>::max();
        int from = -1, to = -1;
        for (int a : nodes)
            for (int b : mainNodes)
            {
                const double d = glm::length(graph.nodes[size_t(a)].p - graph.nodes[size_t(b)].p);
                if (d < gap)
                {
                    gap = d;
                    from = a;
                    to = b;
                }
            }
        double length = 0.0;
        for (int a : nodes)
            for (int e : graph.incident(a))
                if (graph.isStreet(e))
                    length += 0.5 * graph.length(e);
        const Vec2 pa = graph.nodes[size_t(from)].p, pb = graph.nodes[size_t(to)].p;
        const Vec2 ends[] = {pa, pb};
        const bool dry = l.water.empty() || cutAtWater(std::vector<Vec2>(ends, ends + 2), l.water).size() == 1;
        if (gap < 3.0 * p.segment && dry && g.gradeAlong(pa, pb) <= steepLimit(p, 1.5) && !crossesExclusion(l, pa, pb))
            graph.addStreet(pa, pb, StreetClass::Local);
        else if (length >= p.segment)
        {
            // Too far or too steep for a straight link: a street routed over the ground joins it instead.
            const std::vector<Vec2> path = route(l, g, p, pa, pb);
            if (path.size() >= 2)
            {
                // Where the link has to cross water (a moat, a river between quarters) it does so on a bridge.
                for (int e : graph.addPolyline(path, StreetClass::Local))
                {
                    const Vec2 ea = graph.nodes[size_t(graph.edges[size_t(e)].a)].p, eb = graph.nodes[size_t(graph.edges[size_t(e)].b)].p;
                    bool wet = bankDistance((ea + eb) * 0.5, l.water) < 0.0;
                    for (size_t w = 0; w < l.water.size() && !wet; ++w)
                        wet = segmentCrosses(l.water[w].pts, ea, eb);
                    graph.edges[size_t(e)].bridge = wet;
                }
            }
            else
                l.streetWarnings.push_back(std::format("a street network of {:.0f} m near ({:.0f}, {:.0f}) has no way to the town's: left unconnected",
                                                       length / 100.0, pa.x / 100.0, pa.y / 100.0));
        }
        else
            for (int a : nodes)
            {
                const std::vector<int> inc = graph.incident(a);
                for (int e : inc)
                    if (graph.isStreet(e) && graph.edges[size_t(e)].path < 0)
                        graph.removeEdge(e);
            }
    }
}

/**
 * Generated streets other than arterials that still cross or run in water after every generator (a moat dug around
 * streets already grown, a later era's growth along a bank) are removed: only main roads bridge.
 */
void dropStreetsInWater(Layout& l)
{
    if (l.water.empty())
        return;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        const StreetEdge& x = l.graph.edges[e];
        if (!l.graph.isStreet(int(e)) || x.cls == StreetClass::Arterial || x.bridge || x.tunnel || x.path >= 0)
            continue;
        const Vec2 a = l.graph.nodes[size_t(x.a)].p, b = l.graph.nodes[size_t(x.b)].p;
        bool wet = bankDistance((a + b) * 0.5, l.water) < 0.0;
        for (size_t w = 0; w < l.water.size() && !wet; ++w)
            wet = segmentCrosses(l.water[w].pts, a, b);
        if (wet)
            l.graph.removeEdge(int(e));
    }
}

/**
 * Generated street fragments the wall and water cuts left with no way to the town's network are removed, and the count
 * reported; fragments holding a street you drew stay.
 */
void dropStranded(Layout& l)
{
    StreetGraph& graph = l.graph;
    std::vector<int> parent(graph.nodes.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int x) {
        while (parent[size_t(x)] != x)
            x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        return x;
    };
    for (size_t e = 0; e < graph.edges.size(); ++e)
        if (graph.isStreet(int(e)))
            parent[size_t(root(graph.edges[e].a))] = root(graph.edges[e].b);
    int centre = -1;
    double best = std::numeric_limits<double>::max();
    for (size_t n = 0; n < graph.nodes.size(); ++n)
        if (graph.degree(int(n)) > 0)
            if (const double d = glm::length(graph.nodes[n].p - l.center); d < best)
            {
                best = d;
                centre = int(n);
            }
    if (centre < 0)
        return;
    const int main = root(centre);
    std::set<int> drawn;
    for (size_t e = 0; e < graph.edges.size(); ++e)
        if (graph.isStreet(int(e)) && graph.edges[e].path >= 0)
            drawn.insert(root(graph.edges[e].a));
    int removed = 0;
    double length = 0.0;
    for (size_t e = 0; e < graph.edges.size(); ++e)
        if (graph.isStreet(int(e)))
            if (const int r = root(graph.edges[e].a); r != main && !drawn.count(r))
            {
                length += graph.length(int(e));
                graph.removeEdge(int(e));
                ++removed;
            }
    if (removed)
        l.streetWarnings.push_back(std::format("{} street pieces ({:.0f} m) cut off by walls or water were removed", removed, length / 100.0));
}

/**
 * Streets other than arterials that reach a node on a river (snapped onto a bridge mid-span) are pulled back to dry
 * ground and end there.
 */
void pullBackFromWater(Layout& l)
{
    StreetGraph& g = l.graph;
    const size_t nodes = g.nodes.size();
    for (size_t n = 0; n < nodes; ++n)
    {
        double half = 0.0;
        for (int e : g.incident(int(n)))
            if (g.edges[size_t(e)].cls == StreetClass::Water)
                half = std::max(half, 0.5 * g.edges[size_t(e)].width);
        if (half <= 0.0)
            continue;
        std::vector<int> wet;
        for (int e : g.incident(int(n)))
            if (g.isStreet(e) && g.edges[size_t(e)].cls != StreetClass::Arterial)
                wet.push_back(e);
        for (int e : wet)
        {
            const StreetEdge x = g.edges[size_t(e)];
            const Vec2 at = g.nodes[n].p, far = g.nodes[size_t(x.a == int(n) ? x.b : x.a)].p;
            const double len = glm::length(far - at), pull = half + 300.0;
            g.removeEdge(e);
            if (len > pull + 2.0 * g.snap())
                g.addStreet(far, at + (far - at) / len * pull, x);
        }
    }
}

/** A planned line laid as streets: clipped to the outline and radii, split where too steep, gapped at water unless arterial. */
void layLine(Layout& l, const Ground& g, const Profile& p, std::span<const Vec2> pts, StreetClass cls, int era, double inner, double step)
{
    for (const std::vector<Vec2>& run : clipPolyline(pts, l.outline, l.center, inner, 0.0, l.exclusions))
        for (const std::vector<Vec2>& part : climbable(run, g, steepLimit(p, 1.5), step))
        {
            if (cls == StreetClass::Arterial)
                l.graph.addPolyline(part, cls, era);
            else
                for (const std::vector<Vec2>& dry : cutAtWater(part, l.water))
                    l.graph.addPolyline(dry, cls, era);
        }
}

std::vector<Face> layoutFaces(const Layout& l);
} // namespace

double edgeWidth(const Layout& l, int edge)
{
    const StreetEdge& e = l.graph.edges[size_t(edge)];
    if (e.width > 0.0)
        return e.width;
    if (e.profile >= 0 && size_t(e.profile) < l.roads.size())
        return l.roads[size_t(e.profile)].width();
    return streetWidth(l.profile, e.cls);
}

const RoadProfile& edgeProfile(const Layout& l, int edge)
{
    static const RoadProfile fallback;
    const StreetEdge& e = l.graph.edges[size_t(edge)];
    if (e.profile >= 0 && size_t(e.profile) < l.roads.size())
        return l.roads[size_t(e.profile)];
    const size_t c = size_t(std::min(e.cls, StreetClass::Path));
    return c < l.roads.size() ? l.roads[c] : fallback;
}

bool excluded(const Layout& l, Vec2 q) { return insideAny(l.exclusions, q); }

bool crossesExclusion(const Layout& l, Vec2 a, Vec2 b)
{
    for (const Polygon& x : l.exclusions)
        if (contains(x.pts, a) || contains(x.pts, b) || segmentCrosses(x.pts, a, b))
            return true;
    return false;
}

std::vector<Vec2> route(const Layout& l, const Ground& g, const Profile& p, Vec2 a, Vec2 b, double clearance)
{
    LILA_ZONE();

    if (clearance < 0.0)
        clearance = 0.5 * p.widthArterial;
    if (l.routeBook)
        if (const std::vector<Vec2>* booked = l.routeBook->use(a, b, p.maxGrade))
            return *booked;
    auto clearOf = [&](Vec2 x, Vec2 y) {
        for (const Polygon& h : l.exclusions)
            if (segmentDistance(h.pts, x, y) < clearance)
                return false;
        return true;
    };
    if (g.isPlane())
    {
        const Vec2 ends[] = {a, b};
        if (clipPolyline(ends, l.outline, l.center, 0.0, 0.0).size() <= 1 && clearOf(a, b))
            return {a, b};
    }
    Vec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const Vec2& q : l.outline.pts)
    {
        lo = glm::min(lo, q);
        hi = glm::max(hi, q);
    }
    const Vec2 ext = hi - lo;
    // Exclusions need a finer grid, so routes find the gaps between them.
    const double cell = l.exclusions.empty()
                            ? std::max({p.widthArterial, ext.x / 256.0, ext.y / 256.0, 50.0})
                            : std::max({std::min(p.widthArterial, std::max(ext.x, ext.y) / 128.0), ext.x / 640.0, ext.y / 640.0, 50.0});
    const int w = int(std::ceil(ext.x / cell)) + 1, h = int(std::ceil(ext.y / cell)) + 1;
    auto at = [&](int x, int y) { return lo + Vec2(x * cell, y * cell); };
    auto cellOf = [&](Vec2 q) {
        return std::pair<int, int>{std::clamp(int(std::lround((q.x - lo.x) / cell)), 0, w - 1),
                                   std::clamp(int(std::lround((q.y - lo.y) / cell)), 0, h - 1)};
    };
    const auto [sx, sy] = cellOf(a);
    const auto [tx, ty] = cellOf(b);
    std::vector<double> height(size_t(w) * h);
    std::vector<char> open(size_t(w) * h), wet(size_t(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const Vec2 q = at(x, y);
            height[size_t(y) * w + x] = g.height(q);
            open[size_t(y) * w + x] = contains(l.outline.pts, q) && g.solid(q);
            wet[size_t(y) * w + x] = !l.water.empty() && bankDistance(q, l.water) < 0.0;
        }
    // Exclusions close their cells and a margin of half the road around them.
    const double keep = clearance + 0.5 * cell;
    for (const Polygon& x : l.exclusions)
    {
        Vec2 xlo(std::numeric_limits<double>::max()), xhi(-std::numeric_limits<double>::max());
        for (const Vec2& q : x.pts)
        {
            xlo = glm::min(xlo, q);
            xhi = glm::max(xhi, q);
        }
        const auto [x0, y0] = cellOf(xlo - Vec2(keep));
        const auto [x1, y1] = cellOf(xhi + Vec2(keep));
        for (int y = y0; y <= y1; ++y)
            for (int cx = x0; cx <= x1; ++cx)
            {
                const Vec2 q = at(cx, y);
                if (contains(x.pts, q) || boundaryDistance(x.pts, q) < keep)
                    open[size_t(y) * w + cx] = 0;
            }
    }
    open[size_t(sy) * w + sx] = open[size_t(ty) * w + tx] = 1;

    std::vector<double> dist(size_t(w) * h, std::numeric_limits<double>::max());
    std::vector<int> from(size_t(w) * h, -1);
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> q;
    const int start = sy * w + sx, goal = ty * w + tx;
    auto heuristic = [&](int i) {
        const double dx = std::abs(i % w - tx), dy = std::abs(i / w - ty);
        return cell * (std::max(dx, dy) + (std::sqrt(2.0) - 1.0) * std::min(dx, dy));
    };
    dist[size_t(start)] = 0.0;
    q.push({heuristic(start), start});
    while (!q.empty())
    {
        const auto [f, i] = q.top();
        q.pop();
        if (i == goal)
            break;
        if (f - heuristic(i) > dist[size_t(i)] + 1e-9)
            continue;
        const int x = i % w, y = i / w;
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx)
            {
                const int nx = x + dx, ny = y + dy;
                if ((dx == 0 && dy == 0) || nx < 0 || ny < 0 || nx >= w || ny >= h)
                    continue;
                const int j = ny * w + nx;
                if (!open[size_t(j)])
                    continue;
                // No corner cutting past a closed cell: the diagonal would graze an exclusion.
                if (dx != 0 && dy != 0 && (!open[size_t(y * w + nx)] || !open[size_t(ny * w + x)]))
                    continue;
                const double run = cell * std::sqrt(double(dx * dx + dy * dy));
                const double grade = std::abs(height[size_t(j)] - height[size_t(i)]) / run;
                if (grade > steepLimit(p, 2.5))
                    continue;
                const double r = grade / std::max(p.maxGrade, 1e-3);
                // Water costs a bridge: roads cross rivers rarely, and straight across.
                const double nd = dist[size_t(i)] + run * (1.0 + 8.0 * r * r) * (wet[size_t(j)] ? 25.0 : 1.0);
                if (nd < dist[size_t(j)])
                {
                    dist[size_t(j)] = nd;
                    from[size_t(j)] = i;
                    q.push({nd + heuristic(j), j});
                }
            }
    }
    if (from[size_t(goal)] < 0 && goal != start)
        return {};
    std::vector<Vec2> path;
    for (int i = goal; i != -1; i = from[size_t(i)])
        path.push_back(at(i % w, i / w));
    std::reverse(path.begin(), path.end());
    path.front() = a;
    path.back() = b;
    std::vector<char> keepPt(path.size(), 0);
    keepPt.front() = keepPt.back() = 1;
    simplify(path, 0, path.size() - 1, cell * 0.7, keepPt);
    std::vector<Vec2> simple;
    for (size_t k = 0; k < path.size(); ++k)
        if (keepPt[k])
            simple.push_back(path[k]);
    std::vector<Vec2> smooth = chaikin(simple, 2);
    // Smoothing must not pull a bend into an exclusion; where it would, the bend stays sharp.
    for (size_t k = 0; k + 1 < smooth.size(); ++k)
        if (!l.exclusions.empty() && crossesExclusion(l, smooth[k], smooth[k + 1]))
            return simple;
    return smooth;
}

namespace
{
/**
 * Picks street edges by a key from where each one lies, so a pick lands on the same street whatever else the graph
 * holds: u in [0, 1) takes the first edge whose key is at or past it.
 */
class ParentPicker
{
public:
    explicit ParentPicker(const StreetGraph& g) : mGraph(g) { update(); }

    /** Takes in the edges added or reshaped since the last call. */
    void update()
    {
        for (size_t e = mSeen; e < mGraph.edges.size(); ++e)
            add(int(e));
        mSeen = mGraph.edges.size();
    }

    int pick(double u)
    {
        while (!mByKey.empty())
        {
            auto it = mByKey.lower_bound(uint64_t(u * 18446744073709551615.0));
            if (it == mByKey.end())
                it = mByKey.begin();
            const auto [k, e] = *it;
            if (mGraph.isStreet(e) && keyOf(e) == k)
                return e;
            // Split or removed since it went in: it goes back under its key now, if it is still a street.
            mByKey.erase(it);
            add(e);
        }
        return -1;
    }

private:
    const StreetGraph& mGraph;
    std::map<uint64_t, int> mByKey;
    size_t mSeen = 0;

    uint64_t keyOf(int e) const
    {
        const StreetEdge& x = mGraph.edges[size_t(e)];
        auto q = [&](int n) { return std::pair{std::llround(mGraph.nodes[size_t(n)].p.x), std::llround(mGraph.nodes[size_t(n)].p.y)}; };
        const auto qa = q(x.a), qb = q(x.b);
        const auto [lo, hi] = std::minmax(qa, qb);
        return hash64(hash64(hash64(hash64(uint64_t(lo.first)) ^ uint64_t(lo.second)) ^ uint64_t(hi.first)) ^ uint64_t(hi.second));
    }

    void add(int e)
    {
        if (mGraph.isStreet(e))
            mByKey.emplace(keyOf(e), e);
    }
};
} // namespace

void growOrganic(Layout& l, const Ground& g, const Options& o, double limit)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    Rng gateRng(o.seed, 1);
    const double radius = limit > 0.0 ? limit : l.radius;
    const std::vector<Vec2> gates = gatePoints(l, p, gateRng, limit);

    std::vector<Vec2> targets = gates;
    if (p.pattern == Pattern::Linear && gates.size() == 2)
    {
        std::vector<Vec2> through = route(l, g, p, gates[0], gates[1]);
        if (through.empty())
            through = {gates[0], gates[1]};
        l.graph.addPolyline(through, StreetClass::Arterial);
        targets.clear();
    }
    std::vector<Vec2> plaza;
    if (p.plaza && !gates.empty())
    {
        const Vec2 d = gates.front() - l.center;
        plaza = addPlaza(l, p, std::atan2(d.y, d.x));
    }
    Polygon square;
    square.pts = plaza;
    for (const Vec2& t : targets)
    {
        // Roads arrive at the market's edge, so the square itself stays open.
        const Vec2 from = plaza.empty() ? l.center : exitPoint(square, l.center, glm::normalize(t - l.center));
        std::vector<Vec2> path = route(l, g, p, from, t);
        if (path.empty())
            path = {from, t};
        l.graph.addPolyline(path, StreetClass::Arterial);
    }

    const double target = 2.5e-4 * settlementArea(l, limit) * p.density * std::clamp(o.growth, 0.0, 1.0);
    double built = totalStreetLength(l.graph);
    const double snap = l.graph.snap();
    const double reach = std::max(snap, p.spacing * 0.35);
    const int attempts = int(std::clamp(target / std::max(p.segment, 1.0) * 25.0, 50.0, 200000.0));
    const double branchTurn = (90.0 - std::clamp(p.minAngle, 0.0, 89.0)) * kPi / 180.0;
    const double bend = p.turn * kPi / 180.0;
    const double linearDamp = p.pattern == Pattern::Linear ? 0.25 : 1.0;

    ParentPicker parents(l.graph);
    for (int attempt = 0; attempt < attempts && built < target; ++attempt)
    {
        // Each attempt draws from its own stream and picks its parent by where streets lie, not by their order in the
        // graph: an edit somewhere changes only the attempts that land near it.
        Rng rng(o.seed, hash64(0x6A09E667ull + uint64_t(attempt)));
        const int e = parents.pick(rng.next());
        if (e < 0)
            break;
        const StreetEdge parent = l.graph.edges[size_t(e)];
        const Vec2 pa = l.graph.nodes[size_t(parent.a)].p, pb = l.graph.nodes[size_t(parent.b)].p;
        const double plen = glm::length(pb - pa);
        if (plen < 2.0 * snap)
            continue;
        const Vec2 mid = (pa + pb) * 0.5;
        if (rng.next() > classWeight(parent.cls) / 3.0 * density(l, p, mid, radius) * linearDamp)
            continue;

        const Vec2 along = (pb - pa) / plen;
        const Vec2 start = pa + (pb - pa) * rng.range(0.2, 0.8);
        Vec2 dir = rotate(leftNormal(along) * (rng.next() < 0.5 ? -1.0 : 1.0), rng.range(-branchTurn, branchTurn));
        if (!plaza.empty() && contains(plaza, start + dir * (2.0 * snap)))
            dir = -dir;
        const StreetClass cls = childClass(parent.cls, rng.next());

        std::vector<Vec2> pts = {start};
        Vec2 pos = start;
        bool refused = false;
        const int segments = 2 + int(rng.next() * 5.0);
        for (int s = 0; s < segments; ++s)
        {
            double len = p.segment * rng.range(0.7, 1.3);
            Vec2 next = pos + dir * len;
            if (g.gradeAlong(pos, next) > p.maxGrade)
            {
                // Too steep: turn across the slope, the way old roads do.
                bool found = false;
                for (double turn : {25.0, -25.0, 50.0, -50.0, 75.0, -75.0})
                {
                    const Vec2 d = rotate(dir, turn * kPi / 180.0);
                    if (g.gradeAlong(pos, pos + d * len) <= p.maxGrade)
                    {
                        dir = d;
                        next = pos + d * len;
                        found = true;
                        break;
                    }
                }
                if (!found)
                    break;
            }
            // Local rules: stop at the first street crossed, or join one close by.
            Vec2 hit{0.0};
            double sAt = 0.0;
            const double skip = s == 0 ? (snap * 0.2) / len : 0.0;
            int crossed = l.graph.firstCrossing(pos, next, &hit, &sAt);
            if (crossed >= 0 && sAt <= skip)
                crossed = l.graph.firstCrossing(pos + dir * (snap * 0.2), next, &hit, &sAt);
            if (crossed >= 0)
            {
                // Rivers stop every street but the main roads; the street ends on its own bank.
                if (l.graph.edges[size_t(crossed)].cls == StreetClass::Water && cls != StreetClass::Arterial)
                    break;
                pts.push_back(hit);
                break;
            }
            if (!l.water.empty() && bankDistance(next, l.water) < 0.5 * p.widthLocal)
                break;
            if (limit > 0.0 && glm::length(next - l.center) > limit)
                break;
            if (!g.solid(next) || !contains(l.outline.pts, next) || excluded(l, next) || (!plaza.empty() && contains(plaza, next)))
                break;
            if (int n = l.graph.nearestNode(next, reach); n >= 0)
            {
                if (l.graph.degree(n) > 0 && !onWater(l.graph, n))
                    pts.push_back(l.graph.nodes[size_t(n)].p);
                break;
            }
            Vec2 near{0.0};
            if (const int ne = l.graph.nearestEdge(next, reach, &near); ne >= 0)
            {
                if (l.graph.edges[size_t(ne)].cls != StreetClass::Water)
                    pts.push_back(near);
                break;
            }
            // Refuse streets that would run alongside another one too closely.
            const Vec2 m = (pos + next) * 0.5;
            Vec2 side{0.0};
            const int close = l.graph.nearestEdge(m, p.spacing * 0.5, &side);
            if (close >= 0 && close != e)
            {
                const StreetEdge& c = l.graph.edges[size_t(close)];
                const Vec2 cd = glm::normalize(l.graph.nodes[size_t(c.b)].p - l.graph.nodes[size_t(c.a)].p);
                if (std::abs(glm::dot(cd, dir)) > 0.85)
                {
                    refused = true;
                    break;
                }
            }
            pts.push_back(next);
            pos = next;
            dir = rotate(dir, rng.range(-bend, bend));
        }
        if (refused || pts.size() < 2)
        {
            ++l.refused;
            continue;
        }
        l.graph.addPolyline(pts, cls);
        parents.update();
        built += polylineLength(pts);
    }
}

void layGrid(Layout& l, const Ground& g, const Options& o, double inner)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    const double a = p.gridAngle * kPi / 180.0;
    const Vec2 u(std::cos(a), std::sin(a)), v = leftNormal(u);
    double u0 = std::numeric_limits<double>::max(), u1 = -u0, v0 = u0, v1 = -u0;
    for (const Vec2& q : l.outline.pts)
    {
        const Vec2 d = q - l.center;
        u0 = std::min(u0, glm::dot(d, u)), u1 = std::max(u1, glm::dot(d, u));
        v0 = std::min(v0, glm::dot(d, v)), v1 = std::max(v1, glm::dot(d, v));
    }
    const double pu = std::max(p.gridWidth + p.widthLocal, 100.0), pv = std::max(p.gridDepth + p.widthLocal, 100.0);
    const int every = std::max(1, p.arterialEvery);
    const int era = inner > 0.0 ? 1 : 0;
    const double step = std::max(std::min(pu, pv) * 0.25, 200.0);
    auto lay = [&](Vec2 along, Vec2 across, double c0, double c1, double t0, double t1, double pitch) {
        for (int i = int(std::floor(c0 / pitch)); i <= int(std::ceil(c1 / pitch)); ++i)
        {
            const Vec2 base = l.center + across * (i * pitch);
            const Vec2 line[] = {base + along * (t0 - pitch), base + along * (t1 + pitch)};
            layLine(l, g, p, line, i % every == 0 ? StreetClass::Arterial : StreetClass::Local, era, inner, step);
        }
    };
    lay(v, u, u0, u1, v0, v1, pu);
    lay(u, v, v0, v1, u0, u1, pv);
}

void layRadial(Layout& l, const Ground& g, const Options& o)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    const int rings = std::max(1, p.rings), spokes = std::max(3, p.spokes);
    const double step = std::max(p.segment * 0.25, 200.0);
    for (int k = 1; k <= rings; ++k)
    {
        const double r = l.radius * double(k) / (rings + 0.5);
        const int segs = std::max(24, int(2.0 * kPi * r / std::max(p.segment, 100.0)));
        std::vector<Vec2> ring;
        for (int s = 0; s <= segs; ++s)
        {
            const double t = 2.0 * kPi * s / segs;
            ring.push_back(l.center + Vec2(std::cos(t), std::sin(t)) * r);
        }
        layLine(l, g, p, ring, (k == 1 || k == rings) ? StreetClass::Arterial : StreetClass::Collector, 0, 0.0, step);
    }
    const double hub = l.radius / (rings + 0.5);
    for (int s = 0; s < spokes; ++s)
    {
        const double t = 2.0 * kPi * s / spokes + p.gridAngle * kPi / 180.0;
        const Vec2 d(std::cos(t), std::sin(t));
        const Vec2 line[] = {l.center + d * hub, l.center + d * (l.radius * 1.5)};
        layLine(l, g, p, line, StreetClass::Arterial, 0, 0.0, step);
    }
}

void cutLargeBlocks(Layout& l, const Ground& g, const Options& o)
{
    LILA_ZONE();

    const Profile& p = o.profile;
    const StreetClass cls = (p.pattern == Pattern::Grid || p.pattern == Pattern::OrganicGrid) ? StreetClass::Alley : StreetClass::Local;
    for (int pass = 0; pass < 8; ++pass)
    {
        bool cut = false;
        for (const Face& f : layoutFaces(l))
        {
            const Polygon& poly = f.outline;
            if (area(poly) / kM2 <= p.maxBlock || (p.plaza && contains(poly.pts, l.center)))
                continue;
            // Cut across the long way through the middle, along the block's longest side.
            size_t longest = 0;
            double best = 0.0;
            for (size_t k = 0; k < poly.size(); ++k)
            {
                const double len = glm::length(poly.pts[(k + 1) % poly.size()] - poly.pts[k]);
                if (len > best)
                {
                    best = len;
                    longest = k;
                }
            }
            const Vec2 u = glm::normalize(poly.pts[(longest + 1) % poly.size()] - poly.pts[longest]), v = leftNormal(u);
            const Vec2 c = centroid(poly);
            // Seeded by where the block is, so an edit elsewhere leaves this cut where it was.
            Rng rng(o.seed, hash64(hash64(uint64_t(std::llround(c.x / 10.0))) ^ uint64_t(std::llround(c.y / 10.0))) + uint64_t(pass));
            double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
            for (const Vec2& q : poly.pts)
            {
                u0 = std::min(u0, glm::dot(q - c, u)), u1 = std::max(u1, glm::dot(q - c, u));
                v0 = std::min(v0, glm::dot(q - c, v)), v1 = std::max(v1, glm::dot(q - c, v));
            }
            const bool acrossU = (u1 - u0) >= (v1 - v0);
            const Vec2 axis = acrossU ? u : v, lineDir = acrossU ? v : u;
            const double offset = (acrossU ? (u0 + u1) : (v0 + v1)) * 0.5 + rng.range(-0.1, 0.1) * (acrossU ? (u1 - u0) : (v1 - v0));
            const Vec2 o0 = c + axis * offset;
            const double reach = (u1 - u0) + (v1 - v0);
            const Vec2 line[] = {o0 - lineDir * reach, o0 + lineDir * reach};
            // The piece of the cut line that lies in this block.
            for (const std::vector<Vec2>& run : clipPolyline(line, poly, c, 0.0, 0.0))
            {
                if (run.size() < 2 || g.gradeAlong(run.front(), run.back()) > steepLimit(p, 1.5))
                    continue;
                // A block on the river: the lane stops short of the bank.
                for (const std::vector<Vec2>& dry : cutAtWater(run, l.water))
                {
                    l.graph.addStreet(dry.front(), dry.back(), cls);
                    cut = true;
                }
                break;
            }
        }
        if (!cut)
            break;
    }
}

std::vector<Lot> subdivide(const Block& b, const Options& o, int firstId)
{
    const Profile& p = o.profile;
    std::vector<Lot> lots;
    std::vector<Polygon> todo = {b.outline};
    Rng rng(o.seed, 1000003ull + uint64_t(b.id));
    for (int guard = 0; guard < 8192 && !todo.empty(); ++guard)
    {
        Polygon poly = std::move(todo.back());
        todo.pop_back();
        const double a = area(poly);
        // Frame on the longest side facing a street: u along the frontage, v into the lot.
        int front = -1;
        double frontLen = 0.0;
        for (size_t k = 0; k < poly.size(); ++k)
        {
            const double len = glm::length(poly.pts[(k + 1) % poly.size()] - poly.pts[k]);
            if (poly.tag[k] >= 0 && len > frontLen)
            {
                frontLen = len;
                front = int(k);
            }
        }
        bool split = false;
        if (front >= 0 && a > 2.0 * p.minLot * kM2)
        {
            const Vec2 o0 = poly.pts[size_t(front)];
            const Vec2 u = glm::normalize(poly.pts[(size_t(front) + 1) % poly.size()] - o0), v = leftNormal(u);
            double u0 = 1e300, u1 = -1e300, v0 = 1e300, v1 = -1e300;
            for (const Vec2& q : poly.pts)
            {
                u0 = std::min(u0, glm::dot(q - o0, u)), u1 = std::max(u1, glm::dot(q - o0, u));
                v0 = std::min(v0, glm::dot(q - o0, v)), v1 = std::max(v1, glm::dot(q - o0, v));
            }
            const double width = u1 - u0, depth = v1 - v0;
            Vec2 cutAt{0.0}, normal{0.0};
            if (depth > 2.2 * p.lotDepth)
            {
                // Back to back: each half keeps the street it faces.
                normal = v;
                cutAt = o0 + v * (v0 + depth * rng.range(0.45, 0.55));
                split = true;
            }
            else if (width > 1.6 * p.lotWidth)
            {
                const int n = std::max(2, int(std::lround(width / p.lotWidth)));
                const int left = n / 2;
                normal = u;
                cutAt = o0 + u * (u0 + width * (double(left) / n) + rng.range(-0.08, 0.08) * p.lotWidth);
                split = true;
            }
            if (split)
            {
                Polygon x = clipHalfPlane(poly, cutAt, normal), y = clipHalfPlane(poly, cutAt, -normal);
                if (x.pts.empty() || y.pts.empty())
                    split = false;
                else
                {
                    todo.push_back(std::move(x));
                    todo.push_back(std::move(y));
                }
            }
        }
        if (split)
            continue;
        Lot lot;
        lot.id = firstId + int(lots.size());
        lot.block = b.id;
        lot.area = a / kM2;
        lot.random = float(rng.next());
        int best = int(StreetClass::Boundary);
        std::vector<Vec2> fronts;
        for (size_t k = 0; k < poly.size(); ++k)
        {
            const Vec2 e = poly.pts[(k + 1) % poly.size()] - poly.pts[k];
            if (poly.tag[k] < 0 || glm::length(e) < 100.0)
                continue;
            ++lot.frontages;
            best = std::min(best, poly.tag[k]);
            const Vec2 d = glm::normalize(e);
            for (const Vec2& f : fronts)
                if (std::abs(glm::dot(f, d)) < 0.5)
                    lot.corner = true;
            fronts.push_back(d);
        }
        lot.frontClass = StreetClass(std::min(best, int(StreetClass::Alley)));
        lot.outline = std::move(poly);
        lots.push_back(std::move(lot));
    }
    std::sort(lots.begin(), lots.end(), [](const Lot& x, const Lot& y) {
        const Vec2 cx = centroid(x.outline), cy = centroid(y.outline);
        return cx.x != cy.x ? cx.x < cy.x : cx.y < cy.y;
    });
    for (size_t k = 0; k < lots.size(); ++k)
        lots[k].id = firstId + int(k);
    return lots;
}

Envelope envelopeFor(const Lot& lot, const Layout& l, const Ground& g, const Options& o)
{
    const Profile& p = o.profile;
    Envelope env;
    env.lot = lot.id;
    env.use = lot.use;
    env.roof = p.roof;
    env.random = lot.random;
    const Polygon& poly = lot.outline;
    std::vector<double> d(poly.size());
    int front = -1;
    double frontLen = 0.0;
    for (size_t k = 0; k < poly.size(); ++k)
    {
        d[k] = poly.tag[k] >= 0 ? p.frontSetback : p.sideSetback;
        const double len = glm::length(poly.pts[(k + 1) % poly.size()] - poly.pts[k]);
        if (poly.tag[k] >= 0 && len > frontLen)
        {
            frontLen = len;
            front = int(k);
        }
    }
    Polygon fp = inset(poly, d);
    if (fp.pts.empty() || front < 0)
        return env;
    const Vec2 o0 = poly.pts[size_t(front)];
    const Vec2 u = glm::normalize(poly.pts[(size_t(front) + 1) % poly.size()] - o0), v = leftNormal(u);
    // Rear of the building: the yard behind it stays open. -2 marks that cut.
    fp = clipHalfPlane(fp, o0 + v * (p.frontSetback + p.buildDepth), -v, -2);
    if (fp.pts.empty() || area(fp) < 0.25 * p.minLot * kM2)
        return env;
    for (int& t : fp.tag)
        t = t >= 0 ? 0 : (t == -1 && p.sideSetback < 1.0 ? 1 : 2);
    env.footprint = std::move(fp);
    env.front = -v;

    const double dist = std::clamp(glm::length(centroid(env.footprint) - l.center) / std::max(l.radius, 1.0), 0.0, 1.0);
    double floors = p.minFloors + (p.maxFloors - p.minFloors) * std::pow(1.0 - dist, std::max(p.falloff, 0.1));
    floors *= 0.8 + 0.4 * double(lot.random);
    if (lot.use == Use::Commercial)
        floors += 1.0;
    if (lot.use == Use::Civic)
        floors = floors * 1.5 + 1.0;
    floors *= std::max(o.heightScale, 0.0);
    env.floors = std::max(1, int(std::lround(floors)));
    env.height = env.floors * p.floorHeight;

    double lo = std::numeric_limits<double>::max(), hi = -lo;
    std::vector<Vec2> samples = env.footprint.pts;
    samples.push_back(centroid(env.footprint));
    for (const Vec2& q : samples)
    {
        const double h = g.height(q);
        lo = std::min(lo, h);
        hi = std::max(hi, h);
    }
    env.ground = lo;
    env.base = lo - p.foundation;
    env.top = hi + env.height;
    return env;
}

namespace
{
/** A point strictly inside the face, a hair to the left of its longest side. */
Vec2 interiorPoint(const Polygon& f)
{
    const double sign = signedArea(f.pts) >= 0.0 ? 1.0 : -1.0;
    size_t best = 0;
    double len = -1.0;
    for (size_t k = 0; k < f.size(); ++k)
        if (const double d = glm::length(f.pts[(k + 1) % f.size()] - f.pts[k]); d > len)
        {
            len = d;
            best = k;
        }
    const Vec2 a = f.pts[best], b = f.pts[(best + 1) % f.size()];
    const Vec2 d = (b - a) / std::max(len, 1e-12);
    return (a + b) * 0.5 + leftNormal(d) * sign * std::min(1.0, 0.01 * len);
}

/** Where q ends up when it must stand clear of the exclusions: just outside the one it is in, inside the outline. */
Vec2 freePoint(const Layout& l, Vec2 q, double clearance)
{
    auto ok = [&](Vec2 x) {
        if (!contains(l.outline.pts, x))
            return false;
        for (const Polygon& h : l.exclusions)
            if (contains(h.pts, x) || boundaryDistance(h.pts, x) < clearance * 0.99)
                return false;
        return true;
    };
    if (ok(q))
        return q;
    // Out through the nearest side of the exclusion it stands in.
    double best = std::numeric_limits<double>::max();
    Vec2 out = q;
    for (const Polygon& h : l.exclusions)
        for (size_t k = 0; k < h.size(); ++k)
        {
            const Vec2 a = h.pts[k], d = h.pts[(k + 1) % h.size()] - a;
            const double t = std::clamp(glm::dot(q - a, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
            const Vec2 c = a + d * t;
            const double dist = glm::length(c - q);
            if (dist >= best)
                continue;
            const Vec2 outward = glm::length(d) > 1e-9 ? Vec2(d.y, -d.x) / glm::length(d) : Vec2(1.0, 0.0);
            const Vec2 cand = c + outward * (clearance * 1.05 + 1.0);
            if (ok(cand))
            {
                best = dist;
                out = cand;
            }
        }
    if (best < std::numeric_limits<double>::max())
        return out;
    const double step = std::max(clearance, 100.0);
    for (int ring = 1; ring < 400; ++ring)
        for (int k = 0; k < 16; ++k)
        {
            const double a = 2.0 * kPi * k / 16.0;
            const Vec2 cand = q + Vec2(std::cos(a), std::sin(a)) * (step * ring);
            if (ok(cand))
                return cand;
        }
    return q;
}

/**
 * The polygon with no corner closer to the one before than gap and no near-straight corners, so the street graph,
 * which snaps points that close together, holds it exactly as it is.
 */
Polygon spaced(const Polygon& in, double gap)
{
    std::vector<Vec2> pts = in.pts;
    auto offLine = [](Vec2 q, Vec2 a, Vec2 b) {
        const Vec2 d = b - a;
        const double len = glm::length(d);
        return len > 1e-9 ? std::abs(cross(d, q - a)) / len : glm::length(q - a);
    };
    for (bool changed = true; changed && pts.size() > 3;)
    {
        changed = false;
        for (size_t k = 0; k < pts.size() && pts.size() > 3;)
        {
            const size_t n = pts.size();
            const Vec2 prev = pts[(k + n - 1) % n], cur = pts[k], next = pts[(k + 1) % n];
            if (glm::length(cur - prev) < gap || offLine(cur, prev, next) < 0.1 * gap)
            {
                pts.erase(pts.begin() + long(k));
                changed = true;
            }
            else
                ++k;
        }
    }
    return makePolygon(std::move(pts));
}

/** The faces that become blocks: inside the outline and in no exclusion. */
std::vector<Face> layoutFaces(const Layout& l)
{
    std::vector<Face> out;
    for (Face& f : planarFaces(l.graph))
    {
        if (l.exclusions.empty())
        {
            out.push_back(std::move(f));
            continue;
        }
        const Vec2 q = interiorPoint(f.outline);
        if (contains(l.outline.pts, q) && !excluded(l, q))
            out.push_back(std::move(f));
    }
    return out;
}

bool hasHeight(const glm::dvec3& p) { return !std::isnan(p.y); }

Vec2 flat2(const glm::dvec3& p) { return {p.x, p.z}; }

/** The index in l.roads of a named profile (built-in or the site's own), added on first use; -1 when unknown. */
int roadIndex(Layout& l, const Site& site, const std::string& name)
{
    for (size_t k = int(StreetClass::Path) + 1; k < l.roads.size(); ++k)
        if (l.roads[k].name == name)
            return int(k);
    for (const RoadProfile& r : site.roadProfiles)
        if (r.name == name)
        {
            l.roads.push_back(r);
            return int(l.roads.size() - 1);
        }
    RoadProfile r;
    if (!builtinRoadProfile(name, &r))
        return -1;
    l.roads.push_back(r);
    return int(l.roads.size() - 1);
}

double polyLength3(std::span<const glm::dvec3> pts)
{
    double len = 0.0;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        len += glm::length(pts[k + 1] - pts[k]);
    return len;
}

/**
 * The user's streets and routes. Ground legs go into the street network (routed around exclusions and up the slope);
 * legs on the walkable surface become surface paths; a leg from one to the other leaves the surface where it meets the
 * ground nearest the way on.
 */
void layPaths(Layout& l, const Site& site, const Ground& g, const Options& o, const WalkSurface* walk)
{
    LILA_ZONE();

    const double reach = std::max(2.0 * o.walk.step, 300.0);
    for (const SitePath& sp : site.paths)
    {
        PathResult res;
        res.id = sp.id;
        auto fail = [&](std::string why) {
            res.error = std::move(why);
            l.pathResults.push_back(res);
        };
        StreetEdge like;
        like.cls = sp.cls;
        like.width = sp.width;
        like.path = sp.id;
        if (!sp.profile.empty())
        {
            like.profile = roadIndex(l, site, sp.profile);
            if (like.profile < 0)
            {
                fail("unknown road profile '" + sp.profile + "'");
                continue;
            }
        }
        const int profile = like.profile >= 0 ? like.profile : int(std::min(sp.cls, StreetClass::Path));
        const double width = sp.width > 0.0 ? sp.width : (like.profile >= 0 ? l.roads[size_t(profile)].width() : streetWidth(l.profile, sp.cls));
        Profile rp = l.profile;
        if (sp.cls == StreetClass::Path)
            rp.maxGrade *= 2.5; // paths take stairs
        const double clearance = 0.5 * width;

        auto spotOf = [&](const glm::dvec3& p, WalkSurface::Spot* s) {
            return walk && !walk->empty() && hasHeight(p) && walk->locate(p, reach, s);
        };
        auto onSurface = [&](const glm::dvec3& p) {
            WalkSurface::Spot s;
            if (sp.mode == PathMode::Ground || !spotOf(p, &s))
                return false;
            return sp.mode == PathMode::Surface || p.y - g.height(flat2(p)) > o.walk.step;
        };
        auto addSurface = [&](std::vector<glm::dvec3> pts) {
            if (pts.size() < 2)
                return;
            res.surfaceLength += polyLength3(pts);
            res.length += polyLength3(pts);
            l.surfacePaths.push_back({sp.id, std::move(pts), sp.cls, profile, width});
        };
        auto addGround = [&](std::span<const Vec2> pts) {
            for (const std::vector<Vec2>& run : clipPolyline(pts, l.outline, l.center, 0.0, 0.0, l.exclusions))
            {
                l.graph.addPolyline(run, like);
                res.length += polylineLength(run);
            }
        };
        auto routeGround = [&](Vec2 a, Vec2 b) -> bool {
            const Vec2 fa = freePoint(l, a, clearance), fb = freePoint(l, b, clearance);
            if (!contains(l.outline.pts, fa) || !contains(l.outline.pts, fb))
                return false;
            const std::vector<Vec2> path = route(l, g, rp, fa, fb, clearance);
            if (path.size() < 2)
                return false;
            l.graph.addPolyline(path, like);
            res.length += polylineLength(path);
            return true;
        };
        // Surface walk from node s to where it meets the ground best for reaching ground point gp. Returns the walk
        // (from s) ending on the ground clear of exclusions, and that ground point.
        auto toGround = [&](const WalkSurface::Spot& s, Vec2 gp, std::vector<glm::dvec3>* walkOut, Vec2* exit) -> bool {
            const std::vector<double> d = walk->distances(s);
            int bestNode = -1;
            double best = std::numeric_limits<double>::max();
            for (size_t n = 0; n < walk->nodeCount(); ++n)
            {
                if (!std::isfinite(d[n]))
                    continue;
                const glm::dvec3 p = walk->position(int(n));
                if (std::abs(p.y - g.height(flat2(p))) > o.walk.step)
                    continue;
                const double score = d[n] + glm::length(flat2(p) - gp);
                if (score < best)
                {
                    best = score;
                    bestNode = int(n);
                }
            }
            if (bestNode < 0)
                return false;
            *walkOut = walk->route(s, bestNode);
            const Vec2 e = flat2(walk->position(bestNode));
            *exit = freePoint(l, e, clearance);
            if (glm::length(*exit - e) > 1.0)
                walkOut->push_back({exit->x, g.height(*exit), exit->y});
            return walkOut->size() >= 2;
        };

        if (sp.pts.size() < 2)
        {
            fail("needs two or more points");
            continue;
        }
        bool ok = true;
        if (!sp.routed)
        {
            bool surface = walk && !walk->empty() && sp.mode != PathMode::Ground;
            for (const glm::dvec3& p : sp.pts)
                surface = surface && onSurface(p);
            if (surface)
                addSurface(sp.pts);
            else
            {
                std::vector<Vec2> pts;
                for (const glm::dvec3& p : sp.pts)
                    pts.push_back(flat2(p));
                addGround(pts);
                ok = res.length > 0.0;
            }
        }
        else
            for (size_t k = 0; k + 1 < sp.pts.size() && ok; ++k)
            {
                const glm::dvec3 a = sp.pts[k], b = sp.pts[k + 1];
                const bool sa = onSurface(a), sb = onSurface(b);
                if (sp.mode == PathMode::Surface && (!sa || !sb))
                {
                    fail("a stop is not on the walkable surface");
                    ok = false;
                    break;
                }
                if (!sa && !sb)
                {
                    if (!routeGround(flat2(a), flat2(b)))
                    {
                        fail("no way over the ground between stops " + std::to_string(k) + " and " + std::to_string(k + 1));
                        ok = false;
                    }
                }
                else if (sa && sb)
                {
                    WalkSurface::Spot from, to;
                    spotOf(a, &from);
                    spotOf(b, &to);
                    std::vector<glm::dvec3> pts = walk->route(from, to);
                    if (pts.empty())
                    {
                        fail("no walkable way between stops " + std::to_string(k) + " and " + std::to_string(k + 1));
                        ok = false;
                        break;
                    }
                    addSurface(std::move(pts));
                }
                else
                {
                    const glm::dvec3 gp = sa ? b : a;
                    WalkSurface::Spot s;
                    spotOf(sa ? a : b, &s);
                    std::vector<glm::dvec3> w;
                    Vec2 exit{0.0};
                    if (!toGround(s, flat2(gp), &w, &exit))
                    {
                        fail("the walkable surface does not reach the ground");
                        ok = false;
                        break;
                    }
                    if (!sa)
                        std::reverse(w.begin(), w.end());
                    addSurface(std::move(w));
                    if (!routeGround(sa ? exit : flat2(gp), sa ? flat2(gp) : exit))
                    {
                        fail("no way over the ground from the surface to stop " + std::to_string(sa ? k + 1 : k));
                        ok = false;
                    }
                }
            }
        if (!ok)
        {
            if (res.error.empty())
                fail("lies outside the outline or inside an exclusion");
            continue;
        }
        res.ok = true;
        l.pathResults.push_back(res);
    }
}
} // namespace

namespace
{
/** The user's changes to generated streets, by stable id. */
void applyOverrides(Layout& l, const Site& site)
{
    StreetGraph& g = l.graph;
    for (const StreetOverride& o : site.overrides)
    {
        std::vector<int> edges;
        for (size_t e = 0; e < g.edges.size(); ++e)
            if (g.isStreet(int(e)) && g.edges[e].street == o.street)
                edges.push_back(int(e));
        if (edges.empty())
        {
            l.staleOverrides.push_back(o.street);
            continue;
        }
        const int profile = o.profile.empty() ? -2 : roadIndex(l, site, o.profile);
        for (int e : edges)
        {
            if (o.remove)
            {
                g.removeEdge(e);
                continue;
            }
            StreetEdge& x = g.edges[size_t(e)];
            if (o.hasClass)
                x.cls = o.cls;
            if (profile >= 0)
                x.profile = profile;
            if (o.width > 0.0)
                x.width = o.width;
        }
    }
}
} // namespace

void assignStreetIds(Layout& l)
{
    LILA_ZONE();

    StreetGraph& g = l.graph;
    std::vector<char> used(g.edges.size(), 0);
    std::unordered_map<uint64_t, int> seen;
    auto through = [&](int node, int e) {
        if (g.degree(node) != 2)
            return -1;
        for (int x : g.incident(node))
            if (x != e && g.isStreet(x) && g.edges[size_t(x)].path == g.edges[size_t(e)].path)
                return x;
        return -1;
    };
    auto walk = [&](int start, int e) {
        std::vector<int> chain;
        int node = start;
        double length = 0.0;
        while (e >= 0 && !used[size_t(e)])
        {
            used[size_t(e)] = 1;
            chain.push_back(e);
            length += g.length(e);
            const StreetEdge& x = g.edges[size_t(e)];
            node = x.a == node ? x.b : x.a;
            e = through(node, e);
        }
        // Keyed by where it runs, not by how it was found: the same street gets the same id from either end.
        auto q = [](Vec2 p) { return std::pair<int64_t, int64_t>{std::llround(p.x / 10.0), std::llround(p.y / 10.0)}; };
        const auto qa = q(g.nodes[size_t(start)].p), qb = q(g.nodes[size_t(node)].p);
        const auto ends = std::minmax(qa, qb);
        uint64_t h = hash64(uint64_t(ends.first.first));
        for (int64_t v : {ends.first.second, ends.second.first, ends.second.second, int64_t(std::llround(length / 100.0)),
                          int64_t(g.edges[size_t(chain.front())].path)})
            h = hash64(h ^ uint64_t(v));
        uint64_t id = (h & (kStreetIdBit - 1)) | kStreetIdBit;
        while (seen[id]++ > 0)
            id = (hash64(id) & (kStreetIdBit - 1)) | kStreetIdBit;
        for (int c : chain)
            g.edges[size_t(c)].street = id;
    };
    for (size_t n = 0; n < g.nodes.size(); ++n)
        for (int e : std::vector<int>(g.incident(int(n))))
            if (g.isStreet(e) && !used[size_t(e)] && through(int(n), e) < 0)
                walk(int(n), e);
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.isStreet(int(e)) && !used[e])
            walk(g.edges[e].a, int(e));
}

Layout build(const Site& site, const Ground& g, const Options& in, const WalkSurface* walk)
{
    LILA_ZONE();

    Options o = in;
    Profile& p = o.profile;
    for (double* w : {&p.widthArterial, &p.widthCollector, &p.widthLocal, &p.widthAlley, &p.widthPath})
        *w *= std::max(o.widthScale, 0.05);
    for (double* w : {&p.lotWidth, &p.lotDepth, &p.buildDepth})
        *w *= std::max(o.lotScale, 0.05);
    p.minLot *= std::max(o.lotScale * o.lotScale, 0.0025);

    std::vector<Vec2> outline = site.outline;
    if (outline.size() < 3)
    {
        const double h = std::max(site.size, 100.0) * 0.5;
        const Vec2 c = site.hasCenter && !site.attractors.empty() ? site.attractors.front() : Vec2(0.0);
        outline = {c + Vec2(-h, -h), c + Vec2(h, -h), c + Vec2(h, h), c + Vec2(-h, h)};
    }
    Layout l{StreetGraph(std::clamp(p.widthLocal * 0.5, 50.0, 1500.0), std::max({p.segment, p.gridWidth * 0.5, 1000.0}))};
    l.profile = p;
    l.routeBook = site.routeBook;
    for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path})
    {
        RoadProfile r;
        builtinRoadProfile(defaultRoadProfile(c), &r);
        l.roads.push_back(r);
    }
    l.outline = makePolygon(outline);
    if (l.outline.pts.empty())
        throw std::runtime_error("the settlement outline has no area");
    for (const Exclusion& x : site.exclusions)
    {
        Polygon poly = spaced(grow(makePolygon(x.pts), x.margin), 1.05 * l.graph.snap());
        if (poly.pts.empty())
            continue;
        l.exclusions.push_back(std::move(poly));
        l.exclusionIds.push_back(x.id);
    }
    l.center = centroid(l.outline);
    if (site.hasCenter && !site.attractors.empty() && contains(l.outline.pts, site.attractors.front()))
        l.center = site.attractors.front();
    // A structure at the centre: the town gathers at its edge instead.
    l.center = freePoint(l, l.center, std::max(p.widthArterial, 500.0));
    for (const Vec2& q : l.outline.pts)
        l.radius = std::max(l.radius, glm::length(q - l.center));

    l.graph.addPolyline(l.outline.pts, StreetClass::Boundary, 0, true);
    for (const Polygon& x : l.exclusions)
        l.graph.addPolyline(x.pts, StreetClass::Boundary, 0, true);
    for (const WaterLine& w : site.water)
        for (std::vector<Vec2>& run : clipPolyline(w.pts, l.outline, l.center, 0.0, 0.0))
        {
            l.graph.addPolyline(run, StreetClass::Water, 0, false, w.width);
            l.water.push_back({std::move(run), w.width, w.kind});
        }
    for (const std::vector<Vec2>& r : site.roads)
        for (const std::vector<Vec2>& run : clipPolyline(r, l.outline, l.center, 0.0, 0.0, l.exclusions))
            l.graph.addPolyline(run, StreetClass::Arterial);
    for (const std::vector<Vec2>& s : site.streets)
        if (s.size() >= 2)
            for (const std::vector<Vec2>& run : clipPolyline(s, l.outline, l.center, 0.0, 0.0, l.exclusions))
                l.graph.addPolyline(run, StreetClass::Collector);
    // Attractors past the centre pull arterials to them: a bridge, a harbour, a mine.
    for (size_t k = site.hasCenter ? 1 : 0; k < site.attractors.size(); ++k)
        if (contains(l.outline.pts, site.attractors[k]))
        {
            const Vec2 to = freePoint(l, site.attractors[k], 0.5 * p.widthArterial);
            std::vector<Vec2> path = route(l, g, p, l.center, to);
            if (!path.empty())
                l.graph.addPolyline(path, StreetClass::Arterial);
        }

    prepareGenerators(l, g, o);
    if (!layEras(l, g, o))
    switch (p.pattern)
    {
    case Pattern::Organic:
    case Pattern::Linear: growOrganic(l, g, o); break;
    case Pattern::Grid: layGrid(l, g, o); break;
    case Pattern::Radial: layRadial(l, g, o); break;
    case Pattern::OrganicGrid:
    {
        const double core = std::clamp(p.coreRadius, 0.05, 0.95) * l.radius;
        growOrganic(l, g, o, core);
        // Where the walls stood: a ring boulevard, and the grid of the extension beyond it.
        std::vector<Vec2> ring;
        const int segs = std::max(32, int(2.0 * kPi * core / std::max(p.segment, 100.0)));
        for (int s = 0; s <= segs; ++s)
        {
            const double t = 2.0 * kPi * s / segs;
            ring.push_back(l.center + Vec2(std::cos(t), std::sin(t)) * core);
        }
        for (const std::vector<Vec2>& run : clipPolyline(ring, l.outline, l.center, 0.0, 0.0, l.exclusions))
            l.graph.addPolyline(run, StreetClass::Arterial, 1);
        layGrid(l, g, o, core);
        break;
    }
    case Pattern::None: break;
    case Pattern::Tensor:
    case Pattern::Contour:
    case Pattern::Osm:
    case Pattern::Example: layPattern(l, g, o, p.pattern); break;
    }
    // Drawn streets and routes go in over what grew, so drawing one leaves the rest of the layout where it was.
    layPaths(l, site, g, o, walk);
    afterPattern(l, g, o);
    if (o.blocks)
        cutLargeBlocks(l, g, o);
    pullBackFromWater(l);
    if (p.pattern != Pattern::None)
        joinComponents(l, g);
    assignStreetIds(l);
    applyOverrides(l, site);
    afterStreets(l, g, o);
    dropStreetsInWater(l);
    dropStranded(l);
    nameStreets(l, site);
    for (size_t n = 0; n < l.graph.nodes.size(); ++n)
    {
        if (onWater(l.graph, int(n)) && l.graph.degree(int(n)) > 0)
            l.bridges.push_back(l.graph.nodes[n].p);
    }
    if (!o.blocks)
        return l;

    planParcels(l, site, g, o);
    afterLots(l, site, g, o);
    return l;
}
} // namespace settlement
