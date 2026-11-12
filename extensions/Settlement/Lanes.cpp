// Settlement: the lane graph (vehicle and pedestrian lanes, the movements junctions allow, signal phases), bus and tram
// routes through it, and the checks a traffic system needs to hold.

#include "Streetscape.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <tuple>

namespace settlement
{
namespace
{
inline Vec2 laneXZ(const glm::dvec3& p) { return {p.x, p.z}; }
inline Vec2 laneRight(Vec2 d) { return {-d.y, d.x}; }

uint64_t laneMix(uint64_t h, uint64_t v) { return hash64(h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))); }

double polyLength(const std::vector<glm::dvec3>& pts)
{
    double len = 0.0;
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        len += glm::length(pts[k + 1] - pts[k]);
    return len;
}

/** The polyline moved sideways by off (to the right of its direction), corners mitred. */
std::vector<glm::dvec3> offsetLine(const std::vector<glm::dvec3>& pts, double off)
{
    std::vector<glm::dvec3> out(pts);
    if (std::abs(off) < 1e-9 || pts.size() < 2)
        return out;
    for (size_t k = 0; k < pts.size(); ++k)
    {
        auto dir = [&](size_t a, size_t b) {
            const Vec2 d = laneXZ(pts[b]) - laneXZ(pts[a]);
            return glm::length(d) > 1e-9 ? d / glm::length(d) : Vec2(0.0);
        };
        const Vec2 d0 = k > 0 ? dir(k - 1, k) : dir(k, k + 1), d1 = k + 1 < pts.size() ? dir(k, k + 1) : d0;
        Vec2 d = d0 + d1;
        d = glm::length(d) > 1e-9 ? d / glm::length(d) : d1;
        const double c = std::max(glm::dot(laneRight(d), laneRight(d1)), 0.5);
        const Vec2 n = laneRight(d) * (off / c);
        out[k].x += n.x;
        out[k].z += n.y;
    }
    return out;
}

/** The lanes one road carries. */
struct LineLanes
{
    std::vector<int> fwd, back; // innermost first
    int pedR = -1, pedL = -1;   // pedestrian lanes right and left of the line's direction (the same lane when there is one)
};

struct EndRef
{
    int line = 0;
    bool start = true;
};

std::vector<int> incoming(const LineLanes& x, const EndRef& e) { return e.start ? x.back : x.fwd; }
std::vector<int> outgoing(const LineLanes& x, const EndRef& e) { return e.start ? x.fwd : x.back; }
/** The pedestrian lane on the side of this end facing the next end counter-clockwise (in x, z), and the previous. */
int pedNext(const LineLanes& x, const EndRef& e) { return e.start ? x.pedR : x.pedL; }
int pedPrev(const LineLanes& x, const EndRef& e) { return e.start ? x.pedL : x.pedR; }

Vec2 endOut(const RoadNetwork& net, const EndRef& e)
{
    const std::vector<glm::dvec3>& p = net.lines[size_t(e.line)].pts;
    const Vec2 d = e.start ? laneXZ(p[1]) - laneXZ(p[0]) : laneXZ(p[p.size() - 2]) - laneXZ(p.back());
    return glm::length(d) > 1e-9 ? d / glm::length(d) : Vec2(1.0, 0.0);
}

/** A connector's path from the end of one lane to the start of the next, as a few points. */
std::vector<Vec2> connector(const Lane& a, const Lane& b)
{
    const Vec2 p = laneXZ(a.pts.back()), q = laneXZ(b.pts.front());
    Vec2 da = laneXZ(a.pts.back()) - laneXZ(a.pts[a.pts.size() - 2]), db = laneXZ(b.pts[1]) - laneXZ(b.pts[0]);
    da = glm::length(da) > 1e-9 ? da / glm::length(da) : Vec2(1.0, 0.0);
    db = glm::length(db) > 1e-9 ? db / glm::length(db) : Vec2(1.0, 0.0);
    Vec2 c = 0.5 * (p + q);
    const double den = cross(da, db);
    if (std::abs(den) > 1e-6)
    {
        const double t = cross(q - p, db) / den;
        if (t > 0.0 && t < 4.0 * glm::length(q - p))
            c = p + da * t;
    }
    std::vector<Vec2> out;
    for (int i = 0; i <= 8; ++i)
    {
        const double t = i / 8.0;
        out.push_back((1 - t) * (1 - t) * p + 2 * (1 - t) * t * c + t * t * q);
    }
    return out;
}

bool laneSegmentsCross(Vec2 a, Vec2 b, Vec2 c, Vec2 d)
{
    const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a), d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
    return ((d1 > 1e-6 && d2 < -1e-6) || (d1 < -1e-6 && d2 > 1e-6)) && ((d3 > 1e-6 && d4 < -1e-6) || (d3 < -1e-6 && d4 > 1e-6));
}

bool pathsCross(const std::vector<Vec2>& x, const std::vector<Vec2>& y)
{
    for (size_t i = 0; i + 1 < x.size(); ++i)
        for (size_t j = 0; j + 1 < y.size(); ++j)
            if (laneSegmentsCross(x[i], x[i + 1], y[j], y[j + 1]))
                return true;
    return false;
}

