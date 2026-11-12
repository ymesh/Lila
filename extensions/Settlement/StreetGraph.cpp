// Settlement: the planar street graph and the blocks it encloses.

#include "SettlementCore.h"

#include <lila/Profile.h>

#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

namespace settlement
{
namespace
{
/** Parameter of q's projection onto a-b, clamped to the segment. */
double project(Vec2 a, Vec2 b, Vec2 q)
{
    const Vec2 d = b - a;
    const double len2 = glm::dot(d, d);
    return len2 > 1e-12 ? std::clamp(glm::dot(q - a, d) / len2, 0.0, 1.0) : 0.0;
}
} // namespace

const char* className(StreetClass c)
{
    switch (c)
    {
    case StreetClass::Arterial: return "arterial";
    case StreetClass::Collector: return "collector";
    case StreetClass::Local: return "local";
    case StreetClass::Alley: return "alley";
    case StreetClass::Path: return "path";
    case StreetClass::Boundary: return "boundary";
    case StreetClass::Water: return "water";
    }
    return "local";
}

bool parseClass(std::string_view name, StreetClass* out)
{
    for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path, StreetClass::Boundary,
                          StreetClass::Water})
        if (name == className(c))
        {
            *out = c;
            return true;
        }
    return false;
}

StreetGraph::StreetGraph(double snap, double cell) : mSnap(snap), mCell(std::max(cell, 1.0)) {}

template <class F> void StreetGraph::forCells(Vec2 lo, Vec2 hi, F&& f) const
{
    const int x0 = int(std::floor(lo.x / mCell)), x1 = int(std::floor(hi.x / mCell));
    const int y0 = int(std::floor(lo.y / mCell)), y1 = int(std::floor(hi.y / mCell));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            f(key(x, y));
}

int StreetGraph::addNode(Vec2 p)
{
    const int id = int(nodes.size());
    nodes.push_back({p, true});
    mIncident.emplace_back();
    mNodeCells[key(int(std::floor(p.x / mCell)), int(std::floor(p.y / mCell)))].push_back(id);
    return id;
}

int StreetGraph::addEdge(int a, int b, const StreetEdge& like)
{
    const StreetClass cls = like.cls;
    if (a == b || a < 0 || b < 0)
        return -1;
    for (int e : mIncident[size_t(a)])
        if ((edges[size_t(e)].a == b || edges[size_t(e)].b == b))
        {
            // A street laid over the outline, or a more important street over a lesser one, takes over the edge.
            StreetEdge& x = edges[size_t(e)];
            if (cls < x.cls)
            {
                x.cls = cls;
                x.width = like.width;
                x.profile = like.profile;
                x.path = like.path;
            }
            return e;
        }
    const int id = int(edges.size());
    StreetEdge made = like;
    made.a = a;
    made.b = b;
    made.alive = true;
    edges.push_back(made);
    mIncident[size_t(a)].push_back(id);
    mIncident[size_t(b)].push_back(id);
    const Vec2 pa = nodes[size_t(a)].p, pb = nodes[size_t(b)].p;
    forCells(glm::min(pa, pb), glm::max(pa, pb), [&](int64_t k) { mEdgeCells[k].push_back(id); });
    return id;
}

void StreetGraph::removeEdge(int e)
{
    StreetEdge& x = edges[size_t(e)];
    if (!x.alive)
        return;
    x.alive = false;
    for (int n : {x.a, x.b})
    {
        auto& inc = mIncident[size_t(n)];
        inc.erase(std::remove(inc.begin(), inc.end(), e), inc.end());
    }
}

void StreetGraph::reindex()
{
    mIncident.assign(nodes.size(), {});
    mNodeCells.clear();
    mEdgeCells.clear();
    for (size_t n = 0; n < nodes.size(); ++n)
        if (nodes[n].alive)
            mNodeCells[key(int(std::floor(nodes[n].p.x / mCell)), int(std::floor(nodes[n].p.y / mCell)))].push_back(int(n));
    for (size_t e = 0; e < edges.size(); ++e)
    {
        const StreetEdge& x = edges[e];
        if (!x.alive)
            continue;
        mIncident[size_t(x.a)].push_back(int(e));
        mIncident[size_t(x.b)].push_back(int(e));
        const Vec2 pa = nodes[size_t(x.a)].p, pb = nodes[size_t(x.b)].p;
        forCells(glm::min(pa, pb), glm::max(pa, pb), [&](int64_t k) { mEdgeCells[k].push_back(int(e)); });
    }
}

