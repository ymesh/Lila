#include "uv/Seams.h"

#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Normals.h"
#include "uv/Flow.h"
#include "uv/Metrics.h"
#include "uv/OptCuts.h"
#include "uv/Project.h"
#include "uv/Unwrap.h"

#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <tuple>
#include <unordered_map>

namespace lila::uv
{
namespace
{
/** Darts tried on one island before it splits instead. */
constexpr int kMaxDarts = 4;
/** Darts may add up to this share of the island's border length. */
constexpr double kDartShare = 0.35;
/** Angle defect (radians, over two rings) a cap point needs before a dart there is worth trying. */
constexpr double kMinDartCurvature = 0.05;
/** Cap points closer than this (times the square root of the island's area) share one dart. */
constexpr double kDartSpacing = 0.15;

struct Context
{
    const geo::Mesh& mesh;
    AutoSeamOptions opts;
    bool checkDistortion = true;
    std::vector<uint8_t> inSet;     // per face
    std::vector<uint8_t> cut;       // per edge: a seam inside or between islands
    std::vector<uint8_t> dart;      // per edge: cut as a dart, so its island doesn't dart again
    std::vector<uint8_t> mandatory; // per edge: islands never merge across it
    std::vector<double> cost;       // per edge: price of cutting it
    std::vector<double> sharp;      // per edge: 0 flat to 1 at a right angle or more
    std::vector<double> seen;       // per edge: how much the seam views see it, empty without views
    std::vector<glm::dvec3> normal, centroid;
    std::vector<double> area;