/** Strongly connected components (Tarjan, iterative): component per vertex. */
std::vector<int> laneComponents(const std::vector<std::vector<int>>& adj, int* count)
{
    const int n = int(adj.size());
    std::vector<int> index(size_t(n), -1), low(size_t(n), 0), comp(size_t(n), -1);
    std::vector<char> onStack(size_t(n), 0);
    std::vector<int> stack;
    int next = 0, comps = 0;
    for (int root = 0; root < n; ++root)
    {
        if (index[size_t(root)] >= 0)
            continue;
        std::vector<std::pair<int, size_t>> work = {{root, 0}};
        index[size_t(root)] = low[size_t(root)] = next++;
        stack.push_back(root);
        onStack[size_t(root)] = 1;
        while (!work.empty())
        {
            auto& [v, i] = work.back();
            if (i < adj[size_t(v)].size())
            {
                const int w = adj[size_t(v)][i++];
                if (index[size_t(w)] < 0)
                {
                    index[size_t(w)] = low[size_t(w)] = next++;
                    stack.push_back(w);
                    onStack[size_t(w)] = 1;
                    work.push_back({w, 0});
                }
                else if (onStack[size_t(w)])
                    low[size_t(v)] = std::min(low[size_t(v)], index[size_t(w)]);
                continue;
            }
            const int done = v;
            work.pop_back();
            if (!work.empty())
                low[size_t(work.back().first)] = std::min(low[size_t(work.back().first)], low[size_t(done)]);
            if (low[size_t(done)] == index[size_t(done)])
            {
                int w;
                do
                {
                    w = stack.back();
                    stack.pop_back();
                    onStack[size_t(w)] = 0;
                    comp[size_t(w)] = comps;
                } while (w != done);
                ++comps;
            }
        }
    }
    *count = comps;
    return comp;
}

std::vector<char> reach(const std::vector<std::vector<int>>& adj, const std::vector<int>& from)
{
    std::vector<char> seen(adj.size(), 0);
    std::vector<int> todo;
    for (int f : from)
        if (!seen[size_t(f)])
        {
            seen[size_t(f)] = 1;
            todo.push_back(f);
        }
    while (!todo.empty())
    {
        const int v = todo.back();
        todo.pop_back();
        for (int w : adj[size_t(v)])
            if (!seen[size_t(w)])
            {
                seen[size_t(w)] = 1;
                todo.push_back(w);
            }
    }
    return seen;
}

/** Lane adjacency for one mode, over all lanes (others left without links). Pedestrian links go both ways. */
std::vector<std::vector<int>> adjacency(const LaneGraph& g, LaneMode mode, bool reverse = false)
{
    std::vector<std::vector<int>> adj(g.lanes.size());
    for (const LaneLink& k : g.links)
    {
        if (g.lanes[size_t(k.from)].mode != mode || g.lanes[size_t(k.to)].mode != mode)
            continue;
        if (mode == LaneMode::Pedestrian)
        {
            adj[size_t(k.from)].push_back(k.to);
            adj[size_t(k.to)].push_back(k.from);
        }
        else
            adj[size_t(reverse ? k.to : k.from)].push_back(reverse ? k.from : k.to);
    }
    return adj;
}
} // namespace

const char* laneModeName(LaneMode m) { return m == LaneMode::Vehicle ? "vehicle" : "pedestrian"; }

const char* movementName(Movement m)
{
    switch (m)
    {
    case Movement::Through: return "through";
    case Movement::Left: return "left";
    case Movement::Right: return "right";
    case Movement::UTurn: return "uturn";
    case Movement::Corner: return "corner";
    case Movement::Crosswalk: return "crosswalk";
    case Movement::Crossing: return "crossing";
    case Movement::LaneChange: return "laneChange";
    }
    return "through";
}

