#include "uv/Flow.h"

#include "core/Profile.h"
#include "retopo/Internal.h"
#include "uv/Bff.h"
#include "uv/Layout.h"
#include "uv/Unwrap.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace lila::uv
{
namespace
{
constexpr Index kMinTriangles = 8;
/** Polygon edges this much in agreement are a grid (hard surface quads), which beats any flow. */
constexpr double kGridAgreement = 0.4;
/** Flow this much in agreement runs straight across the island. */
constexpr double kFlowAgreement = 0.5;
/** How much bigger than its smallest box an island may get to follow its flow. */
constexpr double kFlowRoom = 1.15;
/** Mean |k1 - k2| times the island's size below which an uncreased island counts as flat. */
constexpr double kMinBend = 0.25;
/** How hard a straightened edge's ends are held to one line, against the flow's own pull. */
constexpr double kStraightWeight = 1000.0;
/** Rings round a fold that the repair frees. */
constexpr int kRepairRings = 3;
/** Edges bending this many degrees (a bevel's segments) pull an unwrap's flow along them, short of being creases. */
constexpr double kSoftCrease = 15.0;
/** Faces within this many degrees of one plane make a flat island, laid out as it is. */
constexpr double kFlatAngle = 1.0;

/** The one of q's four quarter turns about n nearest ref. */
glm::dvec3 matchTurn(const glm::dvec3& q, const glm::dvec3& n, const glm::dvec3& ref)
{
    const glm::dvec3 p = glm::cross(n, q);
    const glm::dvec3 options[] = {q, p, -q, -p};
    glm::dvec3 best = q;
    double bestDot = -2.0;
    for (const glm::dvec3& o : options)
        if (const double d = glm::dot(o, ref); d > bestDot)
        {
            bestDot = d;
            best = o;
        }
    return best;
}
} // namespace

namespace
{
/**
 * The chart as a field surface with its creases as features, and its border too with keepBorder; false when it is too
 * small, or (with needBend) too flat to say.
 */
bool flowSurface(const Chart& ch, retopo::detail::Surface& s, bool needBend, bool keepBorder = false)
{
    if (ch.triangleCount() < kMinTriangles)
        return false;
    s.pos = ch.pos;
    s.tris = ch.tris;
    s.density.assign(s.pos.size(), 1.0f);
    s.buildTopology();
    s.buildGeometry();
    s.detectFeatures(40.0, 75.0, 0.0);
    // Seams are where the island was cut, not how the surface runs: a border holds the field only along a crease.
    bool creased = false;
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.onBoundary[v] && !keepBorder)
            s.feature[v] = retopo::detail::FeatureKind::None;
        else
            creased |= s.feature[v] == retopo::detail::FeatureKind::Line;
    if (creased || !needBend)
        return true;
    // A flat island has no flow of its own: the field there is arbitrary.
    const retopo::detail::Curvature k = retopo::detail::estimateCurvature(s);
    double bend = 0.0, area = 0.0;
    for (Index v = 0; v < s.vertexCount(); ++v)
    {
        bend += s.area[v] * k.anisotropy[v];
        area += s.area[v];
    }
    return area > 0.0 && bend / area * std::sqrt(area) >= kMinBend;
}

/** Angle defect per vertex: how far its corners fall short of a flat 360. */
std::vector<double> angleDefects(const retopo::detail::Surface& s)
{
    std::vector<double> defect(s.pos.size(), glm::two_pi<double>());
    for (const auto& t : s.tris)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 a = s.pos[t[(k + 1) % 3]] - s.pos[t[k]], b = s.pos[t[(k + 2) % 3]] - s.pos[t[k]];
            const double la = glm::length(a), lb = glm::length(b);
            if (la > 0.0 && lb > 0.0)
                defect[t[k]] -= std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0));
        }
    return defect;
}

std::vector<glm::dvec3> solveFlow(const retopo::detail::Surface& s, double softCrease = 0.0)
{
    retopo::detail::FieldOptions fo;
    fo.softCreaseAngle = softCrease;
    fo.scale = 4.0 * s.meanEdgeLength();
    fo.jointStiffness = 0.0;
    return retopo::detail::solveCrossField(s, fo);
}
} // namespace