double StreetGraph::length(int e) const
{
    const StreetEdge& x = edges[size_t(e)];
    return glm::length(nodes[size_t(x.b)].p - nodes[size_t(x.a)].p);
}

int StreetGraph::degree(int node, bool streetsOnly) const
{
    int d = 0;
    for (int e : mIncident[size_t(node)])
        if (!streetsOnly || edges[size_t(e)].cls < StreetClass::Boundary)
            ++d;
    return d;
}

int StreetGraph::splitEdge(int e, double t)
{
    const StreetEdge x = edges[size_t(e)];
    const Vec2 pa = nodes[size_t(x.a)].p, pb = nodes[size_t(x.b)].p;
    const Vec2 p = pa + (pb - pa) * t;
    if (glm::length(p - pa) < mSnap * 0.25)
        return x.a;
    if (glm::length(p - pb) < mSnap * 0.25)
        return x.b;
    const int m = addNode(p);
    removeEdge(e);
    addEdge(x.a, m, x);
    addEdge(m, x.b, x);
    return m;
}

int StreetGraph::nearestNode(Vec2 p, double r) const
{
    int best = -1;
    double bestD = r;
    forCells(p - Vec2(r), p + Vec2(r), [&](int64_t k) {
        auto it = mNodeCells.find(k);
        if (it == mNodeCells.end())
            return;
        for (int n : it->second)
        {
            if (!nodes[size_t(n)].alive)
                continue;
            const double d = glm::length(nodes[size_t(n)].p - p);
            if (d < bestD || (d == bestD && best >= 0 && n < best))
            {
                bestD = d;
                best = n;
            }
        }
    });
    return best;
}

int StreetGraph::nearestEdge(Vec2 p, double r, Vec2* at, double* tOut, int skipNode) const
{
    int best = -1;
    double bestD = r, bestT = 0.0;
    forCells(p - Vec2(r), p + Vec2(r), [&](int64_t k) {
        auto it = mEdgeCells.find(k);
        if (it == mEdgeCells.end())
            return;
        for (int e : it->second)
        {
            const StreetEdge& x = edges[size_t(e)];
            if (!x.alive || x.a == skipNode || x.b == skipNode)
                continue;
            const Vec2 a = nodes[size_t(x.a)].p, b = nodes[size_t(x.b)].p;
            const double t = project(a, b, p);
            const double d = glm::length(a + (b - a) * t - p);
            if (d < bestD || (d == bestD && best >= 0 && e < best))
            {
                bestD = d;
                best = e;
                bestT = t;
            }
        }
    });
    if (best >= 0)
    {
        const StreetEdge& x = edges[size_t(best)];
        const Vec2 a = nodes[size_t(x.a)].p, b = nodes[size_t(x.b)].p;
        if (at)
            *at = a + (b - a) * bestT;
        if (tOut)
            *tOut = bestT;
    }
    return best;
}

int StreetGraph::firstCrossing(Vec2 p, Vec2 q, Vec2* at, double* sOut, int skipA, int skipB) const
{
    int best = -1;
    double bestS = std::numeric_limits<double>::max();
    const Vec2 r = q - p;
    forCells(glm::min(p, q), glm::max(p, q), [&](int64_t k) {
        auto it = mEdgeCells.find(k);
        if (it == mEdgeCells.end())
            return;
        for (int e : it->second)
        {
            const StreetEdge& x = edges[size_t(e)];
            if (!x.alive || x.a == skipA || x.b == skipA || x.a == skipB || x.b == skipB)
                continue;
            const Vec2 c = nodes[size_t(x.a)].p, d = nodes[size_t(x.b)].p;
            const Vec2 sv = d - c;
            const double den = cross(r, sv);
            // Parallel within a relative tolerance: near-collinear streets meet at nodes, never at a computed crossing.
            if (std::abs(den) <= 1e-9 * glm::length(r) * glm::length(sv))
                continue;
            const double s = cross(c - p, sv) / den, t = cross(c - p, r) / den;
            if (s <= 1e-9 || s >= 1.0 - 1e-9 || t < 0.0 || t > 1.0)
                continue;
            // At grazing angles s and t are ill-conditioned: the point must really lie on the other street.
            const Vec2 x0 = p + r * s;
            if (glm::length(c + sv * project(c, d, x0) - x0) > 0.5)
                continue;
            if (s < bestS || (s == bestS && e < best))
            {
                bestS = s;
                best = e;
            }
        }
    });
    if (best >= 0)
    {
        if (at)
            *at = p + r * bestS;
        if (sOut)
            *sOut = bestS;
    }
    return best;
}

