#include "retopo/Internal.h"
#include "retopo/Patch.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
using Chart = std::array<glm::dvec2, 3>;
constexpr double kQuarter = glm::half_pi<double>();

double cross2(const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; }
int mod4(int k) { return ((k % 4) + 4) % 4; }
glm::dvec2 turn2(glm::dvec2 c, int k)
{
    for (int i = 0; i < mod4(k); ++i)
        c = {-c.y, c.x};
    return c;
}
glm::dvec2 axis(int k) { return turn2({1.0, 0.0}, k); }
double angleAbout(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& n) { return std::atan2(glm::dot(n, glm::cross(a, b)), glm::dot(a, b)); }
glm::dvec3 turn3(glm::dvec3 b, const glm::dvec3& n, int k)
{
    for (int i = 0; i < mod4(k); ++i)
        b = glm::cross(n, b);
    return b;
}

/**
 * The surface's triangles in the frames the tracing reads: per triangle one field direction, and its corners in a chart
 * whose x axis is that direction, in lattice steps, so a field line is an axis-aligned segment in each triangle.
 */
struct Layout
{
    const Surface& s;
    std::vector<glm::dvec3> dir;               // per triangle, unit, in its plane
    std::vector<Chart> chart;                  // per triangle
    std::vector<std::array<Index, 3>> triEdge; // surface edge of corner k to k+1
    std::vector<uint8_t> wall;                 // per edge: a feature line or border, where traces end
    std::vector<uint8_t> source;               // per vertex: a pole or feature corner, where traces start
    std::vector<uint8_t> singular;             // per vertex
    std::vector<int> quarters;                 // per vertex: how many quarter turns the field makes round it
    std::vector<glm::dvec3> creaseEnd;         // per vertex: where a crease stops inside the surface, the way it runs on

    Index other(Index e, Index t) const { return s.edgeTris[e][0] == t ? s.edgeTris[e][1] : s.edgeTris[e][0]; }
    int cornerOf(Index t, Index v) const
    {
        for (int k = 0; k < 3; ++k)
            if (s.tris[t][k] == v)
                return k;
        return -1;
    }
    /** Quarter turns k with dir[t] ~ turn(dir[o], k), unfolded about edge e; axis d in t is axis d + k in o. */
    int matching(Index e, Index t, Index o) const
    {
        const glm::dvec3 d = glm::normalize(s.pos[s.edges[e][1]] - s.pos[s.edges[e][0]]);
        const double a = angleAbout(d, dir[t], s.triNormal[t]), b = angleAbout(d, dir[o], s.triNormal[o]);
        return mod4(int(std::lround((a - b) / kQuarter)));
    }
    /** Angle of the edge from its vertex v in t's chart minus in o's, less whole quarter turns: the field's smooth turn. */
    double frameTurn(Index e, Index t, Index o) const
    {
        const Index v = s.edges[e][0], w = s.edges[e][1];
        auto angleIn = [&](Index tri) {
            const glm::dvec2 d = chart[tri][cornerOf(tri, w)] - chart[tri][cornerOf(tri, v)];
            return std::atan2(d.y, d.x);
        };
        const double delta = std::remainder(angleIn(o) - angleIn(t), glm::two_pi<double>());
        return delta - kQuarter * std::round(delta / kQuarter);
    }
};

/**
 * A +1 and a -1 pole closer than a fifth of a cell are one wrinkle in the field, not two poles: neither sends lines, and lines
 * that pass them see no net turn.
 */
void cancelDipoles(Layout& lay, const std::vector<double>& scale)
{
    const Surface& s = lay.s;
    std::vector<Index> poles;
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (lay.singular[v] && std::abs(lay.quarters[v]) == 1)
            poles.push_back(v);
    std::vector<std::tuple<double, Index, Index>> pairs;
    for (size_t i = 0; i < poles.size(); ++i)
        for (size_t j = i + 1; j < poles.size(); ++j)
        {
            const Index a = poles[i], b = poles[j];
            const double d = glm::length(s.pos[a] - s.pos[b]) / (0.5 * (scale[a] + scale[b]));
            if (lay.quarters[a] == -lay.quarters[b] && d < 0.2)
                pairs.push_back({d, a, b});
        }
    std::sort(pairs.begin(), pairs.end());
    int cancelled = 0;
    for (const auto& [d, a, b] : pairs)
        if (lay.singular[a] && lay.singular[b])
        {
            for (Index v : {a, b})
            {
                lay.singular[v] = 0;
                lay.source[v] = s.feature[v] == FeatureKind::Corner;
            }
            ++cancelled;
        }
    if (std::getenv("LILA_PATCH_DEBUG"))
        std::fprintf(stderr, "patch layout: %d close pole pairs cancelled\n", cancelled);
}

Layout makeLayout(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale)
{
    Layout lay{s};
    const Index nt = Index(s.tris.size()), ne = Index(s.edges.size()), nv = s.vertexCount();
    lay.dir.resize(nt);
    std::unordered_map<Index, glm::dvec3> atJunction;
    for (const CreaseJunction& j : creaseJunctions(s))
        for (const CreaseJunction::Wedge& w : j.fan)
        {
            const glm::dvec3 n = glm::normalize(s.triNormal[w.tri]), e0 = glm::normalize(s.pos[w.next] - s.pos[j.vertex]);
            const double turnBy = -w.before - (w.alpha - 1.0) * (w.before + 0.5 * w.angle);
            atJunction[w.tri] = e0 * std::cos(turnBy) + glm::cross(n, e0) * std::sin(turnBy);
        }
    for (Index t = 0; t < nt; ++t)
    {
        const glm::dvec3 n = s.triNormal[t];
        if (auto it = atJunction.find(t); it != atJunction.end())
        {
            lay.dir[t] = it->second;
            continue;
        }
        glm::dvec3 ref(0.0), sum(0.0);
        for (Index v : s.tris[t])
        {
            glm::dvec3 d = q[v] - n * glm::dot(n, q[v]);
            if (glm::length(d) < 1e-12)
                continue;
            d = glm::normalize(d);
            if (glm::length(ref) == 0.0)
                ref = d;
            sum += turn3(d, n, int(std::lround(angleAbout(d, ref, n) / kQuarter)));
        }
        lay.dir[t] = glm::length(sum) > 1e-12 ? glm::normalize(sum) : anyTangent(n);
    }
    lay.chart.resize(nt);
    for (Index t = 0; t < nt; ++t)
    {
        const auto& tri = s.tris[t];
        const glm::dvec3 x = lay.dir[t], y = glm::cross(s.triNormal[t], x);
        const double h = (scale[tri[0]] + scale[tri[1]] + scale[tri[2]]) / 3.0;
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 d = s.pos[tri[k]] - s.pos[tri[0]];
            lay.chart[t][k] = glm::dvec2(glm::dot(d, x), glm::dot(d, y)) / h;
        }
    }
    lay.triEdge.resize(nt);
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const Index a = s.tris[t][k], b = s.tris[t][(k + 1) % 3];
            lay.triEdge[t][k] = kInvalid;
            for (Index i = s.adjStart[a]; i < s.adjStart[a + 1]; ++i)
                if (s.adj[i] == b)
                    lay.triEdge[t][k] = s.adjEdge[i];
        }
    lay.wall.assign(ne, 0);
    for (Index e = 0; e < ne; ++e)
        lay.wall[e] = s.edgeTriCount[e] != 2 || s.edgeFeature[e] || s.edgeBoundary[e];

    // Singular vertices: the field's quarter-wrapped jumps round the fan plus the angle defect make a whole turn.
    std::vector<double> turn(nv, glm::two_pi<double>());
    std::vector<uint8_t> border(nv, 0);
    for (Index e = 0; e < ne; ++e)
        if (s.edgeTriCount[e] != 2)
            border[s.edges[e][0]] = border[s.edges[e][1]] = 1;
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const Index v = s.tris[t][k], next = s.tris[t][(k + 1) % 3], prev = s.tris[t][(k + 2) % 3];
            const glm::dvec3 toNext = glm::normalize(s.pos[next] - s.pos[v]), toPrev = glm::normalize(s.pos[prev] - s.pos[v]);
            const double gamma = angleAbout(toNext, toPrev, s.triNormal[t]);
            turn[v] -= gamma;
            const Index e = lay.triEdge[t][(k + 2) % 3];
            if (e == kInvalid || s.edgeTriCount[e] != 2)
                continue;
            const Index o = lay.other(e, t);
            const double leaving = angleAbout(toNext, lay.dir[t], s.triNormal[t]) - gamma;
            const double entering = angleAbout(toPrev, lay.dir[o], s.triNormal[o]);
            turn[v] += std::remainder(entering - leaving, kQuarter);
        }
    // A saddle the feature pass called a corner (a web, an armpit) is no tip to start lines from: the field's own poles
    // say how the layout turns there.
    std::vector<double> deficit(nv, glm::two_pi<double>());
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 a = s.pos[s.tris[t][(k + 1) % 3]] - s.pos[s.tris[t][k]], b = s.pos[s.tris[t][(k + 2) % 3]] - s.pos[s.tris[t][k]];
            deficit[s.tris[t][k]] -= std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
        }
    std::vector<uint8_t> onWall(nv, 0);
    for (Index e = 0; e < ne; ++e)
        if (lay.wall[e])
            onWall[s.edges[e][0]] = onWall[s.edges[e][1]] = 1;
    // A needle (one point poking out of a ring that bends the other way, left by simplification) turns the frame round
    // it by its own sharp tip, which the field, smooth over the ring, doesn't follow: no pole of the layout's.
    auto needle = [&](Index v) {
        double ring = deficit[v];
        for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
            ring += deficit[s.adj[k]];
        return std::abs(deficit[v]) > glm::radians(75.0) && deficit[v] * ring < 0.0;
    };
    lay.singular.assign(nv, 0);
    lay.source.assign(nv, 0);
    for (Index v = 0; v < nv; ++v)
    {
        lay.singular[v] = !border[v] && std::lround(turn[v] / kQuarter) != 0 && !needle(v);
        lay.quarters.push_back(int(std::lround(turn[v] / kQuarter)));
        const bool saddle = !onWall[v] && !s.pinned[v] && deficit[v] < 0.0;
        lay.source[v] = lay.singular[v] || (s.feature[v] == FeatureKind::Corner && !saddle);
    }
    cancelDipoles(lay, scale);
    // A crease that stops inside a patch would leave a wall ending in it; a line carries it on to the next line instead.
    std::vector<int> walls(nv, 0);
    std::vector<Index> wallTo(nv, kInvalid);
    for (Index e = 0; e < ne; ++e)
        if (lay.wall[e])
            for (int k = 0; k < 2; ++k)
            {
                ++walls[s.edges[e][k]];
                wallTo[s.edges[e][k]] = s.edges[e][1 - k];
            }
    lay.creaseEnd.assign(nv, glm::dvec3(0.0));
    for (Index v = 0; v < nv; ++v)
        if (walls[v] == 1 && !border[v] && !lay.source[v])
            lay.creaseEnd[v] = glm::normalize(s.pos[v] - s.pos[wallTo[v]]);
    return lay;
}

/** One stretch of a trace inside one triangle, in that triangle's chart. */
struct Segment
{
    Index tracer = kInvalid;
    Index tri = kInvalid;
    glm::dvec2 a, b;
    double t0 = 0.0; // trace time at a
    int dirK = 0;
    Index endsOn = kInvalid; // the segment its far end stops on, inside this triangle
};

struct Traces
{
    std::vector<Segment> segments;
    std::vector<double> end;            // per tracer, the time it stops
    std::vector<Index> sourceOf;        // per tracer
    std::vector<std::vector<Index>> in; // per triangle, its segments
    int why[5] = {};                    // ends: on a trace, killed by a trace, wall, source, lost
    std::vector<int> endWhy;            // per tracer
    int loops = 0;                      // lines closed onto themselves
    bool runaway = false;               // lines kept stopping and freeing each other; the layout is no use
};

/**
 * The motorcycle graph: from every source a field line leaves along each axis direction of its fan, and all advance
 * together in lattice steps, each stopping where it meets a line that got there first, a wall or another source.
 */
/**
 * The lines out of an inner pole: the field turns round it by `quarters`, so 4 - quarters lines leave it, evenly round
 * its fan; each goes along the field axis, in the fan triangle whose corner holds it, nearest its even place.
 */
struct PoleLine
{
    Index tri;
    int dir;
    glm::dvec2 free{0.0}; // when set, the first step's direction in tri's chart, off the field axes
};