LaneGraph laneGraph(const Layout& l, const RoadNetwork& net)
{
    LILA_ZONE();

    LaneGraph g;
    std::vector<LineLanes> per(net.lines.size());
    auto addLane = [&](LaneMode mode, int line, int dir, int index, int side, std::vector<glm::dvec3> pts, double width, double speed) {
        Lane x;
        x.id = int(g.lanes.size());
        x.mode = mode;
        x.line = line;
        x.dir = dir;
        x.index = index;
        x.side = side;
        x.length = polyLength(pts);
        x.pts = std::move(pts);
        x.width = width;
        x.speed = speed;
        x.street = net.lines[size_t(line)].street;
        g.lanes.push_back(std::move(x));
        return g.lanes.back().id;
    };
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        if (line.surface || line.startNode < 0 || line.pts.size() < 2)
        {
            g.surfaceLinesSkipped += line.surface ? 1 : 0;
            continue;
        }
        const RoadProfile& p = l.roads[size_t(line.profile)];
        const RoadSection sec = roadSection(l, line);
        const double f = p.width() > 1e-6 ? line.width / p.width() : 1.0;
        const double lw = p.laneWidth * f, m = 0.5 * p.median * f;
        std::vector<glm::dvec3> rev(line.pts.rbegin(), line.pts.rend());
        if (p.lanes > 0 && line.cls != StreetClass::Path)
        {
            if (p.lanes == 1 && !p.oneWay)
            {
                // A single lane both ways: one lane each way down the middle.
                per[k].fwd.push_back(addLane(LaneMode::Vehicle, int(k), 1, 0, 0, line.pts, lw, p.speed));
                per[k].back.push_back(addLane(LaneMode::Vehicle, int(k), -1, 0, 0, rev, lw, p.speed));
            }
            else
            {
                const int nb = p.oneWay ? 0 : p.lanes / 2, nf = p.lanes - nb;
                for (int i = 0; i < nf; ++i)
                    per[k].fwd.push_back(addLane(LaneMode::Vehicle, int(k), 1, i, 0, offsetLine(line.pts, m + (i + 0.5) * lw), lw, p.speed));
                for (int i = 0; i < nb; ++i)
                    per[k].back.push_back(addLane(LaneMode::Vehicle, int(k), -1, i, 0, offsetLine(rev, m + (i + 0.5) * lw), lw, p.speed));
            }
        }
        if (sec.sidewalk > 0.0 && sec.carriage > 0.0 && !sec.kerbless)
        {
            const double off = sec.carriage + 0.5 * sec.sidewalk;
            per[k].pedR = addLane(LaneMode::Pedestrian, int(k), 0, 0, 1, offsetLine(line.pts, off), sec.sidewalk, 5.0);
            per[k].pedL = addLane(LaneMode::Pedestrian, int(k), 0, 0, -1, offsetLine(line.pts, -off), sec.sidewalk, 5.0);
        }
        else
        {
            // Footpaths, and streets without sidewalks where people walk at the edge of the road.
            const double off = sec.carriage > 0.0 && !sec.kerbless ? std::max(0.0, sec.half() - 50.0) : 0.0;
            per[k].pedR = per[k].pedL = addLane(LaneMode::Pedestrian, int(k), 0, 0, 0, offsetLine(line.pts, off), std::max(100.0, 2.0 * sec.half()), 5.0);
        }
    }

    // Line ends gathered by where they meet: at a junction, or at a plain node or dead end of the street graph.
    std::map<std::pair<int, bool>, int> junctionOf;
    for (size_t j = 0; j < net.junctions.size(); ++j)
        for (const RoadEnd& e : net.junctions[j].ends)
            junctionOf[{e.line, e.start}] = int(j);
    std::map<int, std::vector<EndRef>> atNode;
    std::map<int, std::vector<EndRef>> atJunction;
    for (size_t k = 0; k < net.lines.size(); ++k)
    {
        const StreetLine& line = net.lines[k];
        if (line.surface || line.startNode < 0 || line.pts.size() < 2)
            continue;
        for (bool start : {true, false})
        {
            const EndRef e{int(k), start};
            if (auto it = junctionOf.find({int(k), start}); it != junctionOf.end())
                atJunction[it->second].push_back(e);
            else
                atNode[start ? line.startNode : line.endNode].push_back(e);
        }
    }

    auto link = [&](int from, int to, int junction, Movement move, int phase = -1, bool yields = false) {
        if (from >= 0 && to >= 0 && from != to)
            g.links.push_back({from, to, junction, move, phase, yields});
    };
    for (const LineLanes& x : per)
        for (const std::vector<int>* side : {&x.fwd, &x.back})
            for (size_t i = 0; i + 1 < side->size(); ++i)
            {
                link((*side)[i], (*side)[i + 1], -1, Movement::LaneChange);
                link((*side)[i + 1], (*side)[i], -1, Movement::LaneChange);
            }

    auto turnAngle = [&](const EndRef& a, const EndRef& b) {
        const Vec2 dIn = -endOut(net, a), dOut = endOut(net, b);
        return std::atan2(cross(dIn, dOut), glm::dot(dIn, dOut));
    };
    // through: b is where a runs straight on (the straightest way within 45 degrees); every other way is a turn.
    auto connectVehicles = [&](const EndRef& a, const EndRef& b, int junction, const Junction* j, bool through) {
        const std::vector<int> in = incoming(per[size_t(a.line)], a), out = outgoing(per[size_t(b.line)], b);
        if (in.empty() || out.empty())
            return;
        const double ang = turnAngle(a, b);
        // At a plain node there is no other way on, however sharp the bend.
        if (j && std::abs(ang) > glm::radians(160.0))
            return;
        // Positive turns are to the driver's right (Y up, seen from above).
        const Movement move = through ? Movement::Through : ang > 0.0 ? Movement::Right : Movement::Left;
        const StreetClass from = net.lines[size_t(a.line)].cls, to = net.lines[size_t(b.line)].cls;
        const bool signal = j && j->control == Control::Signal;
        if (!signal && from == StreetClass::Alley && to == StreetClass::Arterial && move != Movement::Right)
            return;
        int phase = -1;
        bool opposed = false;
        if (signal)
        {
            int endIndex = -1;
            for (size_t e = 0; e < j->ends.size(); ++e)
                if (j->ends[e].line == a.line && j->ends[e].start == a.start)
                    endIndex = int(e);
            for (size_t p = 0; p < j->phases.size(); ++p)
                if (std::ranges::find(j->phases[p], endIndex) != j->phases[p].end())
                {
                    phase = int(p);
                    opposed = j->phases[p].size() > 1;
                }
        }
        const bool yields = move == Movement::Left && opposed;
        if (move == Movement::Through)
        {
            for (size_t i = 0; i < in.size(); ++i)
                link(in[i], out[std::min(i, out.size() - 1)], junction, move, phase, yields);
            for (size_t o = in.size(); o < out.size(); ++o)
                link(in.back(), out[o], junction, move, phase, yields);
        }
        else if (move == Movement::Right)
            link(in.back(), out.back(), junction, move, phase, yields);
        else
            link(in.front(), out.front(), junction, move, phase, yields);
    };
    auto byAngle = [&](std::vector<EndRef> ends) {
        std::sort(ends.begin(), ends.end(), [&](const EndRef& x, const EndRef& y) {
            const Vec2 dx = endOut(net, x), dy = endOut(net, y);
            return std::atan2(dx.y, dx.x) < std::atan2(dy.y, dy.x);
        });
        return ends;
    };
    auto connectPedestrians = [&](const std::vector<EndRef>& ends, int junction) {
        if (ends.size() == 1)
        {
            link(per[size_t(ends[0].line)].pedR, per[size_t(ends[0].line)].pedL, junction, Movement::Corner);
            return;
        }
        const std::vector<EndRef> sorted = byAngle(ends);
        for (size_t i = 0; i < sorted.size(); ++i)
        {
            const EndRef &a = sorted[i], &b = sorted[(i + 1) % sorted.size()];
            link(pedNext(per[size_t(a.line)], a), pedPrev(per[size_t(b.line)], b), junction, Movement::Corner);
        }
    };

    for (const auto& [ji, ends] : atJunction)
    {
        const Junction& j = net.junctions[size_t(ji)];
        for (const EndRef& a : ends)
        {
            const EndRef* straight = nullptr;
            double best = glm::radians(45.0);
            for (const EndRef& b : ends)
                if ((a.line != b.line || a.start != b.start) && !outgoing(per[size_t(b.line)], b).empty())
                    if (const double t = std::abs(turnAngle(a, b)); t < best)
                    {
                        best = t;
                        straight = &b;
                    }
            for (const EndRef& b : ends)
                if (a.line != b.line || a.start != b.start)
                    connectVehicles(a, b, ji, &j, &b == straight);
        }
        connectPedestrians(ends, ji);
        for (const EndRef& e : ends)
        {
            const LineLanes& x = per[size_t(e.line)];
            if (x.pedR == x.pedL)
                continue;
            int marked = -1;
            for (size_t c = 0; c < j.crosswalks.size(); ++c)
                if (j.ends[size_t(j.crosswalks[c].end)].line == e.line && j.ends[size_t(j.crosswalks[c].end)].start == e.start)
                    marked = int(c);
            // People cross in the first phase that walks them.
            int walkPhase = -1;
            for (size_t p = 0; p < j.walks.size() && walkPhase < 0 && marked >= 0; ++p)
                if (std::ranges::find(j.walks[p], marked) != j.walks[p].end())
                    walkPhase = int(p);
            link(x.pedR, x.pedL, ji, marked >= 0 ? Movement::Crosswalk : Movement::Crossing, walkPhase);
        }
    }
    for (const auto& [node, ends] : atNode)
    {
        if (ends.size() == 1)
        {
            // A dead end: vehicles turn round, and leave or arrive there when it is on the settlement's edge.
            const EndRef& e = ends[0];
            const std::vector<int> in = incoming(per[size_t(e.line)], e), out = outgoing(per[size_t(e.line)], e);
            if (!in.empty() && !out.empty())
                link(in.front(), out.front(), -1, Movement::UTurn);
            const Vec2 p = l.graph.nodes[size_t(node)].p;
            if (!l.outline.pts.empty() && boundaryDistance(l.outline.pts, p) < std::max(2.0 * l.graph.snap(), 500.0))
            {
                for (int x : in)
                    g.lanes[size_t(x)].exit = true;
                for (int x : out)
                    g.lanes[size_t(x)].entry = true;
            }
        }
        else
            for (const EndRef& a : ends)
                for (const EndRef& b : ends)
                    if (a.line != b.line || a.start != b.start)
                        connectVehicles(a, b, -1, nullptr, true);
        connectPedestrians(ends, -1);
    }
    // On a skewed junction a turn from one approach can cross the through movement it shares a phase with: that turn
    // goes on green only when the way is clear, as a permissive turn. Two crossing through movements get phases of their own.
    std::map<std::pair<int, int>, std::vector<size_t>> greens;
    std::map<int, int> lastPhase;
    std::set<std::tuple<int, int, int>> ownPhase; // junction, line, direction of an approach moved to a phase of its own
    for (size_t k = 0; k < g.links.size(); ++k)
        if (const LaneLink& x = g.links[k]; x.junction >= 0 && x.phase >= 0 && !x.yields && g.lanes[size_t(x.from)].mode == LaneMode::Vehicle)
        {
            greens[{x.junction, x.phase}].push_back(k);
            lastPhase[x.junction] = std::max(lastPhase[x.junction], x.phase);
        }
    for (const auto& [key, moves] : greens)
        for (size_t a = 0; a < moves.size(); ++a)
            for (size_t b = a + 1; b < moves.size(); ++b)
            {
                LaneLink &x = g.links[moves[a]], &y = g.links[moves[b]];
                if (x.yields || y.yields)
                    continue;
                const Lane &xf = g.lanes[size_t(x.from)], &yf = g.lanes[size_t(y.from)];
                if (xf.line == yf.line && xf.dir == yf.dir)
                    continue;
                if (x.to != y.to && !pathsCross(connector(xf, g.lanes[size_t(x.to)]), connector(yf, g.lanes[size_t(y.to)])))
                    continue;
                if (y.move != Movement::Through)
                    y.yields = true;
                else if (x.move != Movement::Through)
                    x.yields = true;
                else
                    ownPhase.insert({key.first, yf.line, int(yf.dir)});
            }
    std::map<std::tuple<int, int, int>, int> newPhase;
    for (const auto& approach : ownPhase)
        newPhase[approach] = ++lastPhase[std::get<0>(approach)];
    for (LaneLink& k : g.links)
        if (k.junction >= 0 && k.phase >= 0)
            if (auto it = newPhase.find({k.junction, g.lanes[size_t(k.from)].line, int(g.lanes[size_t(k.from)].dir)}); it != newPhase.end())
                k.phase = it->second;
    return g;
}