int StreetGraph::nodeAt(Vec2 p)
{
    if (int n = nearestNode(p, mSnap); n >= 0)
        return n;
    double t = 0.0;
    if (int e = nearestEdge(p, mSnap, nullptr, &t); e >= 0)
        return splitEdge(e, t);
    return addNode(p);
}

std::vector<int> StreetGraph::addStreet(Vec2 p, Vec2 q, StreetClass cls, int era, double width)
{
    StreetEdge like;
    like.cls = cls;
    like.era = era;
    like.width = width;
    return addStreet(p, q, like);
}

std::vector<int> StreetGraph::addStreet(Vec2 p, Vec2 q, const StreetEdge& like)
{
    std::vector<int> made;
    const int a = nodeAt(p);
    const int b = nodeAt(q);
    if (a == b)
        return made;
    int cur = a;
    for (int guard = 0; guard < 4096 && cur != b; ++guard)
    {
        const Vec2 from = nodes[size_t(cur)].p, to = nodes[size_t(b)].p;
        Vec2 hit{0.0};
        double s = 2.0;
        const int crossed = firstCrossing(from, to, &hit, &s, cur, b);

        // Nodes lying on the way join the street instead of being passed by a hair.
        int onWay = -1;
        double onWayS = s;
        const double r = mSnap * 0.5;
        forCells(glm::min(from, to) - Vec2(r), glm::max(from, to) + Vec2(r), [&](int64_t k) {
            auto it = mNodeCells.find(k);
            if (it == mNodeCells.end())
                return;
            for (int n : it->second)
            {
                if (n == cur || n == b || !nodes[size_t(n)].alive || mIncident[size_t(n)].empty())
                    continue;
                const double t = project(from, to, nodes[size_t(n)].p);
                if (t <= 1e-6 || t >= 1.0 - 1e-6)
                    continue;
                if (glm::length(from + (to - from) * t - nodes[size_t(n)].p) < r && (t < onWayS || (t == onWayS && n < onWay)))
                {
                    onWayS = t;
                    onWay = n;
                }
            }
        });

        int next = b;
        if (onWay >= 0)
            next = onWay;
        else if (crossed >= 0)
        {
            const StreetEdge& x = edges[size_t(crossed)];
            next = splitEdge(crossed, project(nodes[size_t(x.a)].p, nodes[size_t(x.b)].p, hit));
        }
        if (next == cur)
            next = b;
        // Snapping moved the segment a little; whatever it crosses now is split in too.
        for (int fix = 0; fix < 8; ++fix)
        {
            Vec2 at{0.0};
            const int c = firstCrossing(nodes[size_t(cur)].p, nodes[size_t(next)].p, &at, nullptr, cur, next);
            if (c < 0)
                break;
            const StreetEdge& x = edges[size_t(c)];
            const int m = splitEdge(c, project(nodes[size_t(x.a)].p, nodes[size_t(x.b)].p, at));
            if (m == cur || m == next)
                break;
            next = m;
        }
        if (int e = addEdge(cur, next, like); e >= 0)
            made.push_back(e);
        cur = next;
    }
    return made;
}

std::vector<int> StreetGraph::addPolyline(std::span<const Vec2> pts, StreetClass cls, int era, bool closed, double width)
{
    StreetEdge like;
    like.cls = cls;
    like.era = era;
    like.width = width;
    return addPolyline(pts, like, closed);
}

std::vector<int> StreetGraph::addPolyline(std::span<const Vec2> pts, const StreetEdge& like, bool closed)
{
    std::vector<int> made;
    const size_t n = pts.size();
    for (size_t k = 0; k + 1 < n + (closed ? 1 : 0); ++k)
    {
        std::vector<int> e = addStreet(pts[k], pts[(k + 1) % n], like);
        made.insert(made.end(), e.begin(), e.end());
    }
    return made;
}