std::vector<PoleLine> poleLines(const Layout& lay, Index v, Index first)
{
    const Surface& s = lay.s;
    const int n = 4 - lay.quarters[v];
    std::vector<PoleLine> out;
    if (n <= 0)
        return out;
    // The fan, counter-clockwise: from each triangle across its edge from v to the corner before v.
    struct Wedge
    {
        Index tri;
        double start, span, cum;
    };
    std::vector<Wedge> fan;
    double total = 0.0;
    for (Index t = first; fan.size() < 64;)
    {
        const int c = lay.cornerOf(t, v);
        const glm::dvec2 p = lay.chart[t][c], a = lay.chart[t][(c + 1) % 3] - p, b = lay.chart[t][(c + 2) % 3] - p;
        const double start = std::atan2(a.y, a.x);
        const double span = std::fmod(std::atan2(b.y, b.x) - start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
        fan.push_back({t, start, span, total});
        total += span;
        const Index e = lay.triEdge[t][(c + 2) % 3];
        if (e == kInvalid || s.edgeTriCount[e] != 2)
        {
            if (std::getenv("LILA_PATCH_LIST"))
                std::fprintf(stderr, "pole %d: fan open at edge %d (%d tris)\n", int(v), int(e), e == kInvalid ? -1 : int(s.edgeTriCount[e]));
            return out;
        }
        t = lay.other(e, t);
        if (t == first)
            break;
    }
    // Candidates: every axis inside a fan corner, at its place round the fan.
    struct Candidate
    {
        double at;
        Index tri;
        int dir;
    };
    std::vector<Candidate> cand;
    for (const Wedge& w : fan)
        for (int dir = 0; dir < 4; ++dir)
        {
            const double phi = std::fmod(dir * kQuarter - w.start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
            if (phi < w.span)
                cand.push_back({w.cum + phi, w.tri, dir});
        }
    if (std::getenv("LILA_PATCH_LIST") && fan.size() >= 64)
        std::fprintf(stderr, "pole %d: fan didn't close\n", int(v));
    if (cand.empty())
        return out;
    // Even places, starting from the candidate that lets most of the others line up; a place with no axis near it gets
    // a line in its own direction, which follows the field from the next triangle on.
    auto wedgeAt = [&](double at) -> const Wedge& {
        for (const Wedge& w : fan)
            if (at < w.cum + w.span)
                return w;
        return fan.back();
    };
    const double reach = glm::radians(20.0);
    double bestCost = std::numeric_limits<double>::max();
    std::vector<PoleLine> best;
    for (const Candidate& base : cand)
    {
        std::vector<PoleLine> pick;
        double cost = 0.0;
        std::vector<uint8_t> used(cand.size(), 0);
        for (int j = 0; j < n; ++j)
        {
            const double want = std::fmod(base.at + j * total / n, total);
            size_t bi = cand.size();
            double bd = reach;
            for (size_t i = 0; i < cand.size(); ++i)
            {
                double d = std::abs(cand[i].at - want);
                d = std::min(d, total - d);
                if (!used[i] && d < bd)
                {
                    bd = d;
                    bi = i;
                }
            }
            if (bi < cand.size())
            {
                used[bi] = 1;
                pick.push_back({cand[bi].tri, cand[bi].dir});
                cost += bd * bd;
                continue;
            }
            const Wedge& w = wedgeAt(want);
            const double local = w.start + std::clamp(want - w.cum, 1e-3 * w.span, w.span * (1.0 - 1e-3));
            pick.push_back({w.tri, mod4(int(std::lround(local / kQuarter))), glm::dvec2(std::cos(local), std::sin(local))});
            cost += reach * reach;
        }
        if (cost < bestCost)
        {
            bestCost = cost;
            best = pick;
        }
    }
    out = best;
    return out;
}

/** A line started from a point on another, at the time that one passes it, to cut a patch that has too few corners. */
struct ExtraStart
{
    Index tri;
    glm::dvec2 at;
    int dirK;
    double time;
};

Traces trace(const Layout& lay, const std::vector<ExtraStart>& extras = {}, const std::vector<uint8_t>& dropped = {})
{
    LILA_ZONE();
    const Surface& s = lay.s;
    const Index nt = Index(s.tris.size());
    struct Tracer
    {
        Index tri;
        glm::dvec2 at;
        int dirK;
        double time = 0.0;
        int entered = -1;
        int steps = 0;
        bool alive = true;
        glm::dvec2 free{0.0};
    };
    std::vector<Tracer> tracers;
    Traces out;
    out.in.resize(nt);
    // Starts: each fan triangle sends the axis directions inside its corner; the same line leaving through two fan
    // triangles (along their shared edge, or where the field jumps) goes once.
    std::vector<std::vector<glm::dvec3>> leaving(s.pos.size());
    std::vector<uint8_t> poleDone(s.pos.size(), 0);
    // The one axis round a crease end that runs on nearest the crease's own way, over the end's fan.
    auto startCreaseEnd = [&](Index v, Index first) {
        double best = std::cos(glm::radians(45.0));
        Tracer pick{kInvalid, {}, 0};
        for (Index t = first, guard = 0; guard < 64; ++guard)
        {
            const int c = lay.cornerOf(t, v);
            const glm::dvec2 p = lay.chart[t][c], a = lay.chart[t][(c + 1) % 3] - p, b = lay.chart[t][(c + 2) % 3] - p;
            const double start = std::atan2(a.y, a.x);
            const double span = std::fmod(std::atan2(b.y, b.x) - start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
            for (int dir = 0; dir < 4; ++dir)
            {
                const double phi = std::fmod(dir * kQuarter - start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
                if (phi >= span)
                    continue;
                const glm::dvec3 d3 = lay.dir[t] * axis(dir).x + glm::cross(s.triNormal[t], lay.dir[t]) * axis(dir).y;
                if (const double along = glm::dot(d3, lay.creaseEnd[v]); along > best)
                {
                    best = along;
                    pick = {t, p, dir};
                }
            }
            const Index e = lay.triEdge[t][(c + 2) % 3];
            if (e == kInvalid || s.edgeTriCount[e] != 2)
                break;
            t = lay.other(e, t);
            if (t == first)
                break;
        }
        if (pick.tri == kInvalid)
            return;
        tracers.push_back(pick);
        out.sourceOf.push_back(v);
    };
    for (Index t = 0; t < nt; ++t)
        for (int c = 0; c < 3; ++c)
        {
            const Index v = s.tris[t][c];
            if (!lay.source[v])
            {
                if (lay.creaseEnd[v] != glm::dvec3(0.0) && !poleDone[v])
                {
                    poleDone[v] = 1;
                    startCreaseEnd(v, t);
                }
                continue;
            }
            if (lay.singular[v])
            {
                if (!poleDone[v])
                {
                    poleDone[v] = 1;
                    for (const PoleLine& line : poleLines(lay, v, t))
                    {
                        // A pole on a crease corner (a cube's) has walls for some of its lines already.
                        const int lc = lay.cornerOf(line.tri, v);
                        const glm::dvec2 lp = lay.chart[line.tri][lc], la = lay.chart[line.tri][(lc + 1) % 3] - lp,
                                         lb = lay.chart[line.tri][(lc + 2) % 3] - lp;
                        const glm::dvec2 ld = line.free != glm::dvec2(0.0) ? line.free : axis(line.dir);
                        auto along = [&](const glm::dvec2& edge, Index e) {
                            return e != kInvalid && lay.wall[e] && std::abs(std::atan2(cross2(edge, ld), glm::dot(edge, ld))) < glm::radians(15.0);
                        };
                        if (along(la, lay.triEdge[line.tri][lc]) || along(lb, lay.triEdge[line.tri][(lc + 2) % 3]))
                            continue;
                        Tracer tracer{line.tri, lay.chart[line.tri][lay.cornerOf(line.tri, v)], line.dir};
                        tracer.free = line.free;
                        tracers.push_back(tracer);
                        out.sourceOf.push_back(v);
                    }
                }
                continue;
            }
            const glm::dvec2 p = lay.chart[t][c], a = lay.chart[t][(c + 1) % 3] - p, b = lay.chart[t][(c + 2) % 3] - p;
            const double start = std::atan2(a.y, a.x);
            const double span = std::fmod(std::atan2(b.y, b.x) - start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
            for (int dir = 0; dir < 4; ++dir)
            {
                const double phi = std::fmod(dir * kQuarter - start + 2.0 * glm::two_pi<double>(), glm::two_pi<double>());
                if (phi >= span)
                    continue;
                // Along a wall the line is already there. A round crease leaves its corner a few degrees off the field,
                // and a line traced beside it would wall off a sliver.
                const double alongWall = glm::radians(15.0);
                if ((phi < alongWall && lay.wall[lay.triEdge[t][c]]) || (span - phi < alongWall && lay.wall[lay.triEdge[t][(c + 2) % 3]]))
                    continue;
                const glm::dvec3 d3 = lay.dir[t] * axis(dir).x + glm::cross(s.triNormal[t], lay.dir[t]) * axis(dir).y;
                bool dup = false;
                for (const glm::dvec3& o : leaving[v])
                    dup = dup || glm::dot(o, d3) > std::cos(glm::radians(25.0));
                if (dup)
                    continue;
                leaving[v].push_back(d3);
                tracers.push_back({t, p, dir});
                out.sourceOf.push_back(v);
            }
        }
    for (const ExtraStart& x : extras)
    {
        Tracer tracer{x.tri, x.at, x.dirK};
        tracer.time = x.time;
        tracers.push_back(tracer);
        out.sourceOf.push_back(kInvalid);
    }
    out.end.assign(tracers.size(), std::numeric_limits<double>::max());

    using Event = std::pair<double, Index>;
    std::priority_queue<Event, std::vector<Event>, std::greater<>> queue;
    std::vector<uint8_t> queued(tracers.size(), 1);
    for (Index i = 0; i < Index(tracers.size()); ++i)
        if (i < Index(dropped.size()) && dropped[i])
        {
            tracers[i].alive = false;
            out.end[i] = tracers[i].time;
            queued[i] = 0;
        }
        else
            queue.push({tracers[i].time, i});
    // Who each line stopped, so a line cut short later lets go of those it stopped past its new end.
    struct Kill
    {
        Index victim, seg, endsOn;
        double atKiller, victimEnd, before;
        bool wasAlive;
    };
    std::vector<std::vector<Kill>> killsBy(tracers.size());
    // Who stopped on each line, with where they were before the step, to send them on if the line ends short of them.
    struct Stopped
    {
        Index victim, seg;
        double atStopper, victimEnd;
        Tracer before;
    };
    std::vector<std::vector<Stopped>> stoppedOn(tracers.size());
    std::function<void(Index, double)> shorten = [&](Index x, double end) {
        out.end[x] = end;
        for (const Stopped& k : stoppedOn[x])
            if (k.atStopper > end + 1e-12 && out.end[k.victim] == k.victimEnd)
            {
                out.segments[k.seg].b = out.segments[k.seg].a;
                out.segments[k.seg].endsOn = kInvalid;
                out.end[k.victim] = std::numeric_limits<double>::max();
                tracers[k.victim] = k.before;
                if (!queued[k.victim])
                {
                    queued[k.victim] = 1;
                    queue.push({k.before.time, k.victim});
                }
            }
        for (const Kill& k : killsBy[x])
            if (k.atKiller > end + 1e-12 && out.end[k.victim] == k.victimEnd)
            {
                out.end[k.victim] = k.before;
                out.segments[k.seg].endsOn = k.endsOn;
                tracers[k.victim].alive = k.wasAlive;
                if (k.wasAlive && !queued[k.victim])
                {
                    queued[k.victim] = 1;
                    queue.push({tracers[k.victim].time, k.victim});
                }
            }
    };
    // A line can cross every triangle about twice (a turn round a tube and back); more is one circling a point.
    const int kMaxSteps = int(std::min<size_t>(500000, 2 * size_t(nt) + 1000));
    out.endWhy.assign(tracers.size(), -1);
    auto stop = [&](Index ai, double at, int why) {
        out.endWhy[ai] = why;
        out.end[ai] = at;
        tracers[ai].alive = false;
        ++out.why[why];
    };
    size_t events = 0;
    while (!queue.empty())
    {
        if (++events % 4096 == 0)
            Budget::checkpoint();
        // A line freed by a shortened one starts over from where it stopped, so a ring of them can go on for ever.
        if (events > 200 * size_t(nt) + 1000 * tracers.size())
        {
            out.runaway = true;
            break;
        }
        const Index ai = queue.top().second;
        queue.pop();
        queued[ai] = 0;
        Tracer& A = tracers[ai];
        if (!A.alive)
            continue;
        if (++A.steps > kMaxSteps)
        {
            stop(ai, A.time, 4);
            continue;
        }
        const Index t = A.tri;
        const glm::dvec2 d = A.free != glm::dvec2(0.0) ? A.free : axis(A.dirK);
        A.free = glm::dvec2(0.0);
        int exitEdge = -1;
        double exitS = std::numeric_limits<double>::max(), exitR = 0.0;
        // Near a pole the field can run along the edge it came in by; then the line leaves by the nearest other edge.
        for (double slack : {1e-9, 0.5, 1e9})
        {
            for (int k = 0; k < 3; ++k)
            {
                if (k == A.entered)
                    continue;
                const glm::dvec2 p0 = lay.chart[t][k], e = lay.chart[t][(k + 1) % 3] - p0;
                const double denom = cross2(d, e);
                if (std::abs(denom) < 1e-14)
                    continue;
                const double along = cross2(p0 - A.at, e) / denom, r = cross2(p0 - A.at, d) / denom;
                if (along > 1e-12 && r >= -slack && r <= 1.0 + slack && along < exitS)
                {
                    exitS = along;
                    exitEdge = k;
                    exitR = std::clamp(r, 0.0, 1.0);
                }
            }
            if (exitEdge >= 0)
                break;
        }
        if (exitEdge < 0)
        {
            if (std::getenv("LILA_PATCH_DEBUG"))
                std::fprintf(stderr, "lost: tri %d entered %d steps %d at %.4f %.4f dir %d corners %.4f %.4f / %.4f %.4f / %.4f %.4f\n", int(t), A.entered, A.steps,
                             A.at.x, A.at.y, A.dirK, lay.chart[t][0].x, lay.chart[t][0].y, lay.chart[t][1].x, lay.chart[t][1].y, lay.chart[t][2].x, lay.chart[t][2].y);
            stop(ai, A.time, 4);
            continue;
        }
        // Through a plain vertex: cross just beside it, where the next triangle is known, at the same place on both sides.
        if (const int c = exitR < 1e-6 ? exitEdge : exitR > 1.0 - 1e-6 ? (exitEdge + 1) % 3 : -1; c >= 0 && !lay.source[s.tris[t][c]])
            exitR = std::clamp(exitR, 1e-6, 1.0 - 1e-6);
        glm::dvec2 b = (1.0 - exitR) * lay.chart[t][exitEdge] + exitR * lay.chart[t][(exitEdge + 1) % 3];
        exitS = glm::length(b - A.at);
        Index endsOn = kInvalid;
        bool stopped = false;

        // A line round a tube comes back beside itself after a turn; closer than half a row, it closes onto itself with
        // a short step across, or it would wind on as a helix with the same patch both sides.
        if (d == axis(A.dirK))
        {
            constexpr double kLoopAfter = 3.0;
            const glm::dvec2 perp = axis(A.dirK + 1);
            Index self = kInvalid;
            double bestOff = 0.5, bestRun = 0.0;
            for (Index si : out.in[t])
            {
                const Segment& B = out.segments[si];
                const double len = glm::length(B.b - B.a);
                if (B.tracer != ai || B.dirK != A.dirK || B.t0 + len > A.time - kLoopAfter)
                    continue;
                // It runs on until level with the earlier stretch, then steps across.
                const double off = glm::dot(B.a - A.at, perp), along = glm::dot(A.at - B.a, d);
                const double run = std::max(0.0, -along) + 1e-6;
                if (std::abs(off) < std::abs(bestOff) && run < exitS && along + run <= len)
                {
                    bestOff = off;
                    bestRun = run;
                    self = si;
                }
            }
            if (self != kInvalid)
            {
                const glm::dvec2 turn = A.at + bestRun * d;
                out.segments.push_back({ai, t, A.at, turn, A.time, A.dirK, kInvalid});
                out.in[t].push_back(Index(out.segments.size() - 1));
                out.segments.push_back({ai, t, turn, turn + bestOff * perp, A.time + bestRun, mod4(A.dirK + (bestOff > 0.0 ? 1 : 3)), self});
                out.in[t].push_back(Index(out.segments.size() - 1));
                stop(ai, A.time + bestRun + std::abs(bestOff), 0);
                ++out.loops;
                continue;
            }
        }

        // Crossings with lines already through this triangle, nearest first.
        struct Hit
        {
            double sA;
            Index seg;
            double tB;
        };
        std::vector<Hit> hits;
        for (Index si : out.in[t])
        {
            const Segment& B = out.segments[si];
            if (B.tracer == ai || mod4(B.dirK - A.dirK) % 2 == 0)
                continue;
            const glm::dvec2 eb = B.b - B.a, ea = b - A.at;
            const double denom = cross2(ea, eb);
            if (std::abs(denom) < 1e-18)
                continue;
            const double sA = cross2(B.a - A.at, eb) / denom, sB = cross2(B.a - A.at, ea) / denom;
            if (sA > 1e-12 && sA <= 1.0 + 1e-12 && sB >= -1e-12 && sB <= 1.0 + 1e-12)
            {
                const double tB = B.t0 + sB * glm::length(eb);
                if (tB <= out.end[B.tracer] + 1e-12)
                    hits.push_back({sA, si, tB});
            }
        }
        std::sort(hits.begin(), hits.end(), [](const Hit& p, const Hit& q) { return p.sA < q.sA; });
        const Index mine = Index(out.segments.size());
        for (const Hit& h : hits)
        {
            const double tA = A.time + h.sA * exitS;
            if (h.tB <= tA)
            {
                // The other line was here first.
                Tracer before = A;
                before.steps = A.steps;
                stoppedOn[out.segments[h.seg].tracer].push_back({ai, mine, h.tB, tA, before});
                b = A.at + h.sA * (b - A.at);
                endsOn = h.seg;
                stopped = true;
                stop(ai, tA, 0);
                break;
            }
            // This one was here first: the other ends on it.
            const Index bt = out.segments[h.seg].tracer;
            if (h.tB < out.end[bt])
            {
                out.why[1] += tracers[bt].alive;
                out.endWhy[bt] = 1;
                killsBy[ai].push_back({bt, h.seg, out.segments[h.seg].endsOn, tA, h.tB, out.end[bt], tracers[bt].alive});
                tracers[bt].alive = false;
                out.segments[h.seg].endsOn = mine;
                shorten(bt, h.tB);
            }
        }
        out.segments.push_back({ai, t, A.at, b, A.time, A.dirK, endsOn});
        out.in[t].push_back(mine);
        if (stopped)
            continue;

        const Index e = lay.triEdge[t][exitEdge];
        const double tEnd = A.time + exitS;
        const int atCorner = exitR < 1e-7 ? exitEdge : exitR > 1.0 - 1e-7 ? (exitEdge + 1) % 3 : -1;
        if (atCorner >= 0 && lay.source[s.tris[t][atCorner]])
        {
            stop(ai, tEnd, 3);
            continue;
        }
        if (e == kInvalid || lay.wall[e])
        {
            stop(ai, tEnd, 2);
            continue;
        }
        const Index o = lay.other(e, t);
        const Index va = s.tris[t][exitEdge], vb = s.tris[t][(exitEdge + 1) % 3];
        const int ca = lay.cornerOf(o, va), cb = lay.cornerOf(o, vb);
        if (ca < 0 || cb < 0)
        {
            stop(ai, tEnd, 4);
            continue;
        }
        // The edge runs b to a in o; the point keeps its place along it.
        int ko = -1;
        for (int k = 0; k < 3; ++k)
            if (s.tris[o][k] == vb && s.tris[o][(k + 1) % 3] == va)
                ko = k;
        if (ko < 0)
        {
            stop(ai, tEnd, 4);
            continue;
        }
        A.at = (1.0 - exitR) * lay.chart[o][ca] + exitR * lay.chart[o][cb];
        A.dirK = mod4(A.dirK + lay.matching(e, t, o));
        A.tri = o;
        A.entered = ko;
        A.time = tEnd;
        queued[ai] = 1;
        queue.push({A.time, ai});
    }
    // Clip each segment to its trace's end.
    for (Segment& g : out.segments)
    {
        const double len = glm::length(g.b - g.a), end = out.end[g.tracer];
        if (g.t0 + len > end + 1e-12 && len > 0.0)
            g.b = g.a + (std::max(0.0, end - g.t0) / len) * (g.b - g.a);
    }
    return out;
}

/** A convex part of one triangle between traces; each side is a triangle edge (0-2) or a segment (3 + index). */
struct Piece
{
    Index tri = kInvalid;
    std::vector<glm::dvec2> poly;
    std::vector<Index> side;
};

constexpr Index kSegSide = 3;

/** Each triangle cut along its trace segments into convex pieces. */
std::vector<Piece> cutPieces(const Layout& lay, const Traces& tr, int& mismatched, std::vector<uint8_t>& onEdge)
{
    LILA_ZONE();
    std::vector<Piece> pieces;
    const Index nt = Index(lay.s.tris.size());
    for (Index t = 0; t < nt; ++t)
    {
        std::vector<Piece> mine(1);
        mine[0].tri = t;
        mine[0].poly = {lay.chart[t][0], lay.chart[t][1], lay.chart[t][2]};
        mine[0].side = {0, 1, 2};
        // Segments that end on another go after it.
        std::vector<Index> order;
        std::vector<uint8_t> placed(tr.segments.size(), 0);
        std::function<void(Index, int)> place = [&](Index si, int depth) {
            if (placed[si] || depth > 64)
                return;
            placed[si] = 1;
            const Index on = tr.segments[si].endsOn;
            if (on < tr.segments.size() && tr.segments[on].tri == t)
                place(on, depth + 1);
            order.push_back(si);
        };
        for (Index si : tr.in[t])
            if (glm::length(tr.segments[si].b - tr.segments[si].a) > 1e-9)
                place(si, 0);
        for (Index si : order)
        {
            const Segment& g = tr.segments[si];
            const glm::dvec2 mid = 0.5 * (g.a + g.b), dir = glm::normalize(g.b - g.a);
            // The piece holding the segment's middle.
            size_t pi = mine.size();
            for (size_t i = 0; i < mine.size() && pi == mine.size(); ++i)
            {
                bool inside = true;
                const auto& P = mine[i].poly;
                for (size_t k = 0; k < P.size() && inside; ++k)
                    inside = cross2(P[(k + 1) % P.size()] - P[k], mid - P[k]) >= -1e-12;
                if (inside)
                    pi = i;
            }
            if (pi == mine.size())
            {
                ++mismatched;
                continue;
            }
            // Split it along the segment's line.
            const Piece P = mine[pi];
            const size_t n = P.poly.size();
            std::vector<double> sd(n);
            for (size_t k = 0; k < n; ++k)
                sd[k] = cross2(dir, P.poly[k] - g.a);
            Piece left{t}, right{t};
            std::vector<glm::dvec2> chord;
            for (size_t k = 0; k < n; ++k)
            {
                const size_t k1 = (k + 1) % n;
                const glm::dvec2 p = P.poly[k];
                const double eps = 1e-12;
                if (sd[k] >= -eps)
                {
                    left.poly.push_back(p);
                    left.side.push_back(std::abs(sd[k]) <= eps ? kSegSide + si : P.side[k]);
                }
                if (sd[k] <= eps)
                {
                    right.poly.push_back(p);
                    right.side.push_back(std::abs(sd[k]) <= eps ? kSegSide + si : P.side[k]);
                }
                if (std::abs(sd[k]) <= eps)
                    chord.push_back(p);
                if ((sd[k] > eps && sd[k1] < -eps) || (sd[k] < -eps && sd[k1] > eps))
                {
                    const glm::dvec2 x = p + (sd[k] / (sd[k] - sd[k1])) * (P.poly[k1] - p);
                    chord.push_back(x);
                    left.poly.push_back(x);
                    right.poly.push_back(x);
                    left.side.push_back(sd[k] > 0 ? kSegSide + si : P.side[k]);
                    right.side.push_back(sd[k] > 0 ? P.side[k] : kSegSide + si);
                }
            }
            // A vertex on the line starts the segment side; fix sides so each polygon edge keeps the label of the edge it
            // came from, and the chord edge gets the segment.
            auto relabel = [&](Piece& q) {
                const size_t m = q.poly.size();
                for (size_t k = 0; k < m; ++k)
                {
                    const glm::dvec2 p0 = q.poly[k], p1 = q.poly[(k + 1) % m];
                    const bool onLine = std::abs(cross2(dir, p0 - g.a)) <= 1e-9 && std::abs(cross2(dir, p1 - g.a)) <= 1e-9;
                    if (onLine)
                        q.side[k] = kSegSide + si;
                    else
                    {
                        // The original side this edge lies on.
                        for (size_t j = 0; j < n; ++j)
                        {
                            const glm::dvec2 o0 = P.poly[j], o1 = P.poly[(j + 1) % n], oe = o1 - o0;
                            const double l2 = glm::dot(oe, oe);
                            if (l2 <= 0.0)
                                continue;
                            const glm::dvec2 m0 = 0.5 * (p0 + p1);
                            if (std::abs(cross2(oe, m0 - o0)) <= 1e-9 * std::sqrt(l2) && glm::dot(m0 - o0, oe) >= 0.0 && glm::dot(m0 - o0, oe) <= l2)
                            {
                                q.side[k] = P.side[j];
                                break;
                            }
                        }
                    }
                }
            };
            if (left.poly.size() < 3 || right.poly.size() < 3)
            {
                // The segment runs along the piece's side (a line along a triangle edge): that side is the line.
                Piece& Q = mine[pi];
                bool along = false;
                for (size_t k = 0; k < Q.poly.size(); ++k)
                {
                    const glm::dvec2 p0 = Q.poly[k], p1 = Q.poly[(k + 1) % Q.poly.size()];
                    const double tol = 1e-7 * std::max(1.0, glm::length(p1 - p0));
                    if (std::abs(cross2(dir, p0 - g.a)) <= tol && std::abs(cross2(dir, p1 - g.a)) <= tol)
                    {
                        // Both triangles must see the line there, so the edge itself stops patches joining across it.
                        if (Q.side[k] < kSegSide)
                        {
                            if (const Index e = lay.triEdge[t][Q.side[k]]; e != kInvalid)
                                onEdge[e] = 1;
                        }
                        along = true;
                    }
                }
                if (!along)
                    ++mismatched;
                continue;
            }
            relabel(left);
            relabel(right);
            // The chord should be the segment: a segment that ends inside the piece leaves a dangling line.
            if (chord.size() == 2)
            {
                const double c0 = glm::dot(chord[0] - g.a, dir), c1 = glm::dot(chord[1] - g.a, dir), len = glm::length(g.b - g.a);
                const double lo = std::min(c0, c1), hi = std::max(c0, c1);
                if (lo < -1e-6 || hi > len + 1e-6)
                    ++mismatched;
            }
            mine[pi] = std::move(left);
            mine.push_back(std::move(right));
        }
        pieces.insert(pieces.end(), std::make_move_iterator(mine.begin()), std::make_move_iterator(mine.end()));
    }
    return pieces;
}


/** Points welded by position within a tolerance, so pieces that meet at a point share one id. */
class Welder
{
public:
    explicit Welder(double tol) : mTol(tol), mCell(4.0 * tol) {}
    Index id(const glm::dvec3& p)
    {
        const std::array<int64_t, 3> c = cell(p);
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dz = -1; dz <= 1; ++dz)
                    if (auto it = mCells.find(key({c[0] + dx, c[1] + dy, c[2] + dz})); it != mCells.end())
                        for (Index i : it->second)
                            if (glm::length(mPos[i] - p) <= mTol)
                                return i;
        const Index n = Index(mPos.size());
        mPos.push_back(p);
        mCells[key(c)].push_back(n);
        return n;
    }
    const glm::dvec3& pos(Index i) const { return mPos[i]; }
    Index size() const { return Index(mPos.size()); }

private:
    std::array<int64_t, 3> cell(const glm::dvec3& p) const
    {
        return {int64_t(std::floor(p.x / mCell)), int64_t(std::floor(p.y / mCell)), int64_t(std::floor(p.z / mCell))};
    }
    static uint64_t key(const std::array<int64_t, 3>& c)
    {
        return uint64_t(c[0]) * 73856093ull ^ uint64_t(c[1]) * 19349663ull ^ uint64_t(c[2]) * 83492791ull;
    }
    double mTol, mCell;
    std::vector<glm::dvec3> mPos;
    std::unordered_map<uint64_t, std::vector<Index>> mCells;
};

/** A stretch of patch border between two nodes, shared by the patches either side. */
struct Arc
{
    Index a = kInvalid, b = kInvalid; // end nodes, in the first traversal's direction
    std::vector<Index> path;          // points from a to b
    std::vector<double> along;        // lattice length from a to each path point
    double length = 0.0;
    int users = 0;
    bool wall = false, border = false;
    int count = 0; // whole lattice steps, once quantized
};

struct Side
{
    std::vector<std::pair<Index, bool>> arcs; // arc, and whether it runs forward along the side
};

struct PatchShape
{
    std::vector<Side> sides; // counter-clockwise from a corner
};

/**
 * Where the patches either side of a border saw different nodes on it, one side has a single arc and the other a chain
 * of arcs between the same ends, each used once. The single arc gives way to the chain, so both patches share its points.
 */
void matchSplitArcs(std::vector<Arc>& arcs, std::vector<PatchShape>& shapes, bool debug)
{
    std::unordered_map<Index, std::vector<Index>> loose; // node -> single-user inner arcs at it
    for (Index ai = 0; ai < Index(arcs.size()); ++ai)
        if (arcs[ai].users == 1 && !arcs[ai].border)
        {
            loose[arcs[ai].a].push_back(ai);
            loose[arcs[ai].b].push_back(ai);
        }
    int replaced = 0;
    for (PatchShape& shape : shapes)
        for (Side& side : shape.sides)
            for (size_t k = 0; k < side.arcs.size(); ++k)
            {
                const auto [x, fwd] = side.arcs[k];
                const Arc& X = arcs[x];
                if (X.users != 1 || X.border)
                    continue;
                const Index from = fwd ? X.a : X.b, to = fwd ? X.b : X.a;
                // Shortest chain of other loose arcs from `from` to `to`, at most four long.
                std::vector<Index> best, chain;
                std::function<void(Index, double)> walk = [&](Index at, double length) {
                    if (at == to && !chain.empty())
                    {
                        if (std::abs(length - X.length) < 0.5 * X.length + 0.5 && (best.empty() || chain.size() < best.size()))
                            best = chain;
                        return;
                    }
                    if (chain.size() >= 4)
                        return;
                    for (Index y : loose[at])
                        if (y != x && arcs[y].users == 1 && std::find(chain.begin(), chain.end(), y) == chain.end())
                        {
                            chain.push_back(y);
                            walk(arcs[y].a == at ? arcs[y].b : arcs[y].a, length + arcs[y].length);
                            chain.pop_back();
                        }
                };
                walk(from, 0.0);
                if (best.size() < 2)
                    continue;
                std::vector<std::pair<Index, bool>> with;
                Index at = from;
                for (Index y : best)
                {
                    with.push_back({y, arcs[y].a == at});
                    at = arcs[y].a == at ? arcs[y].b : arcs[y].a;
                    ++arcs[y].users;
                }
                arcs[x].users = 0;
                side.arcs.erase(side.arcs.begin() + std::ptrdiff_t(k));
                side.arcs.insert(side.arcs.begin() + std::ptrdiff_t(k), with.begin(), with.end());
                k += with.size() - 1;
                ++replaced;
            }
    if (debug)
        std::fprintf(stderr, "patch arcs: %d single arcs matched to a chain on the other side\n", replaced);
}

/**
 * Turns the traced patches into the quad mesh: borders become loops of arcs between nodes (corners and T-junctions),
 * each arc gets a whole number of steps with opposite sides of every four-sided patch equal (others split into quads
 * round a centre), and each patch is filled with a grid through a Tutte embedding of its triangles, which is one to one.
 */
Extracted buildPatchMesh(const Layout& lay, const std::vector<uint8_t>& blocked, const std::vector<std::vector<uint8_t>>& joined,
                         const std::vector<Piece>& pieces, const std::vector<Index>& patchOf, Index np, double targetCells, std::string& why,
                         std::vector<std::pair<Index, Index>>* merge, std::vector<Index>* wedges = nullptr)
{
    LILA_ZONE();
    const Surface& s = lay.s;
    const bool debug = std::getenv("LILA_PATCH_DEBUG") != nullptr;
    Extracted ex;
    auto at3d = [&](const Piece& P, const glm::dvec2& p) {
        const Chart& c = lay.chart[P.tri];
        const double area = cross2(c[1] - c[0], c[2] - c[0]);
        const glm::dvec3 w(cross2(c[1] - p, c[2] - p) / area, cross2(c[2] - p, c[0] - p) / area, cross2(c[0] - p, c[1] - p) / area);
        return w.x * s.pos[s.tris[P.tri][0]] + w.y * s.pos[s.tris[P.tri][1]] + w.z * s.pos[s.tris[P.tri][2]];
    };
    auto internalSide = [&](const Piece& P, Index side) {
        if (side >= kSegSide)
            return kInvalid;
        const Index e = lay.triEdge[P.tri][side];
        return e == kInvalid || blocked[e] ? kInvalid : e;
    };
    Welder weld(s.meanEdgeLength() * 4e-6);
    const Index npieces = Index(pieces.size());
    std::vector<std::vector<Index>> pid(npieces);
    for (Index pi = 0; pi < npieces; ++pi)
        for (const glm::dvec2& p : pieces[pi].poly)
            pid[pi].push_back(weld.id(at3d(pieces[pi], p)));

    // Field angle each patch fills at each of its points (see the corner count in layoutPatches).
    std::map<std::pair<Index, Index>, double> angleAt;
    for (Index pi = 0; pi < npieces; ++pi)
    {
        const Piece& P = pieces[pi];
        const size_t n = P.poly.size();
        for (size_t k = 0; k < n; ++k)
        {
            const glm::dvec2 u = P.poly[(k + n - 1) % n] - P.poly[k], v = P.poly[(k + 1) % n] - P.poly[k];
            double& a = angleAt[{patchOf[pi], pid[pi][k]}];
            a += std::atan2(cross2(v, u), glm::dot(u, v));
            if (const Index e = internalSide(P, P.side[(k + n - 1) % n]); e != kInvalid && joined[pi][(k + n - 1) % n])
                a += lay.frameTurn(e, P.tri, lay.other(e, P.tri));
        }
    }

    // Border edges, interior on the left; each piece side keeps the points later inserted on it.
    struct Edge
    {
        Index patch, from, to, piece;
        int side;
        double length;
        Index wallEdge;
    };
    std::vector<std::vector<Index>> polyPid = pid; // with inserted nodes
    std::vector<Edge> edges;
    for (Index pi = 0; pi < npieces; ++pi)
    {
        const Piece& P = pieces[pi];
        for (size_t k = 0; k < P.poly.size(); ++k)
        {
            if (internalSide(P, P.side[k]) != kInvalid && joined[pi][k])
                continue;
            const Index a = pid[pi][k], b = pid[pi][(k + 1) % P.poly.size()];
            if (a == b)
                continue;
            edges.push_back({patchOf[pi], a, b, pi, int(k), glm::length(P.poly[(k + 1) % P.poly.size()] - P.poly[k]),
                             P.side[k] < kSegSide && lay.triEdge[P.tri][P.side[k]] != kInvalid && lay.wall[lay.triEdge[P.tri][P.side[k]]]
                                 ? lay.triEdge[P.tri][P.side[k]]
                                 : kInvalid});
        }
    }

    std::vector<uint8_t> isNode;
    std::set<std::pair<Index, Index>> virtualCorner; // (patch, point) corners added to patches with too few
    std::set<std::pair<Index, Index>> dropped;       // (patch, point) corners passed over in patches with too many
    // Split every border edge at the nodes lying inside it, so both patches along a line see the same nodes.
    auto insertNodes = [&]() {
        isNode.resize(weld.size(), 0);
        const double h = s.meanEdgeLength();
        std::unordered_map<uint64_t, std::vector<Index>> grid;
        auto cellOf = [&](const glm::dvec3& p) {
            return std::array<int64_t, 3>{int64_t(std::floor(p.x / h)), int64_t(std::floor(p.y / h)), int64_t(std::floor(p.z / h))};
        };
        auto hash = [](const std::array<int64_t, 3>& c) { return uint64_t(c[0]) * 73856093ull ^ uint64_t(c[1]) * 19349663ull ^ uint64_t(c[2]) * 83492791ull; };
        for (Index i = 0; i < weld.size(); ++i)
            if (isNode[i])
                grid[hash(cellOf(weld.pos(i)))].push_back(i);
        std::vector<Edge> out;
        out.reserve(edges.size());
        for (const Edge& e : edges)
        {
            const glm::dvec3 a = weld.pos(e.from), b = weld.pos(e.to), d = b - a;
            const double l2 = glm::dot(d, d);
            std::vector<std::pair<double, Index>> on;
            const auto ca = cellOf(glm::min(a, b)), cb = cellOf(glm::max(a, b));
            for (int64_t x = ca[0] - 1; x <= cb[0] + 1; ++x)
                for (int64_t y = ca[1] - 1; y <= cb[1] + 1; ++y)
                    for (int64_t z = ca[2] - 1; z <= cb[2] + 1; ++z)
                        if (auto it = grid.find(hash({x, y, z})); it != grid.end())
                            for (Index n : it->second)
                            {
                                if (n == e.from || n == e.to || l2 <= 0.0)
                                    continue;
                                const double t = glm::dot(weld.pos(n) - a, d) / l2;
                                if (t <= 1e-9 || t >= 1.0 - 1e-9)
                                    continue;
                                if (glm::length(a + t * d - weld.pos(n)) <= 1e-6 * std::sqrt(l2) + 1e-7 * h)
                                    on.push_back({t, n});
                            }
            if (on.empty())
            {
                out.push_back(e);
                continue;
            }
            std::sort(on.begin(), on.end());
            on.erase(std::unique(on.begin(), on.end(), [](const auto& x, const auto& y) { return x.second == y.second; }), on.end());
            // The piece's polygon gains the points too, so the patch's triangles reach them.
            std::vector<Index>& poly = polyPid[e.piece];
            auto at = std::find(poly.begin(), poly.end(), e.from);
            size_t pos = size_t(at - poly.begin()) + 1;
            Index prev = e.from;
            double prevT = 0.0;
            for (const auto& [t, n] : on)
            {
                poly.insert(poly.begin() + pos++, n);
                out.push_back({e.patch, prev, n, e.piece, e.side, e.length * (t - prevT), e.wallEdge});
                prev = n;
                prevT = t;
            }
            out.push_back({e.patch, prev, e.to, e.piece, e.side, e.length * (1.0 - prevT), e.wallEdge});
        }
        edges = std::move(out);
    };

    // Loops: each patch's border edges chained head to tail.
    std::vector<std::vector<Index>> loopOf; // per patch, edge indices in order
    // Each patch's border edges joined into one circuit (Hierholzer), so a slit or a pinch is walked out and back; edges
    // left over after that are a second border, a patch that isn't a disk.
    // Pieces by (triangle, side label): who is across a traced side.
    std::map<std::pair<Index, Index>, std::vector<Index>> sideOf;
    for (Index pi = 0; pi < npieces; ++pi)
        for (Index label : pieces[pi].side)
            if (label >= kSegSide)
                sideOf[{pieces[pi].tri, label}].push_back(pi);
    std::vector<std::vector<Index>> piecesAt(weld.size());
    for (Index pi = 0; pi < npieces; ++pi)
        for (Index w : pid[pi])
            piecesAt[w].push_back(pi);
    auto buildLoops = [&]() {
        bool failed = false;
        loopOf.assign(np, {});
        std::vector<std::vector<Index>> ofPatch(np);
        for (Index i = 0; i < Index(edges.size()); ++i)
            ofPatch[edges[i].patch].push_back(i);
        for (Index p = 0; p < np; ++p)
        {
            if (ofPatch[p].empty())
            {
                // A sliver thinner than the weld: it joins a patch it touches.
                Index touching = kInvalid;
                for (Index pi = 0; pi < npieces && touching == kInvalid; ++pi)
                    if (patchOf[pi] == p)
                        for (Index w : pid[pi])
                            for (Index pj : piecesAt[w])
                                if (patchOf[pj] != p)
                                    touching = patchOf[pj];
                if (merge && touching != kInvalid)
                    merge->push_back({p, touching});
                failed = true;
                continue;
            }
            std::unordered_map<Index, std::vector<Index>> outOf;
            std::unordered_map<Index, int> balance;
            for (Index e : ofPatch[p])
            {
                outOf[edges[e].from].push_back(e);
                ++balance[edges[e].from];
                --balance[edges[e].to];
            }
            bool even = true;
            for (const auto& [v, b] : balance)
                even = even && b == 0;
            std::vector<Index> circuit, stack = {ofPatch[p][0]};
            std::unordered_map<Index, size_t> next;
            if (even)
            {
                auto& first = outOf[edges[ofPatch[p][0]].from];
                first.erase(std::find(first.begin(), first.end(), ofPatch[p][0]));
                while (!stack.empty())
                {
                    auto& out = outOf[edges[stack.back()].to];
                    if (!out.empty())
                    {
                        stack.push_back(out.back());
                        out.pop_back();
                    }
                    else
                    {
                        circuit.push_back(stack.back());
                        stack.pop_back();
                    }
                }
                std::reverse(circuit.begin(), circuit.end());
            }
            if (!even && debug && std::getenv("LILA_PATCH_LIST"))
                for (const auto& [v, b] : balance)
                    if (b != 0)
                    {
                        std::fprintf(stderr, "  patch %d: point %d off by %d at %.12f %.12f %.12f;", int(p), int(v), b, weld.pos(v).x, weld.pos(v).y, weld.pos(v).z);
                        for (Index e : ofPatch[p])
                            if (edges[e].from == v || edges[e].to == v)
                                std::fprintf(stderr, " [%s tri %d side %d label %d wall %d]", edges[e].from == v ? "out" : "in", int(pieces[edges[e].piece].tri),
                                             edges[e].side, int(pieces[edges[e].piece].side[edges[e].side]), int(edges[e].wallEdge != kInvalid));
                        std::fprintf(stderr, "\n");
                    }
            if (even && circuit.size() != ofPatch[p].size())
            {
                // Leftover loops with this patch on both sides (a closed line inside it) are dropped.
                std::vector<Index> sorted = circuit;
                std::sort(sorted.begin(), sorted.end());
                bool island = false;
                for (Index e : ofPatch[p])
                    if (!std::binary_search(sorted.begin(), sorted.end(), e))
                        for (Index other : sideOf[std::pair(pieces[edges[e].piece].tri, pieces[edges[e].piece].side[edges[e].side])])
                            island = island || patchOf[other] != p;
                if (!island)
                {
                    loopOf[p] = std::move(circuit);
                    continue;
                }
            }
            if (!even || circuit.size() != ofPatch[p].size())
            {
                if (debug && std::getenv("LILA_PATCH_LIST"))
                    std::fprintf(stderr, "patch %d border: %s, %zu of %zu edges in one circuit\n", int(p), even ? "more than one border" : "edges don't pair up",
                                 circuit.size(), ofPatch[p].size());
                // An island inside (the edges the circuit missed) joins this patch; a patch whose edges don't pair up joins the
                // neighbour it shares most border with.
                std::vector<Index> rest;
                if (even)
                {
                    std::vector<Index> sorted = circuit;
                    std::sort(sorted.begin(), sorted.end());
                    for (Index e : ofPatch[p])
                        if (!std::binary_search(sorted.begin(), sorted.end(), e))
                            rest.push_back(e);
                }
                else
                    rest = ofPatch[p];
                std::map<Index, int> shared;
                for (Index e : rest)
                {
                    const Edge& ed = edges[e];
                    const Index label = pieces[ed.piece].side[ed.side];
                    for (Index other : sideOf[{pieces[ed.piece].tri, label}])
                        if (patchOf[other] != p)
                            ++shared[patchOf[other]];
                }
                if (merge)
                {
                    if (even)
                        for (const auto& [q, n] : shared)
                            merge->push_back({p, q});
                    else if (!shared.empty())
                        merge->push_back({p, std::max_element(shared.begin(), shared.end(), [](const auto& a, const auto& b) { return a.second < b.second; })->first});
                }
                failed = true;
                continue;
            }
            loopOf[p] = std::move(circuit);
        }
        return failed ? "a patch isn't a disk" : "";
    };
    // Turns closer together than half a row (a step across onto a line) count as one: their angles add up, and a step
    // that turns back to straight is no corner at all.
    auto cornersOf = [&](Index p) {
        std::vector<size_t> out; // positions in the loop whose start point is a corner
        const std::vector<Index>& loop = loopOf[p];
        struct Turn
        {
            size_t at;
            double deg, along;
        };
        std::vector<Turn> turns;
        double along = 0.0;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            const Index v = edges[loop[i]].from;
            if (!dropped.count({p, v}))
            {
                if (virtualCorner.count({p, v}))
                    turns.push_back({i, 90.0, along});
                else if (auto it = angleAt.find({p, v}); it != angleAt.end() && std::abs(glm::degrees(it->second) - 180.0) > 30.0)
                    turns.push_back({i, glm::degrees(it->second), along});
            }
            along += edges[loop[i]].length;
        }
        const double total = along;
        constexpr double kNear = 0.5;
        // Start the clusters after a gap, so none wraps round the loop's start.
        size_t first = 0;
        for (size_t k = 0; k < turns.size(); ++k)
        {
            const double gap = k == 0 ? turns[0].along + total - turns.back().along : turns[k].along - turns[k - 1].along;
            if (gap >= kNear)
            {
                first = k;
                break;
            }
        }
        for (size_t n = 0; n < turns.size();)
        {
            const size_t k0 = (first + n) % turns.size();
            double sum = turns[k0].deg - 180.0;
            size_t best = k0;
            size_t len = 1;
            for (; n + len < turns.size(); ++len)
            {
                const size_t k = (first + n + len) % turns.size(), prev = (first + n + len - 1) % turns.size();
                const double gap = std::fmod(turns[k].along - turns[prev].along + total, total);
                if (gap >= kNear)
                    break;
                sum += turns[k].deg - 180.0;
                if (std::abs(turns[k].deg - 90.0) < std::abs(turns[best].deg - 90.0))
                    best = k;
            }
            bool step = false;
            for (size_t j = 0; j < len; ++j)
                step = step || turns[(first + n + j) % turns.size()].deg > 225.0;
            if (step && std::getenv("LILA_CORNER_CLUSTER"))
            {
                const double deg = 180.0 + sum;
                if (deg > 45.0 && deg < 135.0)
                    out.push_back(turns[best].at);
            }
            else
                for (size_t j = 0; j < len; ++j)
                    if (const Turn& tn = turns[(first + n + j) % turns.size()]; tn.deg > 45.0 && tn.deg < 135.0)
                        out.push_back(tn.at);
            n += len;
        }
        std::sort(out.begin(), out.end());
        return out;
    };

    isNode.assign(weld.size(), 0);
    for (const auto& [key, a] : angleAt)
    {
        const double deg = glm::degrees(a);
        if (deg > 45.0 && deg < 135.0)
            isNode[key.second] = 1;
    }
    insertNodes();
    if (const char* e = buildLoops(); *e)
    {
        why = e;
        return {};
    }
    // Every patch is made four-sided: more than four keeps the four nearest a right angle, the others run straight
    // through as points on a side; fewer get corners added, evenly round the border, or for three in the middle of the
    // longest side.
    for (int settle = 0; settle < 4; ++settle)
    {
    bool added = false;
    for (Index p = 0; p < np; ++p)
    {
        const std::vector<size_t> c = cornersOf(p);
        if (c.size() == 4)
            continue;
        if (debug && std::getenv("LILA_PATCH_LIST") && loopOf[p].size() > 20)
        {
            std::fprintf(stderr, "settle %d patch %d: %zu corners at", settle, int(p), c.size());
            for (size_t i : c)
                std::fprintf(stderr, " %zu(pid %d%s)", i, int(edges[loopOf[p][i]].from), virtualCorner.count({p, edges[loopOf[p][i]].from}) ? " v" : "");
            std::fprintf(stderr, " of %zu\n", loopOf[p].size());
        }
        if (c.size() > 4)
        {
            std::vector<std::pair<double, Index>> byAngle;
            for (size_t i : c)
            {
                const Index v = edges[loopOf[p][i]].from;
                const auto it = angleAt.find({p, v});
                byAngle.push_back({it == angleAt.end() ? 0.0 : std::abs(glm::degrees(it->second) - 90.0), v});
            }
            std::sort(byAngle.begin(), byAngle.end());
            for (size_t i = 4; i < byAngle.size(); ++i)
                dropped.insert({p, byAngle[i].second});
            continue;
        }
        const std::vector<Index>& loop = loopOf[p];
        std::vector<double> cum(loop.size() + 1, 0.0);
        for (size_t i = 0; i < loop.size(); ++i)
            cum[i + 1] = cum[i] + edges[loop[i]].length;
        const double total = cum.back();
        std::vector<double> want;
        if (c.empty())
            want = {0.0, 0.25 * total, 0.5 * total, 0.75 * total};
        else if (c.size() == 1)
            want = {cum[c[0]] + 0.25 * total, cum[c[0]] + 0.5 * total, cum[c[0]] + 0.75 * total};
        else if (c.size() == 3)
        {
            double longest = -1.0, at = 0.0;
            for (size_t k = 0; k < 3; ++k)
            {
                const double a = cum[c[k]], b = k + 1 < 3 ? cum[c[k + 1]] : cum[c[0]] + total;
                if (b - a > longest)
                {
                    longest = b - a;
                    at = 0.5 * (a + b);
                }
            }
            want = {at};
        }
        else
        {
            const double a = cum[c[0]], b = cum[c[1]];
            want = {0.5 * (a + b), 0.5 * (b + a + total)};
        }
        for (double w : want)
        {
            w = std::fmod(w, total);
            size_t best = loop.size();
            for (size_t i = 0; i < loop.size(); ++i)
                if (std::find(c.begin(), c.end(), i) == c.end() && (best == loop.size() || std::abs(cum[i] - w) < std::abs(cum[best] - w)))
                    best = i;
            if (best == loop.size())
            {
                // No point between the corners: a new one in the middle of the longest edge, split into both sides.
                size_t longestEdge = 0;
                for (size_t i = 0; i < loop.size(); ++i)
                    if (edges[loop[i]].length > edges[loop[longestEdge]].length)
                        longestEdge = i;
                const Edge& e = edges[loop[longestEdge]];
                const Index mid = weld.id(0.5 * (weld.pos(e.from) + weld.pos(e.to)));
                isNode.resize(weld.size(), 0);
                piecesAt.resize(weld.size());
                isNode[mid] = 1;
                virtualCorner.insert({p, mid});
                added = true;
                continue;
            }
            isNode[edges[loop[best]].from] = 1;
            virtualCorner.insert({p, edges[loop[best]].from});
            added = true;
        }
    }
    if (!added)
        break;
    insertNodes();
    if (const char* e = buildLoops(); *e)
    {
        why = e;
        return {};
    }
    }

    // A patch too small to take three corners joins the neighbour across its longest edge.
    bool tooFew = false;
    for (Index p = 0; p < np; ++p)
    {
        if (cornersOf(p).size() >= 3)
            continue;
        tooFew = true;
        Index across = kInvalid;
        double longest = -1.0;
        for (Index e : loopOf[p])
            if (edges[e].length > longest)
                for (Index other : sideOf[std::pair(pieces[edges[e].piece].tri, pieces[edges[e].piece].side[edges[e].side])])
                    if (patchOf[other] != p)
                    {
                        longest = edges[e].length;
                        across = patchOf[other];
                    }
        if (across == kInvalid)
            for (Index e : loopOf[p])
                for (Index pj : piecesAt[edges[e].from])
                    if (patchOf[pj] != p)
                        across = patchOf[pj];
        if (merge && across != kInvalid)
            merge->push_back({p, across});
    }
    if (tooFew)
    {
        why = "a patch has too few corners";
        return {};
    }

    // Arcs: each loop cut at the nodes, matched with the same stretch seen from the patch on the other side.
    std::vector<Arc> arcs;
    std::map<std::pair<Index, Index>, std::vector<Index>> arcsBetween;
    std::vector<PatchShape> shapes(np);
    int nGons = 0;
    for (Index p = 0; p < np; ++p)
    {
        const std::vector<Index>& loop = loopOf[p];
        const std::vector<size_t> corner = cornersOf(p);
        if (corner.size() < 3)
        {
            why = "a patch has too few corners";
            return {};
        }
        std::vector<uint8_t> isCorner(loop.size(), 0);
        for (size_t c : corner)
            isCorner[c] = 1;
        PatchShape& shape = shapes[p];
        const size_t start = corner[0];
        std::vector<Index> path = {edges[loop[start]].from};
        std::vector<double> along = {0.0};
        bool wall = true, border = true;
        for (size_t step = 0; step < loop.size(); ++step)
        {
            const size_t i = (start + step) % loop.size();
            if (isCorner[i] && step > 0)
                shape.sides.emplace_back();
            if (step == 0)
                shape.sides.emplace_back();
            const Edge& e = edges[loop[i]];
            path.push_back(e.to);
            along.push_back(along.back() + e.length);
            wall = wall && e.wallEdge != kInvalid;
            border = border && e.wallEdge != kInvalid && (s.edgeTriCount[e.wallEdge] != 2 || s.edgeBoundary[e.wallEdge]);
            const size_t next = (i + 1) % loop.size();
            if (!isNode[e.to] && !isCorner[next])
                continue;
            // An arc from path.front() to e.to.
            const Index a = path.front(), b = path.back();
            const std::pair<Index, Index> key{std::min(a, b), std::max(a, b)};
            Index found = kInvalid;
            for (Index ai : arcsBetween[key])
            {
                Arc& arc = arcs[ai];
                if (arc.users >= 2 || arc.a != b || arc.b != a)
                    continue;
                bool shares = path.size() == 2 && arc.path.size() == 2;
                for (size_t k = 1; k + 1 < path.size() && !shares; ++k)
                    shares = std::find(arc.path.begin() + 1, arc.path.end() - 1, path[k]) != arc.path.end() - 1;
                if (shares)
                    found = ai;
            }
            if (found == kInvalid)
            {
                found = Index(arcs.size());
                arcs.push_back({a, b, path, along, along.back(), 0, wall, border});
                arcsBetween[key].push_back(found);
            }
            else
                arcs[found].length = 0.5 * (arcs[found].length + along.back());
            ++arcs[found].users;
            shape.sides.back().arcs.push_back({found, arcs[found].a == a});
            path = {e.to};
            along = {0.0};
            wall = border = true;
        }
        if (shape.sides.size() != 4)
        {
            ++nGons;
            if (debug && std::getenv("LILA_PATCH_LIST"))
                std::fprintf(stderr, "patch %d has %zu sides from %zu corners, %zu border edges\n", int(p), shape.sides.size(), corner.size(), loop.size());
        }
    }

    matchSplitArcs(arcs, shapes, debug);

    // Quantize: whole steps per arc, close to its lattice length times a scale that meets the target count, with
    // opposite sides of every four-sided patch equal. An arc may get none: a sliver between near-parallel lines collapses. Patches with other corner counts stay holes for the hole filler,
    // so they constrain nothing.
    const Index na = Index(arcs.size());
    struct Row
    {
        std::vector<std::pair<Index, double>> terms;
        Index patch;
    };
    std::vector<Row> rows;
    // A four-sided patch whose opposite sides differ a lot isn't a rectangle (a corner in the wrong place); holding its
    // sides equal would drag every patch in line with it, so it is left free, for the hole filler.
    auto latticeLength = [&](const Side& side) {
        double l = 0.0;
        for (const auto& [arc, fwd] : side.arcs)
            l += arcs[arc].length;
        return l;
    };
    std::vector<uint8_t> unequal(np, 0);
    for (Index p = 0; p < np; ++p)
    {
        const PatchShape& shape = shapes[p];
        if (shape.sides.size() != 4)
            continue;
        for (int k = 0; k < 2; ++k)
        {
            const double a = latticeLength(shape.sides[k]), b = latticeLength(shape.sides[k + 2]);
            // Sides two steps apart are held however short: a pattern fills that difference with a valence 6 and two
            // valence 3. Not a near-triangle (a side almost gone), nor a short side on the border, which holding would
            // pull inside.
            const Side& shorter = a < b ? shape.sides[k] : shape.sides[k + 2];
            const bool borderSliver = std::min(a, b) < 1.0 && std::all_of(shorter.arcs.begin(), shorter.arcs.end(), [&](const auto& sa) { return arcs[sa.first].border; });
            const double held = borderSliver || std::min(a, b) < 0.3 ? 0.0 : 2.0;
            // A short strip (a finger side tapering to its tip) is held too: a pattern puts its poles side by side there.
            const double c = latticeLength(shape.sides[k + 1]), d = latticeLength(shape.sides[(k + 3) % 4]);
            const bool strip = std::min(c, d) >= 2.0 * std::max(a, b) && std::max(c, d) <= 20.0 && std::abs(a - b) <= 3.0;
            if (!strip && std::abs(a - b) > std::max({1.5, held, 0.5 * std::max(a, b)}))
            {
                unequal[p] = 1;
                // Far wider at one end (a foot top running on into a shin strip): no pattern fills that, so the layout cuts it.
                if (wedges && std::abs(a - b) >= 6.0 && std::max(a, b) >= 2.5 * std::min(a, b))
                    wedges->push_back(p);
            }
            else if (debug && std::getenv("LILA_PATCH_LIST") && std::abs(a - b) > std::max(1.5, 0.5 * std::max(a, b)))
                std::fprintf(stderr, "held patch %d: sides %.2f %.2f\n", int(p), a, b);
        }
    }
    for (Index p = 0; p < np; ++p)
    {
        const PatchShape& shape = shapes[p];
        if (shape.sides.size() != 4 || unequal[p])
            continue;
        for (int k = 0; k < 2; ++k)
        {
            std::map<Index, double> sum;
            for (const auto& [arc, fwd] : shape.sides[k].arcs)
                sum[arc] += 1.0;
            for (const auto& [arc, fwd] : shape.sides[k + 2].arcs)
                sum[arc] -= 1.0;
            Row row{{}, p};
            for (auto [arc, c] : sum)
                if (c != 0.0)
                    row.terms.push_back({arc, c});
            rows.push_back(std::move(row));
        }
    }
    auto sideLength = [&](const Side& side) {
        double l = 0.0;
        for (const auto& [arc, fwd] : side.arcs)
            l += arcs[arc].length;
        return l;
    };
    const double stiff = 1e6;
    Eigen::SparseMatrix<double> base(na, na);
    {
        std::vector<Eigen::Triplet<double>> trip;
        for (Index i = 0; i < na; ++i)
            trip.emplace_back(i, i, 1.0);
        for (const Row& row : rows)
            for (auto [vi, ci] : row.terms)
                for (auto [vj, cj] : row.terms)
                    trip.emplace_back(vi, vj, stiff * ci * cj);
        base.setFromTriplets(trip.begin(), trip.end());
    }
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;
    ldlt.analyzePattern(base);
    std::vector<double> target(na, 0.0);
    std::vector<std::pair<Index, double>> fixed;
    Eigen::VectorXd x;
    auto solve = [&]() {
        Eigen::SparseMatrix<double> a = base;
        Eigen::VectorXd b(na);
        for (Index i = 0; i < na; ++i)
            b[i] = target[i];
        for (auto [var, value] : fixed)
        {
            a.coeffRef(var, var) += stiff;
            b[var] += stiff * value;
        }
        ldlt.factorize(a);
        if (ldlt.info() != Eigen::Success)
            return false;
        x = ldlt.solve(b);
        return ldlt.info() == Eigen::Success && x.allFinite();
    };
    // Cells: four-sided patches by their sides, the rest by their perimeter as if square.
    auto cells = [&](auto&& value) {
        double sum = 0.0;
        for (const PatchShape& shape : shapes)
        {
            if (shape.sides.size() == 4)
            {
                double w = 0.0, h = 0.0;
                for (const auto& [arc, fwd] : shape.sides[0].arcs)
                    w += value(arc);
                for (const auto& [arc, fwd] : shape.sides[1].arcs)
                    h += value(arc);
                sum += w * h;
            }
            else
            {
                double perimeter = 0.0;
                for (const Side& side : shape.sides)
                    for (const auto& [arc, fwd] : side.arcs)
                        perimeter += value(arc);
                sum += perimeter * perimeter / 16.0;
            }
        }
        return sum;
    };
    double alpha = 1.0;
    auto setTargets = [&]() {
        for (Index a = 0; a < na; ++a)
            target[a] = alpha * arcs[a].length;
    };
    setTargets();
    if (targetCells > 0.0)
        if (const double now = cells([&](Index a) { return target[a]; }); now > 0.0)
            alpha = std::sqrt(targetCells / now);
    std::vector<int> value(na, 0);
    // An arc may only vanish where the patch really is thin: if the 3D distance across a patch is well over what its
    // short sides' lattice length says (its corners sit wrong), those sides keep at least a step, or the collapse would
    // weld points from opposite sides of a tube together.
    std::vector<int> lower(na, std::getenv("LILA_PATCH_MIN1") ? 1 : 0);
    {
        auto midpoint = [&](const Side& side) {
            double total = 0.0;
            for (const auto& [ai, fwd] : side.arcs)
                total += arcs[ai].along.back();
            double want = 0.5 * total;
            for (const auto& [ai, fwd] : side.arcs)
            {
                const Arc& arc = arcs[ai];
                const double l = arc.along.back();
                if (want <= l)
                {
                    const double at = fwd ? want : l - want;
                    size_t j = 1;
                    while (j + 1 < arc.along.size() && arc.along[j] < at)
                        ++j;
                    const double span = arc.along[j] - arc.along[j - 1];
                    const double t = span > 0.0 ? std::clamp((at - arc.along[j - 1]) / span, 0.0, 1.0) : 0.0;
                    return glm::mix(weld.pos(arc.path[j - 1]), weld.pos(arc.path[j]), t);
                }
                want -= l;
            }
            return weld.pos(arcs[side.arcs.back().first].b);
        };
        auto unit3d = [&](const Side& side) {
            double lattice = 0.0, space = 0.0;
            for (const auto& [ai, fwd] : side.arcs)
            {
                const Arc& arc = arcs[ai];
                lattice += arc.length;
                for (size_t k = 1; k < arc.path.size(); ++k)
                    space += glm::length(weld.pos(arc.path[k]) - weld.pos(arc.path[k - 1]));
            }
            return lattice > 1e-9 ? space / lattice : 0.0;
        };
        for (const PatchShape& shape : shapes)
        {
            if (shape.sides.size() != 4)
                continue;
            for (int k = 0; k < 2; ++k)
            {
                const Side &a = shape.sides[k], &b = shape.sides[k + 2], &across0 = shape.sides[k + 1], &across1 = shape.sides[(k + 3) % 4];
                const double unit = 0.5 * (unit3d(a) + unit3d(b));
                const double across = 0.5 * (sideLength(across0) + sideLength(across1));
                if (unit > 0.0 && glm::length(midpoint(a) - midpoint(b)) > unit * std::max(0.75, 3.0 * across))
                    for (const Side* side : {&across0, &across1})
                    {
                        // One step on the side's longest arc keeps it open.
                        Index longest = side->arcs.front().first;
                        for (const auto& [ai, fwd] : side->arcs)
                            if (arcs[ai].length > arcs[longest].length)
                                longest = ai;
                        lower[longest] = 1;
                    }
            }
        }
    }
    std::vector<uint8_t> holeRow(rows.size(), 0);
    // Rounding moves the count, so the scale is corrected and it all goes again; the pass closest to the target is kept.
    std::vector<int> bestValue;
    std::vector<uint8_t> bestHoles;
    double bestOff = std::numeric_limits<double>::max();
    for (int pass = 0; pass < 8; ++pass)
    {
        setTargets();
        fixed.clear();
        if (!solve())
        {
            why = "patch quantization didn't solve";
            return {};
        }
        // Greedy rounding, closest to whole first, re-solving between batches.
        std::vector<uint8_t> isFixed(na, 0);
        for (int round = 0; round < 200; ++round)
        {
            std::vector<std::pair<double, Index>> open;
            for (Index i = 0; i < na; ++i)
                if (!isFixed[i])
                    open.push_back({std::abs(x[i] - std::round(x[i])), i});
            if (open.empty())
                break;
            std::sort(open.begin(), open.end());
            const size_t batch = std::max<size_t>(1, open.size() / 4);
            for (size_t i = 0; i < open.size() && (i < batch || open[i].first < 0.05); ++i)
            {
                isFixed[open[i].second] = 1;
                fixed.push_back({open[i].second, std::max(double(lower[open[i].second]), std::round(x[open[i].second]))});
            }
            if (!solve())
            {
                why = "patch quantization didn't solve";
                return {};
            }
        }
        for (auto [var, v] : fixed)
            value[var] = int(v);
        // Repair: a row left unequal is evened by a chain of +-1 steps through the rows sharing arcs (each arc sits in at
        // most two rows), ending at a row that wants the opposite change or an arc in one row only.
        std::vector<std::vector<std::pair<Index, int>>> rowsOf(na);
        for (Index r = 0; r < Index(rows.size()); ++r)
            for (auto [a, c] : rows[r].terms)
                rowsOf[a].push_back({r, int(c)});
        std::vector<int> res(rows.size(), 0);
        for (Index r = 0; r < Index(rows.size()); ++r)
            for (auto [a, c] : rows[r].terms)
                res[r] += int(c) * value[a];
        auto cost = [&](Index a, int d) {
            const double t = alpha * arcs[a].length;
            return 1.0 + std::max(0.0, std::abs(value[a] + d - t) - std::abs(value[a] - t));
        };
        std::fill(holeRow.begin(), holeRow.end(), 0);
        for (Index r0 = 0; r0 < Index(rows.size()); ++r0)
            for (int tries = 0; res[r0] != 0; ++tries)
            {
                Budget::checkpoint();
                if (tries >= 16)
                {
                    holeRow[r0] = 1;
                    break;
                }
                // Dijkstra over rows: state (row, the change that row needs).
                struct Back
                {
                    Index row, arc;
                    int delta;
                };
                std::unordered_map<Index, double> dist;
                std::unordered_map<Index, Back> back;
                std::unordered_map<Index, int> need;
                using Q = std::pair<double, Index>;
                std::priority_queue<Q, std::vector<Q>, std::greater<>> queue;
                dist[r0] = 0.0;
                need[r0] = res[r0] > 0 ? -1 : 1;
                queue.push({0.0, r0});
                Index endRow = kInvalid, endArc = kInvalid;
                int endDelta = 0;
                Index endFrom = kInvalid;
                double best = std::numeric_limits<double>::max();
                while (!queue.empty())
                {
                    const auto [d, r] = queue.top();
                    queue.pop();
                    if (d > dist[r] || d >= best)
                        continue;
                    const int want = need[r];
                    for (auto [a, c] : rows[r].terms)
                    {
                        const int delta = want * int(c);
                        if (value[a] + delta < lower[a])
                            continue;
                        const double nd = d + cost(a, delta);
                        Index other = kInvalid;
                        int oc = 0;
                        for (auto [r2, c2] : rowsOf[a])
                            if (r2 != r)
                            {
                                other = r2;
                                oc = c2;
                            }
                        const int effect = oc * delta;
                        if (other == kInvalid || (res[other] != 0 && (res[other] > 0) != (effect > 0)))
                        {
                            if (nd < best)
                            {
                                best = nd;
                                endRow = other;
                                endArc = a;
                                endDelta = delta;
                                endFrom = r;
                            }
                            continue;
                        }
                        if (auto it = dist.find(other); it == dist.end() || nd < it->second)
                        {
                            dist[other] = nd;
                            need[other] = -effect;
                            back[other] = {r, a, delta};
                            queue.push({nd, other});
                        }
                    }
                }
                if (endArc == kInvalid)
                {
                    holeRow[r0] = 1;
                    break;
                }
                auto apply = [&](Index a, int delta) {
                    value[a] += delta;
                    for (auto [r2, c2] : rowsOf[a])
                        res[r2] += c2 * delta;
                };
                apply(endArc, endDelta);
                for (Index r = endFrom; r != r0;)
                {
                    const Back& b = back[r];
                    apply(b.arc, b.delta);
                    r = b.row;
                }
                (void)endRow;
            }
        int unmet = 0;
        for (Index r = 0; r < Index(rows.size()); ++r)
            unmet += res[r] != 0;
        const double got = cells([&](Index a) { return double(value[a]); });
        if (debug)
            std::fprintf(stderr, "patch quantization pass %d: %d arcs, %d four-sided of %d, %zu rows, %d unmet, alpha %.3f, cells %.0f (target %.0f)\n", pass,
                         int(na), int(np) - nGons, int(np), rows.size(), unmet, alpha, got, targetCells);
        for (Index r = 0; r < Index(rows.size()); ++r)
            holeRow[r] = res[r] != 0;
        const double off = targetCells > 0.0 ? std::abs(got / targetCells - 1.0) + 0.01 * unmet : 0.0;
        if (off < bestOff)
        {
            bestOff = off;
            bestValue = value;
            bestHoles = holeRow;
        }
        if (targetCells <= 0.0 || off < 0.01)
            break;
        // Damped, since rounding answers a change of scale in steps.
        alpha *= std::pow(targetCells / got, pass < 2 ? 0.5 : 0.3);
    }
    value = bestValue;
    holeRow = bestHoles;
    for (Index a = 0; a < na; ++a)
        arcs[a].count = value[a];
    // Four-sided patches whose sides came out unequal are left for the hole filler too.
    std::vector<uint8_t> hole(np, 0);
    for (Index p = 0; p < np; ++p)
        hole[p] = shapes[p].sides.size() != 4 || unequal[p];
    for (Index r = 0; r < Index(rows.size()); ++r)
        if (holeRow[r])
            hole[rows[r].patch] = 1;
    // A free patch is filled by pattern (Takayama et al. 2014), which needs an even number of steps round it. An odd one
    // changes by a step along a chain of arcs through held patches, each step evened out on the patch's opposite side,
    // until the chain ends on the border or at another odd free patch.
    {
        std::vector<std::vector<std::pair<Index, int>>> heldRowsOf(na);
        for (Index r = 0; r < Index(rows.size()); ++r)
            if (!hole[rows[r].patch])
                for (auto [ai, c] : rows[r].terms)
                    heldRowsOf[ai].push_back({r, int(c)});
        std::vector<std::vector<Index>> patchesOf(na);
        for (Index p = 0; p < np; ++p)
            for (const Side& side : shapes[p].sides)
                for (const auto& [ai, fwd] : side.arcs)
                    if (std::find(patchesOf[ai].begin(), patchesOf[ai].end(), p) == patchesOf[ai].end())
                        patchesOf[ai].push_back(p);
        auto perimeter = [&](Index p) {
            int total = 0;
            for (const Side& side : shapes[p].sides)
                for (const auto& [ai, fwd] : side.arcs)
                    total += arcs[ai].count;
            return total;
        };
        auto stepCost = [&](Index ai, int d) {
            const double t = alpha * arcs[ai].length;
            return 1.0 + std::max(0.0, std::abs(arcs[ai].count + d - t) - std::abs(arcs[ai].count - t));
        };
        // The free patch across arc ai from a held one, or kInvalid on the border.
        auto freeAcross = [&](Index ai) {
            for (Index q : patchesOf[ai])
                if (hole[q])
                    return q;
            return kInvalid;
        };
        for (Index p0 = 0; p0 < np; ++p0)
        {
            if (!hole[p0] || perimeter(p0) % 2 == 0)
                continue;
            // Dijkstra over held rows; a state is a row and the change it needs to balance again.
            struct Back
            {
                Index row, arc;
                int delta;
            };
            std::unordered_map<Index, double> dist;
            std::unordered_map<Index, Back> back;
            std::unordered_map<Index, int> need;
            using Q = std::pair<double, Index>;
            std::priority_queue<Q, std::vector<Q>, std::greater<>> queue;
            double best = std::numeric_limits<double>::max();
            Index endArc = kInvalid, endFrom = kInvalid;
            int endDelta = 0;
            // An end: the arc's other side is the border or an odd free patch other than the start.
            auto consider = [&](Index ai, int delta, double d, Index from) {
                const Index q = freeAcross(ai);
                const bool border = patchesOf[ai].size() < 2;
                bool ok = border;
                for (Index other : patchesOf[ai])
                    ok = ok || (other != p0 && hole[other] && perimeter(other) % 2 != 0);
                (void)q;
                if (ok && d < best)
                {
                    best = d;
                    endArc = ai;
                    endDelta = delta;
                    endFrom = from;
                }
            };
            for (const Side& side : shapes[p0].sides)
                for (const auto& [ai, fwd] : side.arcs)
                    for (int delta : {1, -1})
                    {
                        if (arcs[ai].count + delta < lower[ai])
                            continue;
                        const double d = stepCost(ai, delta);
                        if (heldRowsOf[ai].empty())
                        {
                            consider(ai, delta, d, kInvalid);
                            continue;
                        }
                        if (heldRowsOf[ai].size() != 1)
                            continue;
                        const auto [r, c] = heldRowsOf[ai][0];
                        if (auto it = dist.find(r); it == dist.end() || d < it->second)
                        {
                            dist[r] = d;
                            need[r] = -c * delta;
                            back[r] = {kInvalid, ai, delta};
                            queue.push({d, r});
                        }
                    }
            while (!queue.empty())
            {
                const auto [d, r] = queue.top();
                queue.pop();
                if (d > dist[r] || d >= best)
                    continue;
                for (auto [ai, c] : rows[r].terms)
                {
                    const int delta = need[r] * int(c);
                    if (arcs[ai].count + delta < lower[ai] || ai == back[r].arc)
                        continue;
                    const double nd = d + stepCost(ai, delta);
                    Index other = kInvalid;
                    int oc = 0;
                    for (auto [r2, c2] : heldRowsOf[ai])
                        if (r2 != r)
                        {
                            other = r2;
                            oc = c2;
                        }
                    if (other == kInvalid)
                    {
                        consider(ai, delta, nd, r);
                        continue;
                    }
                    if (auto it = dist.find(other); it == dist.end() || nd < it->second)
                    {
                        dist[other] = nd;
                        need[other] = -oc * delta;
                        back[other] = {r, ai, delta};
                        queue.push({nd, other});
                    }
                }
            }
            if (endArc == kInvalid)
                continue;
            arcs[endArc].count += endDelta;
            for (Index r = endFrom; r != kInvalid;)
            {
                const Back& bk = back[r];
                arcs[bk.arc].count += bk.delta;
                r = bk.row;
            }
        }
    }
    if (debug)
    {
        int notFour = 0, uneven = 0, unmet = 0;
        for (Index p = 0; p < np; ++p)
        {
            notFour += shapes[p].sides.size() != 4;
            uneven += shapes[p].sides.size() == 4 && unequal[p];
        }
        for (Index r = 0; r < Index(rows.size()); ++r)
            unmet += holeRow[r];
        std::fprintf(stderr, "patch holes: %d not four-sided, %d with unequal sides, %d rows unmet, of %d patches\n", notFour, uneven, unmet, int(np));
    }

    // Fill.
    std::unordered_map<Index, Index> nodePoint;
    std::map<std::pair<Index, int>, Index> arcPoint;
    auto addPoint = [&](const glm::dvec3& p, FeatureKind kind, bool border) {
        const Index i = ex.mesh.addPoint(p);
        ex.feature.resize(size_t(i) + 1, FeatureKind::None);
        ex.boundary.resize(size_t(i) + 1, 0);
        ex.feature[i] = kind;
        ex.boundary[i] = border;
        return i;
    };
    std::unordered_map<Index, Index> cornerVertex; // welded point of a feature corner vertex
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.feature[v] == FeatureKind::Corner)
            cornerVertex[weld.id(s.pos[v])] = v;
    // Arc point k of n, counted from its a end.
    auto arcPointId = [&](Index ai, int k) {
        const Arc& arc = arcs[ai];
        if (k == 0 || k == arc.count)
        {
            const Index node = k == 0 ? arc.a : arc.b;
            auto [it, fresh] = nodePoint.try_emplace(node, kInvalid);
            if (fresh)
                it->second = addPoint(weld.pos(node), cornerVertex.count(node) ? FeatureKind::Corner : arc.wall ? FeatureKind::Line : FeatureKind::None, arc.border);
            else if (arc.wall && ex.feature[it->second] == FeatureKind::None)
                ex.feature[it->second] = FeatureKind::Line;
            if (arc.border)
                ex.boundary[it->second] = 1;
            return it->second;
        }
        auto [it, fresh] = arcPoint.try_emplace({ai, k}, kInvalid);
        if (fresh)
        {
            const double want = arc.length * double(k) / double(arc.count);
            const double total = arc.along.back();
            const double at = total > 0.0 ? want * total / arc.length : 0.0;
            size_t j = 1;
            while (j + 1 < arc.along.size() && arc.along[j] < at)
                ++j;
            const double span = arc.along[j] - arc.along[j - 1];
            const double t = span > 0.0 ? std::clamp((at - arc.along[j - 1]) / span, 0.0, 1.0) : 0.0;
            it->second = addPoint(glm::mix(weld.pos(arc.path[j - 1]), weld.pos(arc.path[j]), t), arc.wall ? FeatureKind::Line : FeatureKind::None, arc.border);
        }
        return it->second;
    };
    // The point at whole step c along a side (from its first corner).
    auto sidePoint = [&](const Side& side, int c) {
        for (const auto& [ai, fwd] : side.arcs)
        {
            const int n = arcs[ai].count;
            if (c <= n)
                return arcPointId(ai, fwd ? c : n - c);
            c -= n;
        }
        const auto& [ai, fwd] = side.arcs.back();
        return arcPointId(ai, fwd ? arcs[ai].count : 0);
    };
    auto sideCount = [&](const Side& side) {
        int n = 0;
        for (const auto& [ai, fwd] : side.arcs)
            n += arcs[ai].count;
        return n;
    };

    std::vector<std::array<Index, 4>> quads;
    std::vector<Index> quadPatch; // per quad, for the hole listing
    std::vector<Index> same; // union-find over output points
    auto rootOf = [&](Index x) {
        while (same[x] != x)
            x = same[x] = same[same[x]];
        return x;
    };
    auto unite = [&](Index a, Index b) {
        same.resize(ex.mesh.pointCapacity(), kInvalid);
        for (Index i = 0; i < Index(same.size()); ++i)
            if (same[i] == kInvalid)
                same[i] = i;
        if (debug && std::getenv("LILA_PATCH_LIST") && glm::length(ex.mesh.position(a) - ex.mesh.position(b)) > 0.5)
            std::fprintf(stderr, "weld across %.2f at %.2f %.2f %.2f\n", glm::length(ex.mesh.position(a) - ex.mesh.position(b)), ex.mesh.position(a).x,
                         ex.mesh.position(a).y, ex.mesh.position(a).z);
        same[rootOf(a)] = rootOf(b);
    };
    std::vector<std::vector<Index>> piecesOf(np);
    for (Index pi = 0; pi < npieces; ++pi)
        piecesOf[patchOf[pi]].push_back(pi);
    int tutteFailed = 0, foldedPatches = 0;
    const bool listHoles = debug && std::getenv("LILA_PATCH_LIST");
    auto reportHole = [&](Index p, const char* why) {
        if (!listHoles)
            return;
        glm::dvec3 c(0.0);
        int k = 0;
        for (Index pi : piecesOf[p])
            for (Index w : pid[pi])
            {
                c += weld.pos(w);
                ++k;
            }
        c /= double(std::max(k, 1));
        std::fprintf(stderr, "hole patch %d (%s): %zu sides, counts", int(p), why, shapes[p].sides.size());
        for (const Side& sd : shapes[p].sides)
        {
            int n = 0;
            for (const auto& [ai, fwd] : sd.arcs)
                n += arcs[ai].count;
            std::fprintf(stderr, " %d", n);
        }
        std::fprintf(stderr, ", unequal %d, at %.2f %.2f %.2f\n", int(unequal[p]), c.x, c.y, c.z);
    };
    int patterned = 0, unfilled[5] = {}; // a side without steps, odd, no pattern fits, fill failed, too many sides
    // A free patch takes as many padding loops as bring its quads nearest the count its sides ask for, or its middle
    // comes out in a few big faces.
    auto fillToDensity = [&](const PatchBoundary& b, const PatchShape& shape) {
        std::vector<int> c;
        for (const Side& sd : shape.sides)
            c.push_back(sideCount(sd));
        double want = 0.0;
        if (c.size() == 4)
            want = 0.25 * double(c[0] + c[2]) * double(c[1] + c[3]);
        else
        {
            const double per = double(std::accumulate(c.begin(), c.end(), 0));
            want = per * per / 16.0;
        }
        PatchResult best = fillPatch(b);
        const int most = *std::min_element(c.begin(), c.end()) / 2 + 2;
        for (int pad = 0; pad <= most; ++pad)
        {
            PatchOptions o;
            o.padding = pad;
            PatchResult r = fillPatch(b, o);
            if (r.ok() && (!best.ok() || std::abs(double(r.faces.size()) - want) < std::abs(double(best.faces.size()) - want)))
                best = std::move(r);
        }
        return best;
    };
    double probe[4] = {0.0, 0.0, 0.0, -1.0};
    if (const char* at = std::getenv("LILA_PATCH_AT"))
        std::sscanf(at, "%lf,%lf,%lf,%lf", &probe[0], &probe[1], &probe[2], &probe[3]);
    for (Index p = 0; p < np; ++p)
    {
        if (probe[3] > 0.0)
        {
            glm::dvec3 c(0.0);
            int k = 0;
            for (Index pi : piecesOf[p])
                for (Index w : pid[pi])
                {
                    c += weld.pos(w);
                    ++k;
                }
            c /= double(std::max(k, 1));
            if (glm::length(c - glm::dvec3(probe[0], probe[1], probe[2])) < probe[3])
            {
                std::fprintf(stderr, "probe patch %d hole %d unequal %d pieces %zu at %.2f %.2f %.2f sides", int(p), int(hole[p]), int(unequal[p]), piecesOf[p].size(), c.x, c.y, c.z);
                for (const Side& sd : shapes[p].sides)
                {
                    std::fprintf(stderr, " [");
                    for (const auto& [ai, fwd] : sd.arcs)
                        std::fprintf(stderr, " a%d:%d(%d>%d%s)", int(ai), arcs[ai].count, int(arcs[ai].a), int(arcs[ai].b), fwd ? "" : " rev");
                    std::fprintf(stderr, " ]");
                }
                std::fprintf(stderr, "\n");
            }
        }
        if (hole[p])
        {
            // Free: its border points in order, corners where the sides start, filled by pattern.
            const PatchShape& shape = shapes[p];
            if (shape.sides.size() < 2 || shape.sides.size() > 6)
            {
                ++unfilled[4];
                reportHole(p, "many sides");
                continue;
            }
            PatchBoundary b;
            std::vector<Index> ids;
            bool ok = true;
            // A side that collapsed to no steps joins its two corners: the patch has a side fewer.
            for (const Side& side : shape.sides)
            {
                const int count = sideCount(side);
                if (count == 0)
                    continue;
                b.corners.push_back(int(ids.size()));
                for (int c = 0; c < count; ++c)
                    ids.push_back(sidePoint(side, c));
            }
            ok = b.corners.size() >= 3;
            // Four sides, one collapsed, leaving a triangle whose longest side is as long as the other two together, by
            // count and on the lattice: a strip with a corner partway along one side. Filled as a triangle its two poles land
            // on one point, so it is filled as a rectangle with its fourth corner on the long side.
            if (b.corners.size() == 3 && shape.sides.size() == 4)
            {
                int cnt[3];
                double lat[3] = {};
                for (int k = 0; k < 3; ++k)
                    cnt[k] = (k + 1 < 3 ? b.corners[k + 1] : int(ids.size())) - b.corners[k];
                for (int k = 0; const Side& side : shape.sides)
                    if (sideCount(side) != 0 && k < 3)
                        lat[k++] = latticeLength(side);
                for (int m = 0; m < 3; ++m)
                {
                    const int i = (m + 1) % 3, j = (m + 2) % 3;
                    if (cnt[i] > 0 && cnt[j] > 0 && cnt[m] == cnt[i] + cnt[j] && lat[m] >= 0.93 * (lat[i] + lat[j]) && lat[m] <= 1.02 * (lat[i] + lat[j]))
                    {
                        b.corners.push_back(b.corners[m] + cnt[i]);
                        std::sort(b.corners.begin(), b.corners.end());
                        break;
                    }
                }
            }
            for (Index id : ids)
                b.points.push_back(ex.mesh.position(id));
            if (!ok)
            {
                ++unfilled[0];
                reportHole(p, "empty side");
                continue;
            }
            if (ids.size() % 2 != 0)
            {
                ++unfilled[1];
                reportHole(p, "odd");
                continue;
            }
            if (!patchSidesFillable(patchSideCounts(b)))
            {
                ++unfilled[2];
                reportHole(p, "no pattern");
                continue;
            }
            const PatchResult r = fillToDensity(b, shape);
            if (!r.ok())
            {
                ++unfilled[3];
                reportHole(p, "fill failed");
                continue;
            }
            std::vector<Index> id(r.positions.size());
            for (size_t i = 0; i < r.positions.size(); ++i)
                id[i] = i < ids.size() ? ids[i] : addPoint(r.positions[i], FeatureKind::None, false);
            for (const auto& f : r.faces)
                if (f.size() == 4)
                {
                    quads.push_back({id[f[0]], id[f[1]], id[f[2]], id[f[3]]});
                    quadPatch.push_back(p);
                }
            ++patterned;
            if (listHoles)
            {
                std::fprintf(stderr, "pattern patch %d: %zu poles,", int(p), r.poles.size());
                for (Index pl : r.poles)
                    std::fprintf(stderr, " (%.2f %.2f %.2f)", r.positions[pl].x, r.positions[pl].y, r.positions[pl].z);
                reportHole(p, "pattern");
            }
            continue;
        }
        const PatchShape& shape = shapes[p];
        const size_t n = shape.sides.size();
        if (n != 4)
            reportHole(p, "not four-sided, not free");
        // The patch's triangles: each piece fanned from its centre.
        std::unordered_map<Index, Index> local;
        std::vector<glm::dvec3> pos;
        auto localOf = [&](Index w) {
            auto [it, fresh] = local.try_emplace(w, Index(pos.size()));
            if (fresh)
                pos.push_back(weld.pos(w));
            return it->second;
        };
        std::vector<std::array<Index, 3>> tris;
        std::vector<double> triSize; // the size field's edge length over each fan triangle's surface triangle
        for (Index pi : piecesOf[p])
        {
            const std::vector<Index>& poly = polyPid[pi];
            const Index st = pieces[pi].tri;
            const double chartLen = glm::length(lay.chart[st][1] - lay.chart[st][0]);
            const double size = chartLen > 0.0 ? glm::length(s.pos[s.tris[st][1]] - s.pos[s.tris[st][0]]) / chartLen : 1.0;
            glm::dvec3 c(0.0);
            for (Index w : poly)
                c += weld.pos(w) / double(poly.size());
            const Index centre = Index(pos.size());
            pos.push_back(c);
            for (size_t k = 0; k < poly.size(); ++k)
                if (poly[k] != poly[(k + 1) % poly.size()])
                {
                    tris.push_back({centre, localOf(poly[k]), localOf(poly[(k + 1) % poly.size()])});
                    triSize.push_back(size);
                }
        }
        // Border points on the domain polygon: corners on a regular polygon (a rectangle W x H when four-sided), the
        // rest placed along their side by whole steps.
        std::vector<int> counts(n);
        for (size_t i = 0; i < n; ++i)
            counts[i] = sideCount(shape.sides[i]);
        std::vector<glm::dvec2> corner(n);
        if (n == 4)
            corner = {{0.0, 0.0}, {double(counts[0]), 0.0}, {double(counts[0]), double(counts[1])}, {0.0, double(counts[1])}};
        else
        {
            double perimeter = 0.0;
            for (int c : counts)
                perimeter += c;
            const double r = perimeter / (2.0 * n * std::sin(glm::pi<double>() / n));
            for (size_t i = 0; i < n; ++i)
                corner[i] = r * glm::dvec2(std::cos(glm::two_pi<double>() * i / n), std::sin(glm::two_pi<double>() * i / n));
        }
        std::vector<glm::dvec2> uv(pos.size(), glm::dvec2(0.0));
        std::vector<uint8_t> onBorder(pos.size(), 0);
        {
            // Along each side, each point sits at its arc's whole steps so far plus its share of the arc it is on, by its
            // own lattice length between the arc's nodes.
            const std::vector<Index>& loop = loopOf[p];
            const std::vector<size_t> cs = cornersOf(p);
            std::vector<uint8_t> isCorner(loop.size(), 0);
            for (size_t c : cs)
                isCorner[c] = 1;
            size_t side = 0, arcInSide = 0;
            int before = 0;
            std::vector<std::pair<Index, double>> run; // points of the current arc and their length from its start
            auto flush = [&]() {
                const Side& sd = shape.sides[side];
                const int count = arcInSide < sd.arcs.size() ? arcs[sd.arcs[arcInSide].first].count : 0;
                const double total = run.back().second;
                for (const auto& [li, len] : run)
                {
                    const double u = before + count * (total > 0.0 ? len / total : 0.0);
                    uv[li] = glm::mix(corner[side], corner[(side + 1) % n], counts[side] > 0 ? u / counts[side] : 0.0);
                    onBorder[li] = 1;
                }
                before += count;
                ++arcInSide;
            };
            run.push_back({localOf(edges[loop[cs[0]]].from), 0.0});
            for (size_t step = 0; step < loop.size(); ++step)
            {
                const size_t i = (cs[0] + step) % loop.size(), next = (i + 1) % loop.size();
                const Edge& e = edges[loop[i]];
                run.push_back({localOf(e.to), run.back().second + e.length});
                if (!isNode[e.to] && !isCorner[next])
                    continue;
                flush();
                run = {{localOf(e.to), 0.0}};
                if (isCorner[next])
                {
                    ++side;
                    arcInSide = 0;
                    before = 0;
                    if (side == n)
                        break;
                }
            }
        }
        // Tutte: each inner point at the weighted mean of its neighbours, cotangent weights clamped positive. Each weight
        // also scales by the size field there, so grid lines crowd where faces should be small.
        std::vector<Index> inner(pos.size(), kInvalid);
        Index ni = 0;
        for (size_t i = 0; i < pos.size(); ++i)
            if (!onBorder[i])
                inner[i] = ni++;
        if (ni > 0)
        {
            std::map<std::pair<Index, Index>, double> w;
            for (size_t ti = 0; ti < tris.size(); ++ti)
                for (int k = 0; k < 3; ++k)
                {
                    const auto& t = tris[ti];
                    const Index a = t[k], b = t[(k + 1) % 3], o = t[(k + 2) % 3];
                    const glm::dvec3 ea = pos[a] - pos[o], eb = pos[b] - pos[o];
                    const double cot = glm::dot(ea, eb) / std::max(glm::length(glm::cross(ea, eb)), 1e-300);
                    w[{std::min(a, b), std::max(a, b)}] += 0.5 * cot * triSize[ti];
                }
            std::vector<Eigen::Triplet<double>> trip;
            Eigen::MatrixXd rhs = Eigen::MatrixXd::Zero(ni, 2);
            for (auto [key, sum] : w)
            {
                const double wij = std::max(sum, 1e-3);
                const auto [a, b] = key;
                for (auto [i, j] : {std::pair{a, b}, std::pair{b, a}})
                {
                    if (inner[i] == kInvalid)
                        continue;
                    trip.emplace_back(inner[i], inner[i], wij);
                    if (inner[j] != kInvalid)
                        trip.emplace_back(inner[i], inner[j], -wij);
                    else
                    {
                        rhs(inner[i], 0) += wij * uv[j].x;
                        rhs(inner[i], 1) += wij * uv[j].y;
                    }
                }
            }
            // A point cut off from the border (a sliver's corner) would make it singular; a faint pull to the centre keeps it solvable.
            for (Index i = 0; i < ni; ++i)
                trip.emplace_back(i, i, 1e-9);
            Eigen::SparseMatrix<double> L(ni, ni);
            L.setFromTriplets(trip.begin(), trip.end());
            Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> tutte(L);
            if (tutte.info() != Eigen::Success)
            {
                why = "a patch's embedding didn't solve";
                return {};
            }
            const Eigen::MatrixXd sol = tutte.solve(rhs);
            for (size_t i = 0; i < pos.size(); ++i)
                if (inner[i] != kInvalid)
                    uv[i] = glm::dvec2(sol(inner[i], 0), sol(inner[i], 1));
        }
        // A domain point back on the surface: the embedded triangle holding it.
        auto lift = [&](const glm::dvec2& d) {
            double bestOut = std::numeric_limits<double>::max();
            glm::dvec3 best(0.0);
            for (const auto& t : tris)
            {
                const glm::dvec2 a = uv[t[0]], b = uv[t[1]], c = uv[t[2]];
                const double area = cross2(b - a, c - a);
                if (std::abs(area) < 1e-18)
                    continue;
                const glm::dvec3 l(cross2(b - d, c - d) / area, cross2(c - d, a - d) / area, cross2(a - d, b - d) / area);
                const double out = std::max(0.0, -std::min({l.x, l.y, l.z}));
                if (out < bestOut)
                {
                    bestOut = out;
                    const glm::dvec3 lc = glm::max(l, glm::dvec3(0.0)) / std::max(1e-300, glm::max(l, glm::dvec3(0.0)).x + glm::max(l, glm::dvec3(0.0)).y + glm::max(l, glm::dvec3(0.0)).z);
                    best = lc.x * pos[t[0]] + lc.y * pos[t[1]] + lc.z * pos[t[2]];
                    if (out == 0.0)
                        break;
                }
            }
            tutteFailed += bestOut > 1e-6;
            return best;
        };
        // The surface's normal under a domain point.
        auto normalAt = [&](const glm::dvec2& d) {
            double bestOut = std::numeric_limits<double>::max();
            glm::dvec3 best(0.0);
            for (const auto& t : tris)
            {
                const glm::dvec2 a = uv[t[0]], b = uv[t[1]], c = uv[t[2]];
                const double area = cross2(b - a, c - a);
                if (std::abs(area) < 1e-18)
                    continue;
                const double out = std::max(0.0, -std::min({cross2(b - d, c - d) / area, cross2(c - d, a - d) / area, cross2(a - d, b - d) / area}));
                if (out < bestOut)
                {
                    bestOut = out;
                    best = glm::cross(pos[t[1]] - pos[t[0]], pos[t[2]] - pos[t[0]]);
                    if (out == 0.0)
                        break;
                }
            }
            return best;
        };
        std::map<std::pair<int64_t, int64_t>, Index> innerPoint;
        auto domainPoint = [&](const glm::dvec2& d) {
            const std::pair<int64_t, int64_t> key{std::llround(d.x * 1e6), std::llround(d.y * 1e6)};
            auto [it, fresh] = innerPoint.try_emplace(key, kInvalid);
            if (fresh)
                it->second = addPoint(lift(d), FeatureKind::None, false);
            return it->second;
        };
        auto quad = [&](Index a, Index b, Index c, Index d) { quads.push_back({a, b, c, d}); };
        if (n == 4)
        {
            const int W = counts[0], H = counts[1];
            // Collapsed one way, the patch is a line: its long sides are the same points.
            if (W == 0 || H == 0)
            {
                if (H == 0)
                    for (int i = 0; i <= W; ++i)
                        unite(sidePoint(shape.sides[0], i), sidePoint(shape.sides[2], W - i));
                if (W == 0)
                    for (int j = 0; j <= H; ++j)
                        unite(sidePoint(shape.sides[1], j), sidePoint(shape.sides[3], H - j));
                continue;
            }
            auto at = [&](int i, int j) -> Index {
                if (j == 0)
                    return sidePoint(shape.sides[0], i);
                if (i == W)
                    return sidePoint(shape.sides[1], j);
                if (j == H)
                    return sidePoint(shape.sides[2], W - i);
                if (i == 0)
                    return sidePoint(shape.sides[3], H - j);
                return domainPoint(glm::dvec2(i, j));
            };
            // A patch whose grid folds over the surface (too few steps for its real size, corners in the wrong place) is
            // left for the hole filler.
            std::vector<std::array<Index, 4>> grid;
            bool folded = false;
            for (int j = 0; j < H; ++j)
                for (int i = 0; i < W; ++i)
                {
                    const std::array<Index, 4> q = {at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1)};
                    const glm::dvec3 p0 = ex.mesh.position(q[0]), p1 = ex.mesh.position(q[1]), p2 = ex.mesh.position(q[2]), p3 = ex.mesh.position(q[3]);
                    const glm::dvec3 facing = glm::cross(p2 - p0, p3 - p1);
                    folded = folded || glm::dot(facing, normalAt(glm::dvec2(i + 0.5, j + 0.5))) <= 0.0;
                    grid.push_back(q);
                }
            // A grid that lands folded (too few steps for the patch's real size) is kept: the relax after untangles it,
            // where a hole would take a fan.
            if (folded)
            {
                ++foldedPatches;
                reportHole(p, "folded");
            }
            quads.insert(quads.end(), grid.begin(), grid.end());
            quadPatch.resize(quads.size(), p);
        }
    }
    // Collapsed arcs and patches weld their points; the mesh is built from the welded quads.
    // Over the nodes first: a chain of collapsed arcs welds its ends even where no patch made a point at a node between.
    {
        std::unordered_map<Index, Index> up;
        auto nodeRoot = [&](Index x) {
            for (;;)
            {
                auto it = up.find(x);
                if (it == up.end() || it->second == x)
                    return x;
                x = it->second;
            }
        };
        for (const Arc& arc : arcs)
            if (arc.count == 0)
            {
                const Index ra = nodeRoot(arc.a), rb = nodeRoot(arc.b);
                if (ra != rb)
                    up[ra] = rb;
            }
        std::unordered_map<Index, Index> pointOfClass;
        for (const auto& [node, point] : nodePoint)
            if (auto [it, fresh] = pointOfClass.try_emplace(nodeRoot(node), point); !fresh)
                unite(point, it->second);
    }
    {
        same.resize(ex.mesh.pointCapacity(), kInvalid);
        for (Index i = 0; i < Index(same.size()); ++i)
            if (same[i] == kInvalid)
                same[i] = i;
        Extracted out;
        std::vector<Index> newOf(same.size(), kInvalid);
        std::vector<uint8_t> used(same.size(), 0);
        for (const auto& qd : quads)
            for (Index v : qd)
                used[rootOf(v)] = 1;
        // A welded group sits at its mean.
        std::vector<glm::dvec3> sum(same.size(), glm::dvec3(0.0));
        std::vector<int> members(same.size(), 0);
        for (Index i = 0; i < Index(same.size()); ++i)
        {
            sum[rootOf(i)] += ex.mesh.position(i);
            ++members[rootOf(i)];
        }
        for (Index i = 0; i < Index(same.size()); ++i)
            if (members[i] > 1)
                ex.mesh.setPosition(i, sum[i] / double(members[i]));
        for (Index i = 0; i < Index(same.size()); ++i)
        {
            const Index r = rootOf(i);
            if (!used[r])
                continue;
            if (newOf[r] == kInvalid)
            {
                newOf[r] = out.mesh.addPoint(ex.mesh.position(r));
                out.feature.push_back(FeatureKind::None);
                out.boundary.push_back(0);
            }
            const Index j = newOf[r];
            if (ex.feature[i] == FeatureKind::Corner || (ex.feature[i] == FeatureKind::Line && out.feature[j] == FeatureKind::None))
                out.feature[j] = ex.feature[i];
            out.boundary[j] = out.boundary[j] || ex.boundary[i];
        }
        std::vector<Index> facePatch;
        for (size_t qi = 0; qi < quads.size(); ++qi)
        {
            const auto& qd = quads[qi];
            std::vector<Index> poly;
            for (Index v : qd)
            {
                const Index w = newOf[rootOf(v)];
                if (poly.empty() || poly.back() != w)
                    poly.push_back(w);
            }
            while (poly.size() > 1 && poly.front() == poly.back())
                poly.pop_back();
            if (listHoles && poly.size() < 4)
            {
                const glm::dvec3 c = (ex.mesh.position(qd[0]) + ex.mesh.position(qd[2])) * 0.5;
                std::fprintf(stderr, "quad welded to %zu points at %.2f %.2f %.2f, patch %d\n", poly.size(), c.x, c.y, c.z, int(quadPatch[qi]));
            }
            if (poly.size() >= 3)
                if (const Index f = out.mesh.addFace(poly); f != kInvalid)
                {
                    facePatch.resize(size_t(f) + 1, kInvalid);
                    facePatch[f] = quadPatch[qi];
                }
        }
        if (debug)
        {
            std::fprintf(stderr, "patch weld: %zu quads, %d faces kept\n", quads.size(), int(out.mesh.numFaces()));
            std::vector<int> made(np, 0);
            for (Index pq : quadPatch)
                ++made[pq];
            std::vector<std::pair<double, Index>> gap;
            for (Index p = 0; p < np; ++p)
            {
                double est = 0.0;
                if (shapes[p].sides.size() == 4)
                    est = double(sideCount(shapes[p].sides[0])) * sideCount(shapes[p].sides[1]);
                else
                {
                    double per = 0.0;
                    for (const Side& sd : shapes[p].sides)
                        per += sideCount(sd);
                    est = per * per / 16.0;
                }
                gap.push_back({est - made[p], p});
            }
            std::sort(gap.rbegin(), gap.rend());
            for (size_t i = 0; i < std::min<size_t>(8, gap.size()); ++i)
                std::fprintf(stderr, "patch %d: %.0f cells estimated, %d made, %zu sides, hole %d\n", int(gap[i].second), gap[i].first + made[gap[i].second],
                             made[gap[i].second], shapes[gap[i].second].sides.size(), int(hole[gap[i].second]));
            for (size_t i = 0; i < std::min<size_t>(3, gap.size()); ++i)
            {
                std::fprintf(stderr, "  sides");
                for (const Side& sd : shapes[gap[i].second].sides)
                    std::fprintf(stderr, " %d", sideCount(sd));
                std::fprintf(stderr, "\n");
            }
        }
        if (listHoles)
            for (Index e = 0; e < out.mesh.edgeCapacity(); ++e)
                if (out.mesh.edgeValid(e) && out.mesh.edgeFaceCount(e) == 1)
                {
                    const Index c = out.mesh.edgeCorner(e), a = out.mesh.cornerPoint(c), b = out.mesh.cornerNextPoint(c);
                    if (out.boundary[a] && out.boundary[b])
                        continue;
                    const glm::dvec3 m = 0.5 * (out.mesh.position(a) + out.mesh.position(b));
                    std::fprintf(stderr, "open edge at %.2f %.2f %.2f, patch %d\n", m.x, m.y, m.z, int(facePatch[out.mesh.cornerFace(c)]));
                }
        ex = std::move(out);
    }
    if (debug)
        std::fprintf(stderr, "patch mesh: %d points, %d faces, %d points lifted from outside their patch, %d patches folded, %d filled by pattern (unfilled: %d empty side, %d odd, %d no pattern, %d failed, %d many sides)\n",
                     int(ex.mesh.numPoints()), int(ex.mesh.numFaces()), tutteFailed, foldedPatches, patterned, unfilled[0], unfilled[1], unfilled[2], unfilled[3],
                     unfilled[4]);
    return ex;
}

/**
 * Slivers: patches thinner than half a row along most of their length, between two lines that run side by side, or a
 * line beside a wall (a crease, the border). Of each pair of lines the one whose length is mostly in slivers is left out
 * (marked in `dropped`), so the lines it stopped run on and the other carries the row; beside a wall the line goes.
 * `partnerOf` keeps, per line left out, the line it ran beside, across rounds. Returns how many lines it left out.
 */
size_t dropSlivers(const Layout& lay, const Traces& tr, const std::vector<Piece>& pieces, const std::vector<Index>& patchOf, Index np, std::vector<uint8_t>& dropped,
                   std::vector<Index>& partnerOf)
{
    constexpr double kThin = 0.4;
    const Index wallLine = Index(tr.end.size()); // every wall counts as one line, which can't be left out
    std::vector<double> area(np, 0.0);
    std::vector<std::map<Index, double>> along(np);
    for (Index pi = 0; pi < Index(pieces.size()); ++pi)
    {
        const Piece& P = pieces[pi];
        const size_t n = P.poly.size();
        double a = 0.0;
        for (size_t k = 0; k < n; ++k)
        {
            const glm::dvec2 &u = P.poly[k], &v = P.poly[(k + 1) % n];
            a += cross2(u, v);
            if (P.side[k] >= kSegSide)
                along[patchOf[pi]][tr.segments[P.side[k] - kSegSide].tracer] += glm::length(v - u);
            else if (const Index e = lay.triEdge[P.tri][P.side[k]]; e != kInvalid && lay.wall[e])
                along[patchOf[pi]][wallLine] += glm::length(v - u);
        }
        area[patchOf[pi]] += 0.5 * std::abs(a);
    }
    std::vector<double> length(tr.end.size() + 1, 0.0), inSlivers(tr.end.size() + 1, 0.0);
    for (const Segment& g : tr.segments)
        length[g.tracer] += glm::length(g.b - g.a);
    std::vector<std::pair<Index, Index>> pairs;
    for (Index p = 0; p < np; ++p)
    {
        std::pair<Index, double> first{kInvalid, 0.0}, second{kInvalid, 0.0};
        for (const auto& [t, l] : along[p])
            if (l > first.second)
            {
                second = first;
                first = {t, l};
            }
            else if (l > second.second)
                second = {t, l};
        if (second.first == kInvalid || second.second < 1.0 || area[p] > kThin * 0.5 * (first.second + second.second))
            continue;
        inSlivers[first.first] += second.second;
        inSlivers[second.first] += second.second;
        pairs.push_back({first.first, second.first});
    }
    dropped.resize(tr.end.size(), 0);
    // A source's lines in order round it (a pole's are consecutive tracers). Two neighbours gone would leave a corner
    // over 180 degrees or a loose end, so a line only goes while both its neighbours stay.
    std::map<Index, std::vector<Index>> linesOf;
    for (Index t = 0; t < Index(tr.sourceOf.size()); ++t)
        if (tr.sourceOf[t] != kInvalid)
            linesOf[tr.sourceOf[t]].push_back(t);
    auto blockedDrop = [&](Index t) {
        const std::vector<Index>& lines = linesOf[tr.sourceOf[t]];
        const size_t n = lines.size(), i = size_t(std::find(lines.begin(), lines.end(), t) - lines.begin());
        if (lay.creaseEnd[tr.sourceOf[t]] != glm::dvec3(0.0))
            return false;
        if (n < 3)
            return true;
        if (!lay.singular[tr.sourceOf[t]])
            return std::any_of(lines.begin(), lines.end(), [&](Index l) { return dropped[l] != 0; });
        return bool(dropped[lines[(i + 1) % n]] || dropped[lines[(i + n - 1) % n]]);
    };
    std::sort(pairs.begin(), pairs.end(), [&](const auto& x, const auto& y) { return inSlivers[x.first] + inSlivers[x.second] > inSlivers[y.first] + inSlivers[y.second]; });
    partnerOf.resize(tr.end.size(), kInvalid);
    // Both lines' poles have given up a line already. A valence 3 pole takes its earlier line back where the line that
    // ran beside that one can go instead, which frees it to give up this one: four poles round a square, whose edges are
    // each traced from both ends, need every pole to give up one, which the greedy order alone can miss.
    auto swapFor = [&](Index a, Index b) {
        for (Index line : {a, b})
        {
            const Index src = tr.sourceOf[line];
            if (src == kInvalid || linesOf[src].size() != 3)
                continue;
            for (Index x : linesOf[src])
            {
                const Index y = x != line && dropped[x] ? partnerOf[x] : kInvalid;
                if (y == kInvalid || y >= wallLine || dropped[y] || tr.sourceOf[y] == kInvalid || tr.sourceOf[y] == src || linesOf[tr.sourceOf[y]].size() != 3)
                    continue;
                dropped[x] = 0;
                if (!blockedDrop(y))
                {
                    dropped[y] = 1;
                    if (!blockedDrop(line))
                    {
                        dropped[line] = 1;
                        partnerOf[y] = x;
                        partnerOf[x] = kInvalid;
                        partnerOf[line] = line == a ? b : a;
                        if (std::getenv("LILA_PATCH_LIST"))
                            std::fprintf(stderr, "sliver swap: line %d back, %d and %d left out, pole at %.2f %.2f %.2f, lengths %.2f %.2f %.2f %.2f\n", int(x), int(y), int(line),
                                         lay.s.pos[src].x, lay.s.pos[src].y, lay.s.pos[src].z, length[a], length[b], length[x], length[y]);
                        return true;
                    }
                    dropped[y] = 0;
                }
                dropped[x] = 1;
            }
        }
        return false;
    };
    size_t count = 0;
    for (auto [a, b] : pairs)
    {
        if ((a != wallLine && dropped[a]) || (b != wallLine && dropped[b]))
            continue;
        auto weight = [&](Index t) {
            return t == wallLine || tr.sourceOf[t] == kInvalid || blockedDrop(t) ? -1.0 : inSlivers[t] / std::max(length[t], 1e-9);
        };
        Index drop = weight(a) >= weight(b) ? a : b;
        if (weight(drop) < 0.0 && a != wallLine && b != wallLine && swapFor(a, b))
        {
            ++count;
            continue;
        }
        if (weight(drop) < 0.0)
        {
            if (std::getenv("LILA_PATCH_LIST"))
                for (Index t : {a, b})
                    if (t != wallLine)
                    {
                        const Index src = tr.sourceOf[t];
                        std::fprintf(stderr, "sliver kept: line %d of %d (wall pair %d), source %d", int(t), int(t == a ? b : a), int(a == wallLine || b == wallLine),
                                     int(src));
                        if (src != kInvalid)
                            std::fprintf(stderr, " at %.2f %.2f %.2f singular %d lines %zu", lay.s.pos[src].x, lay.s.pos[src].y, lay.s.pos[src].z,
                                         int(lay.singular[src]), linesOf[src].size());
                        std::fprintf(stderr, "\n");
                    }
            continue;
        }
        dropped[drop] = 1;
        partnerOf[drop] = drop == a ? b : a;
        ++count;
    }
    return count;
}

/** Points within `tol` of each other, chained, share an index (union over a grid of tol-sized cells). */
std::vector<Index> clusterPoints(const std::vector<glm::dvec3>& pts, double tol)
{
    std::vector<Index> up(pts.size());
    std::iota(up.begin(), up.end(), Index(0));
    auto root = [&](Index x) {
        while (up[x] != x)
            x = up[x] = up[up[x]];
        return x;
    };
    using Cell = std::tuple<int64_t, int64_t, int64_t>;
    auto cellOf = [&](const glm::dvec3& p) { return Cell{int64_t(std::floor(p.x / tol)), int64_t(std::floor(p.y / tol)), int64_t(std::floor(p.z / tol))}; };
    std::map<Cell, std::vector<Index>> grid;
    for (Index i = 0; i < Index(pts.size()); ++i)
        grid[cellOf(pts[i])].push_back(i);
    for (const auto& [cell, members] : grid)
    {
        const auto [cx, cy, cz] = cell;
        for (int64_t dx = 0; dx <= 1; ++dx)
            for (int64_t dy = -1; dy <= 1; ++dy)
                for (int64_t dz = -1; dz <= 1; ++dz)
                {
                    // Each pair of neighbouring cells once.
                    if (dx == 0 && (dy < 0 || (dy == 0 && dz < 0)))
                        continue;
                    const auto it = grid.find({cx + dx, cy + dy, cz + dz});
                    if (it == grid.end())
                        continue;
                    for (Index a : members)
                        for (Index b : it->second)
                            if (a != b && glm::length(pts[a] - pts[b]) <= tol)
                                up[root(a)] = root(b);
                }
    }
    std::vector<Index> out(pts.size());
    for (Index i = 0; i < Index(pts.size()); ++i)
        out[i] = root(i);
    return out;
}

struct PatchStats
{
    int tracers = 0, segments = 0, mismatched = 0, pieces = 0, patches = 0, rectangles = 0, reflex = 0, corners[9] = {};
};
} // namespace