void finishSignals(RoadNetwork& net, const Layout& l)
{
    LILA_ZONE();

    if (std::ranges::none_of(net.junctions, [](const Junction& j) { return j.control == Control::Signal; }))
        return;
    const LaneGraph g = laneGraph(l, net);
    std::map<int, std::vector<const LaneLink*>> byJunction;
    for (const LaneLink& k : g.links)
        if (k.junction >= 0 && k.phase >= 0 && g.lanes[size_t(k.from)].mode == LaneMode::Vehicle)
            byJunction[k.junction].push_back(&k);
    for (size_t ji = 0; ji < net.junctions.size(); ++ji)
    {
        Junction& j = net.junctions[ji];
        if (j.control != Control::Signal)
            continue;
        auto endOf = [&](int line, bool start) {
            for (size_t e = 0; e < j.ends.size(); ++e)
                if (j.ends[e].line == line && j.ends[e].start == start)
                    return int(e);
            return -1;
        };
        // Vehicle lanes along their line arrive at its end and leave from its start.
        std::map<int, int> phaseOf;
        std::map<int, std::set<int>> leftBy;
        for (const LaneLink* k : byJunction[int(ji)])
        {
            const Lane &from = g.lanes[size_t(k->from)], &to = g.lanes[size_t(k->to)];
            if (const int e = endOf(from.line, from.dir < 0); e >= 0)
                phaseOf[e] = k->phase;
            if (k->move == Movement::Through && !k->yields)
                if (const int e = endOf(to.line, to.dir > 0); e >= 0)
                    leftBy[k->phase].insert(e);
        }
        for (size_t p = 0; p < j.phases.size(); ++p)
            for (int e : j.phases[p])
                phaseOf.emplace(e, int(p));
        std::map<int, std::vector<int>> groups;
        for (const auto& [e, p] : phaseOf)
            groups[p].push_back(e);
        j.phases.clear();
        std::vector<std::set<int>> left;
        for (auto& [p, ends] : groups)
        {
            j.phases.push_back(std::move(ends));
            left.push_back(leftBy[p]);
        }
        j.walks.assign(j.phases.size(), {});
        std::vector<int> unserved;
        for (size_t c = 0; c < j.crosswalks.size(); ++c)
        {
            const int e = j.crosswalks[c].end;
            bool served = false;
            for (size_t p = 0; p < j.phases.size(); ++p)
                if (std::ranges::find(j.phases[p], e) == j.phases[p].end() && !left[p].contains(e))
                {
                    j.walks[p].push_back(int(c));
                    served = true;
                }
            if (!served)
                unserved.push_back(int(c));
        }
        if (!unserved.empty())
        {
            j.phases.push_back({});
            j.walks.push_back(std::move(unserved));
        }
    }
}

