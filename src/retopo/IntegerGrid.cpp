#include "retopo/IntegerGrid.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
constexpr double kQuarter = glm::half_pi<double>();

/** Coordinates turned by k quarter turns. */
glm::dvec2 turn2(glm::dvec2 c, int k)
{
    for (int i = 0; i < ((k % 4) + 4) % 4; ++i)
        c = {-c.y, c.x};
    return c;
}

int mod4(int k) { return ((k % 4) + 4) % 4; }

double cross2(const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; }

/** Barycentric coordinates of p in a chart triangle; they read the same way for a folded one. */
glm::dvec3 barycentric(const Chart& c, const glm::dvec2& p)
{
    const double area = cross2(c[1] - c[0], c[2] - c[0]);
    if (std::abs(area) < 1e-300)
        return glm::dvec3(-1.0);
    return glm::dvec3(cross2(c[1] - p, c[2] - p), cross2(c[2] - p, c[0] - p), cross2(c[0] - p, c[1] - p)) / area;
}

/** From one triangle's chart to a neighbour's across an edge: a quarter-turn rotation then a whole translation. */
struct Transition
{
    int turns = 0;
    glm::dvec2 shift{0.0};
    glm::dvec2 point(const glm::dvec2& p) const { return turn2(p, turns) + shift; }
    glm::dvec2 dir(const glm::dvec2& d) const { return turn2(d, turns); }
};
} // namespace

/**
 * Quad mesh straight from the integer grid of a seamless parametrization (after QEx, Ebke et al. 2013): its vertices
 * are the whole-number points of the charts, its edges trace the integer lines from point to point across triangles and
 * cuts, and its faces close round the grid cells. Pole for pole it is the parametrization's own layout.
 */
