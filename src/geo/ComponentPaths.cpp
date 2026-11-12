#include "geo/ComponentPaths.h"

#include "core/Profile.h"
#include "geo/Topology.h"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <queue>

namespace lila::geo
{
namespace
{
Index sharedPoint(const Mesh& m, Index a, Index b)
{
    for (int i = 0; i < 2; ++i)
        if (m.edgePoint(a, i) == m.edgePoint(b, 0) || m.edgePoint(a, i) == m.edgePoint(b, 1))
            return m.edgePoint(a, i);
    return kInvalid;
}

/** The quad holding a with b opposite it, or kInvalid. */
Index sharedQuad(const Mesh& m, Index a, Index b)
{
    Index found = kInvalid;
    m.forEachEdgeCorner(a, [&](Index c) {
        Index f = m.cornerFace(c);
        if (found == kInvalid && quadOppositeEdge(m, a, f) == b)
            found = f;
    });
    return found;
}

bool loopClosed(const Mesh& m, const std::vector<Index>& loop)
{
    if (loop.size() < 3)
        return false;
    Index shared = sharedPoint(m, loop.back(), loop[loop.size() - 2]);
    if (shared == kInvalid)
        return false;
    return edgeLoopNext(m, loop.back(), m.edgeOtherPoint(loop.back(), shared)) == loop.front();
}

/** The points an ordered edge chain runs through; a closed chain does not repeat its first point. */
std::vector<Index> chainPoints(const Mesh& m, const std::vector<Index>& edges, bool closed)
{
    if (edges.empty())
        return {};
    if (edges.size() == 1)
        return {m.edgePoint(edges[0], 0), m.edgePoint(edges[0], 1)};
    Index shared = sharedPoint(m, edges[0], edges[1]);
    Index cur = m.edgeOtherPoint(edges[0], shared);
    std::vector<Index> out{cur};
    for (Index e : edges)
    {
        cur = m.edgeOtherPoint(e, cur);
        out.push_back(cur);
    }
    if (closed)
        out.pop_back();
    return out;
}

struct Faces
{
    std::vector<Index> faces;
    bool closed = false;
};

Faces ringFaces(const Mesh& m, const std::vector<Index>& ring)
{
    Faces out;
    for (size_t k = 0; k + 1 < ring.size(); ++k)
        if (Index f = sharedQuad(m, ring[k], ring[k + 1]); f != kInvalid)
            out.faces.push_back(f);
    if (ring.size() >= 3)
        if (Index f = sharedQuad(m, ring.back(), ring.front()); f != kInvalid && (out.faces.empty() || f != out.faces.front()))
        {
            out.faces.push_back(f);
            out.closed = true;
        }
    return out;
}

bool ringClosed(const Mesh& m, const std::vector<Index>& ring) { return ringFaces(m, ring).closed; }

/** s[i] to s[j] inclusive, the shorter way round when closed. */
std::vector<Index> arc(const std::vector<Index>& s, size_t i, size_t j, bool closed)
{
    std::vector<Index> out;
    if (!closed)
    {
        for (size_t k = i;; k = i <= j ? k + 1 : k - 1)
        {
            out.push_back(s[k]);
            if (k == j)
                break;
        }
        return out;
    }
    size_t n = s.size(), fwd = (j + n - i) % n, bwd = (i + n - j) % n;
    for (size_t k = 0; k <= std::min(fwd, bwd); ++k)
        out.push_back(s[fwd <= bwd ? (i + k) % n : (i + n - k) % n]);
    return out;
}

/** Keeps the shorter of best and the arc from a to b along seq, if both lie on it. */
void consider(std::optional<std::vector<Index>>& best, const std::vector<Index>& seq, bool closed, Index a, Index b)
{
    auto ia = std::find(seq.begin(), seq.end(), a), ib = std::find(seq.begin(), seq.end(), b);
    if (ia == seq.end() || ib == seq.end())
        return;
    std::vector<Index> path = arc(seq, size_t(ia - seq.begin()), size_t(ib - seq.begin()), closed);
    if (!best || path.size() < best->size())
        best = std::move(path);
}

struct PointPath
{
    std::vector<Index> points;
    std::vector<Index> edges; // edges[i] joins points[i] and points[i + 1]
};

/** Shortest path by edge length from any source point to any target point. */
std::optional<PointPath> shortestPoints(const Mesh& m, std::span<const Index> sources, std::span<const Index> targets)
{
    std::vector<double> dist(m.pointCapacity(), std::numeric_limits<double>::infinity());
    std::vector<Index> via(m.pointCapacity(), kInvalid);
    std::vector<uint8_t> target = makeMask(m.pointCapacity(), targets);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    for (Index s : sources)
        if (m.pointValid(s))
        {
            dist[s] = 0.0;
            queue.push({0.0, s});
        }
    while (!queue.empty())
    {
        auto [d, p] = queue.top();
        queue.pop();
        if (d > dist[p])
            continue;
        if (target[p])
        {
            PointPath out;
            for (Index cur = p;; cur = m.edgeOtherPoint(via[cur], cur))
            {
                out.points.push_back(cur);
                if (via[cur] == kInvalid)
                    break;
                out.edges.push_back(via[cur]);
            }
            std::reverse(out.points.begin(), out.points.end());
            std::reverse(out.edges.begin(), out.edges.end());
            return out;
        }
        m.forEachPointEdge(p, [&](Index e) {
            Index q = m.edgeOtherPoint(e, p);
            double nd = d + m.edgeLength(e);
            if (nd < dist[q])
            {
                dist[q] = nd;
                via[q] = e;
                queue.push({nd, q});
            }
        });
    }
    return std::nullopt;
}

std::vector<Index> uniqueInOrder(const std::vector<Index>& ids)
{
    std::vector<Index> out;
    for (Index i : ids)
        if (std::find(out.begin(), out.end(), i) == out.end())
            out.push_back(i);
    return out;
}
} // namespace

std::vector<Index> faceLoop(const Mesh& mesh, Index e)
{
    LILA_ZONE();

    if (!mesh.edgeValid(e))
        return {};
    return ringFaces(mesh, edgeRing(mesh, e)).faces;
}

std::vector<Index> pointPathBetween(const Mesh& m, Index a, Index b)
{
    LILA_ZONE();

    if (!m.pointValid(a) || !m.pointValid(b))
        return {};
    if (a == b)
        return {a};
    std::optional<std::vector<Index>> best;
    m.forEachPointEdge(a, [&](Index e) {
        std::vector<Index> loop = edgeLoop(m, e);
        bool closed = loopClosed(m, loop);
        consider(best, chainPoints(m, loop, closed), closed, a, b);
    });
    if (best)
        return *best;
    Index from[] = {a}, to[] = {b};
    if (auto path = shortestPoints(m, from, to))
        return path->points;
    return {};
}

std::vector<Index> edgePathBetween(const Mesh& m, Index a, Index b)
{
    LILA_ZONE();

    if (!m.edgeValid(a) || !m.edgeValid(b))
        return {};
    if (a == b)
        return {a};
    std::optional<std::vector<Index>> best;
    std::vector<Index> loop = edgeLoop(m, a);
    consider(best, loop, loopClosed(m, loop), a, b);
    if (!best)
    {
        std::vector<Index> ring = edgeRing(m, a);
        consider(best, ring, ringClosed(m, ring), a, b);
    }
    if (best)
        return *best;
    Index from[] = {m.edgePoint(a, 0), m.edgePoint(a, 1)}, to[] = {m.edgePoint(b, 0), m.edgePoint(b, 1)};
    auto path = shortestPoints(m, from, to);
    if (!path)
        return {};
    std::vector<Index> out{a};
    out.insert(out.end(), path->edges.begin(), path->edges.end());
    out.push_back(b);
    return uniqueInOrder(out);
}

std::vector<Index> facePathBetween(const Mesh& m, Index a, Index b)
{
    LILA_ZONE();

    if (!m.faceValid(a) || !m.faceValid(b))
        return {};
    if (a == b)
        return {a};
    std::optional<std::vector<Index>> best;
    for (Index c : m.faceCorners(a))
    {
        Faces loop = ringFaces(m, edgeRing(m, m.cornerEdge(c)));
        consider(best, loop.faces, loop.closed, a, b);
    }
    if (best)
        return *best;

    // Across shared edges, weighted by the distance between face centres.
    std::vector<double> dist(m.faceCapacity(), std::numeric_limits<double>::infinity());
    std::vector<Index> via(m.faceCapacity(), kInvalid);
    using Item = std::pair<double, Index>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> queue;
    dist[a] = 0.0;
    queue.push({0.0, a});
    while (!queue.empty())
    {
        auto [d, f] = queue.top();
        queue.pop();
        if (d > dist[f])
            continue;
        if (f == b)
            break;
        glm::dvec3 cf = m.faceCentroid(f);
        m.forEachFaceCorner(f, [&](Index c) {
            m.forEachEdgeCorner(m.cornerEdge(c), [&](Index r) {
                Index g = m.cornerFace(r);
                if (g == f)
                    return;
                double nd = d + glm::length(m.faceCentroid(g) - cf);
                if (nd < dist[g])
                {
                    dist[g] = nd;
                    via[g] = f;
                    queue.push({nd, g});
                }
            });
        });
    }
    if (via[b] == kInvalid)
        return {};
    std::vector<Index> out;
    for (Index f = b; f != kInvalid; f = via[f])
        out.push_back(f);
    std::reverse(out.begin(), out.end());
    return out;
}

std::vector<Index> containedFaces(const Mesh& mesh, std::span<const Index> edges, bool smaller)
{
    LILA_ZONE();

    std::vector<Index> piece, region;
    faceComponents(mesh, piece);
    std::vector<uint8_t> wall = makeMask(mesh.edgeCapacity(), edges);
    Index regions = faceComponents(mesh, region, wall);

    std::vector<double> area(regions, 0.0);
    std::vector<Index> pieceOf(regions, kInvalid);
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
        {
            area[region[f]] += mesh.faceArea(f);
            pieceOf[region[f]] = piece[f];
        }
    std::map<Index, std::vector<Index>> byPiece;
    for (Index r = 0; r < regions; ++r)
        byPiece[pieceOf[r]].push_back(r);

    std::vector<uint8_t> keep(regions, 0);
    for (auto& [p, rs] : byPiece)
    {
        if (rs.size() < 2)
            continue;
        Index largest = *std::max_element(rs.begin(), rs.end(), [&](Index x, Index y) { return area[x] < area[y]; });
        for (Index r : rs)
            keep[r] = smaller ? r != largest : r == largest;
    }
    std::vector<Index> out;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f) && keep[region[f]])
            out.push_back(f);
    return out;
}
} // namespace lila::geo