void addTransit(LaneGraph& g, const Layout& l, const RoadNetwork& net, const TransitOptions& o)
{
    LILA_ZONE();

    const bool medieval = medievalStyle(l);
    const std::string& style = l.profile.name;
    const bool city = style.find("City") != std::string::npos || style.find("manhattan") != std::string::npos ||
                      style.find("scifi") != std::string::npos || style.find("egacity") != std::string::npos;
    const int districts = int(l.districts.size());
    const int buses = o.busRoutes >= 0 ? o.busRoutes : medieval ? 0 : std::min(4, districts / 2);
    bool anyArterial = false;
    for (const Lane& x : g.lanes)
        anyArterial |= x.mode == LaneMode::Vehicle && net.lines[size_t(x.line)].cls == StreetClass::Arterial;
    const int trams = o.tramRoutes >= 0 ? o.tramRoutes : (!medieval && city && anyArterial ? 1 : 0);
    if (buses + trams == 0)
        return;

    std::vector<std::vector<std::pair<int, const LaneLink*>>> out(g.lanes.size());
    for (const LaneLink& k : g.links)
        if (g.lanes[size_t(k.from)].mode == LaneMode::Vehicle)
            out[size_t(k.from)].push_back({k.to, &k});
    auto allowedFor = [&](const std::string& mode, const Lane& x) {
        if (x.mode != LaneMode::Vehicle)
            return false;
        const StreetClass c = net.lines[size_t(x.line)].cls;
        return mode == "tram" ? c == StreetClass::Arterial && x.index == 0 : c <= StreetClass::Collector;
    };
    auto nearest = [&](Vec2 q, const std::function<bool(const Lane&)>& ok) {
        int best = -1;
        double bestD = std::numeric_limits<double>::infinity();
        for (const Lane& x : g.lanes)
            if (ok(x) && x.length > 500.0)
            {
                const double d = glm::length(laneXZ(x.pts[x.pts.size() / 2]) - q);
                if (d < bestD)
                {
                    bestD = d;
                    best = x.id;
                }
            }
        return best;
    };
    // Cheapest lane path by travel time with a small cost per turn, over the lanes ok allows.
    auto path = [&](int from, int to, const std::function<bool(const Lane&)>& ok) {
        std::vector<double> cost(g.lanes.size(), std::numeric_limits<double>::infinity());
        std::vector<int> prev(g.lanes.size(), -1);
        using Item = std::pair<double, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
        cost[size_t(from)] = 0.0;
        heap.push({0.0, from});
        while (!heap.empty())
        {
            const auto [c, v] = heap.top();
            heap.pop();
            if (c > cost[size_t(v)])
                continue;
            if (v == to)
                break;
            for (const auto& [w, k] : out[size_t(v)])
            {
                if (!ok(g.lanes[size_t(w)]))
                    continue;
                const Lane& x = g.lanes[size_t(w)];
                const double step = x.length / std::max(x.speed, 5.0) + (k->move == Movement::Through ? 0.0 : 40.0);
                if (c + step < cost[size_t(w)])
                {
                    cost[size_t(w)] = c + step;
                    prev[size_t(w)] = v;
                    heap.push({c + step, w});
                }
            }
        }
        std::vector<int> lanes;
        if (!std::isfinite(cost[size_t(to)]))
            return lanes;
        for (int v = to; v >= 0; v = prev[size_t(v)])
            lanes.push_back(v);
        std::reverse(lanes.begin(), lanes.end());
        return lanes;
    };

    // Terminals: district centres around the centre, opposite ones paired; the settlement's exits when it has too few
    // districts.
    std::vector<std::pair<Vec2, uint64_t>> ends;
    std::vector<std::string> endNames;
    for (const District& d : l.districts)
    {
        ends.push_back({d.center, d.id});
        endNames.push_back(d.name);
    }
    if (ends.size() < 2)
    {
        ends.clear();
        endNames.clear();
        for (const Lane& x : g.lanes)
            if (x.exit)
            {
                ends.push_back({laneXZ(x.pts.back()), laneMix(x.street, 0x45584954)});
                endNames.push_back(streetName(l, x.street));
            }
    }
    std::vector<size_t> order(ends.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        const Vec2 da = ends[a].first - l.center, db = ends[b].first - l.center;
        return std::atan2(da.y, da.x) < std::atan2(db.y, db.x);
    });

    auto build = [&](const std::string& mode, int number, size_t a, size_t b) {
        const auto allowed = [&](const Lane& x) { return allowedFor(mode, x); };
        const auto anyVehicle = [](const Lane& x) { return x.mode == LaneMode::Vehicle; };
        const uint64_t id = laneMix(laneMix(mode == "tram" ? 0x7472616Dull : 0x627573ull, ends[a].second), ends[b].second);
        TransitRoute legs[2];
        for (int leg = 0; leg < 2; ++leg)
        {
            TransitRoute& r = legs[leg];
            const size_t from = leg == 0 ? a : b, to = leg == 0 ? b : a;
            r.id = laneMix(id, uint64_t(leg)) & kStreetscapeIdMask;
            r.mode = mode;
            r.direction = leg == 0 ? "out" : "back";
            r.name = std::string(mode == "tram" ? "Tram " : "Bus ") + std::to_string(number) + ": " + endNames[from] + " - " + endNames[to];
            auto lay = [&](const std::function<bool(const Lane&)>& ok) {
                const int s = nearest(ends[from].first, ok), e = nearest(ends[to].first, ok), via = nearest(l.center, ok);
                if (s < 0 || e < 0)
                    return std::vector<int>{};
                if (via >= 0 && via != s && via != e)
                {
                    std::vector<int> first = path(s, via, ok), second = path(via, e, ok);
                    if (!first.empty() && !second.empty())
                    {
                        first.insert(first.end(), second.begin() + 1, second.end());
                        return first;
                    }
                }
                return path(s, e, ok);
            };
            r.lanes = lay(allowed);
            if (r.lanes.empty())
            {
                r.lanes = lay(anyVehicle);
                if (!r.lanes.empty())
                    r.note = mode == "tram" ? "no way along the arterials' inner lanes: the tram shares the general lanes"
                                            : "no way along arterials and collectors alone: the route uses minor streets";
            }
            if (r.lanes.empty())
            {
                r.note = mode == "tram" ? "no way along the arterials' inner lanes" : "no way between its ends";
                continue;
            }
            for (int x : r.lanes)
                r.length += g.lanes[size_t(x)].length;
            // Stops by spacing on lanes long enough to hold one, the first and last at the ends.
            const double spacing = mode == "tram" ? o.tramStopSpacing : o.busStopSpacing;
            double travelled = 0.0, nextStop = 0.0;
            for (size_t i = 0; i < r.lanes.size(); ++i)
            {
                const Lane& x = g.lanes[size_t(r.lanes[i])];
                const bool last = i + 1 == r.lanes.size();
                if ((travelled + x.length >= nextStop || last) && x.length >= 2000.0)
                {
                    const glm::dvec3 p = x.pts[x.pts.size() / 2];
                    TransitStop s;
                    s.lane = x.id;
                    s.p = p;
                    s.id = laneMix(laneMix(r.id, x.street), uint64_t(std::llround(p.x / 1000.0)) * 31 + uint64_t(std::llround(p.z / 1000.0))) & kStreetscapeIdMask;
                    s.name = streetName(l, x.street);
                    if (s.name.empty())
                        s.name = "Stop " + std::to_string(r.stops.size() + 1);
                    r.stops.push_back(std::move(s));
                    nextStop = travelled + x.length * 0.5 + spacing;
                }
                travelled += x.length;
            }
        }
        // Reachable: each lane leads to the next, and each leg's far end leads on to the other leg's start.
        std::vector<std::vector<int>> adj(g.lanes.size());
        std::set<std::pair<int, int>> linked;
        for (const LaneLink& k : g.links)
            if (g.lanes[size_t(k.from)].mode == LaneMode::Vehicle)
            {
                adj[size_t(k.from)].push_back(k.to);
                linked.insert({k.from, k.to});
            }
        for (int leg = 0; leg < 2; ++leg)
        {
            TransitRoute& r = legs[leg];
            if (r.lanes.empty())
                continue;
            bool ok = true;
            for (size_t i = 0; i + 1 < r.lanes.size(); ++i)
                ok &= linked.count({r.lanes[i], r.lanes[i + 1]}) > 0;
            const TransitRoute& other = legs[1 - leg];
            if (!other.lanes.empty())
                ok &= reach(adj, {r.lanes.back()})[size_t(other.lanes.front())] != 0;
            else
                ok = false;
            r.reachable = ok;
        }
        for (TransitRoute& r : legs)
            g.routes.push_back(std::move(r));
    };
    const size_t n = order.size();
    if (n < 2)
    {
        TransitRoute r;
        r.mode = "bus";
        r.note = "no districts or exits to run routes between";
        g.routes.push_back(std::move(r));
        return;
    }
    for (int k = 0; k < buses; ++k)
        build("bus", k + 1, order[size_t(k) % n], order[(size_t(k) + n / 2) % n]);
    for (int k = 0; k < trams; ++k)
        build("tram", k + 1, order[(size_t(k) * 2 + 1) % n], order[(size_t(k) * 2 + 1 + n / 2) % n]);
}