Extracted extractIntegerGrid(const Surface& s, const std::vector<Chart>& chart)
{
    const Surface& m = s;
    // Each triangle's surface edge per local side k (corners k and k + 1).
    std::vector<std::array<Index, 3>> triEdges(m.tris.size());
    {
        std::unordered_map<uint64_t, Index> edgeOf;
        for (Index e = 0; e < Index(m.edges.size()); ++e)
            edgeOf.emplace(edgeKey(m.edges[e][0], m.edges[e][1]), e);
        for (size_t t = 0; t < m.tris.size(); ++t)
            for (int k = 0; k < 3; ++k)
                triEdges[t][k] = edgeOf.at(edgeKey(m.tris[t][k], m.tris[t][(k + 1) % 3]));
    }
    LILA_ZONE();

    const Index nt = Index(m.tris.size());
    constexpr double eps = 1e-7;
    auto localEdge = [&](Index t, Index a, Index b) {
        for (int k = 0; k < 3; ++k)
            if ((m.tris[t][k] == a && m.tris[t][(k + 1) % 3] == b) || (m.tris[t][k] == b && m.tris[t][(k + 1) % 3] == a))
                return k;
        return -1;
    };
    // Neighbour across local edge k (corners k and k + 1), or kInvalid at a border.
    auto across = [&](Index t, int k) {
        const Index e = triEdges[t][k];
        if (m.edgeTriCount[e] != 2)
            return kInvalid;
        return m.edgeTris[e][0] == t ? m.edgeTris[e][1] : m.edgeTris[e][0];
    };
    auto transition = [&](Index t, int k, Index o) {
        const Index a = m.tris[t][k], b = m.tris[t][(k + 1) % 3];
        const int ko = localEdge(o, a, b);
        auto cornerIn = [&](Index tri, Index v) {
            for (int c = 0; c < 3; ++c)
                if (m.tris[tri][c] == v)
                    return c;
            return 0;
        };
        const glm::dvec2 pa = chart[t][k], pb = chart[t][(k + 1) % 3];
        const glm::dvec2 qa = chart[o][cornerIn(o, a)], qb = chart[o][cornerIn(o, b)];
        Transition tr;
        if (ko >= 0 && glm::length(pb - pa) > 1e-12 && glm::length(qb - qa) > 1e-12)
            tr.turns = mod4(int(std::lround(std::atan2(cross2(pb - pa, qb - qa), glm::dot(pb - pa, qb - qa)) / kQuarter)));
        const glm::dvec2 sh = qa - turn2(pa, tr.turns);
        tr.shift = glm::dvec2(std::round(sh.x), std::round(sh.y));
        return tr;
    };

    // Grid points: every whole-number point of every unfolded chart triangle, joined across shared edges and corners.
    struct Occurrence
    {
        Index tri;
        glm::dvec2 p;
        glm::dvec3 bary;
    };
    std::vector<Occurrence> occ;
    std::unordered_map<uint64_t, Index> occOf;
    auto key = [](Index t, const glm::dvec2& p) {
        const uint64_t i = uint64_t(int64_t(std::llround(p.x)) + (1 << 19)) & 0xfffff;
        const uint64_t j = uint64_t(int64_t(std::llround(p.y)) + (1 << 19)) & 0xfffff;
        return (uint64_t(t) << 40) | (i << 20) | j;
    };
    for (Index t = 0; t < nt; ++t)
    {
        const Chart& c = chart[t];
        if (cross2(c[1] - c[0], c[2] - c[0]) <= 0.0)
            continue;
        const glm::dvec2 lo = glm::min(c[0], glm::min(c[1], c[2])), hi = glm::max(c[0], glm::max(c[1], c[2]));
        for (double i = std::ceil(lo.x - eps); i <= hi.x + eps; i += 1.0)
            for (double j = std::ceil(lo.y - eps); j <= hi.y + eps; j += 1.0)
            {
                const glm::dvec2 p(i, j);
                const glm::dvec3 b = barycentric(c, p);
                if (std::min(b.x, std::min(b.y, b.z)) < -eps)
                    continue;
                occOf.emplace(key(t, p), Index(occ.size()));
                occ.push_back({t, p, glm::clamp(b, 0.0, 1.0)});
            }
    }
    const Index no = Index(occ.size());
    std::vector<Index> parent(no);
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    // The occurrence a neighbour has of the same point, across the edge opposite corner c.
    auto acrossOcc = [&](Index o, int c) {
        const Index t = occ[o].tri;
        const int k = (c + 1) % 3;
        const Index n = across(t, k);
        if (n == kInvalid)
            return kInvalid;
        auto it = occOf.find(key(n, transition(t, k, n).point(occ[o].p)));
        return it == occOf.end() ? kInvalid : it->second;
    };
    for (Index o = 0; o < no; ++o)
        for (int c = 0; c < 3; ++c)
            if (occ[o].bary[c] <= eps)
                if (Index n = acrossOcc(o, c); n != kInvalid)
                    parent[find(o)] = find(n);

    // The directions leaving each occurrence: the axis directions inside its triangle's angle at the point, start
    // inclusive, so round a shared point each direction belongs to one triangle.
    struct Slot
    {
        Index occ;
        int dir; // quarter turns from +u in the occurrence's chart
        Index vertex;
        int order = 0;
    };
    std::vector<Slot> slots;
    std::vector<Index> nextOcc(no, kInvalid); // the occurrence round the point that follows this one counter-clockwise
    for (Index o = 0; o < no; ++o)
    {
        const Chart& c = chart[occ[o].tri];
        const glm::dvec3& b = occ[o].bary;
        int zeros = 0, zero = -1, one = -1;
        for (int k = 0; k < 3; ++k)
        {
            if (b[k] <= eps)
            {
                ++zeros;
                zero = k;
            }
            else
                one = k;
        }
        double start = 0.0, span = glm::two_pi<double>();
        int endCorner = -1; // the corner opposite the edge the angle ends on
        if (zeros == 1)
        {
            const glm::dvec2 d = c[(zero + 2) % 3] - c[(zero + 1) % 3];
            start = std::atan2(d.y, d.x);
            span = glm::pi<double>();
            endCorner = zero;
        }
        else if (zeros >= 2)
        {
            const glm::dvec2 a = c[(one + 1) % 3] - c[one], e = c[(one + 2) % 3] - c[one];
            start = std::atan2(a.y, a.x);
            span = std::remainder(std::atan2(e.y, e.x) - start, glm::two_pi<double>());
            if (span < 0.0)
                span += glm::two_pi<double>();
            endCorner = (one + 1) % 3;
        }
        if (endCorner >= 0)
            nextOcc[o] = acrossOcc(o, endCorner);
        // At a border the angle closes on the border itself, and the line back along it belongs here too.
        const bool openEnd = endCorner >= 0 && across(occ[o].tri, (endCorner + 1) % 3) == kInvalid;
        for (int dir = 0; dir < 4; ++dir)
        {
            double phi = std::fmod(dir * kQuarter - start, glm::two_pi<double>());
            if (phi < 0.0)
                phi += glm::two_pi<double>();
            if (phi > glm::two_pi<double>() - 1e-9)
                phi = 0.0;
            if (phi < span - 1e-9 || (openEnd && phi < span + 1e-9))
                slots.push_back({o, dir, find(o), int(std::lround(phi * 1e6))});
        }
    }

    // Counter-clockwise order of each grid point's directions, walking its occurrences round the point.
    std::unordered_map<Index, std::vector<Index>> slotsAt;
    for (Index sIdx = 0; sIdx < Index(slots.size()); ++sIdx)
        slotsAt[slots[sIdx].vertex].push_back(sIdx);
    std::vector<int> fanPos(no, 0);
    {
        std::vector<Index> prevOcc(no, kInvalid);
        for (Index o = 0; o < no; ++o)
            if (nextOcc[o] != kInvalid)
                prevOcc[nextOcc[o]] = o;
        std::vector<uint8_t> done(no, 0);
        for (Index o = 0; o < no; ++o)
        {
            if (done[o])
                continue;
            Index first = o;
            for (Index walk = 0, p = prevOcc[o]; p != kInvalid && p != o && walk < 64; ++walk, p = prevOcc[p])
                first = p;
            int pos = 0;
            for (Index cur = first; cur != kInvalid && !done[cur] && pos < 64; cur = nextOcc[cur])
            {
                done[cur] = 1;
                fanPos[cur] = pos++;
            }
        }
    }
    for (auto& [vertex, list] : slotsAt)
        std::sort(list.begin(), list.end(), [&](Index a, Index b) {
            return std::make_pair(fanPos[slots[a].occ], slots[a].order) < std::make_pair(fanPos[slots[b].occ], slots[b].order);
        });
    std::vector<int> rank(slots.size(), 0);
    for (auto& [vertex, list] : slotsAt)
        for (size_t i = 0; i < list.size(); ++i)
            rank[list[i]] = int(i);

    // The slot of a grid point's occurrence in tri leaving along dir, turning through the point's other occurrences if
    // the direction lies in another triangle's angle.
    auto slotFor = [&](Index o, int dir) -> Index {
        const Index vertex = find(o);
        Index cur = o;
        int d = dir;
        for (int walk = 0; walk < 32 && cur != kInvalid; ++walk)
        {
            for (Index sIdx : slotsAt[vertex])
                if (slots[sIdx].occ == cur && slots[sIdx].dir == d)
                    return sIdx;
            const Index n = nextOcc[cur];
            if (n == kInvalid)
                break;
            const Index t = occ[cur].tri, tn = occ[n].tri;
            int k = -1;
            for (int e = 0; e < 3; ++e)
                if (across(t, e) == tn)
                    k = e;
            if (k < 0)
                break;
            d = mod4(d + transition(t, k, tn).turns);
            cur = n;
        }
        return kInvalid;
    };

    // Trace each slot's integer line to the grid point one step along it.
    std::vector<Index> arrive(slots.size(), kInvalid);
    for (Index sIdx = 0; sIdx < Index(slots.size()); ++sIdx)
    {
        Index t = occ[slots[sIdx].occ].tri;
        glm::dvec2 at = occ[slots[sIdx].occ].p;
        glm::dvec2 dir = turn2({1.0, 0.0}, slots[sIdx].dir);
        glm::dvec2 goal = at + dir;
        for (int step = 0; step < 256 && t != kInvalid; ++step)
        {
            const glm::dvec3 bg = barycentric(chart[t], goal);
            if (std::min(bg.x, std::min(bg.y, bg.z)) >= -eps)
            {
                auto it = occOf.find(key(t, goal));
                if (it != occOf.end())
                    arrive[sIdx] = slotFor(it->second, mod4(int(std::lround(std::atan2(-dir.y, -dir.x) / kQuarter))));
                break;
            }
            const glm::dvec3 ba = barycentric(chart[t], at);
            // Through a corner both edges tie; take the one with a triangle beyond it, so a line along a border stays on.
            int exitCorner = -1;
            double first = std::numeric_limits<double>::max();
            for (int c = 0; c < 3; ++c)
                if (bg[c] < -eps)
                {
                    const double sAlong = std::max(0.0, ba[c]) / std::max(1e-300, std::max(0.0, ba[c]) - bg[c]);
                    const bool open = across(t, (c + 1) % 3) != kInvalid;
                    const bool tie = exitCorner >= 0 && std::abs(sAlong - first) < 1e-9;
                    if ((!tie && sAlong < first) || (tie && open))
                    {
                        first = std::min(first, sAlong);
                        exitCorner = c;
                    }
                }
            if (exitCorner < 0)
                break;
            const int k = (exitCorner + 1) % 3;
            const Index n = across(t, k);
            if (n == kInvalid)
                break;
            const Transition tr = transition(t, k, n);
            const glm::dvec2 exitPoint = at + first * (goal - at);
            at = tr.point(exitPoint);
            goal = tr.point(goal);
            goal = glm::dvec2(std::round(goal.x), std::round(goal.y));
            dir = tr.dir(dir);
            t = n;
        }
    }

    // Faces: from each half-edge, at its end turn to the direction just clockwise of the way back.
    std::vector<uint8_t> used(slots.size(), 0);
    std::vector<std::vector<Index>> faces;
    for (Index start = 0; start < Index(slots.size()); ++start)
    {
        if (used[start] || arrive[start] == kInvalid || arrive[arrive[start]] != start)
            continue;
        std::vector<Index> face;
        Index cur = start;
        bool closed = false;
        for (int step = 0; step < 8; ++step)
        {
            if (arrive[cur] == kInvalid || arrive[arrive[cur]] != cur)
                break;
            used[cur] = 1;
            face.push_back(slots[cur].vertex);
            const Index back = arrive[cur];
            const auto& list = slotsAt[slots[back].vertex];
            const Index next = list[(size_t(rank[back]) + list.size() - 1) % list.size()];
            if (next == start)
            {
                closed = true;
                break;
            }
            if (used[next])
                break;
            cur = next;
        }
        if (!closed || face.size() < 3 || face.size() > 6)
            continue;
        std::vector<Index> sorted = face;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            continue;
        faces.push_back(std::move(face));
    }

    // Points where the surface has them, tagged with the feature or border they sit on.
    auto surfaceEdge = [&](Index a, Index b) {
        for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
            if (s.adj[k] == b)
                return s.adjEdge[k];
        return kInvalid;
    };
    Extracted ex;
    std::unordered_map<Index, Index> pointOf;
    std::map<uint64_t, int> edgeUse;
    for (const auto& f : faces)
    {
        bool ok = true;
        for (size_t k = 0; k < f.size() && ok; ++k)
            ok = edgeUse[edgeKey(f[k], f[(k + 1) % f.size()])] < 2;
        if (!ok)
            continue;
        std::vector<Index> pts;
        for (Index v : f)
        {
            auto [it, fresh] = pointOf.try_emplace(v, kInvalid);
            if (fresh)
            {
                const Occurrence& o = occ[v];
                const auto& tri = m.tris[o.tri];
                it->second = ex.mesh.addPoint(o.bary.x * m.pos[tri[0]] + o.bary.y * m.pos[tri[1]] + o.bary.z * m.pos[tri[2]]);
                FeatureKind kind = FeatureKind::None;
                bool border = false;
                int nonzero = 0, at = -1;
                for (int k = 0; k < 3; ++k)
                    if (o.bary[k] > eps)
                    {
                        ++nonzero;
                        at = k;
                    }
                if (nonzero == 1)
                {
                    kind = s.feature[tri[at]];
                    border = s.onBoundary[tri[at]] != 0;
                }
                else if (nonzero == 2)
                {
                    int zero = 0;
                    for (int k = 0; k < 3; ++k)
                        if (o.bary[k] <= eps)
                            zero = k;
                    const Index a = tri[(zero + 1) % 3], b = tri[(zero + 2) % 3];
                    if (Index se = surfaceEdge(a, b); se != kInvalid && s.edgeFeature[se])
                        {
                            kind = FeatureKind::Line;
                            border = s.edgeBoundary[se] != 0;
                        }
                }
                ex.feature.push_back(kind);
                ex.boundary.push_back(border);
            }
            pts.push_back(it->second);
        }
        if (ex.mesh.addFace(pts) == kInvalid)
            continue;
        for (size_t k = 0; k < f.size(); ++k)
            ++edgeUse[edgeKey(f[k], f[(k + 1) % f.size()])];
    }
    return ex;
}
} // namespace lila::retopo::detail