std::vector<glm::dvec3> chartFlow(const Chart& ch)
{
    LILA_ZONE();

    retopo::detail::Surface s;
    if (!flowSurface(ch, s, true))
        return {};
    return solveFlow(s);
}

std::vector<FlowCone> flowCones(const Chart& ch)
{
    LILA_ZONE();

    retopo::detail::Surface s;
    if (!flowSurface(ch, s, false))
        return {};
    const std::vector<glm::dvec3> q = solveFlow(s);
    const std::vector<double> defect = angleDefects(s);
    // The defect summed over two rings, so it reads the same on a fine mesh as on a coarse one.
    auto around = [&](Index v) {
        std::vector<Index> ring{v};
        std::unordered_set<Index> seen{v};
        for (int step = 0; step < 2; ++step)
            for (size_t i = 0, n = ring.size(); i < n; ++i)
                for (Index k = s.adjStart[ring[i]]; k < s.adjStart[ring[i] + 1]; ++k)
                    if (seen.insert(s.adj[k]).second)
                        ring.push_back(s.adj[k]);
        double sum = 0.0;
        for (Index w : ring)
            if (!s.onBoundary[w])
                sum += defect[w];
        return sum;
    };
    std::vector<FlowCone> out;
    for (const retopo::detail::FieldSingularity& sg : retopo::detail::fieldSingularities(s, q))
    {
        // The triangle's corner where the surface is most curved, off the border.
        Index best = kInvalid;
        double most = -1.0;
        for (Index v : s.tris[sg.tri])
            if (!s.onBoundary[v] && std::abs(defect[v]) > most)
            {
                most = std::abs(defect[v]);
                best = v;
            }
        if (best != kInvalid)
            out.push_back({best, sg.index, std::abs(around(best))});
    }
    std::sort(out.begin(), out.end(), [](const FlowCone& a, const FlowCone& b) { return a.curvature > b.curvature; });
    return out;
}

double flowAngle(const Chart& ch, double* agreement)
{
    LILA_ZONE();

    if (agreement)
        *agreement = 0.0;
    const std::vector<glm::dvec3> q = chartFlow(ch);
    if (q.size() != ch.pos.size())
        return 0.0;
    double s = 0.0, c = 0.0, total = 0.0;
    for (const auto& t : ch.tris)
    {
        const glm::dvec3 e1 = ch.pos[t[1]] - ch.pos[t[0]], e2 = ch.pos[t[2]] - ch.pos[t[0]];
        const glm::dvec3 n = glm::cross(e1, e2);
        const double area = 0.5 * glm::length(n);
        if (area <= 0.0)
            continue;
        const glm::dvec3 nn = n / (2.0 * area);
        glm::dvec3 d = q[t[0]];
        d += matchTurn(q[t[1]], nn, d) + matchTurn(q[t[2]], nn, d);
        d -= nn * glm::dot(nn, d);
        // d = a e1 + b e2 in the triangle's plane, carried to uv by the same weights.
        const double g11 = glm::dot(e1, e1), g12 = glm::dot(e1, e2), g22 = glm::dot(e2, e2);
        const double det = g11 * g22 - g12 * g12;
        if (std::abs(det) < 1e-30)
            continue;
        const double r1 = glm::dot(d, e1), r2 = glm::dot(d, e2);
        const double a = (r1 * g22 - r2 * g12) / det, b = (r2 * g11 - r1 * g12) / det;
        const glm::dvec2 u = a * (ch.uv[t[1]] - ch.uv[t[0]]) + b * (ch.uv[t[2]] - ch.uv[t[0]]);
        if (glm::length(u) <= 0.0)
            continue;
        const double ang = 4.0 * std::atan2(u.y, u.x);
        s += area * std::sin(ang);
        c += area * std::cos(ang);
        total += area;
    }
    if (total <= 0.0)
        return 0.0;
    if (agreement)
        *agreement = std::hypot(s, c) / total;
    return -0.25 * std::atan2(s, c);
}