json checkLaneGraph(const LaneGraph& g, const RoadNetwork& net)
{
    LILA_ZONE();

    json out = json::object();
    bool ok = true;
    json modes = json::object();
    for (LaneMode mode : {LaneMode::Vehicle, LaneMode::Pedestrian})
    {
        std::vector<int> lanes;
        for (const Lane& x : g.lanes)
            if (x.mode == mode)
                lanes.push_back(x.id);
        json m = {{"lanes", lanes.size()}, {"components", 0}, {"isolated", json::array()}, {"cannotReachExit", json::array()}, {"unreachableExits", json::array()}};
        if (lanes.empty())
        {
            modes[laneModeName(mode)] = m;
            continue;
        }
        const std::vector<std::vector<int>> adj = adjacency(g, mode), radj = adjacency(g, mode, true);
        int count = 0;
        const std::vector<int> comp = laneComponents(adj, &count);
        std::map<int, int> sizes;
        for (int x : lanes)
            ++sizes[comp[size_t(x)]];
        int main = -1, mainSize = 0;
        for (const auto& [c, s] : sizes)
            if (s > mainSize)
            {
                mainSize = s;
                main = c;
            }
        m["components"] = sizes.size();
        json isolated = json::array();
        for (int x : lanes)
            if (comp[size_t(x)] != main && isolated.size() < 50)
                isolated.push_back({{"lane", x}, {"street", g.lanes[size_t(x)].street}, {"line", g.lanes[size_t(x)].line}});
        m["isolated"] = isolated;
        m["isolatedCount"] = lanes.size() - size_t(mainSize);
        ok &= sizes.size() == 1;
        if (mode == LaneMode::Vehicle)
        {
            std::vector<int> exitsIn, exitsOut;
            for (int x : lanes)
            {
                if (g.lanes[size_t(x)].exit)
                    exitsIn.push_back(x);
                if (g.lanes[size_t(x)].entry)
                    exitsOut.push_back(x);
            }
            m["exits"] = exitsIn.size();
            if (!exitsIn.empty())
            {
                const std::vector<char> toExit = reach(radj, exitsIn);
                int mainLane = -1;
                for (int x : lanes)
                    if (comp[size_t(x)] == main)
                        mainLane = x;
                const std::vector<char> fromMain = reach(adj, {mainLane});
                json cannot = json::array(), unreached = json::array();
                int nCannot = 0, nUnreached = 0;
                for (int x : lanes)
                    if (!toExit[size_t(x)] && ++nCannot <= 50)
                        cannot.push_back(x);
                for (int x : exitsOut)
                    if (!fromMain[size_t(x)] && ++nUnreached <= 50)
                        unreached.push_back(x);
                m["cannotReachExit"] = cannot;
                m["unreachableExits"] = unreached;
                ok &= nCannot == 0 && nUnreached == 0;
            }
        }
        modes[laneModeName(mode)] = m;
    }
    out["modes"] = modes;

    // Greens: no two protected movements of one phase cross or merge, and every signalised approach has a phase.
    json conflicts = json::array(), unphased = json::array();
    std::map<std::pair<int, int>, std::vector<const LaneLink*>> greens;
    for (const LaneLink& k : g.links)
        if (k.junction >= 0 && k.phase >= 0 && !k.yields && g.lanes[size_t(k.from)].mode == LaneMode::Vehicle)
            greens[{k.junction, k.phase}].push_back(&k);
    for (const auto& [key, moves] : greens)
        for (size_t a = 0; a < moves.size(); ++a)
            for (size_t b = a + 1; b < moves.size(); ++b)
            {
                const LaneLink &x = *moves[a], &y = *moves[b];
                const Lane &xf = g.lanes[size_t(x.from)], &yf = g.lanes[size_t(y.from)];
                if (xf.line == yf.line && xf.dir == yf.dir)
                    continue; // one approach
                const bool merge = x.to == y.to;
                if (!merge && !pathsCross(connector(xf, g.lanes[size_t(x.to)]), connector(yf, g.lanes[size_t(y.to)])))
                    continue;
                conflicts.push_back({{"junction", key.first},
                                     {"phase", key.second},
                                     {"kind", merge ? "merge" : "cross"},
                                     {"a", {{"from", x.from}, {"to", x.to}, {"move", movementName(x.move)}}},
                                     {"b", {{"from", y.from}, {"to", y.to}, {"move", movementName(y.move)}}}});
            }
    for (size_t j = 0; j < net.junctions.size(); ++j)
    {
        const Junction& jn = net.junctions[j];
        if (jn.control != Control::Signal)
            continue;
        for (size_t e = 0; e < jn.ends.size(); ++e)
        {
            if (!jn.ends[e].vehicle)
                continue;
            bool phased = false;
            for (const std::vector<int>& p : jn.phases)
                phased |= std::ranges::find(p, int(e)) != p.end();
            if (!phased)
                unphased.push_back({{"junction", j}, {"end", e}});
        }
    }
    out["conflictingGreens"] = conflicts;
    out["unphasedApproaches"] = unphased;
    ok &= conflicts.empty() && unphased.empty();

    json routes = json::array();
    for (const TransitRoute& r : g.routes)
        if (!r.reachable)
            routes.push_back({{"route", r.id}, {"name", r.name}, {"mode", r.mode}, {"direction", r.direction}, {"note", r.note}});
    out["unreachableRoutes"] = routes;
    ok &= routes.empty();
    out["surfaceLinesSkipped"] = g.surfaceLinesSkipped;
    out["ok"] = ok;
    return out;
}