    explicit Context(const geo::Mesh& m) : mesh(m) {}
};

/** Raises the cost of every edge a seam view sees: facing it, by how squarely, with nothing in between. */
void applyViews(Context& ctx)
{
    if (ctx.opts.views.empty())
        return;
    LILA_ZONE();

    const geo::Mesh& m = ctx.mesh;
    geo::MeshBvh bvh(m);
    const double eps = glm::length(m.bounds().size()) * 1e-5;
    ctx.seen.assign(m.edgeCapacity(), 0.0);
    tbb::parallel_for(Index(0), m.edgeCapacity(), [&](Index e) {
        if (!m.edgeValid(e) || m.edgeCorner(e) == kInvalid)
            return;
        glm::dvec3 n(0.0);
        m.forEachEdgeCorner(e, [&](Index c) { n += m.faceNormal(m.cornerFace(c)); });
        if (glm::length(n) < 1e-12)
            return;
        n = glm::normalize(n);
        const glm::dvec3 mid = 0.5 * (m.position(m.edgePoint(e, 0)) + m.position(m.edgePoint(e, 1)));
        double seen = 0.0;
        for (const SeamView& v : ctx.opts.views)
        {
            glm::dvec3 d = v.position - mid;
            double dist = glm::length(d);
            if (v.planeNormal)
            {
                const double h = glm::dot(d, *v.planeNormal);
                d = h < 0.0 ? -*v.planeNormal : *v.planeNormal;
                dist = std::abs(h);
            }
            else if (dist >= 1e-12)
                d /= dist;
            if (dist < 1e-12)
                continue;
            const double facing = glm::dot(n, d);
            // Facing away counts a little in the other direction, so seams settle on the far side rather than anywhere unseen.
            if (facing <= 0.0)
                seen += 0.05 * v.weight * facing;
            else if (!bvh.occluded(mid + n * eps, d, dist))
                seen += v.weight * (0.25 + 0.75 * facing);
        }
        ctx.seen[e] = seen;
        ctx.cost[e] *= std::max(0.05, 1.0 + 8.0 * seen);
    });
}

void setup(Context& ctx, std::span<const Index> faces, const std::vector<float>& vis)
{
    LILA_ZONE();

    const geo::Mesh& m = ctx.mesh;
    ctx.inSet.assign(m.faceCapacity(), 0);
    for (Index f : faces)
        ctx.inSet[f] = 1;
    ctx.normal.assign(m.faceCapacity(), glm::dvec3(0.0));
    ctx.centroid.assign(m.faceCapacity(), glm::dvec3(0.0));
    ctx.area.assign(m.faceCapacity(), 0.0);
    tbb::parallel_for(size_t(0), faces.size(), [&](size_t i) {
        Index f = faces[i];
        ctx.normal[f] = m.faceNormal(f);
        ctx.centroid[f] = m.faceCentroid(f);
        ctx.area[f] = m.faceArea(f);
    });
    ctx.cut.assign(m.edgeCapacity(), 0);
    ctx.dart.assign(m.edgeCapacity(), 0);
    ctx.mandatory.assign(m.edgeCapacity(), 0);
    ctx.cost.assign(m.edgeCapacity(), 0.0);
    ctx.sharp.assign(m.edgeCapacity(), 0.0);
    tbb::parallel_for(Index(0), m.edgeCapacity(), [&](Index e) {
        if (!m.edgeValid(e) || m.edgeCorner(e) == kInvalid)
            return;
        Index c = m.edgeCorner(e), r = m.cornerRadial(c);
        double len = m.edgeLength(e);
        double sharpness = 0.0, concave = 1.0;
        glm::dvec3 n = m.faceNormal(m.cornerFace(c));
        if (r != c && m.cornerRadial(r) == c)
        {
            Index f1 = m.cornerFace(c), f2 = m.cornerFace(r);
            double angle = dihedralAngle(m, e);
            sharpness = std::clamp(angle / 90.0, 0.0, 1.0);
            n = glm::normalize(m.faceNormal(f1) + m.faceNormal(f2) + glm::dvec3(1e-12));
            if (glm::dot(m.faceNormal(f1), m.faceCentroid(f2) - m.faceCentroid(f1)) > 1e-9 * len)
                concave = 0.6;
        }
        double v = 1.0;
        if (!vis.empty())
        {
            v = 0.5 * (vis[m.edgePoint(e, 0)] + vis[m.edgePoint(e, 1)]);
            // Undersides are seen less than tops (Y up).
            v *= 0.7 + 0.3 * (0.5 + 0.5 * n.y);
        }
        ctx.sharp[e] = sharpness;
        ctx.cost[e] = std::max(len, 1e-9) * (0.15 + 0.85 * v) * (1.0 - 0.8 * sharpness) * concave;
    });
    applyViews(ctx);
    const std::vector<Index>& mirror = ctx.opts.mirrorEdge;
    for (Index e = 0; e < Index(mirror.size()) && e < m.edgeCapacity(); ++e)
        if (const Index o = mirror[e]; o != kInvalid && o > e && o < m.edgeCapacity())
        {
            ctx.cost[e] = ctx.cost[o] = 0.5 * (ctx.cost[e] + ctx.cost[o]);
            if (!ctx.seen.empty())
                ctx.seen[e] = ctx.seen[o] = std::max(ctx.seen[e], ctx.seen[o]);
        }
}

/** The face across e from f inside the set, or kInvalid. */
Index across(const Context& ctx, Index e, Index f)
{
    const geo::Mesh& m = ctx.mesh;
    Index c = m.edgeCorner(e);
    if (c == kInvalid)
        return kInvalid;
    Index r = m.cornerRadial(c);
    if (r == c || m.cornerRadial(r) != c)
        return kInvalid;
    Index g = m.cornerFace(c) == f ? m.cornerFace(r) : m.cornerFace(c);
    return g != f && ctx.inSet[g] ? g : kInvalid;
}

struct Dual
{
    std::vector<uint32_t> start;
    std::vector<uint32_t> nbr;   // local face
    std::vector<Index> edge;     // mesh edge
};

/** Face adjacency within the island through edges that are not cut. faces must be sorted. */
Dual dualGraph(const Context& ctx, std::span<const Index> faces)
{
    Dual d;
    d.start.assign(faces.size() + 1, 0);
    auto local = [&](Index f) { return uint32_t(std::lower_bound(faces.begin(), faces.end(), f) - faces.begin()); };
    for (size_t i = 0; i < faces.size(); ++i)
    {
        ctx.mesh.forEachFaceCorner(faces[i], [&](Index c) {
            Index e = ctx.mesh.cornerEdge(c);
            if (ctx.cut[e])
                return;
            Index g = across(ctx, e, faces[i]);
            if (g == kInvalid)
                return;
            uint32_t lg = local(g);
            if (lg >= faces.size() || faces[lg] != g)
                return;
            d.nbr.push_back(lg);
            d.edge.push_back(e);
        });
        d.start[i + 1] = uint32_t(d.nbr.size());
    }
    return d;
}

std::vector<double> dualDistances(const Context& ctx, std::span<const Index> faces, const Dual& d, uint32_t from)
{
    std::vector<double> dist(faces.size(), std::numeric_limits<double>::infinity());
    using Item = std::pair<double, uint32_t>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    dist[from] = 0.0;
    pq.emplace(0.0, from);
    while (!pq.empty())
    {
        auto [dd, f] = pq.top();
        pq.pop();
        if (dd > dist[f])
            continue;
        for (uint32_t k = d.start[f]; k < d.start[f + 1]; ++k)
        {
            uint32_t g = d.nbr[k];
            double nd = dd + glm::length(ctx.centroid[faces[g]] - ctx.centroid[faces[f]]);
            if (nd < dist[g])
            {
                dist[g] = nd;
                pq.emplace(nd, g);
            }
        }
    }
    return dist;
}

uint32_t farthest(const std::vector<double>& dist)
{
    uint32_t best = 0;
    for (uint32_t i = 0; i < dist.size(); ++i)
        if (std::isfinite(dist[i]) && dist[i] > dist[best])
            best = i;
    return best;
}

/** Connected parts of each label, as sorted mesh face lists. */
std::vector<std::vector<Index>> labelParts(std::span<const Index> faces, const Dual& d, const std::vector<int>& label)
{
    std::vector<std::vector<Index>> parts;
    std::vector<uint8_t> seen(faces.size(), 0);
    for (uint32_t s = 0; s < faces.size(); ++s)
    {
        if (seen[s])
            continue;
        std::vector<uint32_t> stack{s};
        seen[s] = 1;
        std::vector<Index> part;
        while (!stack.empty())
        {
            uint32_t f = stack.back();
            stack.pop_back();
            part.push_back(faces[f]);
            for (uint32_t k = d.start[f]; k < d.start[f + 1]; ++k)
            {
                uint32_t g = d.nbr[k];
                if (!seen[g] && label[g] == label[f])
                {
                    seen[g] = 1;
                    stack.push_back(g);
                }
            }
        }
        std::sort(part.begin(), part.end());
        parts.push_back(std::move(part));
    }
    return parts;
}

/**
 * Moves a two-region border to the cheapest cut within a few face rings of where it is (min cut, Edmonds-Karp
 * on the dual graph). Straightens staircase borders into short seams that follow creases and stay out of sight.
 */
void refineBorder(const Context& ctx, std::span<const Index> faces, const Dual& d, std::vector<int>& label)
{
    LILA_ZONE();

    const uint32_t n = uint32_t(faces.size());
    const int rings = std::clamp(int(std::sqrt(double(n)) / 8.0), 2, 10);
    std::vector<int> depth(n, -1);
    std::vector<uint32_t> queue;
    for (uint32_t f = 0; f < n; ++f)
        for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
            if (label[d.nbr[j]] != label[f] && depth[f] < 0)
            {
                depth[f] = 0;
                queue.push_back(f);
            }
    for (size_t q = 0; q < queue.size(); ++q)
    {
        uint32_t f = queue[q];
        if (depth[f] + 1 >= rings)
            continue;
        for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
        {
            uint32_t g = d.nbr[j];
            if (depth[g] < 0 && label[g] == label[f])
            {
                depth[g] = depth[f] + 1;
                queue.push_back(g);
            }
        }
    }
    std::vector<int> node(n, -1);
    std::vector<uint32_t> band;
    for (uint32_t f = 0; f < n; ++f)
        if (depth[f] >= 0)
        {
            node[f] = int(band.size());
            band.push_back(f);
        }
    if (band.empty())
        return;
    const int S = int(band.size()), T = S + 1;
    struct Arc
    {
        int to;
        double cap;
    };
    std::vector<Arc> arcs;
    std::vector<std::vector<int>> adj(size_t(T) + 1);
    auto link = [&](int a, int b, double c) {
        adj[size_t(a)].push_back(int(arcs.size()));
        arcs.push_back({b, c});
        adj[size_t(b)].push_back(int(arcs.size()));
        arcs.push_back({a, c});
    };
    for (uint32_t f : band)
        for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
        {
            uint32_t g = d.nbr[j];
            double c = std::max(ctx.cost[d.edge[j]], 1e-12);
            if (node[g] >= 0)
            {
                if (f < g)
                    link(node[f], node[g], c);
            }
            else
                link(node[f], label[g] == 0 ? S : T, c);
        }
    // Edmonds-Karp: few augmentations, since cuts are short.
    std::vector<int> via(size_t(T) + 1);
    for (int iter = 0; iter < 100000; ++iter)
    {
        std::fill(via.begin(), via.end(), -1);
        std::vector<int> bfs{S};
        via[size_t(S)] = -2;
        for (size_t q = 0; q < bfs.size() && via[size_t(T)] == -1; ++q)
            for (int a : adj[size_t(bfs[q])])
                if (arcs[size_t(a)].cap > 1e-12 && via[size_t(arcs[size_t(a)].to)] == -1)
                {
                    via[size_t(arcs[size_t(a)].to)] = a;
                    bfs.push_back(arcs[size_t(a)].to);
                }
        if (via[size_t(T)] == -1)
            break;
        double push = std::numeric_limits<double>::max();
        for (int v = T; v != S; v = arcs[size_t(via[size_t(v)] ^ 1)].to)
            push = std::min(push, arcs[size_t(via[size_t(v)])].cap);
        for (int v = T; v != S; v = arcs[size_t(via[size_t(v)] ^ 1)].to)
        {
            arcs[size_t(via[size_t(v)])].cap -= push;
            arcs[size_t(via[size_t(v)] ^ 1)].cap += push;
        }
    }
    std::vector<uint8_t> source(size_t(T) + 1, 0);
    std::vector<int> stack{S};
    source[size_t(S)] = 1;
    while (!stack.empty())
    {
        int v = stack.back();
        stack.pop_back();
        for (int a : adj[size_t(v)])
            if (arcs[size_t(a)].cap > 1e-12 && !source[size_t(arcs[size_t(a)].to)])
            {
                source[size_t(arcs[size_t(a)].to)] = 1;
                stack.push_back(arcs[size_t(a)].to);
            }
    }
    std::vector<int> next = label;
    for (uint32_t f : band)
        next[f] = source[size_t(node[f])] ? 0 : 1;
    if (std::count(next.begin(), next.end(), 0) > 0 && std::count(next.begin(), next.end(), 1) > 0)
        label = std::move(next);
}

/**
 * Splits an island in two by growing regions from two far apart faces (Lloyd style, a few rounds).
 * Fronts slow down over curvature and sharp edges, so the border lands on creases.
 */
std::vector<std::vector<Index>> splitTwo(const Context& ctx, std::span<const Index> faces)
{
    LILA_ZONE();

    if (faces.size() < 2)
        return {std::vector<Index>(faces.begin(), faces.end())};
    Dual d = dualGraph(ctx, faces);
    uint32_t s0 = farthest(dualDistances(ctx, faces, d, 0));
    uint32_t s1 = farthest(dualDistances(ctx, faces, d, s0));
    if (s0 == s1)
        s1 = s0 == 0 ? 1 : 0;
    uint32_t seeds[2] = {s0, s1};
    std::vector<int> label(faces.size(), -1);
    for (int round = 0; round < 4; ++round)
    {
        glm::dvec3 rn[2] = {ctx.normal[faces[seeds[0]]], ctx.normal[faces[seeds[1]]]};
        if (round > 0)
        {
            rn[0] = rn[1] = glm::dvec3(0.0);
            for (uint32_t f = 0; f < faces.size(); ++f)
                if (label[f] >= 0)
                    rn[label[f]] += ctx.area[faces[f]] * ctx.normal[faces[f]];
            for (glm::dvec3& n : rn)
                n = glm::length(n) > 0.0 ? glm::normalize(n) : glm::dvec3(0.0, 1.0, 0.0);
        }
        std::vector<double> dist(faces.size(), std::numeric_limits<double>::infinity());
        std::fill(label.begin(), label.end(), -1);
        using Item = std::tuple<double, uint32_t, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
        for (int k = 0; k < 2; ++k)
        {
            dist[seeds[k]] = 0.0;
            pq.emplace(0.0, seeds[k], k);
        }
        while (!pq.empty())
        {
            auto [dd, f, k] = pq.top();
            pq.pop();
            if (label[f] >= 0)
                continue;
            label[f] = k;
            for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
            {
                uint32_t g = d.nbr[j];
                if (label[g] >= 0)
                    continue;
                double step = glm::length(ctx.centroid[faces[g]] - ctx.centroid[faces[f]]);
                double bend = 1.0 - glm::dot(ctx.normal[faces[g]], rn[k]);
                double nd = dd + step * (1.0 + 3.0 * bend) * (1.0 + 4.0 * ctx.sharp[d.edge[j]]);
                if (nd < dist[g])
                {
                    dist[g] = nd;
                    pq.emplace(nd, g, k);
                }
            }
        }
        for (int& l : label)
            if (l < 0)
                l = 0;
        if (round == 3)
            break;
        // Recentre each seed on the face deepest inside its region.
        std::vector<int> depth(faces.size(), -1);
        std::vector<uint32_t> queue;
        for (uint32_t f = 0; f < faces.size(); ++f)
        {
            bool border = false;
            for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
                border |= label[d.nbr[j]] != label[f];
            if (border)
            {
                depth[f] = 0;
                queue.push_back(f);
            }
        }
        for (size_t q = 0; q < queue.size(); ++q)
        {
            uint32_t f = queue[q];
            for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
            {
                uint32_t g = d.nbr[j];
                if (depth[g] < 0 && label[g] == label[f])
                {
                    depth[g] = depth[f] + 1;
                    queue.push_back(g);
                }
            }
        }
        int bestDepth[2] = {-2, -2};
        for (uint32_t f = 0; f < faces.size(); ++f)
            if (depth[f] > bestDepth[label[f]])
            {
                bestDepth[label[f]] = depth[f];
                seeds[label[f]] = f;
            }
        if (bestDepth[0] < -1 || bestDepth[1] < -1)
            break;
    }
    refineBorder(ctx, faces, d, label);
    // Smooth the border: faces mostly surrounded by the other region join it, which removes notches and tabs.
    std::vector<int> before = label;
    for (int pass = 0; pass < 4; ++pass)
    {
        bool changed = false;
        for (uint32_t f = 0; f < faces.size(); ++f)
        {
            int same = 0, other = 0;
            for (uint32_t j = d.start[f]; j < d.start[f + 1]; ++j)
                (label[d.nbr[j]] == label[f] ? same : other) += 1;
            if (other >= 2 && other > same)
            {
                label[f] = 1 - label[f];
                changed = true;
            }
        }
        if (!changed)
            break;
    }
    if (std::count(label.begin(), label.end(), 0) == 0 || std::count(label.begin(), label.end(), 1) == 0)
        label = std::move(before);
    return labelParts(faces, d, label);
}

/** Cheapest edge path joining the chart's first boundary loop to another, as mesh edges. */
std::vector<Index> cutPath(const Context& ctx, const Chart& ch, const std::vector<std::vector<Index>>& loops)
{
    const geo::Mesh& m = ctx.mesh;
    const Index V = ch.vertexCount();
    std::unordered_map<Index, int> uses;
    for (Index c : ch.corners)
        ++uses[m.cornerEdge(c)];
    std::vector<std::vector<std::pair<Index, Index>>> adj(V); // (vertex, mesh edge)
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            Index e = m.cornerEdge(ch.corners[k]);
            if (uses[e] < 2 || ctx.cut[e])
                continue;
            Index a = ch.cornerVertex[k];
            Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            adj[a].emplace_back(b, e);
        }
    std::vector<int> loopOf(V, -1);
    for (size_t l = 0; l < loops.size(); ++l)
        for (Index v : loops[l])
            loopOf[v] = int(l);
    std::vector<double> dist(V, std::numeric_limits<double>::infinity());
    std::vector<std::pair<Index, Index>> prev(V, {kInvalid, kInvalid});
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    for (Index v : loops[0])
    {
        dist[v] = 0.0;
        pq.emplace(0.0, v);
    }
    Index hit = kInvalid;
    while (!pq.empty())
    {
        auto [dd, v] = pq.top();
        pq.pop();
        if (dd > dist[v])
            continue;
        if (loopOf[v] > 0)
        {
            hit = v;
            break;
        }
        for (auto [w, e] : adj[v])
        {
            double nd = dd + ctx.cost[e];
            if (nd < dist[w])
            {
                dist[w] = nd;
                prev[w] = {v, e};
                pq.emplace(nd, w);
            }
        }
    }
    std::vector<Index> path;
    for (Index v = hit; v != kInvalid && prev[v].first != kInvalid; v = prev[v].first)
        path.push_back(prev[v].second);
    return path;
}

/** Cheapest edge path from a chart vertex to the chart's border, as mesh edges, leaving out edges in cut. */
std::vector<Index> dartPath(const Context& ctx, const Chart& ch, Index from, std::span<const uint8_t> cut)
{
    const geo::Mesh& m = ctx.mesh;
    const Index V = ch.vertexCount();
    std::unordered_map<Index, int> uses;
    for (Index c : ch.corners)
        ++uses[m.cornerEdge(c)];
    std::vector<std::vector<std::pair<Index, Index>>> adj(V);
    std::vector<uint8_t> border(V, 0);
    for (size_t f = 0; f + 1 < ch.faceStart.size(); ++f)
        for (Index k = ch.faceStart[f]; k < ch.faceStart[f + 1]; ++k)
        {
            const Index e = m.cornerEdge(ch.corners[k]);
            const Index a = ch.cornerVertex[k];
            const Index b = ch.cornerVertex[k + 1 < ch.faceStart[f + 1] ? k + 1 : ch.faceStart[f]];
            if (uses[e] < 2 || cut[e])
            {
                border[a] = border[b] = 1;
                continue;
            }
            adj[a].emplace_back(b, e);
        }
    std::vector<double> dist(V, std::numeric_limits<double>::infinity());
    std::vector<std::pair<Index, Index>> prev(V, {kInvalid, kInvalid});
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> pq;
    dist[from] = 0.0;
    pq.emplace(0.0, from);
    Index hit = kInvalid;
    while (!pq.empty())
    {
        auto [dd, v] = pq.top();
        pq.pop();
        if (dd > dist[v])
            continue;
        if (border[v])
        {
            hit = v;
            break;
        }
        for (auto [w, e] : adj[v])
            if (const double nd = dd + ctx.cost[e]; nd < dist[w])
            {
                dist[w] = nd;
                prev[w] = {v, e};
                pq.emplace(nd, w);
            }
    }
    std::vector<Index> path;
    for (Index v = hit; v != kInvalid && prev[v].first != kInvalid; v = prev[v].first)
        path.push_back(prev[v].second);
    return path;
}

enum class Verdict
{
    Accept,
    Cut,
    Split,
};

struct Evaluation
{
    Verdict verdict = Verdict::Accept;
    std::vector<Index> cuts;
    bool darts = false;
    std::vector<std::vector<Index>> parts;
    Chart chart;
};

Chart islandChart(const Context& ctx, std::span<const Index> faces, size_t* count = nullptr)
{
    ChartOptions co;
    co.useSeamAttr = false;
    co.extraSeams = ctx.cut;
    std::vector<Chart> charts = buildCharts(ctx.mesh, faces, co);
    if (count)
        *count = charts.size();
    return charts.empty() ? Chart{} : std::move(charts.front());
}

/**
 * Darts that let an island flatten: cuts from its flow's cap points (most curved first, up to kMaxDarts) to its border,
 * kept only when they bring it under the distortion limit without passing kDartShare of its border's length. Empty when
 * they don't, or when the island has darts already. The border loop is in the island's own (unflattened) chart.
 */
std::vector<Index> tryDarts(const Context& ctx, std::span<const Index> faces, const std::vector<Index>& loop)
{
    const geo::Mesh& m = ctx.mesh;
    for (Index f : faces)
        for (Index c : m.faceCorners(f))
            if (ctx.dart[m.cornerEdge(c)])
                return {};
    const Chart ch = islandChart(ctx, faces);
    double perimeter = 0.0;
    for (size_t i = 0; i < loop.size(); ++i)
        perimeter += glm::distance(ch.pos[loop[i]], ch.pos[loop[(i + 1) % loop.size()]]);
    std::vector<uint8_t> cut = ctx.cut;
    std::vector<Index> darts, dartedAt;
    double length = 0.0;
    int made = 0;
    const double spacing = kDartSpacing * std::sqrt(ch.area3d());
    const std::vector<FlowCone> found = flowCones(ch);
    for (const FlowCone& cone : found)
    {
        if (cone.index <= 0 || cone.curvature < kMinDartCurvature || made >= kMaxDarts)
            continue;
        // A cone beside one already darted (singular points come in clusters round a cap) is open already.
        bool open = false;
        for (Index v : dartedAt)
            open |= glm::distance(ch.pos[v], ch.pos[cone.vertex]) < spacing;
        if (open)
            continue;
        dartedAt.push_back(cone.vertex);
        const std::vector<Index> path = dartPath(ctx, ch, cone.vertex, cut);
        if (path.empty())
            continue;
        for (Index e : path)
        {
            cut[e] = 1;
            length += m.edgeLength(e);
            darts.push_back(e);
        }
        ++made;
        if (length > kDartShare * perimeter)
            return {};
        ChartOptions co;
        co.useSeamAttr = false;
        co.extraSeams = cut;
        std::vector<Chart> charts = buildCharts(m, faces, co);
        if (charts.size() != 1)
            return {};
        UnwrapOptions uo;
        uo.iterations = 6;
        uo.usePins = false;
        const UnwrapResult r = unwrapChart(charts.front(), uo);
        if (r.ok && r.distortion <= ctx.opts.maxDistortion && !chartOverlaps(charts.front()))
            return darts;
    }
    return {};
}

Evaluation evaluate(const Context& ctx, std::span<const Index> faces)
{
    Evaluation ev;
    ChartOptions co;
    co.useSeamAttr = false;
    co.extraSeams = ctx.cut;
    std::vector<Chart> charts = buildCharts(ctx.mesh, faces, co);
    if (charts.size() > 1)
    {
        ev.verdict = Verdict::Split;
        for (Chart& c : charts)
            ev.parts.push_back(std::move(c.faces));
        return ev;
    }
    Chart& ch = charts.front();
    auto loops = ch.boundaryLoops();
    auto split = [&] {
        ev.parts = splitTwo(ctx, faces);
        if (ev.parts.size() < 2)
        {
            ev.verdict = Verdict::Accept;
            ev.chart = std::move(ch);
            ev.parts.clear();
            return;
        }
        ev.verdict = Verdict::Split;
    };
    if (loops.empty())
    {
        split();
        return ev;
    }
    if (loops.size() >= 2)
    {
        ev.cuts = cutPath(ctx, ch, loops);
        if (ev.cuts.empty())
            split();
        else
            ev.verdict = Verdict::Cut;
        return ev;
    }
    if (ch.euler() != 1)
    {
        split();
        return ev;
    }
    if (!ctx.checkDistortion)
    {
        ev.chart = std::move(ch);
        return ev;
    }
    UnwrapOptions uo;
    uo.iterations = 6;
    uo.usePins = false;
    UnwrapResult r = unwrapChart(ch, uo);
    bool bad = !r.ok || r.distortion > ctx.opts.maxDistortion || chartOverlaps(ch);
    if (bad && faces.size() >= size_t(std::max(2, ctx.opts.minSplitFaces)))
    {
        if (ctx.opts.darts)
            if (std::vector<Index> darts = tryDarts(ctx, faces, loops.front()); !darts.empty())
            {
                ev.cuts = std::move(darts);
                ev.darts = true;
                ev.verdict = Verdict::Cut;
                return ev;
            }
        split();
        return ev;
    }
    ev.chart = std::move(ch);
    return ev;
}

/** Runs islands through evaluate until every one is an acceptable disk. */
std::vector<Chart> settle(Context& ctx, std::vector<std::vector<Index>> queue)
{
    LILA_ZONE();

    std::vector<Chart> done;
    for (int wave = 0; wave < 200 && !queue.empty(); ++wave)
    {
        std::vector<Evaluation> ev(queue.size());
        tbb::parallel_for(size_t(0), queue.size(), size_t(1), [&](size_t i) { ev[i] = evaluate(ctx, queue[i]); });
        std::vector<std::vector<Index>> next;
        for (size_t i = 0; i < queue.size(); ++i)
        {
            switch (ev[i].verdict)
            {
            case Verdict::Accept: done.push_back(std::move(ev[i].chart)); break;
            case Verdict::Cut:
                for (Index e : ev[i].cuts)
                    ctx.cut[e] = 1;
                if (ev[i].darts)
                    for (Index e : ev[i].cuts)
                        ctx.dart[e] = 1;
                next.push_back(std::move(queue[i]));
                break;
            case Verdict::Split:
                for (auto& p : ev[i].parts)
                    next.push_back(std::move(p));
                break;
            }
        }
        queue = std::move(next);
    }
    for (auto& faces : queue)
        done.push_back(islandChart(ctx, faces));
    return done;
}

/** Merges neighbouring islands across soft borders while the union still flattens well. */
void mergeCharts(Context& ctx, std::vector<Chart>& charts)
{
    LILA_ZONE();

    const geo::Mesh& m = ctx.mesh;
    std::vector<uint8_t> alive(charts.size(), 1);
    for (int pass = 0; pass < 6; ++pass)
    {
        std::vector<Index> label(m.faceCapacity(), kInvalid);
        for (size_t i = 0; i < charts.size(); ++i)
            if (alive[i])
                for (Index f : charts[i].faces)
                    label[f] = Index(i);
        std::map<std::pair<Index, Index>, double> shared;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
        {
            if (!m.edgeValid(e) || ctx.cut[e] || ctx.mandatory[e])
                continue;
            Index c = m.edgeCorner(e);
            if (c == kInvalid)
                continue;
            Index g = across(ctx, e, m.cornerFace(c));
            Index f = m.cornerFace(c);
            if (g == kInvalid || !ctx.inSet[f] || label[f] == label[g])
                continue;
            shared[{std::min(label[f], label[g]), std::max(label[f], label[g])}] += m.edgeLength(e);
        }
        std::vector<std::pair<double, std::pair<Index, Index>>> pairs;
        for (auto& [k, len] : shared)
            pairs.emplace_back(len, k);
        std::stable_sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<uint8_t> taken(charts.size(), 0);
        std::vector<std::pair<Index, Index>> batch;
        for (auto& [len, k] : pairs)
        {
            if (taken[k.first] || taken[k.second])
                continue;
            if (charts[k.first].faces.size() + charts[k.second].faces.size() > 20000)
                continue;
            taken[k.first] = taken[k.second] = 1;
            batch.push_back(k);
        }
        if (batch.empty())
            break;
        std::vector<std::optional<Chart>> merged(batch.size());
        tbb::parallel_for(size_t(0), batch.size(), size_t(1), [&](size_t i) {
            std::vector<Index> faces = charts[batch[i].first].faces;
            faces.insert(faces.end(), charts[batch[i].second].faces.begin(), charts[batch[i].second].faces.end());
            std::sort(faces.begin(), faces.end());
            size_t count = 0;
            Chart ch = islandChart(ctx, faces, &count);
            if (count != 1 || ch.boundaryLoops().size() != 1 || ch.euler() != 1)
                return;
            UnwrapOptions uo;
            uo.iterations = 6;
            uo.usePins = false;
            UnwrapResult r = unwrapChart(ch, uo);
            if (r.ok && r.distortion <= ctx.opts.maxDistortion && !chartOverlaps(ch))
                merged[i] = std::move(ch);
        });
        bool any = false;
        for (size_t i = 0; i < batch.size(); ++i)
            if (merged[i])
            {
                charts[batch[i].first] = std::move(*merged[i]);
                charts[batch[i].second] = Chart{};
                alive[batch[i].second] = 0;
                any = true;
            }
        if (!any)
            break;
    }
    std::vector<Chart> out;
    for (size_t i = 0; i < charts.size(); ++i)
        if (alive[i])
            out.push_back(std::move(charts[i]));
    charts = std::move(out);
}

/** Seams on the island borders and cuts; returns how many of the faces' edges are seams. */
Index writeSeams(geo::Mesh& mesh, const Context& ctx, const std::vector<Chart>& charts, bool keep)
{
    std::vector<Index> label(mesh.faceCapacity(), kInvalid);
    for (size_t i = 0; i < charts.size(); ++i)
        for (Index f : charts[i].faces)
            label[f] = Index(i);
    std::span<bool> seam = mesh.edgeAttrs().getOrAdd<bool>(geo::attr::kSeam);
    Index n = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e) || mesh.edgeCorner(e) == kInvalid)
            continue;
        bool touches = false, border = false;
        int count = 0;
        Index first = kInvalid;
        mesh.forEachEdgeCorner(e, [&](Index c) {
            Index f = mesh.cornerFace(c);
            ++count;
            if (ctx.inSet[f])
            {
                touches = true;
                if (first == kInvalid)
                    first = label[f];
                else if (label[f] != first)
                    border = true;
            }
            else
                border = true;
        });
        if (!touches)
            continue;
        bool s = count > 1 && (border || ctx.cut[e]);
        seam[e] = s || (keep && seam[e]);
        n += seam[e] ? 1 : 0;
    }
    return n;
}
} // namespace