double uprightAngle(const Chart& ch)
{
    if (ch.uv.size() < 2)
        return 0.0;
    double agreement = 0.0;
    const double grid = gridAngle(ch, &agreement);
    if (agreement >= kGridAgreement)
        return grid;
    const std::vector<glm::dvec2> hull = convexHull(ch.uv);
    glm::dvec2 size(0.0);
    const double box = minAreaRectAngle(hull, &size);
    const double flow = flowAngle(ch, &agreement);
    if (agreement < kFlowAgreement)
        return box;
    const double c = std::cos(flow), sn = std::sin(flow);
    glm::dvec2 lo(std::numeric_limits<double>::max()), hi(-std::numeric_limits<double>::max());
    for (const glm::dvec2& p : hull)
    {
        const glm::dvec2 r(p.x * c - p.y * sn, p.x * sn + p.y * c);
        lo = glm::min(lo, r);
        hi = glm::max(hi, r);
    }
    const double flowArea = (hi.x - lo.x) * (hi.y - lo.y), boxArea = size.x * size.y;
    if (flowArea > kFlowRoom * boxArea)
        return box;
    // The flow is the same a quarter turn round; take the one that lays the long side along U, as the box does.
    return hi.y - lo.y > hi.x - lo.x ? flow + 0.5 * glm::pi<double>() : flow;
}

std::vector<Index> flowConeVertices(const Chart& ch, int count, double minCurvature)
{
    const double spacing = 3.0 * std::sqrt(std::max(ch.area3d(), 0.0) / std::max<Index>(1, ch.triangleCount()));
    std::vector<Index> out;
    for (const FlowCone& c : flowCones(ch))
    {
        if (count > 0 ? int(out.size()) >= count : c.curvature < minCurvature)
            break;
        bool near = false;
        for (Index v : out)
            near |= glm::distance(ch.pos[v], ch.pos[c.vertex]) < spacing;
        if (!near)
            out.push_back(c.vertex);
    }
    return out;
}