json checkLanes(const Layout& l, const Ground& ground, const TransitOptions& o)
{
    const RoadNetwork net = roadNetwork(l, ground);
    LaneGraph g = laneGraph(l, net);
    addTransit(g, l, net, o);
    return checkLaneGraph(g, net);
}

json toJson(const LaneGraph& g, bool lanes)
{
    json counts = json::object(), moves = json::object();
    for (const Lane& x : g.lanes)
        counts[laneModeName(x.mode)] = counts.value(laneModeName(x.mode), 0) + 1;
    for (const LaneLink& k : g.links)
        moves[movementName(k.move)] = moves.value(movementName(k.move), 0) + 1;
    json routes = json::array();
    for (const TransitRoute& r : g.routes)
    {
        json stops = json::array();
        for (const TransitStop& s : r.stops)
            stops.push_back({{"id", s.id}, {"name", s.name}, {"lane", s.lane}, {"point", json::array({s.p.x, s.p.y, s.p.z})}});
        routes.push_back({{"id", r.id},
                          {"mode", r.mode},
                          {"name", r.name},
                          {"direction", r.direction},
                          {"lanes", r.lanes},
                          {"stops", stops},
                          {"lengthM", r.length / 100.0},
                          {"reachable", r.reachable},
                          {"note", r.note}});
    }
    json out = {{"lanes", counts}, {"links", moves}, {"routes", routes}};
    if (lanes)
    {
        json all = json::array();
        for (const Lane& x : g.lanes)
        {
            json pts = json::array();
            for (const glm::dvec3& p : x.pts)
                pts.push_back(json::array({p.x, p.y, p.z}));
            all.push_back({{"id", x.id},
                           {"mode", laneModeName(x.mode)},
                           {"line", x.line},
                           {"dir", x.dir},
                           {"index", x.index},
                           {"side", x.side},
                           {"street", x.street},
                           {"width", x.width},
                           {"speed", x.speed},
                           {"exit", x.exit},
                           {"entry", x.entry},
                           {"points", pts}});
        }
        json links = json::array();
        for (const LaneLink& k : g.links)
            links.push_back({{"from", k.from}, {"to", k.to}, {"junction", k.junction}, {"move", movementName(k.move)}, {"phase", k.phase}, {"yields", k.yields}});
        out["laneList"] = all;
        out["linkList"] = links;
    }
    return out;
}
} // namespace settlement