Extracted layoutPatches(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale, double targetCells, std::string* error,
                        const std::vector<glm::dvec3>& crosses)
{
    LILA_ZONE();
    auto fail = [&](const std::string& why) {
        if (error)
            *error = why;
        return Extracted{};
    };
    const bool debug = std::getenv("LILA_PATCH_DEBUG") != nullptr;
    const Layout lay = makeLayout(s, q, scale);
    if (const char* at = std::getenv("LILA_PATCH_AT"))
    {
        glm::dvec3 c(0.0);
        double r = 0.0;
        std::sscanf(at, "%lf,%lf,%lf,%lf", &c.x, &c.y, &c.z, &r);
        for (Index v = 0; v < s.vertexCount(); ++v)
            if (lay.source[v] && glm::length(s.pos[v] - c) < r)
                std::fprintf(stderr, "probe source %d at %.3f %.3f %.3f: quarters %d, feature %d, scale %.3f\n", int(v), s.pos[v].x, s.pos[v].y, s.pos[v].z,
                             lay.quarters[v], int(s.feature[v]), scale[v]);
    }
    auto at3d = [&](const Piece& P, const glm::dvec2& p) {
        const Chart& c = lay.chart[P.tri];
        const double area = cross2(c[1] - c[0], c[2] - c[0]);
        const glm::dvec3 w(cross2(c[1] - p, c[2] - p) / area, cross2(c[2] - p, c[0] - p) / area, cross2(c[0] - p, c[1] - p) / area);
        return w.x * s.pos[s.tris[P.tri][0]] + w.y * s.pos[s.tris[P.tri][1]] + w.z * s.pos[s.tris[P.tri][2]];
    };
    const double unit = s.meanEdgeLength() * 1e-6;
    std::vector<ExtraStart> crossStarts;
    for (const glm::dvec3& c : crosses)
    {
        // The nearest triangle, by its centroid, and the point in it, by barycentrics clamped inside.
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (Index t = 0; t < Index(s.tris.size()); ++t)
        {
            const double d = glm::length((s.pos[s.tris[t][0]] + s.pos[s.tris[t][1]] + s.pos[s.tris[t][2]]) / 3.0 - c);
            if (d < bestD)
            {
                bestD = d;
                best = t;
            }
        }
        if (best == kInvalid || bestD > 4.0 * s.meanEdgeLength())
            continue;
        const glm::dvec3 a = s.pos[s.tris[best][0]], e1 = s.pos[s.tris[best][1]] - a, e2 = s.pos[s.tris[best][2]] - a, d = c - a;
        const double d11 = glm::dot(e1, e1), d12 = glm::dot(e1, e2), d22 = glm::dot(e2, e2), den = d11 * d22 - d12 * d12;
        double v = den > 0.0 ? (d22 * glm::dot(d, e1) - d12 * glm::dot(d, e2)) / den : 1.0 / 3.0;
        double w = den > 0.0 ? (d11 * glm::dot(d, e2) - d12 * glm::dot(d, e1)) / den : 1.0 / 3.0;
        v = std::clamp(v, 0.05, 0.9);
        w = std::clamp(w, 0.05, 0.95 - v);
        const Chart& ch = lay.chart[best];
        const glm::dvec2 at = (1.0 - v - w) * ch[0] + v * ch[1] + w * ch[2];
        for (int k = 0; k < 4; ++k)
            crossStarts.push_back({best, at, k, 0.0});
    }
    std::vector<ExtraStart> extras;
    Traces tr;
    std::vector<uint8_t> slitTracer; // per tracer: a line with the same patch on both sides
    PatchStats st;
    std::vector<Piece> pieces;
    std::vector<Index> patchOf;
    std::vector<int> corners, reflex;
    std::vector<uint8_t> blocked;            // walls, and triangle edges a line runs along
    std::vector<std::vector<uint8_t>> joined; // per piece side: joined to a piece across it
    Index np = 0;
    int loopRounds = 0;
    const bool noSources = std::none_of(lay.source.begin(), lay.source.end(), [](uint8_t v) { return v != 0; });
    /**
     * A wedge cut from the middle of the patch's longest piece edge can land near one end of a long line, cutting off a
     * kite there. Where the patch runs at least 20 lattice along one line and the cut starts in an outer quarter of its own
     * line's run, it starts at the middle of that longest run instead.
     */
    auto moveToRunMiddle = [&](const std::vector<uint8_t>& want, std::vector<ExtraStart>& cut, const std::vector<Index>& cutTracer) {
        struct Run
        {
            double lo = std::numeric_limits<double>::max(), hi = -std::numeric_limits<double>::max(), length = 0.0;
        };
        auto edgeTimes = [&](const Piece& P, size_t k) {
            const Segment& g = tr.segments[P.side[k] - kSegSide];
            const glm::dvec2 a = P.poly[k], b = P.poly[(k + 1) % P.poly.size()];
            return std::pair{g.t0 + glm::length(a - g.a), g.t0 + glm::length(b - g.a)};
        };
        auto lineEdge = [&](const Piece& P, size_t k) { return P.side[k] >= kSegSide && !slitTracer[tr.segments[P.side[k] - kSegSide].tracer]; };
        std::map<std::pair<Index, Index>, Run> runs; // (patch, tracer)
        for (Index pi = 0; pi < Index(pieces.size()); ++pi)
        {
            const Piece& P = pieces[pi];
            if (!want[patchOf[pi]])
                continue;
            for (size_t k = 0; k < P.poly.size(); ++k)
                if (lineEdge(P, k))
                {
                    const auto [ta, tb] = edgeTimes(P, k);
                    Run& r = runs[{patchOf[pi], tr.segments[P.side[k] - kSegSide].tracer}];
                    r.lo = std::min({r.lo, ta, tb});
                    r.hi = std::max({r.hi, ta, tb});
                    r.length += glm::length(P.poly[(k + 1) % P.poly.size()] - P.poly[k]);
                }
        }
        std::vector<double> best(np, 0.0), middle(np, 0.0);
        std::vector<Index> line(np, kInvalid);
        for (const auto& [key, r] : runs)
            if (r.length > best[key.first])
            {
                best[key.first] = r.length;
                line[key.first] = key.second;
                middle[key.first] = 0.5 * (r.lo + r.hi);
            }
        for (Index p = 0; p < np; ++p)
        {
            const auto own = cutTracer[p] == kInvalid ? runs.end() : runs.find({p, cutTracer[p]});
            if (best[p] < 20.0 || own == runs.end() || own->second.hi <= own->second.lo)
            {
                line[p] = kInvalid;
                continue;
            }
            const double f = (cut[p].time - own->second.lo) / (own->second.hi - own->second.lo);
            if (f >= 0.25 && f <= 0.75)
                line[p] = kInvalid;
        }
        std::vector<double> gap(np, std::numeric_limits<double>::max());
        for (Index pi = 0; pi < Index(pieces.size()); ++pi)
        {
            const Piece& P = pieces[pi];
            const Index patch = patchOf[pi];
            if (!want[patch] || line[patch] == kInvalid)
                continue;
            glm::dvec2 centroid(0.0);
            for (const glm::dvec2& p : P.poly)
                centroid += p / double(P.poly.size());
            for (size_t k = 0; k < P.poly.size(); ++k)
            {
                if (!lineEdge(P, k) || tr.segments[P.side[k] - kSegSide].tracer != line[patch])
                    continue;
                const Segment& g = tr.segments[P.side[k] - kSegSide];
                const auto [ta, tb] = edgeTimes(P, k);
                const double lo = std::min(ta, tb), hi = std::max(ta, tb), m = middle[patch];
                const double off = m < lo ? lo - m : m > hi ? m - hi : 0.0;
                if (off >= gap[patch] || hi <= lo)
                    continue;
                gap[patch] = off;
                const double t = std::clamp(m, lo, hi);
                const glm::dvec2 a = P.poly[k], b = P.poly[(k + 1) % P.poly.size()];
                const glm::dvec2 at = a + (t - ta) / (tb - ta) * (b - a);
                const int inward = cross2(axis(g.dirK), centroid - at) > 0.0 ? 1 : 3;
                cut[patch] = {P.tri, at, mod4(g.dirK + inward), t};
            }
        }
    };
    // A line across each wanted patch from the middle of its longest traced side, started as that side's line passes;
    // a patch walled in by creases alone (the side of a cylinder) gets it from the middle of its longest wall side, and
    // one with no side at all (a torus has no poles) from the middle of its first piece. On a surface with no poles, a
    // patch with no corners whose only sides are lines with it on both sides (that seed's loop) gets two lines across,
    // a quarter and three quarters along the loop: one crossing would leave a patch meeting itself there, its four
    // corners at one point.
    auto cutsFor = [&](const std::vector<uint8_t>& want, bool fromRunMiddle = false) {
        std::vector<double> longest(np, 0.0), longestWall(np, 0.0);
        std::vector<Index> slitOnly(np, kInvalid), cutTracer(np, kInvalid);
        std::vector<ExtraStart> cut(np, ExtraStart{kInvalid}), wallCut(np, ExtraStart{kInvalid}), seed(np, ExtraStart{kInvalid});
        for (Index pi = 0; pi < Index(pieces.size()); ++pi)
        {
            const Piece& P = pieces[pi];
            const Index patch = patchOf[pi];
            if (!want[patch])
                continue;
            glm::dvec2 centroid(0.0);
            for (const glm::dvec2& p : P.poly)
                centroid += p / double(P.poly.size());
            if (seed[patch].tri == kInvalid)
                seed[patch] = {P.tri, centroid, 0, 0.0};
            for (size_t k = 0; k < P.poly.size(); ++k)
            {
                if (P.side[k] < kSegSide)
                {
                    const Index e = lay.triEdge[P.tri][P.side[k]];
                    const glm::dvec2 a = P.poly[k], b = P.poly[(k + 1) % P.poly.size()];
                    const double len = glm::length(b - a);
                    if (e == kInvalid || !lay.wall[e] || len <= longestWall[patch])
                        continue;
                    const glm::dvec2 mid = 0.5 * (a + b), inward = centroid - mid;
                    int best = 0;
                    for (int d = 1; d < 4; ++d)
                        if (glm::dot(axis(d), inward) > glm::dot(axis(best), inward))
                            best = d;
                    longestWall[patch] = len;
                    // Just inside the wall, so the line starts in the patch rather than on its border.
                    wallCut[patch] = {P.tri, mid + 1e-6 * inward, best, 0.0};
                    continue;
                }
                if (slitTracer[tr.segments[P.side[k] - kSegSide].tracer])
                {
                    slitOnly[patch] = tr.segments[P.side[k] - kSegSide].tracer;
                    continue;
                }
                const glm::dvec2 a = P.poly[k], b = P.poly[(k + 1) % P.poly.size()];
                const double len = glm::length(b - a);
                if (len <= longest[patch])
                    continue;
                const Segment& g = tr.segments[P.side[k] - kSegSide];
                const glm::dvec2 mid = 0.5 * (a + b);
                const int inward = cross2(axis(g.dirK), centroid - mid) > 0.0 ? 1 : 3;
                longest[patch] = len;
                cutTracer[patch] = g.tracer;
                cut[patch] = {P.tri, mid, mod4(g.dirK + inward), g.t0 + glm::length(mid - g.a)};
            }
        }
        if (fromRunMiddle)
            moveToRunMiddle(want, cut, cutTracer);
        std::vector<ExtraStart> out;
        bool looped = false;
        auto acrossLoop = [&](Index tracer) {
            double start = std::numeric_limits<double>::max();
            for (const Segment& g : tr.segments)
                if (g.tracer == tracer)
                    start = std::min(start, g.t0);
            std::vector<ExtraStart> starts;
            for (double f : {0.25, 0.75})
            {
                const double t = start + f * (tr.end[tracer] - start);
                for (const Segment& g : tr.segments)
                    if (const double len = glm::length(g.b - g.a); g.tracer == tracer && len > 0.0 && t >= g.t0 && t <= g.t0 + len)
                    {
                        starts.push_back({g.tri, g.a + (t - g.t0) / len * (g.b - g.a), mod4(g.dirK + 1), t});
                        break;
                    }
            }
            return starts;
        };
        for (Index p = 0; p < np; ++p)
            if (cut[p].tri != kInvalid)
                out.push_back(cut[p]);
            else if (wallCut[p].tri != kInvalid)
                out.push_back(wallCut[p]);
            else if (slitOnly[p] != kInvalid && corners[p] == 0 && noSources)
            {
                for (const ExtraStart& e : acrossLoop(slitOnly[p]))
                    out.push_back(e);
                looped = true;
            }
            else if (seed[p].tri != kInvalid)
                out.push_back(seed[p]);
        loopRounds += looped;
        if (looped && debug)
            std::fprintf(stderr, "patch cuts: across a loop, round allowance %d\n", 3 + loopRounds);
        return out;
    };
    Extracted ex;
    std::string why;
    std::vector<uint8_t> dropped; // per tracer: a line left out, running alongside another too close for a row between
    std::vector<Index> partnerOf; // per tracer left out: the line it ran beside
    int dropRounds = 0;
    std::vector<Index> wedges;
    // Wedges the mesh build finds get a line across, and the layout is traced and built again, twice at most.
    for (int wedgePass = 0;; ++wedgePass)
    {
        for (int round = 0;; ++round)
        {
            st = {};
            std::vector<ExtraStart> starts = crossStarts;
            starts.insert(starts.end(), extras.begin(), extras.end());
            tr = trace(lay, starts, dropped);
            if (tr.runaway)
                return fail("patch lines kept stopping each other");
            st.tracers = int(tr.end.size());
            st.segments = int(tr.segments.size());
            blocked = lay.wall;
            pieces = cutPieces(lay, tr, st.mismatched, blocked);
            st.pieces = int(pieces.size());

            // Patches: pieces joined across inner, non-wall triangle edges where their sides overlap.
            std::vector<Index> parent(pieces.size());
            std::iota(parent.begin(), parent.end(), Index(0));
            auto find = [&](Index x) {
                while (parent[x] != x)
                    x = parent[x] = parent[parent[x]];
                return x;
            };
            struct Span
            {
                double lo, hi;
                Index piece;
                size_t side;
            };
            std::vector<std::vector<Span>> spans(s.edges.size());
            // Along the edge from its lower vertex; the same in both triangles' charts, which are each the 3D triangle scaled.
            auto edgeParam = [&](Index t, int k, const glm::dvec2& p, Index e) {
                const glm::dvec2 a = lay.chart[t][k], b = lay.chart[t][(k + 1) % 3];
                const double l2 = glm::dot(b - a, b - a);
                const double r = l2 > 0.0 ? glm::dot(p - a, b - a) / l2 : 0.0;
                return s.tris[t][k] == s.edges[e][0] ? r : 1.0 - r;
            };
            auto internalSide = [&](const Piece& P, Index side) {
                if (side >= kSegSide)
                    return kInvalid;
                const Index e = lay.triEdge[P.tri][side];
                return e == kInvalid || blocked[e] ? kInvalid : e;
            };
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
            {
                const Piece& P = pieces[pi];
                for (size_t k = 0; k < P.poly.size(); ++k)
                    if (const Index e = internalSide(P, P.side[k]); e != kInvalid)
                    {
                        const int tk = int(P.side[k]);
                        const double r0 = edgeParam(P.tri, tk, P.poly[k], e), r1 = edgeParam(P.tri, tk, P.poly[(k + 1) % P.poly.size()], e);
                        spans[e].push_back({std::min(r0, r1), std::max(r0, r1), pi, k});
                    }
            }
            joined.assign(pieces.size(), {});
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
                joined[pi].assign(pieces[pi].poly.size(), 0);
            for (Index e = 0; e < Index(s.edges.size()); ++e)
                for (size_t i = 0; i < spans[e].size(); ++i)
                    for (size_t j = i + 1; j < spans[e].size(); ++j)
                    {
                        const Span &a = spans[e][i], &b = spans[e][j];
                        if (pieces[a.piece].tri != pieces[b.piece].tri && std::min(a.hi, b.hi) - std::max(a.lo, b.lo) > 1e-9)
                        {
                            parent[find(a.piece)] = find(b.piece);
                            joined[a.piece][a.side] = joined[b.piece][b.side] = 1;
                        }
                    }
            std::unordered_map<Index, Index> patchIndex;
            patchOf.assign(pieces.size(), 0);
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
                patchOf[pi] = patchIndex.try_emplace(find(pi), Index(patchIndex.size())).first->second;
            np = Index(patchIndex.size());
            st.patches = int(np);
            if (extras.empty() && dropRounds < 6)
            {
                const size_t more = dropSlivers(lay, tr, pieces, patchOf, np, dropped, partnerOf);
                if (debug)
                    std::fprintf(stderr, "patch slivers round %d: %zu lines dropped\n", dropRounds, more);
                if (more > 0)
                {
                    ++dropRounds;
                    --round;
                    continue;
                }
                dropRounds = 6;
            }

            // Corners: at each point on a patch's border, the angle its pieces fill there, measured against the field, so the
            // field's own smooth turning across triangle edges doesn't count and a corner reads as a quarter turn.
            struct At
            {
                double angle = 0.0;
                bool border = false;
                glm::dvec3 pos{0.0};
            };
            std::vector<glm::dvec3> cornerPos;
            std::vector<size_t> firstCorner(pieces.size(), 0);
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
            {
                firstCorner[pi] = cornerPos.size();
                for (const glm::dvec2& p : pieces[pi].poly)
                    cornerPos.push_back(at3d(pieces[pi], p));
            }
            const std::vector<Index> cluster = clusterPoints(cornerPos, unit);
            std::map<std::pair<Index, Index>, At> angleAt;
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
            {
                const Piece& P = pieces[pi];
                const size_t n = P.poly.size();
                const Index* key = cluster.data() + firstCorner[pi];
                // Points of one cluster in a row (a line stepping round a vertex just beside it) are one corner, measured
                // between the points either side: the steps' own directions are noise at that scale.
                std::vector<std::pair<size_t, size_t>> runs; // first, last
                for (size_t k = 0; k < n; ++k)
                    if (runs.empty() || key[k] != key[runs.back().second])
                        runs.push_back({k, k});
                    else
                        runs.back().second = k;
                if (runs.size() > 1 && key[runs.back().first] == key[runs.front().first])
                {
                    runs.front().first = runs.back().first;
                    runs.pop_back();
                }
                if (runs.size() < 3)
                    continue;
                const size_t m = runs.size();
                for (size_t r = 0; r < m; ++r)
                {
                    const auto [first, last] = runs[r];
                    const size_t prev = runs[(r + m - 1) % m].second, next = runs[(r + 1) % m].first;
                    const Index before = P.side[(first + n - 1) % n], after = P.side[last];
                    const glm::dvec2 u = P.poly[prev] - P.poly[first], v = P.poly[next] - P.poly[last];
                    At& at = angleAt[{patchOf[pi], key[first]}];
                    at.pos = cornerPos[firstCorner[pi] + first];
                    at.angle += std::atan2(cross2(v, u), glm::dot(u, v));
                    at.border = at.border || internalSide(P, before) == kInvalid || internalSide(P, after) == kInvalid;
                    if (const Index e = internalSide(P, before); e != kInvalid)
                        at.angle += lay.frameTurn(e, P.tri, lay.other(e, P.tri));
                }
            }
            corners.assign(np, 0);
            reflex.assign(np, 0);
            for (const auto& [key, at] : angleAt)
            {
                if (!at.border)
                    continue;
                const double deg = glm::degrees(at.angle);
                if (deg > 45.0 && deg < 135.0)
                    ++corners[key.first];
                else if (deg > 225.0)
                {
                    if (std::getenv("LILA_PATCH_LIST") && reflex[key.first] < 3)
                        std::fprintf(stderr, "reflex in %d: %.1f deg at %.4f %.4f %.4f\n", int(key.first), deg, at.pos.x, at.pos.y, at.pos.z);
                    ++reflex[key.first];
                }
            }
            // A patch with too few corners (a ring round a bare tube, a strip) or a reflex one (where a dropped line left a
            // corner bent inward) gets a line across it from the middle of its longest side, and the layout is traced again.
            // Three rounds of cuts, and one more for each that cut across a loop.
            if (round >= 3 + loopRounds)
                break;
            // A line with the same patch both sides is the only cut through a tube, which leaves the patch no disk: a
            // second cut from the far side splits it in two.
            std::vector<Index> sideA(tr.segments.size(), kInvalid);
            std::vector<uint8_t> slit(tr.segments.size(), 1), slitPatch(np, 0);
            slitTracer.assign(tr.end.size(), 0);
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
                for (Index side : pieces[pi].side)
                    if (side >= kSegSide)
                    {
                        Index& a = sideA[side - kSegSide];
                        if (a == kInvalid)
                            a = patchOf[pi];
                        else if (a != patchOf[pi])
                            slit[side - kSegSide] = 0;
                    }
            std::vector<int> slitCount(tr.end.size(), 0), segCount(tr.end.size(), 0);
            for (size_t g = 0; g < tr.segments.size(); ++g)
                if (sideA[g] != kInvalid)
                {
                    ++segCount[tr.segments[g].tracer];
                    slitCount[tr.segments[g].tracer] += slit[g];
                }
            for (size_t g = 0; g < tr.segments.size(); ++g)
                if (sideA[g] != kInvalid && slit[g] && 2 * slitCount[tr.segments[g].tracer] > segCount[tr.segments[g].tracer])
                {
                    slitTracer[tr.segments[g].tracer] = 1;
                    slitPatch[sideA[g]] = 1;
                }
            std::vector<uint8_t> want(np, 0);
            for (Index p = 0; p < np; ++p)
                want[p] = corners[p] < 4 || reflex[p] || slitPatch[p];
            const std::vector<ExtraStart> more = cutsFor(want);
            if (more.empty())
                break;
            extras.insert(extras.end(), more.begin(), more.end());
        }
        if (std::getenv("LILA_PATCH_LIST"))
        {
            // Slits: a line with the same patch on both sides.
            std::vector<std::vector<Index>> sideOf(tr.segments.size());
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
                for (Index side : pieces[pi].side)
                    if (side >= kSegSide)
                        sideOf[side - kSegSide].push_back(patchOf[pi]);
            std::map<Index, int> slitTracers;
            for (size_t g = 0; g < sideOf.size(); ++g)
                if (sideOf[g].size() >= 2 && std::all_of(sideOf[g].begin(), sideOf[g].end(), [&](Index p) { return p == sideOf[g][0]; }))
                    ++slitTracers[tr.segments[g].tracer];
            for (const auto& [tracer, n] : slitTracers)
            {
                const Segment* last = nullptr;
                for (const Segment& g : tr.segments)
                    if (g.tracer == tracer && glm::length(g.b - g.a) > 1e-9)
                        last = &g;
                std::fprintf(stderr, "slit tracer %d: %d slit segments, source %d, end %.3f why %d, last tri %d ends on %d\n", int(tracer), n,
                             int(tr.sourceOf[tracer]), tr.end[tracer], tr.endWhy[tracer], last ? int(last->tri) : -1, last ? int(last->endsOn) : -2);
            }
            {
                std::map<Index, int> lines;
                for (Index i = 0; i < Index(tr.sourceOf.size()); ++i)
                    if (tr.sourceOf[i] != kInvalid)
                        ++lines[tr.sourceOf[i]];
                std::map<std::pair<int, int>, int> table;
                for (const auto& [v, n] : lines)
                    if (lay.singular[v])
                        ++table[{lay.quarters[v], n}];
                for (const auto& [k, n] : table)
                    std::fprintf(stderr, "poles turning %d with %d lines: %d\n", k.first, k.second, n);
                std::map<int, int> none;
                for (Index v = 0; v < Index(lay.singular.size()); ++v)
                    if (lay.singular[v] && !lines.count(v))
                        ++none[lay.quarters[v]];
                for (const auto& [k, n] : none)
                    std::fprintf(stderr, "poles turning %d with no lines: %d\n", k, n);
            }
            std::vector<glm::dvec3> centre(np, glm::dvec3(0.0));
            std::vector<int> count(np, 0);
            for (Index pi = 0; pi < Index(pieces.size()); ++pi)
            {
                centre[patchOf[pi]] += at3d(pieces[pi], pieces[pi].poly[0]);
                ++count[patchOf[pi]];
            }
            for (Index p = 0; p < np; ++p)
                if (corners[p] != 4 || reflex[p])
                {
                    const glm::dvec3 c = centre[p] / double(count[p]);
                    std::fprintf(stderr, "patch %d: %d corners, %d reflex, %d pieces, at %.2f %.2f %.2f\n", int(p), corners[p], reflex[p], count[p], c.x, c.y, c.z);
                }
        }
        for (Index p = 0; p < np; ++p)
        {
            ++st.corners[std::min(8, corners[p])];
            st.rectangles += corners[p] == 4 && reflex[p] == 0;
            st.reflex += reflex[p] > 0;
        }
        if (std::getenv("LILA_PATCH_DEBUG"))
        {
            int sing = 0;
            for (uint8_t v : lay.singular)
                sing += v;
            std::fprintf(stderr, "patches: %d singular, %d tracers, %d segments, %d mismatched, %d pieces, %d patches, %d rectangles, %d with reflex corners; corners",
                         sing, st.tracers, st.segments, st.mismatched, st.pieces, st.patches, st.rectangles, st.reflex);
            for (int c = 0; c < 9; ++c)
                std::fprintf(stderr, " %d:%d", c, st.corners[c]);
            std::fprintf(stderr, "; ends: on a line %d, killed %d, wall %d, source %d, lost %d, %d loops\n", tr.why[0], tr.why[1], tr.why[2], tr.why[3], tr.why[4], tr.loops);
            if (const char* dump = std::getenv("LILA_PATCH_DUMP"))
                if (FILE* f = std::fopen(dump, "w"))
                {
                    for (const Piece& P : pieces)
                        for (const glm::dvec2& p : P.poly)
                        {
                            const glm::dvec3 x = at3d(P, p);
                            std::fprintf(f, "v %.9g %.9g %.9g\n", x.x, x.y, x.z);
                        }
                    Index base = 1;
                    for (Index pi = 0; pi < Index(pieces.size()); ++pi)
                    {
                        const Index patch = patchOf[pi];
                        std::fprintf(f, "# fc %d %d\nf", int(patch), int(corners[patch] == 4 && reflex[patch] == 0));
                        for (size_t k = 0; k < pieces[pi].poly.size(); ++k)
                            std::fprintf(f, " %d", int(base + k));
                        std::fprintf(f, "\n");
                        base += Index(pieces[pi].poly.size());
                    }
                    std::fclose(f);
                }
        }
        // Patches that aren't disks merge with the neighbours that close them up, and the mesh is built again.
        for (int pass = 0; pass < 8; ++pass)
        {
            std::vector<std::pair<Index, Index>> merge;
            why.clear();
            wedges.clear();
            ex = buildPatchMesh(lay, blocked, joined, pieces, patchOf, np, targetCells, why, &merge, &wedges);
            if (debug && !merge.empty())
                std::fprintf(stderr, "patch merge pass %d: %zu merges\n", pass, merge.size());
            if (merge.empty())
                break;
            std::vector<Index> up(np);
            std::iota(up.begin(), up.end(), Index(0));
            auto root = [&](Index x) {
                while (up[x] != x)
                    x = up[x] = up[up[x]];
                return x;
            };
            for (auto [a, b] : merge)
                up[root(a)] = root(b);
            std::unordered_map<Index, Index> renumber;
            for (Index& p : patchOf)
                p = renumber.try_emplace(root(p), Index(renumber.size())).first->second;
            np = Index(renumber.size());
        }
        if (wedges.empty() || wedgePass >= 2)
            break;
        std::vector<uint8_t> want(np, 0);
        for (Index p : wedges)
            if (p < np)
                want[p] = 1;
        corners.resize(np, 4);
        const std::vector<ExtraStart> more = cutsFor(want, true);
        if (debug)
            std::fprintf(stderr, "wedge pass %d: %zu wedges, %zu cuts\n", wedgePass, wedges.size(), more.size());
        if (more.empty())
            break;
        extras.insert(extras.end(), more.begin(), more.end());
    }
    if (!why.empty())
        return fail(why);
    // Free patches filled by pattern come out with fewer quads than their sides count for, so the count is met on the
    // mesh built: past 3% off, the target scales by what it missed by, twice at most.
    double asked = targetCells;
    for (int again = 0; again < 2 && targetCells > 0.0 && ex.mesh.numFaces() > 0; ++again)
    {
        const double got = double(ex.mesh.numFaces());
        if (std::abs(got / targetCells - 1.0) < 0.03)
            break;
        asked *= targetCells / got;
        std::string retryWhy;
        Extracted retry = buildPatchMesh(lay, blocked, joined, pieces, patchOf, np, asked, retryWhy, nullptr);
        if (!retryWhy.empty() || retry.mesh.numFaces() == 0)
            break;
        if (debug)
            std::fprintf(stderr, "patch count: %.0f faces, rebuilt for %.0f: %d\n", got, asked, int(retry.mesh.numFaces()));
        if (std::abs(double(retry.mesh.numFaces()) / targetCells - 1.0) >= std::abs(got / targetCells - 1.0))
            break;
        ex = std::move(retry);
    }
    return ex;
}
} // namespace lila::retopo::detail