std::vector<Face> planarFaces(const StreetGraph& g) { return planarFaces(g, nullptr); }

std::vector<Face> planarFaces(const StreetGraph& g, const std::vector<int>* fromEdges)
{
    LILA_ZONE();

    const size_t ne = g.edges.size();
    auto from = [&](int h) { return h & 1 ? g.edges[size_t(h >> 1)].b : g.edges[size_t(h >> 1)].a; };
    auto to = [&](int h) { return h & 1 ? g.edges[size_t(h >> 1)].a : g.edges[size_t(h >> 1)].b; };
    // Half-edges leaving each node sorted by angle, made the first time a walk reaches the node.
    std::vector<std::vector<int>> out(g.nodes.size());
    std::vector<char> sorted(g.nodes.size(), 0);
    std::vector<int> slot(2 * ne, -1);
    for (size_t e = 0; e < ne; ++e)
        if (g.edges[e].alive)
        {
            out[size_t(g.edges[e].a)].push_back(int(2 * e));
            out[size_t(g.edges[e].b)].push_back(int(2 * e + 1));
        }
    auto leaving = [&](int n) -> const std::vector<int>& {
        std::vector<int>& hs = out[size_t(n)];
        if (sorted[size_t(n)])
            return hs;
        sorted[size_t(n)] = 1;
        const Vec2 p = g.nodes[size_t(n)].p;
        std::sort(hs.begin(), hs.end(), [&](int x, int y) {
            const Vec2 dx = g.nodes[size_t(to(x))].p - p, dy = g.nodes[size_t(to(y))].p - p;
            const double ax = std::atan2(dx.y, dx.x), ay = std::atan2(dy.y, dy.x);
            return ax != ay ? ax < ay : x < y;
        });
        for (size_t i = 0; i < hs.size(); ++i)
            slot[size_t(hs[i])] = int(i);
        return hs;
    };
    auto next = [&](int h) {
        const int twin = h ^ 1;
        leaving(from(twin));
        const auto& hs = leaving(to(h));
        const int n = int(hs.size());
        return hs[size_t((slot[size_t(twin)] - 1 + n) % n)];
    };

    std::vector<Face> faces;
    std::vector<char> seen(2 * ne, 0);
    std::vector<int> all;
    if (!fromEdges)
    {
        all.resize(ne);
        std::iota(all.begin(), all.end(), 0);
    }
    for (int ei : fromEdges ? *fromEdges : all)
    {
        const size_t e = size_t(ei);
        if (e >= ne || !g.edges[e].alive)
            continue;
        for (int start : {int(2 * e), int(2 * e + 1)})
        {
            if (seen[size_t(start)])
                continue;
            std::vector<int> nodeIds, tags;
            int h = start;
            for (int guard = 0; guard < int(2 * ne) + 1 && !seen[size_t(h)]; ++guard)
            {
                seen[size_t(h)] = 1;
                nodeIds.push_back(from(h));
                tags.push_back(h >> 1);
                h = next(h);
            }
            // Dead ends walk out and back (A, B, A): drop the spike until none is left.
            for (bool changed = true; changed && nodeIds.size() >= 3;)
            {
                changed = false;
                const size_t n = nodeIds.size();
                for (size_t i = 0; i < n; ++i)
                {
                    const size_t prev = (i + n - 1) % n, nx = (i + 1) % n;
                    if (nodeIds[prev] == nodeIds[nx])
                    {
                        // Drop the first A and B: the side leaving the second A is the one that carries on.
                        const size_t lo = std::min(prev, i), hi = std::max(prev, i);
                        nodeIds.erase(nodeIds.begin() + long(hi));
                        tags.erase(tags.begin() + long(hi));
                        nodeIds.erase(nodeIds.begin() + long(lo));
                        tags.erase(tags.begin() + long(lo));
                        changed = true;
                        break;
                    }
                }
            }
            if (nodeIds.size() < 3)
                continue;
            Face f;
            for (size_t i = 0; i < nodeIds.size(); ++i)
                f.outline.pts.push_back(g.nodes[size_t(nodeIds[i])].p);
            f.outline.tag = tags;
            if (signedArea(f.outline.pts) > 1e-3)
                faces.push_back(std::move(f));
        }
    }
    return faces;
}
} // namespace settlement