std::vector<float> pointVisibility(const geo::Mesh& mesh, int rays)
{
    LILA_ZONE();

    std::vector<float> vis(mesh.pointCapacity(), 1.0f);
    if (mesh.numFaces() == 0 || rays <= 0)
        return vis;
    geo::MeshBvh bvh(mesh);
    std::vector<glm::vec3> normals = geo::pointNormals(mesh);
    double diag = glm::length(mesh.bounds().size());
    double eps = diag * 1e-5;
    // Fibonacci points over the hemisphere, cosine-ish spread.
    std::vector<glm::dvec3> dirs(static_cast<size_t>(rays));
    for (int i = 0; i < rays; ++i)
    {
        double z = 1.0 - (i + 0.5) / rays;
        double r = std::sqrt(std::max(0.0, 1.0 - z * z));
        double phi = i * glm::pi<double>() * (3.0 - std::sqrt(5.0));
        dirs[size_t(i)] = {r * std::cos(phi), r * std::sin(phi), z};
    }
    tbb::parallel_for(Index(0), mesh.pointCapacity(), [&](Index p) {
        if (!mesh.pointValid(p))
            return;
        glm::dvec3 n = glm::dvec3(normals[p]);
        if (glm::length(n) < 0.5)
            return;
        glm::dvec3 u, v;
        planeFrame(n, u, v);
        glm::dvec3 o = mesh.position(p) + n * eps;
        int open = 0;
        for (const glm::dvec3& d : dirs)
            if (!bvh.occluded(o, d.x * u + d.y * v + d.z * n, diag))
                ++open;
        vis[p] = float(open) / float(rays);
    });
    return vis;
}