namespace
{
/** Shortest paths along polygon edges from each vertex to the chart's border or a path before it, as vertex pairs. */
std::vector<std::pair<Index, Index>> pathsToBorder(const Chart& ch, std::span<const Index> from)
{
    const size_t V = ch.pos.size();
    std::vector<std::vector<Index>> adj(V);
    std::unordered_map<uint64_t, int> uses;
    auto key = [](Index a, Index b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
            ++uses[key(t[k], t[(k + 1) % 3])];
    std::vector<uint8_t> reached(V, 0);
    for (const auto& [k, n] : uses)
        if (n == 1)
            reached[Index(k >> 32)] = reached[Index(k & 0xffffffffu)] = 1;
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            const Index a = ch.cornerVertex[k];
            const Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            adj[a].push_back(b);
            adj[b].push_back(a);
        }
    std::vector<std::pair<Index, Index>> out;
    std::vector<double> dist(V);
    std::vector<Index> prev(V);
    for (Index start : from)
    {
        if (reached[start])
            continue;
        std::fill(dist.begin(), dist.end(), std::numeric_limits<double>::infinity());
        std::fill(prev.begin(), prev.end(), kInvalid);
        using Item = std::pair<double, Index>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
        dist[start] = 0.0;
        pq.emplace(0.0, start);
        Index hit = kInvalid;
        while (!pq.empty())
        {
            auto [d, v] = pq.top();
            pq.pop();
            if (d > dist[v])
                continue;
            if (reached[v])
            {
                hit = v;
                break;
            }
            for (Index w : adj[v])
                if (const double nd = d + glm::distance(ch.pos[w], ch.pos[v]); nd < dist[w])
                {
                    dist[w] = nd;
                    prev[w] = v;
                    pq.emplace(nd, w);
                }
        }
        for (Index v = hit; v != kInvalid && prev[v] != kInvalid; v = prev[v])
        {
            out.emplace_back(prev[v], v);
            reached[prev[v]] = 1;
        }
    }
    return out;
}

/** v turned by the smallest rotation that takes unit normal a onto b: across a fold, about the fold. */
glm::dvec3 transport(const glm::dvec3& v, const glm::dvec3& a, const glm::dvec3& b)
{
    const glm::dvec3 k = glm::cross(a, b);
    const double sn = glm::length(k), cs = glm::dot(a, b);
    if (sn < 1e-12)
        return cs >= 0.0 ? v : -v;
    const glm::dvec3 axis = k / sn;
    return v * cs + glm::cross(axis, v) * sn + axis * glm::dot(axis, v) * (1.0 - cs);
}

/** Per triangle, a unit direction of the field in its plane, turned so neighbours across uncut edges agree. */
std::vector<glm::dvec3> combedDirections(const Chart& ch, const std::vector<glm::dvec3>& q, const std::vector<glm::dvec3>& vertexNormal)
{
    const size_t T = ch.tris.size();
    std::vector<glm::dvec3> n(T), d(T, glm::dvec3(0.0));
    for (size_t t = 0; t < T; ++t)
    {
        const auto& tri = ch.tris[t];
        const glm::dvec3 c = glm::cross(ch.pos[tri[1]] - ch.pos[tri[0]], ch.pos[tri[2]] - ch.pos[tri[0]]);
        n[t] = glm::length(c) > 0.0 ? glm::normalize(c) : glm::dvec3(0.0, 0.0, 1.0);
        // Each corner's direction turned from its own tangent plane into the triangle's: at a corner where creases meet
        // the averaged normal is far from every face's, and projecting would turn it.
        const glm::dvec3 q0 = transport(q[tri[0]], vertexNormal[tri[0]], n[t]);
        glm::dvec3 a = q0;
        a += matchTurn(transport(q[tri[1]], vertexNormal[tri[1]], n[t]), n[t], q0) + matchTurn(transport(q[tri[2]], vertexNormal[tri[2]], n[t]), n[t], q0);
        a -= n[t] * glm::dot(n[t], a);
        if (glm::length(a) < 1e-12)
        {
            a = ch.pos[tri[1]] - ch.pos[tri[0]];
            a -= n[t] * glm::dot(n[t], a);
        }
        d[t] = glm::length(a) > 0.0 ? glm::normalize(a) : glm::dvec3(1.0, 0.0, 0.0);
    }
    std::unordered_map<uint64_t, std::vector<size_t>> byEdge;
    auto key = [](Index a, Index b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
    for (size_t t = 0; t < T; ++t)
        for (int k = 0; k < 3; ++k)
            byEdge[key(ch.tris[t][k], ch.tris[t][(k + 1) % 3])].push_back(t);
    std::vector<uint8_t> done(T, 0);
    for (size_t seed = 0; seed < T; ++seed)
    {
        if (done[seed])
            continue;
        done[seed] = 1;
        std::queue<size_t> open;
        open.push(seed);
        while (!open.empty())
        {
            const size_t t = open.front();
            open.pop();
            for (int k = 0; k < 3; ++k)
                for (size_t u : byEdge[key(ch.tris[t][k], ch.tris[t][(k + 1) % 3])])
                    if (!done[u])
                    {
                        // The parent's direction, turned over the shared edge into the neighbour's plane, picks its quarter turn.
                        d[u] = matchTurn(d[u], n[u], transport(d[t], n[t], n[u]));
                        done[u] = 1;
                        open.push(u);
                    }
        }
    }
    return d;
}

/** Per triangle: area and, in a frame of its own (e1, e2 = n x e1), the gradients of its three corner hat functions. */
struct TriangleGrad
{
    double area = 0.0;
    glm::dvec3 e1{0.0}, e2{0.0};
    std::array<glm::dvec2, 3> g{};
};

TriangleGrad triangleGrad(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& p2)
{
    TriangleGrad tg;
    const glm::dvec3 c = glm::cross(p1 - p0, p2 - p0);
    const double twice = glm::length(c);
    if (twice <= 0.0)
        return tg;
    const glm::dvec3 n = c / twice;
    tg.area = 0.5 * twice;
    tg.e1 = glm::normalize(p1 - p0);
    tg.e2 = glm::cross(n, tg.e1);
    const glm::dvec2 x[3] = {{0.0, 0.0}, {glm::dot(p1 - p0, tg.e1), glm::dot(p1 - p0, tg.e2)}, {glm::dot(p2 - p0, tg.e1), glm::dot(p2 - p0, tg.e2)}};
    for (int i = 0; i < 3; ++i)
    {
        // The gradient of corner i's hat is its opposite edge turned a quarter inward, over twice the area.
        const glm::dvec2 e = x[(i + 2) % 3] - x[(i + 1) % 3];
        tg.g[size_t(i)] = glm::dvec2(-e.y, e.x) / twice;
    }
    return tg;
}
size_t foldedCount(const Chart& ch)
{
    size_t n = 0;
    for (const auto& t : ch.tris)
    {
        const glm::dvec2 e1 = ch.uv[t[1]] - ch.uv[t[0]], e2 = ch.uv[t[2]] - ch.uv[t[0]];
        n += e1.x * e2.y - e1.y * e2.x <= 0.0;
    }
    return n;
}

/**
 * Re-solves the folded triangles and a few rings round them with the stretch unwrap (LSCM then SLIM), the rest of the
 * island held where the flow put it. Returns the vertices freed; the chart's own pins come back as they were.
 */
size_t repairFolds(Chart& ch)
{
    const Index V = ch.vertexCount();
    std::vector<std::vector<Index>> adj(V);
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
        {
            adj[t[k]].push_back(t[(k + 1) % 3]);
            adj[t[(k + 1) % 3]].push_back(t[k]);
        }
    std::vector<uint8_t> loose(V, 0);
    std::vector<Index> front;
    for (const auto& t : ch.tris)
    {
        const glm::dvec2 e1 = ch.uv[t[1]] - ch.uv[t[0]], e2 = ch.uv[t[2]] - ch.uv[t[0]];
        if (e1.x * e2.y - e1.y * e2.x <= 0.0)
            for (Index v : t)
                if (!loose[v])
                {
                    loose[v] = 1;
                    front.push_back(v);
                }
    }
    for (int ring = 0; ring < kRepairRings; ++ring)
    {
        std::vector<Index> next;
        for (Index v : front)
            for (Index w : adj[v])
                if (!loose[w])
                {
                    loose[w] = 1;
                    next.push_back(w);
                }
        front = std::move(next);
    }
    const std::vector<uint8_t> pins = ch.pinned;
    const std::vector<glm::dvec2> before = ch.uv;
    size_t freed = 0;
    for (Index v = 0; v < V; ++v)
    {
        ch.pinned[v] = loose[v] ? 0 : 1;
        freed += loose[v];
    }
    UnwrapOptions uo;
    uo.usePins = true;
    const UnwrapResult r = unwrapChart(ch, uo);
    // A repair that moved the held part, or left more folds, isn't one.
    bool held = r.ok;
    for (Index v = 0; v < V && held; ++v)
        held = loose[v] || glm::distance(ch.uv[v], before[v]) < 1e-9 * (1.0 + glm::length(before[v]));
    ch.pinned = pins;
    if (!held)
    {
        ch.uv = before;
        return 0;
    }
    return freed;
}
} // namespace

/**
 * A flat island laid out as it is, projected onto its own plane, turned so its border's edges (weighted by length) run
 * as nearly along U and V as they can. False when the island isn't flat within kFlatAngle.
 */
bool unwrapFlat(Chart& ch)
{
    glm::dvec3 sum(0.0);
    for (const auto& t : ch.tris)
        sum += glm::cross(ch.pos[t[1]] - ch.pos[t[0]], ch.pos[t[2]] - ch.pos[t[0]]);
    if (glm::length(sum) <= 0.0)
        return false;
    const glm::dvec3 n = glm::normalize(sum);
    const double cosFlat = std::cos(glm::radians(kFlatAngle));
    for (const auto& t : ch.tris)
    {
        const glm::dvec3 c = glm::cross(ch.pos[t[1]] - ch.pos[t[0]], ch.pos[t[2]] - ch.pos[t[0]]);
        if (glm::length(c) > 0.0 && glm::dot(glm::normalize(c), n) < cosFlat)
            return false;
    }
    glm::dvec3 e1 = std::abs(n.x) < 0.9 ? glm::cross(n, glm::dvec3(1.0, 0.0, 0.0)) : glm::cross(n, glm::dvec3(0.0, 1.0, 0.0));
    e1 = glm::normalize(e1);
    const glm::dvec3 e2 = glm::cross(n, e1);
    for (size_t v = 0; v < ch.pos.size(); ++v)
        ch.uv[v] = glm::dvec2(glm::dot(ch.pos[v], e1), glm::dot(ch.pos[v], e2));
    // The border's edge directions folded four ways, so edges along U and V both count.
    std::unordered_map<uint64_t, int> uses;
    auto key = [](Index a, Index b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
    for (const auto& t : ch.tris)
        for (int k = 0; k < 3; ++k)
            ++uses[key(t[k], t[(k + 1) % 3])];
    double sn = 0.0, cs = 0.0;
    for (const auto& [k, count] : uses)
        if (count == 1)
        {
            const glm::dvec2 e = ch.uv[Index(k & 0xffffffffu)] - ch.uv[Index(k >> 32)];
            const double l = glm::length(e);
            if (l <= 0.0)
                continue;
            const double a = 4.0 * std::atan2(e.y, e.x);
            sn += l * std::sin(a);
            cs += l * std::cos(a);
        }
    const double turn = -0.25 * std::atan2(sn, cs), c = std::cos(turn), si = std::sin(turn);
    for (glm::dvec2& p : ch.uv)
        p = glm::dvec2(p.x * c - p.y * si, p.x * si + p.y * c);
    return true;
}

FlowUnwrapResult flowUnwrap(Chart& ch, const FlowUnwrapOptions& opts)
{
    LILA_ZONE();

    FlowUnwrapResult res;
    if (ch.triangleCount() > 0 && unwrapFlat(ch))
    {
        res.ok = true;
        res.flat = true;
        return res;
    }
    retopo::detail::Surface s;
    if (!flowSurface(ch, s, false, true))
        return res;
    const std::vector<glm::dvec3> field = solveFlow(s, kSoftCrease);
    if (field.size() != ch.pos.size())
        return res;

    // Darts from the field's singular points out to the border, so the rest combs into one consistent frame.
    // Each from the singular triangle's corner where the surface is most curved, where the turning really sits.
    const std::vector<double> defect = angleDefects(s);
    std::vector<Index> singular;
    for (const retopo::detail::FieldSingularity& sg : retopo::detail::fieldSingularities(s, field))
    {
        Index best = kInvalid;
        for (Index v : s.tris[sg.tri])
            if (!s.onBoundary[v] && (best == kInvalid || std::abs(defect[v]) > std::abs(defect[best])))
                best = v;
        if (best != kInvalid && std::find(singular.begin(), singular.end(), best) == singular.end())
            singular.push_back(best);
    }
    const std::vector<std::pair<Index, Index>> darts = pathsToBorder(ch, singular);
    const std::vector<Index> origin = cutChart(ch, darts);
    res.darts = singular.size();
    std::vector<glm::dvec3> q(ch.pos.size()), normal(ch.pos.size());
    for (size_t v = 0; v < q.size(); ++v)
    {
        q[v] = field[origin[v]];
        normal[v] = s.normal[origin[v]];
    }
    const std::vector<glm::dvec3> dir = combedDirections(ch, q, normal);

    // Least squares for u and v: grad u along the direction, grad v a quarter turn from it, both at unit scale.
    const Index V = ch.vertexCount();
    std::vector<TriangleGrad> grads(ch.tris.size());
    std::vector<Eigen::Triplet<double>> trip;
    Eigen::VectorXd bu = Eigen::VectorXd::Zero(V), bv = Eigen::VectorXd::Zero(V);
    for (size_t t = 0; t < ch.tris.size(); ++t)
    {
        const auto& tri = ch.tris[t];
        const TriangleGrad tg = triangleGrad(ch.pos[tri[0]], ch.pos[tri[1]], ch.pos[tri[2]]);
        grads[t] = tg;
        if (tg.area <= 0.0)
            continue;
        const glm::dvec2 du(glm::dot(dir[t], tg.e1), glm::dot(dir[t], tg.e2)), dv(-du.y, du.x);
        for (int i = 0; i < 3; ++i)
        {
            for (int j = 0; j < 3; ++j)
                trip.emplace_back(int(tri[i]), int(tri[j]), tg.area * glm::dot(tg.g[size_t(i)], tg.g[size_t(j)]));
            bu[tri[i]] += tg.area * glm::dot(tg.g[size_t(i)], du);
            bv[tri[i]] += tg.area * glm::dot(tg.g[size_t(i)], dv);
        }
    }
    // One point held at the origin takes out the free translation.
    double diag = 0.0;
    for (const auto& tp : trip)
        if (tp.row() == tp.col())
            diag += tp.value();
    const double anchor = diag / std::max<Index>(1, V);
    auto solveWith = [&](const std::vector<std::array<Index, 2>>& sameU, const std::vector<std::array<Index, 2>>& sameV, Eigen::VectorXd& u,
                         Eigen::VectorXd& v) {
        auto system = [&](const std::vector<std::array<Index, 2>>& same) {
            std::vector<Eigen::Triplet<double>> all = trip;
            all.emplace_back(0, 0, anchor);
            const double w = kStraightWeight * anchor;
            for (const auto& [a, b] : same)
            {
                all.emplace_back(int(a), int(a), w);
                all.emplace_back(int(b), int(b), w);
                all.emplace_back(int(a), int(b), -w);
                all.emplace_back(int(b), int(a), -w);
            }
            Eigen::SparseMatrix<double> A(V, V);
            A.setFromTriplets(all.begin(), all.end());
            return A;
        };
        Eigen::SimplicialLDLT<Eigen::SparseMatrix<double>> su(system(sameU)), sv(system(sameV));
        if (su.info() != Eigen::Success || sv.info() != Eigen::Success)
            return false;
        u = su.solve(bu);
        v = sv.solve(bv);
        return su.info() == Eigen::Success && sv.info() == Eigen::Success;
    };
    Eigen::VectorXd u, v;
    if (!solveWith({}, {}, u, v))
        return res;

    // Border and crease edges already running within straightAngle of an axis are laid exactly along it.
    std::vector<std::array<Index, 2>> sameU, sameV;
    if (opts.straighten)
    {
        std::unordered_map<uint64_t, int> uses;
        auto key = [](Index a, Index b) { return a < b ? (uint64_t(a) << 32) | b : (uint64_t(b) << 32) | a; };
        for (const auto& t : ch.tris)
            for (int k = 0; k < 3; ++k)
                ++uses[key(t[k], t[(k + 1) % 3])];
        std::unordered_map<uint64_t, Index> surfaceEdge;
        for (Index e = 0; e < Index(s.edges.size()); ++e)
            surfaceEdge[key(s.edges[e][0], s.edges[e][1])] = e;
        const double tol = std::sin(glm::radians(opts.straightAngle));
        for (size_t t = 0; t < ch.tris.size(); ++t)
            for (int k = 0; k < 3; ++k)
            {
                const Index a = ch.tris[t][k], b = ch.tris[t][(k + 1) % 3];
                if (a > b && uses[key(a, b)] > 1)
                    continue;
                const auto it = surfaceEdge.find(key(origin[a], origin[b]));
                const bool line = uses[key(a, b)] == 1 || (it != surfaceEdge.end() && s.edgeFeature[it->second]);
                if (!line)
                    continue;
                const glm::dvec2 e(u[b] - u[a], v[b] - v[a]);
                const double l = glm::length(e);
                if (l <= 0.0)
                    continue;
                if (std::abs(e.y) / l < tol)
                    sameV.push_back({a, b});
                else if (std::abs(e.x) / l < tol)
                    sameU.push_back({a, b});
            }
        if ((!sameU.empty() || !sameV.empty()) && !solveWith(sameU, sameV, u, v))
            return res;
        res.straightened = sameU.size() + sameV.size();
    }
    for (Index i = 0; i < V; ++i)
        ch.uv[i] = glm::dvec2(u[i], v[i]);
    res.folded = foldedCount(ch);
    if (res.folded > 0)
    {
        res.repaired = repairFolds(ch);
        res.folded = foldedCount(ch);
    }
    res.ok = true;
    return res;
}
} // namespace lila::uv
