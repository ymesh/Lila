#include "retopo/Internal.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::retopo::detail
{
namespace
{
constexpr double kSeamQuarter = glm::half_pi<double>();

double wrapQuarter(double a) { return std::remainder(a, kSeamQuarter); }

/** Signed angle from a to b about n. */
double seamAngleAbout(const glm::dvec3& a, const glm::dvec3& b, const glm::dvec3& n)
{
    return std::atan2(glm::dot(n, glm::cross(a, b)), glm::dot(a, b));
}

/** b turned by k quarter turns about n. */
glm::dvec3 turn(glm::dvec3 b, const glm::dvec3& n, int k)
{
    for (int i = 0; i < ((k % 4) + 4) % 4; ++i)
        b = glm::cross(n, b);
    return b;
}

/** Coordinates turned by k quarter turns. */
glm::dvec2 seamTurn2(glm::dvec2 c, int k)
{
    for (int i = 0; i < ((k % 4) + 4) % 4; ++i)
        c = {-c.y, c.x};
    return c;
}

int seamMod4(int k) { return ((k % 4) + 4) % 4; }

/** The surface with one field direction per triangle, so singularities sit on vertices. */
struct ParamMesh
{
    std::vector<glm::dvec3> pos;
    std::vector<double> scale;
    std::vector<std::array<Index, 3>> tris;
    std::vector<glm::dvec3> normal; // per triangle
    std::vector<glm::dvec3> dir;    // per triangle, unit, in its plane
    Index surfaceVertices = 0;

    // Edges: sorted endpoints and up to two triangles, with the side each triangle has it on.
    std::vector<std::array<Index, 2>> edges;
    std::vector<std::array<Index, 2>> edgeTris;
    std::vector<uint8_t> edgeTriCount;
    std::vector<std::array<Index, 3>> triEdges;
    std::vector<uint8_t> boundary; // per vertex
};

/** Field directions for the triangles round each crease junction, from the junction's sector maps. */
std::unordered_map<Index, glm::dvec3> junctionDirections(const Surface& s)
{
    std::unordered_map<Index, glm::dvec3> out;
    for (const CreaseJunction& j : creaseJunctions(s))
        for (const CreaseJunction::Wedge& w : j.fan)
        {
            const glm::dvec3 n = glm::normalize(s.triNormal[w.tri]);
            const glm::dvec3 e0 = glm::normalize(s.pos[w.next] - s.pos[j.vertex]);
            const double phi = w.before + 0.5 * w.angle;
            // Relative to the wedge's first edge, which sits at angle `before` in its sector.
            const double turnBy = -w.before - (w.alpha - 1.0) * phi;
            out[w.tri] = e0 * std::cos(turnBy) + glm::cross(n, e0) * std::sin(turnBy);
        }
    return out;
}

/** Each triangle's corners in its own chart of the parametrization. */
using SeamChart = std::array<glm::dvec2, 3>;

void buildEdges(ParamMesh& m);

ParamMesh buildParamMesh(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale)
{
    ParamMesh m;
    m.pos = s.pos;
    m.scale = scale;
    m.surfaceVertices = s.vertexCount();
    // The given vertices' directions, matched by quarter turns to the first and averaged in the plane.
    auto meanDir = [&](std::initializer_list<Index> verts, const glm::dvec3& n) {
        glm::dvec3 ref(0.0), sum(0.0);
        for (Index v : verts)
        {
            glm::dvec3 d = q[v] - n * glm::dot(n, q[v]);
            if (glm::length(d) < 1e-12)
                continue;
            d = glm::normalize(d);
            if (glm::length(ref) == 0.0)
                ref = d;
            sum += turn(d, n, int(std::lround(seamAngleAbout(d, ref, n) / kSeamQuarter)));
        }
        return glm::length(sum) > 1e-12 ? glm::normalize(sum) : anyTangent(n);
    };
    const std::unordered_map<Index, glm::dvec3> atJunction = junctionDirections(s);
    for (size_t t = 0; t < s.tris.size(); ++t)
    {
        const auto& tri = s.tris[t];
        glm::dvec3 n = s.triNormal[t];
        if (auto it = atJunction.find(Index(t)); it != atJunction.end())
        {
            m.tris.push_back(tri);
            m.normal.push_back(n);
            m.dir.push_back(it->second);
            continue;
        }
        // A singular triangle's mean direction is arbitrary, so its turn lands on one of its corners, which is where the
        // parametrization wants it.
        m.tris.push_back(tri);
        m.normal.push_back(n);
        m.dir.push_back(meanDir({tri[0], tri[1], tri[2]}, n));
    }

    buildEdges(m);
    return m;
}

/** Edges, the triangles on each side, each triangle's edges and the border vertices, from m.tris. */
void buildEdges(ParamMesh& m)
{
    m.edges.clear();
    m.edgeTris.clear();
    m.edgeTriCount.clear();
    std::unordered_map<uint64_t, Index> edgeOf;
    m.triEdges.assign(m.tris.size(), {kInvalid, kInvalid, kInvalid});
    for (size_t t = 0; t < m.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index a = m.tris[t][k], b = m.tris[t][(k + 1) % 3];
            auto [it, fresh] = edgeOf.try_emplace(edgeKey(a, b), Index(m.edges.size()));
            if (fresh)
            {
                m.edges.push_back({std::min(a, b), std::max(a, b)});
                m.edgeTris.push_back({Index(t), kInvalid});
                m.edgeTriCount.push_back(1);
            }
            else
            {
                Index e = it->second;
                if (m.edgeTriCount[e] < 2)
                    m.edgeTris[e][1] = Index(t);
                m.edgeTriCount[e] = uint8_t(std::min(3, m.edgeTriCount[e] + 1));
            }
            m.triEdges[t][k] = it->second;
        }
    m.boundary.assign(m.pos.size(), 0);
    for (size_t e = 0; e < m.edges.size(); ++e)
        if (m.edgeTriCount[e] != 2)
            m.boundary[m.edges[e][0]] = m.boundary[m.edges[e][1]] = 1;
}

/** Quarter turns k with dir[t] ~ turn(dir[other], k) across edge e, unfolded about the edge. */
int matching(const ParamMesh& m, Index e, Index t, Index other)
{
    glm::dvec3 d = glm::normalize(m.pos[m.edges[e][1]] - m.pos[m.edges[e][0]]);
    double a = seamAngleAbout(d, m.dir[t], m.normal[t]);
    double b = seamAngleAbout(d, m.dir[other], m.normal[other]);
    return seamMod4(int(std::lround((a - b) / kSeamQuarter)));
}

/**
 * Per vertex, whether the field turns round it: the quarter-wrapped jumps of the field across the fan's edges, measured
 * against those edges, plus the angle defect the fan closes over, come to a whole quarter turn or more.
 */
std::vector<uint8_t> singularVertices(const ParamMesh& m)
{
    std::vector<double> turn(m.pos.size(), glm::two_pi<double>());
    for (size_t t = 0; t < m.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index v = m.tris[t][k], next = m.tris[t][(k + 1) % 3], prev = m.tris[t][(k + 2) % 3];
            glm::dvec3 toNext = glm::normalize(m.pos[next] - m.pos[v]), toPrev = glm::normalize(m.pos[prev] - m.pos[v]);
            double gamma = seamAngleAbout(toNext, toPrev, m.normal[t]);
            turn[v] -= gamma;
            // Leaving t across edge v-prev into the triangle that has it as its first edge from v.
            Index e = m.triEdges[t][(k + 2) % 3];
            if (m.edgeTriCount[e] != 2)
                continue;
            Index other = m.edgeTris[e][0] == Index(t) ? m.edgeTris[e][1] : m.edgeTris[e][0];
            double leaving = seamAngleAbout(toNext, m.dir[t], m.normal[t]) - gamma;
            double entering = seamAngleAbout(toPrev, m.dir[other], m.normal[other]);
            turn[v] += wrapQuarter(entering - leaving);
        }
    std::vector<uint8_t> out(m.pos.size(), 0);
    for (size_t v = 0; v < m.pos.size(); ++v)
        out[v] = !m.boundary[v] && std::lround(turn[v] / kSeamQuarter) != 0;
    return out;
}


double seamCross2(const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; }

/** Barycentric coordinates of p in a chart triangle; they read the same way for a folded one. */
glm::dvec3 barycentric(const SeamChart& c, const glm::dvec2& p)
{
    const double area = seamCross2(c[1] - c[0], c[2] - c[0]);
    if (std::abs(area) < 1e-300)
        return glm::dvec3(-1.0);
    return glm::dvec3(seamCross2(c[1] - p, c[2] - p), seamCross2(c[2] - p, c[0] - p), seamCross2(c[0] - p, c[1] - p)) / area;
}

/** From one triangle's chart to a neighbour's across an edge: a quarter-turn rotation then a whole translation. */
struct Transition
{
    int turns = 0;
    glm::dvec2 shift{0.0};
    glm::dvec2 point(const glm::dvec2& p) const { return seamTurn2(p, turns) + shift; }
    glm::dvec2 dir(const glm::dvec2& d) const { return seamTurn2(d, turns); }
};

/**
 * Smooths the charts over the folded parts of a parametrization: each free vertex there moves to the mean of its
 * neighbours, read through the transitions round its fan, the way a Tutte embedding unfolds a disk. Vertices in fixed
 * keep their place, so the whole-number singularities, features and borders stay put.
 */
void untangleCharts(const ParamMesh& m, std::vector<SeamChart>& chart, const std::vector<uint8_t>& fixed, int rings, int sweeps)
{
    LILA_ZONE();

    const Index nt = Index(m.tris.size()), nv = Index(m.pos.size());
    auto folded = [&](Index t) { return seamCross2(chart[t][1] - chart[t][0], chart[t][2] - chart[t][0]) <= 0.0; };
    std::vector<uint8_t> loose(nv, 0);
    for (Index t = 0; t < nt; ++t)
        if (folded(t))
            for (Index v : m.tris[t])
                loose[v] = 1;
    for (int ring = 0; ring < rings; ++ring)
    {
        std::vector<uint8_t> grown = loose;
        for (Index t = 0; t < nt; ++t)
            if (loose[m.tris[t][0]] || loose[m.tris[t][1]] || loose[m.tris[t][2]])
                for (Index v : m.tris[t])
                    grown[v] = 1;
        loose.swap(grown);
    }
    std::vector<std::vector<Index>> fan(nv);
    for (Index t = 0; t < nt; ++t)
        for (Index v : m.tris[t])
            if (loose[v] && !fixed[v] && !m.boundary[v])
                fan[v].push_back(t);
    auto cornerIn = [&](Index t, Index v) {
        for (int c = 0; c < 3; ++c)
            if (m.tris[t][c] == v)
                return c;
        return -1;
    };
    // Per loose vertex, the transition from its first fan triangle's chart to each of the others', found by walking
    // the fan across the edges it shares round the vertex.
    std::vector<std::vector<Transition>> toFan(nv);
    for (Index v = 0; v < nv; ++v)
    {
        if (fan[v].empty())
            continue;
        const auto& tris = fan[v];
        std::vector<Transition> tr(tris.size());
        std::vector<uint8_t> have(tris.size(), 0);
        have[0] = 1;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (size_t i = 0; i < tris.size(); ++i)
                if (have[i])
                    for (size_t j = 0; j < tris.size(); ++j)
                    {
                        if (have[j])
                            continue;
                        // A shared edge through v: the other corner they have in common.
                        Index other = kInvalid;
                        for (Index a : m.tris[tris[i]])
                            if (a != v && cornerIn(tris[j], a) >= 0)
                                other = a;
                        if (other == kInvalid)
                            continue;
                        const glm::dvec2 pa = chart[tris[i]][cornerIn(tris[i], v)], pb = chart[tris[i]][cornerIn(tris[i], other)];
                        const glm::dvec2 qa = chart[tris[j]][cornerIn(tris[j], v)], qb = chart[tris[j]][cornerIn(tris[j], other)];
                        Transition step;
                        if (glm::length(pb - pa) > 1e-12 && glm::length(qb - qa) > 1e-12)
                            step.turns = seamMod4(int(std::lround(std::atan2(seamCross2(pb - pa, qb - qa), glm::dot(pb - pa, qb - qa)) / kSeamQuarter)));
                        const glm::dvec2 sh = qa - seamTurn2(pa, step.turns);
                        step.shift = glm::dvec2(std::round(sh.x), std::round(sh.y));
                        tr[j].turns = seamMod4(tr[i].turns + step.turns);
                        tr[j].shift = seamTurn2(tr[i].shift, step.turns) + step.shift;
                        have[j] = 1;
                        grew = true;
                    }
        }
        if (std::find(have.begin(), have.end(), uint8_t(0)) != have.end())
        {
            fan[v].clear();
            continue;
        }
        toFan[v] = std::move(tr);
    }
    for (int sweep = 0; sweep < sweeps; ++sweep)
        for (Index v = 0; v < nv; ++v)
        {
            if (fan[v].empty())
                continue;
            glm::dvec2 sum(0.0);
            int count = 0;
            for (size_t i = 0; i < fan[v].size(); ++i)
            {
                const Index t = fan[v][i];
                const Transition& tr = toFan[v][i];
                for (int c = 0; c < 3; ++c)
                    if (m.tris[t][c] != v)
                    {
                        // Back from this triangle's chart into the first one's.
                        sum += seamTurn2(chart[t][c] - tr.shift, -tr.turns);
                        ++count;
                    }
            }
            const glm::dvec2 p = sum / double(count);
            for (size_t i = 0; i < fan[v].size(); ++i)
                chart[fan[v][i]][cornerIn(fan[v][i], v)] = toFan[v][i].point(p);
        }
}

/**
 * Quad mesh straight from the integer grid of a seamless parametrization (after QEx, Ebke et al. 2013): its vertices
 * are the whole-number points of the charts, its edges trace the integer lines from point to point across triangles and
 * cuts, and its faces close round the grid cells. Pole for pole it is the parametrization's own layout.
 */
Extracted extractIntegerGrid(const Surface& s, const ParamMesh& m, const std::vector<SeamChart>& chart)
{
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
        const Index e = m.triEdges[t][k];
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
            tr.turns = seamMod4(int(std::lround(std::atan2(seamCross2(pb - pa, qb - qa), glm::dot(pb - pa, qb - qa)) / kSeamQuarter)));
        const glm::dvec2 sh = qa - seamTurn2(pa, tr.turns);
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
        const SeamChart& c = chart[t];
        if (seamCross2(c[1] - c[0], c[2] - c[0]) <= 0.0)
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
        const SeamChart& c = chart[occ[o].tri];
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
            double phi = std::fmod(dir * kSeamQuarter - start, glm::two_pi<double>());
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
            d = seamMod4(d + transition(t, k, tn).turns);
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
        glm::dvec2 dir = seamTurn2({1.0, 0.0}, slots[sIdx].dir);
        glm::dvec2 goal = at + dir;
        for (int step = 0; step < 256 && t != kInvalid; ++step)
        {
            const glm::dvec3 bg = barycentric(chart[t], goal);
            if (std::min(bg.x, std::min(bg.y, bg.z)) >= -eps)
            {
                auto it = occOf.find(key(t, goal));
                if (it != occOf.end())
                    arrive[sIdx] = slotFor(it->second, seamMod4(int(std::lround(std::atan2(-dir.y, -dir.x) / kSeamQuarter))));
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
                if (nonzero == 1 && tri[at] < m.surfaceVertices)
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
                    if (a < m.surfaceVertices && b < m.surfaceVertices)
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
} // namespace

std::vector<glm::dvec3> solveSeamlessPositions(const Surface& s, const std::vector<glm::dvec3>& q, const std::vector<double>& scale,
                                               std::string* error, std::vector<std::array<int, 2>>* edgeSteps, Extracted* grid,
                                               int foldRetries, Index* folds, ContinuousCharts* continuous)
{
    LILA_ZONE();

    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        return std::vector<glm::dvec3>{};
    };
    ParamMesh m = buildParamMesh(s, q, scale);
    // For the continuous charts a triangle touching a cone takes its direction from its other corners, since the field at
    // the cone itself points nowhere in particular.
    if (continuous && m.tris.size() == s.tris.size())
    {
        const std::vector<uint8_t> cone = singularVertices(m);
        for (size_t t = 0; t < m.tris.size(); ++t)
        {
            const auto& tri = m.tris[t];
            if (!cone[tri[0]] && !cone[tri[1]] && !cone[tri[2]])
                continue;
            const glm::dvec3 n = m.normal[t];
            glm::dvec3 ref(0.0), sum(0.0);
            for (Index v : tri)
            {
                if (cone[v])
                    continue;
                glm::dvec3 d = q[v] - n * glm::dot(n, q[v]);
                if (glm::length(d) < 1e-12)
                    continue;
                d = glm::normalize(d);
                if (glm::length(ref) == 0.0)
                    ref = d;
                sum += turn(d, n, int(std::lround(seamAngleAbout(d, ref, n) / kSeamQuarter)));
            }
            if (glm::length(sum) > 1e-12)
                m.dir[t] = glm::normalize(sum);
        }
        if (std::getenv("LILA_SEAMLESS_DEBUG"))
        {
            const std::vector<uint8_t> after = singularVertices(m);
            int a = 0, b = 0;
            for (size_t v = 0; v < cone.size(); ++v)
            {
                a += cone[v];
                b += after[v];
            }
            std::fprintf(stderr, "seamless: cone fan directions from their other corners, cones %d -> %d\n", a, b);
        }
    }
    const Index nt = Index(m.tris.size()), nv = Index(m.pos.size()), ne = Index(m.edges.size());

    // Comb: turn each triangle's direction so it matches its parent in a spanning tree of the triangles, then the tree's
    // edges need no transition.
    std::vector<uint8_t> treeEdge(ne, 0), seen(nt, 0);
    for (Index root = 0; root < nt; ++root)
    {
        if (seen[root])
            continue;
        seen[root] = 1;
        std::vector<Index> todo = {root};
        for (size_t head = 0; head < todo.size(); ++head)
        {
            Index t = todo[head];
            for (int k = 0; k < 3; ++k)
            {
                Index e = m.triEdges[t][k];
                if (m.edgeTriCount[e] != 2)
                    continue;
                Index other = m.edgeTris[e][0] == t ? m.edgeTris[e][1] : m.edgeTris[e][0];
                if (seen[other])
                    continue;
                seen[other] = 1;
                treeEdge[e] = 1;
                m.dir[other] = turn(m.dir[other], m.normal[other], matching(m, e, t, other));
                todo.push_back(other);
            }
        }
    }

    // Cut graph: the edges the tree doesn't cross, pruned back to what joins singularities, borders and handles.
    std::vector<uint8_t> singular = singularVertices(m);
    std::vector<uint8_t> cut(ne, 0);
    std::vector<int> degree(nv, 0);
    for (Index e = 0; e < ne; ++e)
        if (m.edgeTriCount[e] == 2 && !treeEdge[e])
        {
            cut[e] = 1;
            ++degree[m.edges[e][0]];
            ++degree[m.edges[e][1]];
        }
    {
        std::vector<std::vector<Index>> vertexEdges(nv);
        for (Index e = 0; e < ne; ++e)
            if (cut[e])
            {
                vertexEdges[m.edges[e][0]].push_back(e);
                vertexEdges[m.edges[e][1]].push_back(e);
            }
        std::vector<Index> todo;
        auto prunable = [&](Index v) { return degree[v] == 1 && !singular[v] && !m.boundary[v]; };
        for (Index v = 0; v < nv; ++v)
            if (prunable(v))
                todo.push_back(v);
        while (!todo.empty())
        {
            Index v = todo.back();
            todo.pop_back();
            if (!prunable(v))
                continue;
            for (Index e : vertexEdges[v])
                if (cut[e])
                {
                    cut[e] = 0;
                    Index w = m.edges[e][0] == v ? m.edges[e][1] : m.edges[e][0];
                    --degree[v];
                    --degree[w];
                    if (prunable(w))
                        todo.push_back(w);
                    break;
                }
        }
    }

    // Wedges: the corners round a vertex not separated by a cut (or a border) share one parameter value.
    std::vector<Index> parent(size_t(nt) * 3);
    std::iota(parent.begin(), parent.end(), Index(0));
    auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    auto cornerOf = [&](Index t, Index v) {
        for (int k = 0; k < 3; ++k)
            if (m.tris[t][k] == v)
                return t * 3 + Index(k);
        return kInvalid;
    };
    for (Index e = 0; e < ne; ++e)
        if (m.edgeTriCount[e] == 2 && !cut[e])
            for (Index v : m.edges[e])
                parent[find(cornerOf(m.edgeTris[e][0], v))] = find(cornerOf(m.edgeTris[e][1], v));
    std::vector<Index> wedgeOf(size_t(nt) * 3);
    std::unordered_map<Index, Index> wedgeOfRoot;
    for (Index c = 0; c < nt * 3; ++c)
    {
        auto [it, fresh] = wedgeOfRoot.try_emplace(find(c), Index(wedgeOfRoot.size()));
        wedgeOf[c] = it->second;
    }
    const Index nw = Index(wedgeOfRoot.size());

    // Each cut edge has an integer translation; along a chain the shared vertices tie them together.
    std::vector<Index> cutIndex(ne, kInvalid);
    Index nc = 0;
    for (Index e = 0; e < ne; ++e)
        if (cut[e])
            cutIndex[e] = nc++;

    // Unknowns: (u, v) per wedge, then (ju, jv) per cut edge.
    const Index nx = 2 * nw + 2 * nc;
    auto jVar = [&](Index c) { return 2 * nw + 2 * c; };

    // Gradient fit: across each triangle edge, the parameter changes by the edge in the triangle's frame over the spacing.
    std::vector<Eigen::Triplet<double>> hTrip;
    Eigen::VectorXd rhs = Eigen::VectorXd::Zero(nx);
    double meanW = 0.0;
    for (Index t = 0; t < nt; ++t)
    {
        const auto& tri = m.tris[t];
        glm::dvec3 x = m.dir[t], y = glm::cross(m.normal[t], x);
        double h = (m.scale[tri[0]] + m.scale[tri[1]] + m.scale[tri[2]]) / 3.0;
        for (int k = 0; k < 3; ++k)
        {
            Index a = tri[k], b = tri[(k + 1) % 3], o = tri[(k + 2) % 3];
            glm::dvec3 ea = m.pos[a] - m.pos[o], eb = m.pos[b] - m.pos[o];
            double cotan = glm::dot(ea, eb) / std::max(glm::length(glm::cross(ea, eb)), 1e-300);
            double w = std::max(0.5 * cotan, 1e-3);
            meanW += w;
            glm::dvec3 d = m.pos[b] - m.pos[a];
            glm::dvec2 g(glm::dot(d, x) / h, glm::dot(d, y) / h);
            Index wa = wedgeOf[t * 3 + k], wb = wedgeOf[t * 3 + (k + 1) % 3];
            for (int c = 0; c < 2; ++c)
            {
                Index ia = 2 * wa + c, ib = 2 * wb + c;
                hTrip.emplace_back(ia, ia, w);
                hTrip.emplace_back(ib, ib, w);
                hTrip.emplace_back(ia, ib, -w);
                hTrip.emplace_back(ib, ia, -w);
                rhs[ib] += w * g[c];
                rhs[ia] -= w * g[c];
            }
        }
    }
    meanW /= std::max<Index>(1, nt * 3);
    for (Index i = 0; i < nx; ++i)
        hTrip.emplace_back(i, i, 1e-8 * meanW);

    // Transitions: across a cut, P(t side) = turn(P(other side), -k) + j.
    struct Row
    {
        std::vector<std::pair<Index, double>> terms;
        double value = 0.0;
    };
    std::vector<Row> rows;
    {
        for (Index e = 0; e < ne; ++e)
        {
            if (!cut[e])
                continue;
            Index t = m.edgeTris[e][0], o = m.edgeTris[e][1];
            int k = matching(m, e, t, o);
            Index j = jVar(cutIndex[e]);
            for (Index v : m.edges[e])
            {
                Index wa = wedgeOf[cornerOf(t, v)], wb = wedgeOf[cornerOf(o, v)];
                // Rows of turn2(., -k) as a matrix: columns are its images of (1, 0) and (0, 1).
                glm::dvec2 c0 = seamTurn2({1.0, 0.0}, -k), c1 = seamTurn2({0.0, 1.0}, -k);
                for (int r = 0; r < 2; ++r)
                {
                    Row row;
                    row.terms.push_back({2 * wa + r, 1.0});
                    if (c0[r] != 0.0)
                        row.terms.push_back({2 * wb, -c0[r]});
                    if (c1[r] != 0.0)
                        row.terms.push_back({2 * wb + 1, -c1[r]});
                    row.terms.push_back({j + r, -1.0});
                    rows.push_back(std::move(row));
                }
            }
        }
    }

    // Integer variables: cut translations, and singular vertices land on lattice points.
    // Feature lines and borders stay on lattice lines: along each such edge the coordinate across it is constant and
    // whole, and corners land on lattice points.
    std::vector<Index> featureInts;
    {
        std::vector<uint8_t> added(nx, 0);
        auto addInt = [&](Index var) {
            if (!added[var])
            {
                added[var] = 1;
                featureInts.push_back(var);
            }
        };
        auto surfaceEdge = [&](Index a, Index b) {
            for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
                if (s.adj[k] == b)
                    return s.adjEdge[k];
            return kInvalid;
        };
        for (Index e = 0; e < ne; ++e)
        {
            Index a = m.edges[e][0], b = m.edges[e][1];
            if (a >= m.surfaceVertices || b >= m.surfaceVertices)
                continue;
            Index se = surfaceEdge(a, b);
            if (se == kInvalid || !(s.edgeFeature[se] || s.edgeBoundary[se]))
                continue;
            for (int side = 0; side < std::min<int>(2, m.edgeTriCount[e]); ++side)
            {
                Index t = m.edgeTris[e][side];
                glm::dvec3 d = m.pos[b] - m.pos[a];
                glm::dvec3 xd = m.dir[t], yd = glm::cross(m.normal[t], xd);
                int across = std::abs(glm::dot(d, xd)) >= std::abs(glm::dot(d, yd)) ? 1 : 0;
                Index wa = wedgeOf[cornerOf(t, a)], wb = wedgeOf[cornerOf(t, b)];
                rows.push_back({{{2 * wa + across, 1.0}, {2 * wb + across, -1.0}}, 0.0});
                addInt(2 * wa + across);
                addInt(2 * wb + across);
            }
        }
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
                if (Index v = m.tris[t][k]; v < m.surfaceVertices && s.feature[v] == FeatureKind::Corner)
                {
                    addInt(2 * wedgeOf[t * 3 + k]);
                    addInt(2 * wedgeOf[t * 3 + k] + 1);
                }
    }

    // Singular vertices first: their coordinates decide the translations of the cuts that end at them.
    std::vector<Index> singularInts, cutInts;
    {
        std::vector<uint8_t> added(nw, 0);
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
                if (singular[m.tris[t][k]] && !added[wedgeOf[t * 3 + k]])
                {
                    Index w = wedgeOf[t * 3 + k];
                    added[w] = 1;
                    singularInts.insert(singularInts.end(), {2 * w, 2 * w + 1});
                }
    }
    for (Index c = 0; c < nc; ++c)
        cutInts.insert(cutInts.end(), {jVar(c), jVar(c) + 1});
    const bool anyIntegers = !singularInts.empty() || !featureInts.empty() || !cutInts.empty();

    Eigen::SparseMatrix<double> hess(nx, nx);
    hess.setFromTriplets(hTrip.begin(), hTrip.end());
    std::vector<std::pair<Index, double>> fixedVars;
    Eigen::VectorXd x;
    // Transitions and fixed integers enter as stiff penalties, which keeps the system positive definite even where
    // constraints repeat (a chain constrains its shared vertices from both edges).
    const double stiff = 1e6 * meanW;
    // Fixing an integer only adds to its diagonal, so the pattern is analysed once and each round refactors numerically.
    Eigen::SparseMatrix<double> base(nx, nx);
    Eigen::VectorXd baseRhs = rhs;
    {
        std::vector<Eigen::Triplet<double>> trip;
        trip.reserve(hess.nonZeros() + rows.size() * 16);
        for (int k = 0; k < hess.outerSize(); ++k)
            for (Eigen::SparseMatrix<double>::InnerIterator it(hess, k); it; ++it)
                trip.emplace_back(Index(it.row()), Index(it.col()), it.value());
        for (const Row& row : rows)
            for (auto [vi, ci] : row.terms)
            {
                for (auto [vj, cj] : row.terms)
                    trip.emplace_back(vi, vj, stiff * ci * cj);
                baseRhs[vi] += stiff * ci * row.value;
            }
        base.setFromTriplets(trip.begin(), trip.end());
        base.makeCompressed();
    }
    Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> ldlt;
    ldlt.analyzePattern(base);
    auto solve = [&]() {
        Eigen::SparseMatrix<double> a = base;
        Eigen::VectorXd b = baseRhs;
        for (auto [var, value] : fixedVars)
        {
            a.coeffRef(var, var) += stiff;
            b[var] += stiff * value;
        }
        ldlt.factorize(a);
        if (ldlt.info() != Eigen::Success)
            return false;
        Eigen::VectorXd sol = ldlt.solve(b);
        if (ldlt.info() != Eigen::Success || !sol.allFinite())
            return false;
        x = std::move(sol);
        return true;
    };

    // Greedy rounding: fix the integers closest to a whole number first, a growing batch at a time, re-solving between.
    std::vector<uint8_t> isFixed(nx, 0);
    if (!anyIntegers)
        fixedVars.push_back({0, 0.0}); // no cuts and no singularities: pin the translation
    auto countFlipped = [&]() {
        Index flipped = 0;
        for (Index t = 0; t < nt; ++t)
        {
            auto at = [&](int k) { return glm::dvec2(x[2 * wedgeOf[t * 3 + k]], x[2 * wedgeOf[t * 3 + k] + 1]); };
            glm::dvec2 a = at(1) - at(0), b = at(2) - at(0);
            flipped += a.x * b.y - a.y * b.x <= 0.0;
        }
        return flipped;
    };
    const bool debug = std::getenv("LILA_SEAMLESS_DEBUG") != nullptr;
    if (!solve())
        return fail("seamless parametrization didn't solve");
    if (debug)
    {
        Index sing = 0;
        for (Index v = 0; v < nv; ++v)
            sing += singular[v];
        std::fprintf(stderr, "seamless: %d tris, %zu field poles, %d singular verts, %d cut edges, %zu feature ints; continuous flips %d\n",
                     int(nt), fieldSingularities(s, q).size(), int(sing), int(nc), featureInts.size(), int(countFlipped()));
    }
    if (continuous)
    {
        if (m.tris.size() != s.tris.size())
            return fail("the parametrization's triangles don't match the surface's");
        continuous->dir = m.dir;
        continuous->chart.assign(nt, {});
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
            {
                const Index w = wedgeOf[t * 3 + k];
                continuous->chart[t][k] = glm::dvec2(x[2 * w], x[2 * w + 1]);
            }
        for (Index t = 0; t < nt; ++t)
            continuous->folded += seamCross2(continuous->chart[t][1] - continuous->chart[t][0], continuous->chart[t][2] - continuous->chart[t][0]) <= 0.0;
        if (debug)
            std::fprintf(stderr, "  continuous charts: %d folded\n", int(continuous->folded));
        return std::vector<glm::dvec3>(s.pos.begin(), s.pos.end());
    }
    int retries = 0;
    for (const std::vector<Index>* stage : {&singularInts, &featureInts, &cutInts})
    {
        if (debug)
            std::fprintf(stderr, "  before stage of %zu ints: flips %d\n", stage->size(), int(countFlipped()));
        for (int round = 0; round < 64; ++round)
        {
            Budget::checkpoint();
            std::vector<std::pair<double, Index>> open;
            for (Index var : *stage)
                if (!isFixed[var])
                    open.push_back({std::abs(x[var] - std::round(x[var])), var});
            if (open.empty())
                break;
            std::sort(open.begin(), open.end());
            size_t batch = std::max<size_t>(1, open.size() / 3);
            std::vector<Index> chosen;
            for (size_t i = 0; i < open.size() && (i < batch || open[i].first < 0.05); ++i)
                chosen.push_back(open[i].second);
            if (stage != &singularInts)
            {
                for (Index var : chosen)
                {
                    isFixed[var] = 1;
                    fixedVars.push_back({var, std::round(x[var])});
                }
                if (!solve())
                    return fail("seamless parametrization didn't solve after rounding");
                continue;
            }
            // Where singularities land decides most folds: a batch that adds some is undone and split, and a single
            // coordinate that still does tries its other whole value. Capped, so a crowd of singularities stays quick.
            Index before = countFlipped();
            std::vector<std::vector<Index>> work = {chosen};
            while (!work.empty())
            {
                Budget::checkpoint();
                std::vector<Index> vars = std::move(work.back());
                work.pop_back();
                const size_t mark = fixedVars.size();
                const Eigen::VectorXd keep = x;
                for (Index var : vars)
                {
                    isFixed[var] = 1;
                    fixedVars.push_back({var, std::round(x[var])});
                }
                if (!solve())
                    return fail("seamless parametrization didn't solve after rounding");
                const Index after = countFlipped();
                if (after <= before || retries >= foldRetries)
                {
                    before = after;
                    continue;
                }
                ++retries;
                if (vars.size() > 1)
                {
                    for (size_t i = mark; i < fixedVars.size(); ++i)
                        isFixed[fixedVars[i].first] = 0;
                    fixedVars.resize(mark);
                    x = keep;
                    const size_t half = vars.size() / 2;
                    work.emplace_back(vars.begin() + half, vars.end());
                    work.emplace_back(vars.begin(), vars.begin() + half);
                    continue;
                }
                const double nearest = fixedVars.back().second;
                fixedVars.back().second = keep[vars[0]] > nearest ? nearest + 1.0 : nearest - 1.0;
                if (!solve())
                    return fail("seamless parametrization didn't solve after rounding");
                if (const Index other = countFlipped(); other < after)
                    before = other;
                else
                {
                    fixedVars.back().second = nearest;
                    if (!solve())
                        return fail("seamless parametrization didn't solve after rounding");
                    before = after;
                }
            }
        }
    }
    const Index flipped = countFlipped();
    if (debug)
        std::fprintf(stderr, "  final flips %d of %d\n", int(flipped), int(nt));
    if (folds)
        *folds = flipped;

    // Folded triangles mean the rounding forced the lattice through itself; the local position field does better then.
    if (flipped > nt / 200)
        return fail("the seamless parametrization folds");

    if (grid)
    {
        std::vector<SeamChart> chart(nt);
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
            {
                // Whole values held by stiff penalties come out a hair off; snap them so points land exactly on corners and edges.
                const Index w = wedgeOf[t * 3 + k];
                auto snap = [](double v) { return std::abs(v - std::round(v)) < 1e-5 ? std::round(v) : v; };
                chart[t][k] = glm::dvec2(snap(x[2 * w]), snap(x[2 * w + 1]));
            }
        std::vector<uint8_t> held(nv, 0);
        for (Index v = 0; v < nv; ++v)
            held[v] = singular[v] || (v < m.surfaceVertices && s.feature[v] != FeatureKind::None);
        auto foldedCount = [&]() {
            int n = 0;
            for (Index t = 0; t < nt; ++t)
                n += seamCross2(chart[t][1] - chart[t][0], chart[t][2] - chart[t][0]) <= 0.0;
            return n;
        };
        const int foldedBefore = foldedCount();
        untangleCharts(m, chart, held, 2, 50);
        const int foldedAfter = foldedCount();
        if (folds)
            *folds = Index(foldedAfter);
        if (debug)
            std::fprintf(stderr, "  untangle: folded %d -> %d\n", foldedBefore, foldedAfter);
        *grid = extractIntegerGrid(s, m, chart);
    }

    // Each surface vertex's nearest lattice point, from any one of its corners.
    std::vector<glm::dvec3> o(s.vertexCount());
    std::vector<uint8_t> have(s.vertexCount(), 0);
    for (Index t = 0; t < nt; ++t)
        for (int k = 0; k < 3; ++k)
        {
            Index v = m.tris[t][k];
            if (v >= m.surfaceVertices || have[v])
                continue;
            have[v] = 1;
            Index w = wedgeOf[t * 3 + k];
            glm::dvec3 xd = m.dir[t], yd = glm::cross(m.normal[t], xd);
            double u = x[2 * w], vv = x[2 * w + 1];
            o[v] = m.pos[v] - scale[v] * ((u - std::round(u)) * xd + (vv - std::round(vv)) * yd);
        }
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (!have[v])
            o[v] = s.pos[v];
    if (edgeSteps)
    {
        // Within one triangle all three corners share a chart, so rounding there gives the step exactly.
        edgeSteps->assign(s.edges.size(), {0, 0});
        for (Index t = 0; t < nt; ++t)
            for (int k = 0; k < 3; ++k)
            {
                const Index a = m.tris[t][k], b = m.tris[t][(k + 1) % 3];
                if (a >= m.surfaceVertices || b >= m.surfaceVertices)
                    continue;
                Index se = kInvalid;
                for (Index i = s.adjStart[a]; i < s.adjStart[a + 1] && se == kInvalid; ++i)
                    if (s.adj[i] == b)
                        se = s.adjEdge[i];
                if (se == kInvalid)
                    continue;
                const Index wa = wedgeOf[t * 3 + k], wb = wedgeOf[t * 3 + (k + 1) % 3];
                std::array<int, 2> step = {int(std::lround(x[2 * wb]) - std::lround(x[2 * wa])),
                                           int(std::lround(x[2 * wb + 1]) - std::lround(x[2 * wa + 1]))};
                if (s.edges[se][0] != a)
                    step = {-step[0], -step[1]};
                (*edgeSteps)[se] = step;
            }
    }
    return o;
}
} // namespace lila::retopo::detail