std::vector<double> seamCosts(const geo::Mesh& mesh, std::span<const Index> faceIds, bool visibility, std::span<const SeamView> views)
{
    LILA_ZONE();

    Context ctx(mesh);
    ctx.opts.views.assign(views.begin(), views.end());
    setup(ctx, liveFaces(mesh, faceIds), visibility ? pointVisibility(mesh) : std::vector<float>{});
    return std::move(ctx.cost);
}

AutoSeamResult autoSeams(geo::Mesh& mesh, std::span<const Index> faceIds, const AutoSeamOptions& opts)
{
    LILA_ZONE();

    if (opts.method == SeamMethod::OptCuts)
        return optCuts(mesh, faceIds, opts).seams;
    AutoSeamResult result;
    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return result;
    Context ctx(mesh);
    ctx.opts = opts;
    setup(ctx, faces, opts.visibility ? pointVisibility(mesh) : std::vector<float>{});

    std::span<const bool> hard = std::as_const(mesh).edgeAttrs().get<bool>(geo::attr::kHard);
    std::span<const bool> seam = std::as_const(mesh).edgeAttrs().get<bool>(geo::attr::kSeam);
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        // A sharp edge in plain view of a seam view is a choice, not a must.
        const bool inView = !ctx.seen.empty() && ctx.seen[e] > 0.5;
        bool must = (dihedralAngle(mesh, e) > opts.hardAngle && !inView) || (opts.useHardEdges && !hard.empty() && hard[e]) ||
                    (opts.keepSeams && !seam.empty() && seam[e]);
        ctx.cut[e] = ctx.mandatory[e] = must ? 1 : 0;
    }
    ChartOptions co;
    co.useSeamAttr = false;
    co.extraSeams = ctx.cut;
    std::vector<Chart> charts = settle(ctx, islandFaces(mesh, faces, co));
    if (opts.merge)
        mergeCharts(ctx, charts);
    result.seamEdges = writeSeams(mesh, ctx, charts, opts.keepSeams);
    result.charts = std::move(charts);
    return result;
}

Index cutToDisks(geo::Mesh& mesh, std::span<const Index> faceIds)
{
    LILA_ZONE();

    std::vector<Index> faces = liveFaces(mesh, faceIds);
    if (faces.empty())
        return 0;
    Context ctx(mesh);
    ctx.checkDistortion = false;
    setup(ctx, faces, {});
    std::span<const bool> seam = std::as_const(mesh).edgeAttrs().get<bool>(geo::attr::kSeam);
    Index before = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        if (!seam.empty() && seam[e] && mesh.edgeValid(e))
        {
            ctx.cut[e] = ctx.mandatory[e] = 1;
            ++before;
        }
    ChartOptions co;
    co.useSeamAttr = false;
    co.extraSeams = ctx.cut;
    std::vector<std::vector<Index>> islands = islandFaces(mesh, faces, co);
    std::vector<Chart> charts = settle(ctx, islands);
    if (charts.size() == islands.size())
    {
        bool same = true;
        for (Index e = 0; e < mesh.edgeCapacity() && same; ++e)
            same = ctx.cut[e] == ctx.mandatory[e];
        if (same)
            return 0;
    }
    writeSeams(mesh, ctx, charts, true);
    std::span<const bool> after = std::as_const(mesh).edgeAttrs().get<bool>(geo::attr::kSeam);
    Index total = 0;
    for (Index e = 0; e < mesh.edgeCapacity(); ++e)
        total += mesh.edgeValid(e) && after[e] ? 1 : 0;
    return total > before ? total - before : 0;
}
} // namespace lila::uv
