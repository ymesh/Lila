#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Knife.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Topology.h"
#include "retopo/Internal.h"
#include "retopo/IntegerGrid.h"
#include "retopo/Isotropic.h"

#include <AutoRemesher/AutoRemesher>
#include <AutoRemesher/IsotropicRemesher>
#include <AutoRemesher/Parameterizer>
#include <AutoRemesher/QuadExtractor>

#include <tbb/parallel_for.h>

#include <algorithm>
#include <map>
#include <numeric>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <cstring>
#include <optional>
#include <queue>
#include <unordered_map>

namespace lila::retopo
{
using namespace detail;

namespace
{
class Stopwatch
{
public:
    explicit Stopwatch(std::vector<std::pair<std::string, double>>& out) : mOut(out) {}
    void lap(const char* name)
    {
        auto now = std::chrono::steady_clock::now();
        mOut.emplace_back(name, std::chrono::duration<double>(now - mLast).count());
        mLast = now;
        // Roughly where each stage ends in a typical run, for the progress bar.
        static const std::pair<const char*, double> kAt[] = {{"bvh", 0.03},   {"surface", 0.12}, {"density", 0.18}, {"isotropic", 0.4},
                                                             {"cover", 0.75}, {"extract", 0.9},  {"finish", 1.0}};
        double at = mAt;
        for (auto [stage, f] : kAt)
            if (std::string_view(stage) == name)
                at = f;
        mAt = at;
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "stage %s\n", name);
        Budget::reached(name, at);
    }

private:
    std::vector<std::pair<std::string, double>>& mOut;
    std::chrono::steady_clock::time_point mLast = std::chrono::steady_clock::now();
    double mAt = 0.0;
};

std::vector<std::vector<Index>> pointNeighbours(const geo::Mesh& m)
{
    std::vector<std::vector<Index>> nb(m.pointCapacity());
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            m.forEachPointEdge(p, [&](Index e) { nb[p].push_back(m.edgeOtherPoint(e, p)); });
    return nb;
}

/** How much further than the crease reach a border point is pulled onto the source's border. */
constexpr double kBorderReach = 6.0;

/** Snaps each point to its feature (corner, crease line) or the surface; demotes points whose feature isn't near. */
void projectPoints(geo::Mesh& m, std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& creases,
                   const FeatureLines& border, double reach)
{
    LILA_ZONE();

    // One inner point per corner, the nearest: two snapped onto it would pinch the mesh there.
    std::map<std::array<double, 3>, std::pair<double, Index>> claimed;
    std::vector<std::pair<Index, std::array<double, 3>>> claims;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q;
        if (!m.pointValid(p) || feature[p] != FeatureKind::Corner || m.isBoundaryPoint(p) || !creases.closestCorner(m.position(p), reach, q))
            continue;
        const std::array<double, 3> key{q.x, q.y, q.z};
        const double d = glm::distance(m.position(p), q);
        auto [it, fresh] = claimed.try_emplace(key, d, p);
        if (!fresh && d < it->second.first)
            it->second = {d, p};
        claims.push_back({p, key});
    }
    for (const auto& [p, key] : claims)
        if (claimed[key].second != p)
            feature[p] = FeatureKind::Line;
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p))
            return;
        // A border point belongs on the source's border however far the layout left it.
        const bool onBorder = m.isBoundaryPoint(p);
        const FeatureLines& lines = onBorder ? border : creases;
        const double r = onBorder ? kBorderReach * reach : reach;
        glm::dvec3 at = m.position(p), q;
        if (feature[p] == FeatureKind::Corner)
        {
            if (lines.closestCorner(at, reach, q))
            {
                m.setPosition(p, q);
                return;
            }
            feature[p] = FeatureKind::Line;
        }
        if (feature[p] == FeatureKind::Line)
        {
            if (lines.closest(at, r, q))
            {
                m.setPosition(p, q);
                return;
            }
            feature[p] = FeatureKind::None;
        }
        geo::BvhClosest c;
        if (bvh.closestPoint(at, c))
            m.setPosition(p, c.point);
    });
}

/** The lattice spacing anywhere near the surface: the scale at the nearest surface vertex. */
class SizeField
{
public:
    SizeField(const Surface& s, const std::vector<double>& scale) : mS(s), mScale(scale)
    {
        double sum = 0.0;
        for (double v : scale)
            sum += v;
        mCell = std::max(1e-12, sum / std::max<double>(1.0, double(scale.size())));
        for (Index v = 0; v < s.vertexCount(); ++v)
            mGrid[key(cellOf(s.pos[v]))].push_back(v);
    }
    double at(const glm::dvec3& p) const
    {
        const glm::ivec3 c = cellOf(p);
        for (int r = 1; r <= 3; ++r)
        {
            Index best = kInvalid;
            double bestD = std::numeric_limits<double>::max();
            for (int x = -r; x <= r; ++x)
                for (int y = -r; y <= r; ++y)
                    for (int z = -r; z <= r; ++z)
                        if (auto it = mGrid.find(key(c + glm::ivec3(x, y, z))); it != mGrid.end())
                            for (Index v : it->second)
                                if (const double d = glm::length(mS.pos[v] - p); d < bestD)
                                {
                                    bestD = d;
                                    best = v;
                                }
            if (best != kInvalid)
                return mScale[best];
        }
        return mCell;
    }

private:
    glm::ivec3 cellOf(const glm::dvec3& p) const { return glm::ivec3(glm::floor(p / mCell)); }
    static int64_t key(const glm::ivec3& c) { return (int64_t(c.x) * 73856093) ^ (int64_t(c.y) * 19349663) ^ (int64_t(c.z) * 83492791); }
    const Surface& mS;
    const std::vector<double>& mScale;
    double mCell = 1.0;
    std::unordered_map<int64_t, std::vector<Index>> mGrid;
};

/**
 * Tangential Laplacian relax that slides points along the surface and crease points along their creases. With a seam
 * axis, the half of a symmetric remesh: border points on the plane relax as if their mirrored neighbours were there,
 * so the rows crossing the seam meet it straight, and stay on the plane. With a size field, each neighbour pulls by
 * the inverse of the size between them, so edges settle at lengths in proportion to it instead of all alike.
 */
void relaxAlongFeatures(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& creases,
                        const FeatureLines& border, double h, int iterations, int seamAxis = -1, const SizeField* sizes = nullptr)
{
    LILA_ZONE();

    auto nb = pointNeighbours(m);
    std::vector<glm::dvec3> next(m.pointCapacity());
    std::vector<double> size(m.pointCapacity(), 1.0);
    for (int it = 0; it < iterations; ++it)
    {
        Budget::checkpoint();
        if (sizes && it % 5 == 0)
            tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
                if (m.pointValid(p))
                    size[p] = sizes->at(m.position(p));
            });
        std::vector<glm::vec3> normals = geo::pointNormals(m);
        tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
            if (!m.pointValid(p))
                return;
            glm::dvec3 at = m.position(p);
            next[p] = at;
            if (feature[p] == FeatureKind::Corner || nb[p].empty())
                return;
            if (seamAxis >= 0 && std::abs(at[seamAxis]) < 0.35 * h && m.isBoundaryPoint(p))
            {
                // A neighbour off the plane has a mirror twin, so it counts twice; the pair averages onto the plane, so only
                // its other coordinates count.
                glm::dvec3 sum(0.0);
                double weight = 0.0;
                for (Index o : nb[p])
                {
                    const double w = std::abs(m.position(o)[seamAxis]) < 0.35 * h && m.isBoundaryPoint(o) ? 1.0 : 2.0;
                    sum += w * m.position(o);
                    weight += w;
                }
                glm::dvec3 target = glm::mix(at, sum / weight, 0.5);
                target[seamAxis] = 0.0;
                geo::BvhClosest c;
                if (bvh.closestPoint(target, c))
                    target = c.point;
                target[seamAxis] = 0.0;
                next[p] = target;
                return;
            }
            glm::dvec3 avg(0.0);
            double weight = 0.0;
            int count = 0;
            for (Index o : nb[p])
                if (feature[p] == FeatureKind::None || feature[o] != FeatureKind::None)
                {
                    const double w = 2.0 / (size[p] + size[o]);
                    avg += w * m.position(o);
                    weight += w;
                    ++count;
                }
            if (count == 0)
                return;
            avg /= weight;
            if (feature[p] == FeatureKind::Line)
            {
                // Slide only between the two crease neighbours.
                if (count != 2)
                    return;
                const FeatureLines& lines = m.isBoundaryPoint(p) ? border : creases;
                glm::dvec3 target = glm::mix(at, avg, 0.5), q;
                if (lines.closest(target, 0.5 * h, q))
                    next[p] = q;
                return;
            }
            glm::dvec3 n(normals[p]);
            glm::dvec3 d = avg - at;
            d -= n * glm::dot(n, d);
            geo::BvhClosest c;
            if (bvh.closestPoint(at + 0.5 * d, c))
                next[p] = c.point;
        });
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p))
                m.setPosition(p, next[p]);
    }
}

/**
 * Fills a groove along the middle of a mirrored result: the points on the plane and six rings either side are smoothed
 * off the source, less with each ring out, and only straight outwards, so a ridge along the middle (a nose) and the
 * layout on the surface stay as they are. Each point is averaged only with its neighbours a ring nearer
 * and a ring further, so the smoothing runs across the middle along the rows and the profile along it (a nose, a
 * chin) keeps its shape. Plain averaging: a groove is several faces wide, and a pass that kept the broad shape
 * (Taubin) kept the groove with it.
 */
void smoothMiddle(geo::Mesh& m, int axis, double strength)
{
    constexpr int kRings = 6, kPasses = 20;
    const auto nb = pointNeighbours(m);
    std::vector<int> ring(m.pointCapacity(), -1);
    std::vector<Index> front;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.position(p)[axis] == 0.0)
        {
            ring[p] = 0;
            front.push_back(p);
        }
    for (int r = 1; r <= kRings; ++r)
    {
        std::vector<Index> next;
        for (Index p : front)
            for (Index o : nb[p])
                if (ring[o] < 0)
                {
                    ring[o] = r;
                    next.push_back(o);
                }
        front.swap(next);
    }
    // Only where the rows meet the middle in a V: a groove the faces are fine enough to follow round (a philtrum under
    // small faces) bends them a little at each point and is left alone. A point's row takes its middle point's share.
    constexpr double kLevel = glm::radians(8.0), kSharp = glm::radians(25.0);
    std::vector<double> share(m.pointCapacity(), 0.0);
    {
        std::vector<Index> order;
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (ring[p] != 0)
                continue;
            order.push_back(p);
            glm::dvec3 out(0.0);
            int outs = 0;
            for (Index o : nb[p])
                if (ring[o] == 1 && m.position(o)[axis] > 0.0)
                {
                    out += m.position(o) - m.position(p);
                    ++outs;
                }
            if (outs == 0)
                continue;
            // The row leaves the middle at this angle to the plane's normal; level across is zero.
            out /= double(outs);
            const double across = std::abs(out[axis]), length = glm::length(out);
            const double bend = length > 0.0 ? std::acos(std::clamp(across / length, 0.0, 1.0)) : 0.0;
            share[p] = std::clamp((bend - kLevel) / (kSharp - kLevel), 0.0, 1.0);
        }
        for (size_t i = 0; i < order.size(); ++i)
            for (Index o : nb[order[i]])
                if (ring[o] == ring[order[i]] + 1)
                {
                    if (share[o] == 0.0)
                        order.push_back(o);
                    share[o] = std::max(share[o], share[order[i]]);
                }
    }
    std::vector<glm::dvec3> moved(m.pointCapacity());
    for (int pass = 0; pass < kPasses; ++pass)
    {
        const double step = 0.5;
        const std::vector<glm::vec3> normals = geo::pointNormals(m);
        for (Index p = 0; p < m.pointCapacity(); ++p)
        {
            if (!m.pointValid(p))
                continue;
            moved[p] = m.position(p);
            if (ring[p] < 0 || nb[p].empty())
                continue;
            glm::dvec3 mean(0.0);
            int across = 0;
            for (Index o : nb[p])
                if (ring[o] != ring[p])
                {
                    mean += m.position(o);
                    ++across;
                }
            if (across == 0)
                continue;
            mean /= double(across);
            const double weight = strength * share[p] * (1.0 - double(ring[p]) / double(kRings + 1));
            glm::dvec3 move = weight * step * (mean - moved[p]);
            const glm::dvec3 n(normals[p]);
            // Straight out only: sliding along the surface as well would drag the faces off the shape they sit on.
            moved[p] += std::max(0.0, glm::dot(move, n)) * n;
            if (ring[p] == 0)
                moved[p][axis] = 0.0;
        }
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && ring[p] >= 0)
                m.setPosition(p, moved[p]);
    }
}

/** Faces facing against the source surface under them. */
std::vector<Index> foldedFaces(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh)
{
    std::vector<Index> out;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        geo::BvhClosest c;
        if (bvh.closestPoint(m.faceCentroid(f), c) && glm::dot(m.faceAreaVector(f), src.faceNormal(c.face)) < 0.0)
            out.push_back(f);
    }
    return out;
}

bool isClosed(const geo::Mesh& m)
{
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.isBoundaryEdge(e))
            return false;
    return true;
}

/**
 * Caps the lattice spacing where the surface comes close to itself across a gap, like a ball sitting in a pocket. A lattice
 * cell wider than the gap bridges it and the result collapses into it. At most twice as fine. The cap grows out gradually
 * so the spacing still varies slowly.
 */
void fitGaps(const Surface& s, const geo::Mesh& src, const geo::MeshBvh& bvh, std::vector<double>& scale)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<double> cap(n, std::numeric_limits<double>::max());
    tbb::parallel_for(Index(0), n, [&](Index v) {
        const glm::dvec3 nrm = s.normal[v];
        if (glm::dot(nrm, nrm) < 0.5)
            return;
        // Out along the normal to a surface facing back: a gap across air. Hitting the far side of a thin wall from
        // inside isn't one, and the two sides of a wall don't bridge.
        const double eps = 1e-3 * scale[v];
        geo::BvhHit hit;
        // The far side has to face straight back and be a fair part of a cell away: noise bumps next door are hit close
        // by or at a glancing angle.
        if (bvh.raycast(s.pos[v] + eps * nrm, nrm, hit, 2.0 * scale[v], false) && hit.t > 0.2 * scale[v] && hit.frontFacing &&
            hit.face != geo::kInvalid && glm::dot(src.faceNormal(hit.face), nrm) < -0.7)
            cap[v] = std::min(cap[v], std::max(0.8 * (hit.t + eps), scale[v] / 2.0));
    });
    // The caps fade out over a few cells around where they were found, so the spacing still varies slowly.
    for (int it = 0; it < 30; ++it)
    {
        bool changed = false;
        for (Index v = 0; v < n; ++v)
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
            {
                Index w = s.adj[k];
                double limit = cap[w] + 0.25 * glm::length(s.pos[v] - s.pos[w]);
                if (cap[v] > limit)
                {
                    cap[v] = limit;
                    changed = true;
                }
            }
        if (!changed)
            break;
    }
    for (Index v = 0; v < n; ++v)
        scale[v] = std::min(scale[v], cap[v]);
}

/** The density multiplier the handles give a point: a handle of scale s wants edges s times as long, so 1 / s^2 the faces. */
double handleDensity(const glm::dvec3& p, const std::vector<DensityHandle>& handles)
{
    double rho = 1.0;
    for (const DensityHandle& d : handles)
    {
        if (!(d.radius > 0.0))
            continue;
        const double t = 1.0 - glm::distance(p, d.position) / d.radius;
        if (t <= 0.0)
            continue;
        const double ease = t * t * (3.0 - 2.0 * t);
        const double scale = 1.0 + (std::clamp(d.scale, 0.35, 2.8) - 1.0) * ease;
        rho /= scale * scale;
    }
    return rho;
}

/**
 * Joins the loose ends of a cut's chain: each end to the nearest other end within reach, by the shortest path along the
 * mesh's edges. An open guide keeps the two ends farthest apart.
 */
void closeChainGaps(const geo::Mesh& m, std::vector<std::array<Index, 2>>& chainEdges, std::unordered_set<uint64_t>& have, double reach,
                    bool closed)
{
    std::unordered_map<Index, int> degree;
    for (const auto& [a, b] : chainEdges)
    {
        ++degree[a];
        ++degree[b];
    }
    std::vector<Index> ends;
    for (const auto& [v, d] : degree)
        if (d == 1)
            ends.push_back(v);
    if (!closed && ends.size() >= 2)
    {
        // The guide's own two ends: the pair farthest apart.
        size_t bi = 0, bj = 1;
        double far = -1.0;
        for (size_t i = 0; i < ends.size(); ++i)
            for (size_t j = i + 1; j < ends.size(); ++j)
                if (const double d = glm::distance(m.position(ends[i]), m.position(ends[j])); d > far)
                {
                    far = d;
                    bi = i;
                    bj = j;
                }
        const Index keepA = ends[bi], keepB = ends[bj];
        ends.erase(std::remove_if(ends.begin(), ends.end(), [&](Index v) { return v == keepA || v == keepB; }), ends.end());
    }
    std::unordered_set<Index> joined;
    for (Index a : ends)
    {
        if (joined.contains(a))
            continue;
        std::unordered_map<Index, double> dist{{a, 0.0}};
        std::unordered_map<Index, Index> from;
        std::vector<std::pair<double, Index>> heap{{0.0, a}};
        Index found = kInvalid;
        while (!heap.empty())
        {
            std::pop_heap(heap.begin(), heap.end(), std::greater<>());
            const auto [d, u] = heap.back();
            heap.pop_back();
            if (d > dist[u])
                continue;
            if (u != a && degree.contains(u) && degree[u] == 1 && !joined.contains(u))
            {
                found = u;
                break;
            }
            m.forEachPointEdge(u, [&](Index e) {
                const Index w = m.edgeOtherPoint(e, u);
                const double nd = d + m.edgeLength(e);
                if (nd > reach)
                    return;
                auto it = dist.find(w);
                if (it == dist.end() || nd < it->second)
                {
                    dist[w] = nd;
                    from[w] = u;
                    heap.push_back({nd, w});
                    std::push_heap(heap.begin(), heap.end(), std::greater<>());
                }
            });
        }
        if (found == kInvalid)
            continue;
        for (Index u = found; u != a; u = from[u])
        {
            const Index p = from[u];
            if (have.insert(edgeKey(p, u)).second)
                chainEdges.push_back({p, u});
        }
        joined.insert(a);
        joined.insert(found);
    }
}

/**
 * Cuts each feature guide into the mesh with the knife, so an edge chain runs exactly along it; returns those edges
 * as point pairs. Guide points are taken onto the surface, a few per source edge apart, so the cut between two of them
 * follows the surface closely. dropped counts the guides that couldn't be cut in.
 */
std::vector<std::array<Index, 2>> imprintGuides(geo::Mesh& m, const std::vector<std::vector<glm::dvec3>>& guides, int& dropped)
{
    LILA_ZONE();

    double edge = 0.0;
    Index count = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            edge += m.edgeLength(e);
            ++count;
        }
    const double step = std::max(1e-9, 2.0 * edge / std::max<Index>(1, count));
    std::vector<std::array<Index, 2>> out;
    // A guide along the mesh's own border is an edge loop already; cutting along it would tear the border.
    auto alongBorder = [&](const std::vector<glm::dvec3>& guide) {
        std::vector<std::array<glm::dvec3, 2>> segs;
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (m.edgeValid(e) && m.isBoundaryEdge(e))
                segs.push_back({m.position(m.edgePoint(e, 0)), m.position(m.edgePoint(e, 1))});
        if (segs.empty())
            return false;
        for (const glm::dvec3& p : guide)
            if (glm::distance(closestOnSegments(p, segs), p) > 0.5 * step)
                return false;
        return true;
    };
    for (const auto& guide : guides)
    {
        if (guide.size() < 2 || alongBorder(guide))
            continue;
        const geo::MeshBvh bvh(m);
        const bool closed = glm::distance(guide.front(), guide.back()) < 1e-6 * step;
        std::vector<geo::KnifePoint> points;
        auto add = [&](const glm::dvec3& p) {
            geo::BvhClosest c;
            if (bvh.closestPoint(p, c) && c.face != kInvalid)
                points.push_back(geo::KnifePoint::face(c.face, c.point));
        };
        const size_t last = closed ? guide.size() - 1 : guide.size();
        for (size_t i = 0; i < last; ++i)
        {
            add(guide[i]);
            if (i + 1 < guide.size())
            {
                const int pieces = int(std::ceil(glm::distance(guide[i], guide[i + 1]) / step));
                for (int k = 1; k < pieces; ++k)
                    add(glm::mix(guide[i], guide[i + 1], double(k) / pieces));
            }
        }
        if (points.size() < 2)
        {
            ++dropped;
            continue;
        }
        geo::KnifeOptions ko;
        ko.closed = closed;
        auto borderEdges = [](const geo::Mesh& mesh) {
            Index n = 0;
            for (Index e = 0; e < mesh.edgeCapacity(); ++e)
                n += mesh.edgeValid(e) && mesh.isBoundaryEdge(e);
            return n;
        };
        // A cut that opens the mesh (a guide grazing a border, a knife path that misses) is undone.
        const geo::Mesh before = m;
        const Index open = borderEdges(m);
        const std::vector<Index> cut = geo::knifeCut(m, points, ko);
        if (borderEdges(m) > open)
        {
            m = before;
            ++dropped;
            continue;
        }
        // The cut's own edges, not the diagonals it adds inside the faces it splits: ends and middle on the cut's path.
        std::vector<std::array<glm::dvec3, 2>> segs;
        for (size_t i = 0; i + 1 < points.size(); ++i)
            segs.push_back({points[i].position, points[i + 1].position});
        if (closed)
            segs.push_back({points.back().position, points.front().position});
        auto onPath = [&](const glm::dvec3& at) { return glm::distance(closestOnSegments(at, segs), at) < 0.05 * step; };
        std::unordered_set<Index> chain;
        for (Index e : cut)
        {
            if (!m.edgeValid(e))
                continue;
            const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
            if (onPath(a) && onPath(b) && onPath(0.5 * (a + b)))
            {
                out.push_back({m.edgePoint(e, 0), m.edgePoint(e, 1)});
                chain.insert(m.edgePoint(e, 0));
                chain.insert(m.edgePoint(e, 1));
            }
        }
        // The knife leaves out stretches that already run along an edge: an edge between two of the chain's points,
        // along the path, closes such a gap.
        std::unordered_set<uint64_t> have;
        for (const auto& [x, y] : out)
            have.insert(edgeKey(x, y));
        for (Index p : chain)
            m.forEachPointEdge(p, [&](Index e) {
                const Index q = m.edgeOtherPoint(e, p);
                if (p < q && chain.contains(q) && !have.contains(edgeKey(p, q)) && onPath(0.5 * (m.position(p) + m.position(q))))
                {
                    out.push_back({p, q});
                    have.insert(edgeKey(p, q));
                }
            });
        closeChainGaps(m, out, have, step, closed);
    }
    return out;
}

/**
 * The same on a working surface: its hard edges, density and pins carry over, and the guides' edges join the hard set.
 * Returns how many guides couldn't be cut in.
 */
int imprintGuides(Surface& s, const std::vector<std::vector<glm::dvec3>>& guides)
{
    std::vector<Index> sizes(s.tris.size(), 3), corners;
    corners.reserve(s.tris.size() * 3);
    for (const auto& t : s.tris)
        corners.insert(corners.end(), t.begin(), t.end());
    geo::Mesh m = geo::Mesh::fromPolygons(s.pos, sizes, corners);
    if (m.pointCapacity() != Index(s.pos.size()))
        return int(guides.size());
    int dropped = 0;
    const std::vector<std::array<Index, 2>> cut = imprintGuides(m, guides, dropped);
    Surface out = surfaceFromMesh(m, false);
    if (out.pos.size() < s.pos.size())
        return int(guides.size());
    for (size_t v = 0; v < s.density.size(); ++v)
        out.density[v] = s.density[v];
    out.hard = s.hard;
    for (const auto& [a, b] : cut)
        out.hard.insert(edgeKey(a, b));
    out.pins = s.pins;
    s = std::move(out);
    return dropped;
}

/**
 * Caps the lattice spacing on thin parts, like fingers, so at least `around` edges go round them: the thickness straight
 * through the shape (to the inside of the far wall) stands in for a tube's diameter. The cap grows out gradually, like
 * fitGaps'. cf is the lattice's coarsening, the spacing per result edge.
 */
void fitThickness(const Surface& s, const geo::Mesh& src, const geo::MeshBvh& bvh, const std::vector<double>& curvature, int around, double cf,
                  std::vector<double>& scale)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    const double perDiameter = cf * 3.141592653589793 / double(around);
    std::vector<double> cap(n, std::numeric_limits<double>::max());
    tbb::parallel_for(Index(0), n, [&](Index v) {
        if (const double t = roundThickness(s, src, bvh, curvature, v, scale[v] / perDiameter); t > 0.0)
            cap[v] = std::max(perDiameter * t, 0.25 * scale[v]);
    });
    for (int it = 0; it < 30; ++it)
    {
        bool changed = false;
        for (Index v = 0; v < n; ++v)
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
            {
                const double limit = cap[s.adj[k]] + 0.25 * glm::length(s.pos[v] - s.pos[s.adj[k]]);
                if (cap[v] > limit)
                {
                    cap[v] = limit;
                    changed = true;
                }
            }
        if (!changed)
            break;
    }
    for (Index v = 0; v < n; ++v)
        scale[v] = std::min(scale[v], cap[v]);
}

Index defects(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh, bool sourceClosed);

/** Working triangle edges per quad edge: triangles about as long as the quads resolve the flow and keep the solve small. */
constexpr double kTriangleRatio = 1.0;

double triangleRatio() { return kTriangleRatio; }

/** A sharp tip cut off the working surface and built back on the result: see clipSharpTips and closeTips. */
struct ClippedTip
{
    /** The cut is the plane across `axis` at `depth` from the tip, out to `radius` from the axis. */
    glm::dvec3 tip, axis;
    double depth = 0.0, radius = 0.0;
    /** The creases running across the tip (a prism's edge between its two corners) the rungs end on; empty for a point. */
    std::vector<std::array<glm::dvec3, 2>> core;
    /** The cut's own points on the surface. */
    std::vector<glm::dvec3> rim;
};

/** The source surface, its features and the quad size wanted over it, then the working triangles the quads are laid out on. */
struct Work
{
    std::vector<ClippedTip> tips;
    geo::Mesh src;
    std::optional<geo::MeshBvh> bvh;
    bool closed = false;
    Surface s;
    bool symmetric = false;
    geo::SymmetryPlane plane;
    /** The mean quad edge asked for. */
    double h0 = 0.0;
    double adaptivity = 0.0;
    /**
     * Laid out by AutoRemesher's own stages as they come: its simplifier, isotropic triangles, curvature sizes and quad
     * extraction. A smooth shape with nothing to hold (no creases, pins or asked poles) takes this, an open one with
     * its borders capped: the stock extraction fills a border in or leaves it ragged.
     */
    bool stock = false;
    /**
     * Sized by the stock layout's curvature measure, with Lila's size controls as factors and limits on it. Creased
     * shapes keep Lila's own size field: read across a crease, that measure calls every edge of a cube curved.
     */
    bool stockSizes = false;
    /** The quads asked of the stock layout, over all of iso off its caps. */
    double stockQuads = 0.0;
    /**
     * A symmetric remesh laid out on the closed whole: iso is the positive half joined to its mirror image, with its edges
     * along the plane hard so the layout keeps them as one edge loop.
     */
    bool wholeLayout = false;
    /**
     * The caps an open shape's borders were closed with for the stock layout, which only lays out closed shapes well:
     * the border edges are hard in iso, so the layout runs a loop along each, and the quads on the caps are dropped.
     */
    geo::Mesh caps;
    std::optional<geo::MeshBvh> capsBvh;
    /**
     * The caps that stand in for a creased shape's flat faces (a bust's cut base): laid out and kept, their rims held as
     * edge loops like any cap's.
     */
    geo::Mesh keptCaps;
    std::optional<geo::MeshBvh> keptCapsBvh;
    /** Whether a point of the working triangles lies on a kept cap: they are flat, so on one is on its plane. */
    bool onKeptCap(const glm::dvec3& p) const
    {
        geo::BvhClosest on;
        return keptCapsBvh && keptCapsBvh->closestPoint(p, on, 1e-4 * h0);
    }
    /** Whether a point of the capped surface lies on a cap rather than on the source. */
    bool onCap(const glm::dvec3& p) const
    {
        if (!capsBvh)
            return false;
        geo::BvhClosest onCaps, onSource;
        if (!capsBvh->closestPoint(p, onCaps))
            return false;
        return !simpleBvh->closestPoint(p, onSource) || glm::distance(onCaps.point, p) < glm::distance(onSource.point, p);
    }
    /** The surface simplified to a few triangles per working triangle, which the working triangles lie on. */
    geo::Mesh simple;
    std::optional<geo::MeshBvh> simpleBvh;
    /** The feature lines off the borders, and the borders. */
    FeatureLines creases, border;
    /** The edges imprinted along edge loop curves: feature edges for the layout to follow, but no crease of the shape. */
    std::vector<std::array<glm::dvec3, 2>> guideSegs;
    /** What the result should say about how it was made. */
    std::vector<std::string> notes;
    Surface iso;
    /** Quad edge length per vertex of iso. */
    std::vector<double> isoSize;
    std::string error;

    Work() = default;
    Work(const Work&) = delete;
};

/**
 * How much the quad size may grow per unit of distance across the surface. A layout can't follow a size that changes
 * faster: where it is asked to, it strings poles along the change.
 */
constexpr double kSizeGrowth = 0.3;

/** Caps size so it grows by at most rate times the distance from any smaller size, out along the edges. */
void limitGrowth(const Surface& s, std::vector<double>& size, double rate)
{
    const Index n = s.vertexCount();
    std::vector<std::pair<double, Index>> heap;
    for (Index v = 0; v < n; ++v)
        heap.push_back({size[v], v});
    std::make_heap(heap.begin(), heap.end(), std::greater<>());
    while (!heap.empty())
    {
        std::pop_heap(heap.begin(), heap.end(), std::greater<>());
        const auto [at, v] = heap.back();
        heap.pop_back();
        if (at > size[v])
            continue;
        for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
        {
            const Index u = s.adj[k];
            const double limit = size[v] + rate * glm::distance(s.pos[u], s.pos[v]);
            if (size[u] > limit)
            {
                size[u] = limit;
                heap.push_back({limit, u});
                std::push_heap(heap.begin(), heap.end(), std::greater<>());
            }
        }
    }
}

/** How sizes follow curvature: the working triangles' importance curve, or the quads' scaling by curvature against its mean. */
enum class CurvatureSizing
{
    Triangles,
    Quads,
};

/**
 * Per vertex, the edge length multiplier curvature asks for (autoremesher's measures). Triangles: the normal's turn per
 * unit length round the vertex against its 75th percentile, squared into an importance, down to 0.35 where curved and
 * up to 3 where flat. Quads: the steepest normal turn per length to a neighbour against the median, raised to -strength,
 * 0.3 to 3.
 */
std::vector<double> curvatureMultiplier(const Surface& s, double strength, CurvatureSizing sizing)
{
    const Index n = s.vertexCount();
    std::vector<double> mult(n, 1.0);
    if (strength <= 0.0)
        return mult;
    std::vector<double> curv(n, 0.0);
    // The bend across each edge between its two triangles; a feature edge's bend is a crease, not curvature.
    tbb::parallel_for(Index(0), n, [&](Index v) {
        double sum = 0.0, len = 0.0, steepest = 0.0;
        for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
        {
            const Index u = s.adj[k], e = s.adjEdge[k];
            const double d = glm::distance(s.pos[u], s.pos[v]);
            if (d <= 0.0)
                continue;
            double turn = 0.0;
            if (s.edgeTriCount[e] == 2 && !(e < Index(s.edgeFeature.size()) && s.edgeFeature[e]))
                turn = std::acos(std::clamp(glm::dot(s.triNormal[s.edgeTris[e][0]], s.triNormal[s.edgeTris[e][1]]), -1.0, 1.0));
            sum += turn;
            len += d;
            steepest = std::max(steepest, turn / d);
        }
        curv[v] = sizing == CurvatureSizing::Triangles ? (len > 0.0 ? sum / len : 0.0) : steepest;
    });
    if (sizing == CurvatureSizing::Triangles)
    {
        std::vector<double> nonZero;
        for (double c : curv)
            if (c > 1e-12)
                nonZero.push_back(c);
        if (nonZero.empty())
            return mult;
        const size_t at = (nonZero.size() - 1) * 3 / 4;
        std::nth_element(nonZero.begin(), nonZero.begin() + std::ptrdiff_t(at), nonZero.end());
        const double reference = std::max(nonZero[at], 1e-12);
        const double gain = std::min(strength, 2.0) * 7.0;
        std::vector<double> importance(n);
        double weighted = 0.0, area = 0.0;
        for (Index v = 0; v < n; ++v)
        {
            const double x = std::min(4.0, curv[v] / reference);
            importance[v] = 1.0 + gain * x * x;
            weighted += s.area[v] * importance[v];
            area += s.area[v];
        }
        const double mean = area > 0.0 ? weighted / area : 1.0;
        for (Index v = 0; v < n; ++v)
            mult[v] = std::clamp(std::sqrt(mean / importance[v]), 0.35, 3.0);
        return mult;
    }
    // Against the median: the mean is pulled up by slivers and sharp spots until the rest reads as flat.
    std::vector<double> sorted = curv;
    std::nth_element(sorted.begin(), sorted.begin() + std::ptrdiff_t(sorted.size() / 2), sorted.end());
    const double mean = sorted.empty() ? 0.0 : sorted[sorted.size() / 2];
    if (mean <= 0.0)
        return mult;
    for (Index v = 0; v < n; ++v)
        mult[v] = std::clamp(std::pow(std::max(curv[v] / mean, 1e-3), -strength), 0.3, 3.0);
    return mult;
}

/**
 * The quad edge length per vertex of s: painted density, curvature, joints and handles over the face budget, capped at
 * gaps and thin parts.
 */
std::vector<double> sizeField(const Surface& s, const Work& w, const QuadRemeshOptions& opts, double faces, CurvatureSizing sizing)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    const double h0 = w.h0;
    const Curvature curv = estimateCurvature(s);
    std::vector<double> rho(n);
    for (Index v = 0; v < n; ++v)
        rho[v] = std::clamp(double(s.density[v]), 1.0 / 16.0, 16.0);
    // Painted density spreads so the size changes over several quads; the cover wants it slowly varying.
    const double meanEdge = std::max(s.meanEdgeLength(), 1e-12);
    const int spread = int(std::clamp(3.0 * h0 / meanEdge, 2.0, 60.0));
    for (int it = 0; it < spread; ++it)
    {
        std::vector<double> next(n);
        for (Index v = 0; v < n; ++v)
        {
            double sum = rho[v];
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
                sum += rho[s.adj[k]];
            next[v] = sum / double(1 + s.adjStart[v + 1] - s.adjStart[v]);
        }
        rho = std::move(next);
    }
    const std::vector<double> bend = curvatureMultiplier(s, 4.0 * opts.adaptivity, sizing);
    for (Index v = 0; v < n; ++v)
        rho[v] /= bend[v] * bend[v];
    // Joints get theirs after the spread, which would flatten a fold's narrow peak. A fold reads as a saddle only along
    // its crease, so the band widens from there, halving every two quads.
    if (opts.jointDensity > 0.0)
    {
        std::vector<double> joint = saddleAmount(s, 2.0 * h0);
        const double perQuad = std::max(1.0, h0 / meanEdge);
        const double decay = std::pow(0.5, 1.0 / (2.0 * perQuad));
        const int rings = int(std::min(80.0, opts.jointDensityReach * perQuad));
        for (int it = 0; it < rings; ++it)
        {
            std::vector<double> next = joint;
            for (Index v = 0; v < n; ++v)
                for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
                    next[v] = std::max(next[v], joint[s.adj[k]] * decay);
            joint.swap(next);
        }
        for (Index v = 0; v < n; ++v)
            rho[v] = std::clamp(rho[v] * (1.0 + 2.0 * opts.jointDensity * joint[v]), 1.0 / 16.0, 16.0);
    }
    if (!opts.densityHandles.empty())
        for (Index v = 0; v < n; ++v)
            rho[v] = std::clamp(rho[v] * handleDensity(s.pos[v], opts.densityHandles), 1.0 / 16.0, 16.0);
    double h = h0;
    if (opts.targetEdgeLength <= 0.0)
    {
        double weighted = 0.0;
        for (Index v = 0; v < n; ++v)
            weighted += s.area[v] * rho[v];
        h = std::sqrt(weighted / faces);
    }
    std::vector<double> size(n);
    for (Index v = 0; v < n; ++v)
        size[v] = h / std::sqrt(rho[v]);
    if (opts.resolveGaps && w.closed)
        fitGaps(s, w.src, *w.bvh, size);
    if (opts.edgesAround > 0 && w.closed)
        fitThickness(s, w.src, *w.bvh, curv.magnitude, opts.edgesAround, 1.0, size);
    limitGrowth(s, size, kSizeGrowth);
    return size;
}

/**
 * Makes a working surface mirror symmetric with an edge chain along the plane: points near the plane go onto it, the
 * triangles still crossing are cut there, and the positive half is joined to its mirror image. Returns the chain.
 */
std::unordered_set<uint64_t> symmetrize(Surface& iso, const geo::SymmetryPlane& plane, int axis)
{
    // A point within reach of the plane moves onto it, so the cut passes no closer than that to any point it leaves:
    // no slivers along the chain.
    constexpr double kReach = 0.3;
    for (Index v = 0; v < iso.vertexCount(); ++v)
    {
        double length = 0.0;
        const Index n = iso.adjStart[v + 1] - iso.adjStart[v];
        for (Index k = iso.adjStart[v]; k < iso.adjStart[v + 1]; ++k)
            length += glm::distance(iso.pos[v], iso.pos[iso.adj[k]]);
        if (n > 0 && std::abs(plane.distance(iso.pos[v])) < kReach * length / double(n))
            iso.pos[v] = plane.project(iso.pos[v]);
    }
    clipSurface(iso, plane, axis);
    iso.buildTopology();
    std::vector<Index> mirrorOf;
    Surface whole = mirrorSurface(iso, plane, mirrorOf);
    std::unordered_set<uint64_t> chain;
    for (size_t e = 0; e < iso.edges.size(); ++e)
    {
        const Index a = iso.edges[e][0], b = iso.edges[e][1];
        if (iso.edgeTriCount[e] == 1 && mirrorOf[a] == a && mirrorOf[b] == b)
            chain.insert(edgeKey(a, b));
    }
    iso = std::move(whole);
    iso.buildTopology();
    iso.buildGeometry();
    return chain;
}

/**
 * Closes an open working surface with a cap over each border loop: a fan to the loop's centre, remeshed to the working
 * triangles' size with the loop left as it is. The border edges are returned, for the layout to hold as edge loops.
 * False when a border isn't a plain loop, or the caps would be a large part of the result (a sheet seen from one side
 * has a border all round it and a cap the size of itself). With `border` given, only the loops along it are the
 * source's own borders; the caps over the others go to `kept`, and those loops are left as they are.
 */
bool capBorders(Surface& iso, geo::Mesh& caps, std::unordered_set<uint64_t>& chain, const FeatureLines* border = nullptr, double reach = 0.0,
                geo::Mesh* kept = nullptr)
{
    constexpr double kLargestShare = 0.3;
    // Each border edge the way its triangle runs it, so the caps face the same way round.
    std::unordered_map<Index, Index> next;
    for (const auto& t : iso.tris)
        for (int k = 0; k < 3; ++k)
        {
            const Index a = t[k], b = t[(k + 1) % 3];
            for (Index j = iso.adjStart[a]; j < iso.adjStart[a + 1]; ++j)
                if (iso.adj[j] == b && iso.edgeTriCount[iso.adjEdge[j]] == 1)
                {
                    if (next.contains(a))
                        return false;
                    next[a] = b;
                }
        }
    if (next.empty())
        return true;
    const double edge = iso.meanEdgeLength();
    std::vector<Index> capCorners, keptCorners;
    std::vector<std::array<Index, 3>> added;
    std::vector<glm::dvec3> addedPoints;
    double capArea = 0.0;
    std::unordered_set<Index> done;
    std::vector<std::vector<Index>> loops;
    const double surfaceArea = iso.totalArea();
    for (const auto& [first, unused] : next)
    {
        if (done.contains(first))
            continue;
        std::vector<Index> loop;
        for (Index at = first; !done.contains(at); at = next.at(at))
        {
            done.insert(at);
            loop.push_back(at);
            if (!next.contains(next.at(at)))
                return false;
        }
        if (loop.size() < 3 || next.at(loop.back()) != first)
            return false;
        // Before any cap is built: one too large isn't worth remeshing to find out.
        glm::dvec3 middle(0.0);
        for (Index v : loop)
            middle += iso.pos[v] / double(loop.size());
        for (size_t i = 0; i < loop.size(); ++i)
            capArea += 0.5 * glm::length(glm::cross(iso.pos[loop[i]] - middle, iso.pos[loop[(i + 1) % loop.size()]] - middle));
        if (capArea > kLargestShare * surfaceArea)
            return false;
        loops.push_back(std::move(loop));
    }
    for (const std::vector<Index>& loop : loops)
    {
        bool own = true;
        if (border)
        {
            glm::dvec3 q;
            for (Index v : loop)
                own = own && border->closest(iso.pos[v], reach, q);
        }
        // A border left by deleting faces is a sawtooth, and the layout would turn at every tooth. The working border
        // is evened out along itself; the result's border points go back onto the source's own.
        for (int pass = 0; own && pass < 12; ++pass)
        {
            std::vector<glm::dvec3> evened(loop.size());
            for (size_t i = 0; i < loop.size(); ++i)
                evened[i] = 0.5 * iso.pos[loop[i]] + 0.25 * (iso.pos[loop[(i + loop.size() - 1) % loop.size()]] + iso.pos[loop[(i + 1) % loop.size()]]);
            for (size_t i = 0; i < loop.size(); ++i)
                iso.pos[loop[i]] = evened[i];
        }
        glm::dvec3 centre(0.0);
        double longest = 0.0;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            centre += iso.pos[loop[i]] / double(loop.size());
            longest = std::max(longest, glm::distance(iso.pos[loop[i]], iso.pos[loop[(i + 1) % loop.size()]]));
        }
        std::vector<AutoRemesher::Vector3> vertices;
        std::vector<std::vector<size_t>> triangles;
        for (Index v : loop)
            vertices.push_back(AutoRemesher::Vector3(iso.pos[v].x, iso.pos[v].y, iso.pos[v].z));
        vertices.push_back(AutoRemesher::Vector3(centre.x, centre.y, centre.z));
        for (size_t i = 0; i < loop.size(); ++i)
        {
            // Against the surface's own direction along the edge.
            triangles.push_back({(i + 1) % loop.size(), i, loop.size()});
        }
        // No shorter than the loop's own edges, which stay whole: the cap has to meet the surface point for point.
        AutoRemesher::IsotropicRemesher remesher(vertices, triangles);
        remesher.setTargetEdgeLength(std::max(edge, longest));
        remesher.setSharpEdgeDegrees(180.0);
        if (!remesher.remesh())
            return false;
        std::map<std::array<double, 3>, Index> onLoop;
        for (Index v : loop)
            onLoop[{iso.pos[v].x, iso.pos[v].y, iso.pos[v].z}] = v;
        std::vector<Index> id;
        size_t met = 0;
        for (const auto& p : remesher.remeshedVertices())
        {
            const glm::dvec3 at(p.x(), p.y(), p.z());
            if (auto it = onLoop.find({at.x, at.y, at.z}); it != onLoop.end())
            {
                id.push_back(it->second);
                ++met;
            }
            else
            {
                id.push_back(Index(iso.pos.size() + addedPoints.size()));
                addedPoints.push_back(at);
            }
        }
        if (met != loop.size())
            return false;
        for (const auto& t : remesher.remeshedTriangles())
        {
            added.push_back({id[t[0]], id[t[1]], id[t[2]]});
            std::vector<Index>& corners = own ? capCorners : keptCorners;
            corners.insert(corners.end(), {id[t[0]], id[t[1]], id[t[2]]});
        }
        for (size_t i = 0; i < loop.size(); ++i)
            chain.insert(edgeKey(loop[i], loop[(i + 1) % loop.size()]));
    }
    iso.pos.insert(iso.pos.end(), addedPoints.begin(), addedPoints.end());
    iso.density.resize(iso.pos.size(), 1.0f);
    for (const auto& t : added)
        iso.tris.push_back(t);
    caps = geo::Mesh::fromPolygons(iso.pos, std::vector<Index>(capCorners.size() / 3, 3), capCorners);
    if (kept)
        *kept = geo::Mesh::fromPolygons(iso.pos, std::vector<Index>(keptCorners.size() / 3, 3), keptCorners);
    iso.buildTopology();
    iso.buildGeometry();
    return true;
}

/**
 * Cuts the sharp tips off a closed creased surface: where three or more creases meet and two of them bound a sector of
 * a face under 45 degrees (a star prism's points, a blade's tip), lines of the layout held along both sides run
 * together and the cover collapses all the way up the spike. Each such tip is cut across where its sector is a quad
 * wide, leaving a hole whose rim is a border to the layout; closeTips builds the tip back on the result. s needs
 * buildTopology, buildGeometry and detectFeatures, and needs them again when anything was cut.
 */
void clipSharpTips(Surface& s, double h, double sharpAngleDeg, std::vector<ClippedTip>& tips)
{
    constexpr double kSharpest = 0.7853981633974483, kWidth = 1.0, kDeepest = 6.0, kLargestShare = 0.25;
    const Index n = s.vertexCount();
    const double cosSharp = sharpAngleDeg >= 0.0 ? std::cos(glm::radians(sharpAngleDeg)) : -2.0;
    auto sharp = [&](Index e) {
        if (s.edgeTriCount[e] != 2)
            return false;
        return s.edgeFeature[e] != 0 || glm::dot(s.triNormal[s.edgeTris[e][0]], s.triNormal[s.edgeTris[e][1]]) < cosSharp;
    };
    std::vector<std::vector<Index>> around(n);
    for (Index t = 0; t < Index(s.tris.size()); ++t)
        for (Index v : s.tris[t])
            around[v].push_back(t);
    struct Candidate
    {
        double angle;
        Index v;
        glm::dvec3 axis;
    };
    std::vector<Candidate> candidates;
    for (Index v = 0; v < n; ++v)
    {
        if (s.onBoundary[v])
            continue;
        std::unordered_map<Index, Index> edgeTo, byFirst;
        int creases = 0;
        for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
        {
            edgeTo[s.adj[k]] = s.adjEdge[k];
            creases += sharp(s.adjEdge[k]);
        }
        if (creases < 3)
            continue;
        auto sides = [&](Index t, Index& a, Index& b) {
            const auto& tri = s.tris[t];
            const int k = tri[0] == v ? 0 : tri[1] == v ? 1 : 2;
            a = tri[(k + 1) % 3];
            b = tri[(k + 2) % 3];
        };
        for (Index t : around[v])
        {
            Index a, b;
            sides(t, a, b);
            byFirst[a] = t;
        }
        Candidate best{kSharpest, kInvalid, glm::dvec3(0.0)};
        for (Index start : around[v])
        {
            Index a, b;
            sides(start, a, b);
            if (!sharp(edgeTo[a]))
                continue;
            const Index first = a;
            double angle = 0.0;
            bool closed = false;
            Index t = start;
            for (size_t guard = 0; guard < around[v].size(); ++guard)
            {
                sides(t, a, b);
                angle += std::acos(std::clamp(glm::dot(glm::normalize(s.pos[a] - s.pos[v]), glm::normalize(s.pos[b] - s.pos[v])), -1.0, 1.0));
                if (sharp(edgeTo[b]))
                {
                    closed = true;
                    break;
                }
                auto next = byFirst.find(b);
                if (next == byFirst.end())
                    break;
                t = next->second;
            }
            if (closed && angle > 1e-3 && angle < best.angle)
                best = {angle, v, glm::normalize(glm::normalize(s.pos[first] - s.pos[v]) + glm::normalize(s.pos[b] - s.pos[v]))};
        }
        if (best.v != kInvalid)
            candidates.push_back(best);
    }
    if (candidates.empty())
        return;
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.angle < b.angle; });
    const double total = s.totalArea();
    // Per vertex, the tip whose cut takes it off.
    std::vector<int> region(n, -1);
    std::vector<ClippedTip> cut;
    for (const Candidate& c : candidates)
    {
        if (region[c.v] >= 0)
            continue;
        ClippedTip tip;
        tip.tip = s.pos[c.v];
        tip.axis = c.axis;
        tip.depth = std::min(kDeepest * h, 0.5 * kWidth * h / std::tan(0.5 * c.angle));
        auto along = [&](Index v) { return glm::dot(s.pos[v] - tip.tip, tip.axis); };
        // What the plane takes off: the points this side of it joined to the tip.
        std::vector<Index> taken{c.v}, stack{c.v};
        std::unordered_set<Index> inside{c.v};
        bool clean = true;
        double area = 0.0;
        while (!stack.empty() && clean)
        {
            const Index v = stack.back();
            stack.pop_back();
            clean = !s.onBoundary[v] && region[v] < 0;
            area += s.area[v];
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1] && clean; ++k)
            {
                const Index u = s.adj[k];
                clean = region[u] < 0;
                if (along(u) < tip.depth && inside.insert(u).second)
                {
                    taken.push_back(u);
                    stack.push_back(u);
                }
            }
        }
        if (!clean || area > kLargestShare * total)
            continue;
        const int id = int(cut.size());
        for (Index v : taken)
            region[v] = id;
        for (size_t e = 0; e < s.edges.size(); ++e)
        {
            const Index a = s.edges[e][0], b = s.edges[e][1];
            if (region[a] != id || region[b] != id || !sharp(Index(e)))
                continue;
            const glm::dvec3 dir = glm::normalize(s.pos[b] - s.pos[a]);
            if (std::abs(glm::dot(dir, tip.axis)) < 0.5)
                tip.core.push_back({s.pos[a], s.pos[b]});
        }
        cut.push_back(std::move(tip));
    }
    if (cut.empty())
        return;
    std::unordered_map<uint64_t, Index> onEdge;
    auto crossing = [&](Index in, Index out) {
        const uint64_t key = edgeKey(in, out);
        if (auto it = onEdge.find(key); it != onEdge.end())
            return it->second;
        ClippedTip& tip = cut[size_t(region[in])];
        const double a = glm::dot(s.pos[in] - tip.tip, tip.axis), b = glm::dot(s.pos[out] - tip.tip, tip.axis);
        const double t = std::clamp((tip.depth - a) / std::max(b - a, 1e-300), 0.0, 1.0);
        const Index made = Index(s.pos.size());
        s.pos.push_back(glm::mix(s.pos[in], s.pos[out], t));
        s.density.push_back(glm::mix(s.density[in], s.density[out], float(t)));
        tip.radius = std::max(tip.radius, glm::distance(s.pos[made], tip.tip + tip.depth * tip.axis));
        tip.rim.push_back(s.pos[made]);
        if (s.hard.contains(key))
            s.hard.insert(edgeKey(made, out));
        onEdge[key] = made;
        return made;
    };
    std::vector<std::array<Index, 3>> kept;
    for (const auto& tri : s.tris)
    {
        int in = 0;
        for (Index v : tri)
            in += region[v] >= 0;
        if (in == 0)
            kept.push_back(tri);
        if (in == 0 || in == 3)
            continue;
        // The part beyond the plane: the points off the tip and the two crossings.
        std::vector<Index> poly;
        for (int k = 0; k < 3; ++k)
        {
            const Index a = tri[k], b = tri[(k + 1) % 3];
            if (region[a] < 0)
                poly.push_back(a);
            if ((region[a] < 0) != (region[b] < 0))
                poly.push_back(region[a] >= 0 ? crossing(a, b) : crossing(b, a));
        }
        for (size_t k = 1; k + 1 < poly.size(); ++k)
            kept.push_back({poly[0], poly[k], poly[k + 1]});
    }
    s.tris = std::move(kept);
    tips.insert(tips.end(), cut.begin(), cut.end());
}

/**
 * Builds the tips clipSharpTips cut off back onto the result: each hole left round a cut is closed with rungs of faces
 * running from its rim to the tip's own creases, a ring a quad apart, the last ring on the crease (or the tip point).
 * Returns how many tips were closed.
 */
int closeTips(geo::Mesh& m, const std::vector<ClippedTip>& tips, double h, const geo::MeshBvh& surface)
{
    if (tips.empty())
        return 0;
    // A cut the extraction closed over: the faces lying on its plane go, and leave its rim.
    {
        bool any = false;
        for (Index f = 0; f < m.faceCapacity(); ++f)
        {
            if (!m.faceValid(f))
                continue;
            const glm::dvec3 centre = m.faceCentroid(f), normal = m.faceNormal(f);
            for (const ClippedTip& tip : tips)
                if (std::abs(glm::dot(centre - tip.tip, tip.axis) - tip.depth) < 0.25 * h &&
                    glm::distance(centre, tip.tip + tip.depth * tip.axis) < tip.radius && std::abs(glm::dot(normal, tip.axis)) > 0.7)
                {
                    m.removeFace(f);
                    any = true;
                    break;
                }
        }
        if (any)
            m.removeAllLoose();
    }
    // Each border edge the way its face runs it.
    std::unordered_map<Index, Index> next;
    auto trace = [&] {
        next.clear();
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.forEachFaceCorner(f, [&](Index c) {
                    if (m.isBoundaryEdge(m.cornerEdge(c)))
                        next[m.cornerPoint(c)] = m.cornerNextPoint(c);
                });
    };
    trace();
    // Two rim points taken onto the same corner of a cut are one point.
    {
        std::vector<Index> target(m.pointCapacity(), kInvalid);
        bool any = false;
        for (const auto& [a, b] : next)
            if (glm::distance(m.position(a), m.position(b)) < 1e-6 * h)
            {
                Index to = a;
                while (target[to] != kInvalid)
                    to = target[to];
                if (to != b)
                {
                    target[b] = to;
                    any = true;
                }
            }
        if (any)
        {
            for (Index p = 0; p < Index(target.size()); ++p)
                if (target[p] != kInvalid)
                    while (target[target[p]] != kInvalid)
                        target[p] = target[target[p]];
            geo::weldPoints(m, target);
            m.removeAllLoose();
            trace();
        }
    }
    std::vector<std::vector<Index>> loops;
    std::unordered_set<Index> seen;
    for (const auto& [start, unused] : next)
    {
        if (seen.contains(start))
            continue;
        std::vector<Index> loop;
        Index at = start;
        while (!seen.contains(at) && next.contains(at))
        {
            seen.insert(at);
            loop.push_back(at);
            at = next.at(at);
        }
        if (at == start && loop.size() >= 3)
            loops.push_back(std::move(loop));
    }
    int closed = 0;
    std::vector<uint8_t> used(loops.size(), 0);
    for (const ClippedTip& tip : tips)
    {
        const glm::dvec3 middle = tip.tip + tip.depth * tip.axis;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            if (used[l])
                continue;
            std::vector<Index>& loop = loops[l];
            std::vector<uint8_t> atCut(loop.size());
            size_t count = 0;
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const glm::dvec3 at = m.position(loop[i]);
                // On the cut's plane and by its rim: the plane alone passes through the next tip's rim too.
                if (std::abs(glm::dot(at - tip.tip, tip.axis) - tip.depth) > 0.75 * h)
                    continue;
                for (size_t k = 0; k < tip.rim.size() && !atCut[i]; ++k)
                    atCut[i] = glm::distance(at, tip.rim[k]) < 2.5 * h;
                count += atCut[i];
            }
            if (std::getenv("LILA_TIP_LOG") && count > 0)
            {
                std::string flags;
                for (uint8_t f : atCut)
                    flags += f ? 'x' : '.';
                std::fprintf(stderr, "tip loop %s\n", flags.c_str());
            }
            if (5 * count < 3 * loop.size() || count < 3)
                continue;
            if (count < loop.size())
            {
                // The rim with a notch in it, where the layout left a face or two out beside the cut: each notch is
                // closed with one face, and the rim runs straight past it.
                size_t first = 0;
                while (!atCut[first] || atCut[(first + loop.size() - 1) % loop.size()])
                    first = (first + 1) % loop.size();
                std::vector<Index> rim, notch;
                bool whole = true;
                for (size_t n = 0; n < loop.size() && whole; ++n)
                {
                    const size_t i = (first + n) % loop.size();
                    if (!atCut[i])
                    {
                        notch.push_back(loop[i]);
                        whole = notch.size() <= 4;
                        continue;
                    }
                    if (!notch.empty())
                    {
                        // Against the border's own direction, from the rim point after the notch back to the one before.
                        std::vector<Index> face{loop[i]};
                        face.insert(face.end(), notch.rbegin(), notch.rend());
                        face.push_back(rim.back());
                        m.addFace(std::span<const Index>(face));
                        notch.clear();
                    }
                    rim.push_back(loop[i]);
                }
                if (!whole)
                    continue;
                if (!notch.empty())
                {
                    std::vector<Index> face{rim.front()};
                    face.insert(face.end(), notch.rbegin(), notch.rend());
                    face.push_back(rim.back());
                    m.addFace(std::span<const Index>(face));
                }
                loop = std::move(rim);
            }
            used[l] = 1;
            // Where each rim point's rungs end: the nearest point of the tip's creases, those close together as one.
            std::vector<glm::dvec3> ends;
            std::vector<Index> endPoint, endOf(loop.size());
            double furthest = 0.0;
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const glm::dvec3 at = m.position(loop[i]);
                const glm::dvec3 foot = tip.core.empty() ? tip.tip : closestOnSegments(at, tip.core);
                furthest = std::max(furthest, glm::distance(at, foot));
                size_t same = ends.size();
                for (size_t k = 0; k < ends.size() && same == ends.size(); ++k)
                    if (glm::distance(ends[k], foot) < 0.4 * h)
                        same = k;
                if (same == ends.size())
                {
                    ends.push_back(foot);
                    endPoint.push_back(m.addPoint(foot));
                }
                endOf[i] = Index(same);
            }
            // Along the tip's crease, for the ends that fall between a face's own two.
            glm::dvec3 along(0.0);
            for (const auto& [a, b] : tip.core)
                if (glm::distance(a, b) > glm::length(along))
                    along = b - a;
            auto between = [&](Index from, Index to) {
                std::vector<std::pair<double, Index>> found;
                const double a = glm::dot(ends[from], along), b = glm::dot(ends[to], along);
                for (size_t k = 0; k < ends.size(); ++k)
                    if (const double at = glm::dot(ends[k], along); Index(k) != from && Index(k) != to && at > std::min(a, b) && at < std::max(a, b))
                        found.push_back({a < b ? at : -at, endPoint[k]});
                std::sort(found.begin(), found.end());
                return found;
            };
            const int rungs = std::clamp(int(std::lround(furthest / h)), 1, 8);
            // ring[j][i]: rim point i moved j rungs towards its end.
            std::vector<std::vector<Index>> ring(size_t(rungs) + 1, std::vector<Index>(loop.size()));
            for (size_t i = 0; i < loop.size(); ++i)
            {
                ring[0][i] = loop[i];
                ring[size_t(rungs)][i] = endPoint[endOf[i]];
                for (int j = 1; j < rungs; ++j)
                {
                    const glm::dvec3 at = glm::mix(m.position(loop[i]), ends[endOf[i]], double(j) / double(rungs));
                    geo::BvhClosest on;
                    ring[size_t(j)][i] = m.addPoint(surface.closestPoint(at, on, 2.0 * h) ? on.point : at);
                }
            }
            // Two rim edges running to the same end close with one quad between them, not a triangle each.
            std::vector<uint8_t> paired(loop.size(), 0);
            const size_t last = size_t(rungs) - 1;
            for (size_t i = 0; i < loop.size() && loop.size() >= 4; ++i)
            {
                const size_t before = (i + loop.size() - 1) % loop.size(), k = (i + 1) % loop.size(), after = (k + 1) % loop.size();
                if (paired[before] || paired[i] || paired[k] || endOf[i] != endOf[k] || endOf[k] != endOf[after] || ring[last][i] == ring[last][k] ||
                    ring[last][k] == ring[last][after] || ring[last][i] == ring[last][after])
                    continue;
                // Not across one of the tip's own creases: the two triangles have to lie in one face of the shape.
                const glm::dvec3 end = ends[endOf[k]], shared = m.position(ring[last][k]);
                const glm::dvec3 one = glm::cross(m.position(ring[last][i]) - shared, end - shared), two = glm::cross(end - shared, m.position(ring[last][after]) - shared);
                if (glm::dot(glm::normalize(one), glm::normalize(two)) < 0.9)
                    continue;
                const std::vector<Index> face{ring[last][after], ring[last][k], ring[last][i], endPoint[endOf[k]]};
                m.addFace(std::span<const Index>(face));
                paired[i] = paired[k] = 1;
            }
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const size_t k = (i + 1) % loop.size();
                for (size_t j = 0; j < size_t(rungs); ++j)
                {
                    if (j == last && paired[i])
                        continue;
                    // Against the rim edge's own direction, so the new face winds with the one across it.
                    // The other side of the tip may have a row more: its end on the crease joins this face's last edge.
                    std::vector<Index> face;
                    for (Index p : {ring[j][k], ring[j][i], ring[j + 1][i]})
                        if (std::find(face.begin(), face.end(), p) == face.end())
                            face.push_back(p);
                    if (j + 1 == size_t(rungs) && endOf[i] != endOf[k])
                        for (const auto& [at, p] : between(endOf[i], endOf[k]))
                            face.push_back(p);
                    if (std::find(face.begin(), face.end(), ring[j + 1][k]) == face.end())
                        face.push_back(ring[j + 1][k]);
                    if (face.size() >= 3)
                        m.addFace(std::span<const Index>(face));
                }
            }
            ++closed;
        }
    }
    return closed;
}

/** LILA_ISO_DUMP: the working triangles as an OBJ, their feature edges as lines. */
void dumpWorking(const Surface& iso)
{
    const char* dump = std::getenv("LILA_ISO_DUMP");
    FILE* f = dump ? std::fopen(dump, "w") : nullptr;
    if (!f)
        return;
    for (const glm::dvec3& p : iso.pos)
        std::fprintf(f, "v %.9g %.9g %.9g\n", p.x, p.y, p.z);
    for (const auto& t : iso.tris)
        std::fprintf(f, "f %d %d %d\n", int(t[0] + 1), int(t[1] + 1), int(t[2] + 1));
    for (uint64_t key : iso.hard)
        std::fprintf(f, "l %d %d\n", int(key >> 32) + 1, int(key & 0xffffffffu) + 1);
    std::fclose(f);
}

/**
 * A creased shape whose creases are all rims of flat faces on an otherwise smooth body (a sculpted bust cut flat at the
 * base): per triangle whether it is the body's, with the flat faces to come off for caps to stand in. False for any other creased shape.
 * A few stray sharp edges on the body itself (under a crease for every fifty quads' worth of surface) don't count and
 * come back in `stray`. s needs detectFeatures; h is the quad edge.
 */
bool withoutFlatFaces(const Surface& s, double h, std::vector<uint8_t>& body, std::vector<size_t>& stray)
{
    const auto why = [](int check) {
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "flat faces: check %d failed\n", check);
        return false;
    };
    const double tolerance = 0.1 * h;
    constexpr double kLargestShare = 0.3;
    const Index nt = Index(s.tris.size());
    std::vector<Index> parent(nt);
    std::iota(parent.begin(), parent.end(), Index(0));
    const auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeTriCount[e] == 2 && !s.edgeFeature[e])
            parent[find(s.edgeTris[e][0])] = find(s.edgeTris[e][1]);
    std::vector<double> area(nt, 0.0);
    double total = 0.0;
    for (Index t = 0; t < nt; ++t)
    {
        area[find(t)] += s.triArea[t];
        total += s.triArea[t];
    }
    const Index main = Index(std::max_element(area.begin(), area.end()) - area.begin());
    if (area[main] < (1.0 - kLargestShare) * total)
        return why(1);
    // Every crease has the body on one side and a face on the other.
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeFeature[e] && !s.edgeBoundary[e])
        {
            // An edge the simplifying left with three triangles is no crease of the shape.
            if (s.edgeTriCount[e] != 2)
            {
                stray.push_back(e);
                continue;
            }
            const Index a = find(s.edgeTris[e][0]), b = find(s.edgeTris[e][1]);
            if (a == main && b == main)
                stray.push_back(e);
            else if (a != main && b != main)
                return why(3);
        }
    double strayLength = 0.0;
    for (size_t e : stray)
        if (s.edgeTriCount[e] == 2)
            strayLength += glm::distance(s.pos[s.edges[e][0]], s.pos[s.edges[e][1]]);
    if (strayLength * h > 0.02 * total)
        return why(4);
    // Each face flat, and a disc: one rim, no handle.
    struct Face
    {
        glm::dvec3 normal{0.0}, centre{0.0};
        std::unordered_set<Index> points;
        int triangles = 0, edges = 0;
        /** Holds an edge with three triangles, so its counts say nothing of its shape. */
        bool tangled = false;
    };
    std::unordered_map<Index, Face> faces;
    for (Index t = 0; t < nt; ++t)
        if (const Index c = find(t); c != main)
        {
            Face& f = faces[c];
            f.normal += s.triNormal[t] * s.triArea[t];
            f.centre += (s.pos[s.tris[t][0]] + s.pos[s.tris[t][1]] + s.pos[s.tris[t][2]]) * (s.triArea[t] / 3.0);
            f.points.insert(s.tris[t].begin(), s.tris[t].end());
            ++f.triangles;
        }
    if (faces.empty())
        return why(5);
    for (size_t e = 0; e < s.edges.size(); ++e)
        for (int k = 0; k < std::min<int>(s.edgeTriCount[e], 2); ++k)
            if (const Index c = find(s.edgeTris[e][k]); c != main && (k == 0 || find(s.edgeTris[e][0]) != c))
            {
                ++faces[c].edges;
                faces[c].tangled = faces[c].tangled || s.edgeTriCount[e] > 2;
            }
    for (auto& [c, f] : faces)
    {
        if (area[c] <= 0.0 || (!f.tangled && int(f.points.size()) - f.edges + f.triangles != 1))
            return why(6);
        f.centre /= area[c];
        f.normal = glm::normalize(f.normal);
        for (Index p : f.points)
            if (std::abs(glm::dot(s.pos[p] - f.centre, f.normal)) > tolerance)
                return why(7);
    }
    body.resize(nt);
    for (Index t = 0; t < nt; ++t)
        body[t] = find(t) == main;
    return true;
}

/** Working triangles per quad for the stock stages, which size their quads off the triangles' mean edge. */
constexpr double kStockTriangles = 6.0;

/**
 * The stock stages' working triangles. False when the simplified surface turns out to have creases to hold, which the
 * stock isotropic pass can't be told about; s is then spent and has to be read again. Creases that are only the rims
 * of flat faces are held all the same: the faces come off, the body is laid out as an open shape, and its caps are kept.
 */
bool prepareStock(Work& w, const QuadRemeshOptions& opts, double faces, double halves, Stopwatch& clock, bool keepCaps)
{
    LILA_ZONE();

    Surface& s = w.s;
    std::vector<AutoRemesher::Vector3> vertices;
    vertices.reserve(s.pos.size());
    for (const glm::dvec3& p : s.pos)
        vertices.push_back(AutoRemesher::Vector3(p.x, p.y, p.z));
    std::vector<std::vector<size_t>> triangles;
    triangles.reserve(s.tris.size());
    for (const auto& t : s.tris)
        triangles.push_back({size_t(t[0]), size_t(t[1]), size_t(t[2])});
    const double voxel = std::sqrt(s.totalArea() / (kStockTriangles * faces * halves) / (0.86602540378 * 0.5));
    std::vector<AutoRemesher::Vector3> simpleVertices;
    std::vector<std::vector<size_t>> simpleTriangles;
    const AutoRemesher::ProgressHandler progress = [](float, const char*) { Budget::checkpoint(); };
    // Creases are Lila's to find (by angle and length, below); the stock pass's own angle test takes a sculpt's every
    // sharp wrinkle for one.
    AutoRemesher::AutoRemesher::resample(vertices, triangles, voxel, 4.0 * opts.adaptivity, 180.0, 0.0, 0, nullptr, nullptr, &progress,
                                         &simpleVertices, &simpleTriangles);
    auto fill = [](Surface& to, const std::vector<AutoRemesher::Vector3>& v, const std::vector<std::vector<size_t>>& t) {
        to.pos.clear();
        to.tris.clear();
        for (const auto& p : v)
            to.pos.push_back(glm::dvec3(p.x(), p.y(), p.z()));
        for (const auto& f : t)
            to.tris.push_back({Index(f[0]), Index(f[1]), Index(f[2])});
        to.density.assign(to.pos.size(), 1.0f);
        to.hard.clear();
        to.buildTopology();
        to.buildGeometry();
    };
    fill(s, simpleVertices, simpleTriangles);
    s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * w.h0);
    bool creased = false;
    for (size_t e = 0; e < s.edges.size(); ++e)
        creased = creased || (s.edgeFeature[e] && s.edgeTriCount[e] == 2);
    if (creased)
    {
        std::vector<uint8_t> body;
        std::vector<size_t> stray;
        if (!keepCaps || std::getenv("LILA_QR_NO_KEPT_CAPS") || !withoutFlatFaces(s, w.h0, body, stray))
            return false;
        for (size_t e : stray)
        {
            s.edgeFeature[e] = 0;
            for (Index v : s.edges[e])
                s.feature[v] = FeatureKind::None;
        }
        // The stock isotropic pass crumples a flat face made of slivers, so it only sees the body: the source's own
        // triangles that lie on the simplified body.
        std::vector<Index> sizes(s.tris.size(), 3), corners;
        for (const auto& t : s.tris)
            corners.insert(corners.end(), t.begin(), t.end());
        const geo::Mesh simplified = geo::Mesh::fromPolygons(s.pos, sizes, corners);
        const geo::MeshBvh onSimplified(simplified);
        const Surface source = surfaceFromMesh(w.src, opts.useHardEdges);
        std::vector<Index> local(source.pos.size(), kInvalid);
        vertices.clear();
        triangles.clear();
        for (const auto& t : source.tris)
        {
            geo::BvhClosest hit;
            if (!onSimplified.closestPoint((source.pos[t[0]] + source.pos[t[1]] + source.pos[t[2]]) / 3.0, hit) || hit.face == kInvalid ||
                !body[hit.face])
                continue;
            std::vector<size_t> tri;
            for (Index v : t)
            {
                if (local[v] == kInvalid)
                {
                    local[v] = Index(vertices.size());
                    vertices.push_back(AutoRemesher::Vector3(source.pos[v].x, source.pos[v].y, source.pos[v].z));
                }
                tri.push_back(size_t(local[v]));
            }
            triangles.push_back(std::move(tri));
        }
        AutoRemesher::AutoRemesher::resample(vertices, triangles, voxel, 4.0 * opts.adaptivity, 180.0, 0.0, 0, nullptr, nullptr, &progress, nullptr,
                                             nullptr);
        w.creases.build(s, 0.5 * w.h0, false, true);
    }
    w.border.build(s, 0.5 * w.h0, true);
    clock.lap("surface");
    {
        std::vector<Index> sizes(s.tris.size(), 3), corners;
        corners.reserve(s.tris.size() * 3);
        for (const auto& t : s.tris)
            corners.insert(corners.end(), t.begin(), t.end());
        w.simple = geo::Mesh::fromPolygons(s.pos, sizes, corners);
        w.simpleBvh.emplace(w.simple);
    }
    fill(w.iso, vertices, triangles);
    // Kept caps go on first, so the chain on the symmetry plane runs across them too.
    if (creased && !capBorders(w.iso, w.caps, w.iso.hard, &w.border, 0.75 * w.h0, &w.keptCaps))
    {
        w.iso = Surface();
        return false;
    }
    if (w.symmetric)
    {
        const std::unordered_set<uint64_t> chain = symmetrize(w.iso, w.plane, opts.symmetryAxis);
        w.iso.hard.insert(chain.begin(), chain.end());
        w.wholeLayout = true;
    }
    if (!w.closed && !creased && !capBorders(w.iso, w.caps, w.iso.hard))
    {
        w.iso = Surface();
        w.wholeLayout = false;
        return false;
    }
    if (w.caps.numFaces() > 0)
        w.capsBvh.emplace(w.caps);
    if (w.keptCaps.numFaces() > 0)
        w.keptCapsBvh.emplace(w.keptCaps);
    w.iso.detectFeatures(-1.0, 75.0, 0.0);
    w.isoSize.assign(w.iso.pos.size(), w.h0);
    w.stock = w.stockSizes = true;
    dumpWorking(w.iso);
    w.stockQuads = opts.targetEdgeLength > 0.0 ? s.totalArea() / (w.h0 * w.h0) : faces * halves;
    if (std::getenv("LILA_STAGE_LOG"))
        std::fprintf(stderr, "stock triangles: %zu simplified, %zu working, edge %g, %u cap and %u kept cap triangles, %zu held edges\n", s.tris.size(),
                     w.iso.tris.size(), voxel, unsigned(w.caps.numFaces()), unsigned(w.keptCaps.numFaces()), w.iso.hard.size());
    Budget::note(std::to_string(w.iso.tris.size()) + " working triangles for about " + std::to_string(int(faces * halves)) + " faces");
    clock.lap("isotropic");
    return true;
}

void prepareWork(Work& w, const geo::Mesh& source, const QuadRemeshOptions& opts, Stopwatch& clock, bool keepCaps = true)
{
    LILA_ZONE();

    if (source.numFaces() == 0)
    {
        w.error = "source has no faces";
        return;
    }
    w.src = source;
    w.src.compact();
    w.adaptivity = opts.adaptivity;
    w.bvh.emplace(w.src);
    w.closed = isClosed(w.src);
    clock.lap("bvh");

    Surface& s = w.s;
    s = surfaceFromMesh(w.src, opts.useHardEdges);
    s.pins = opts.pinnedPoints;
    w.symmetric = opts.symmetryAxis >= 0 && opts.symmetryAxis < 3;
    if (w.symmetric)
    {
        w.plane.normal = glm::dvec3(0.0);
        w.plane.normal[opts.symmetryAxis] = 1.0;
    }
    // Sizes are for the half that is remeshed; the shape is simplified whole and clipped after, since simplifying the
    // clipped half folds triangles beside its cut border.
    const double halves = w.symmetric ? 2.0 : 1.0;
    const double area = s.totalArea() / halves;
    const double faces = std::max(8.0, double(opts.targetFaces) / halves);
    w.h0 = opts.targetEdgeLength > 0.0 ? opts.targetEdgeLength : std::sqrt(area / faces);
    const double triEdge = w.h0 / triangleRatio();
    const double workTris = std::max(64.0, area / (0.433 * triEdge * triEdge));
    // Anything the stock stages can't hold takes Lila's own feature-aware triangles and sizes.
    const bool plain = s.hard.empty() && opts.pinnedPoints.empty() && opts.featureGuides.empty() && !opts.poles &&
                       !std::getenv("LILA_QR_OWN");
    if (plain)
    {
        if (prepareStock(w, opts, faces, halves, clock, keepCaps))
            return;
        s = surfaceFromMesh(w.src, opts.useHardEdges);
        s.pins = opts.pinnedPoints;
    }
    const bool decimated = double(s.tris.size()) > 8.0 * halves * workTris;
    if (decimated)
        decimateSurface(s, Index(4.0 * halves * workTris));
    if (w.symmetric)
    {
        clipSurface(s, w.plane, opts.symmetryAxis);
        if (s.tris.empty())
        {
            w.error = "nothing on the positive side of the symmetry plane";
            return;
        }
    }
    // Long source triangles split down first: remeshing straight from a few long slivers piles up short edges it can't
    // collapse.
    refineSurface(s, triEdge);
    if (!opts.featureGuides.empty())
    {
        const std::unordered_set<uint64_t> own = s.hard;
        if (const int dropped = imprintGuides(s, opts.featureGuides))
            w.notes.push_back(std::to_string(dropped) + " of " + std::to_string(opts.featureGuides.size()) +
                              " edge loop curves couldn't be cut into the surface, so the layout doesn't follow them");
        for (uint64_t key : s.hard)
            if (!own.contains(key))
                w.guideSegs.push_back({s.pos[Index(key >> 32)], s.pos[Index(key & 0xffffffffu)]});
    }
    s.buildTopology();
    s.buildGeometry();
    s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * w.h0);
    if (decimated && opts.smoothInput)
    {
        smoothSurface(s, *w.bvh, 8);
        s.buildGeometry();
        s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * w.h0);
    }
    clock.lap("surface");

    // The working triangles lie on the simplified surface, which keeps the shape but not the sculpted pores that would
    // read as curvature everywhere. Taken, with the creases, before any tip is cut off: the tips go back on.
    {
        std::vector<Index> sizes(s.tris.size(), 3), corners;
        corners.reserve(s.tris.size() * 3);
        for (const auto& t : s.tris)
            corners.insert(corners.end(), t.begin(), t.end());
        w.simple = geo::Mesh::fromPolygons(s.pos, sizes, corners);
        w.simpleBvh.emplace(w.simple);
    }
    w.creases.build(s, 0.5 * w.h0, false, true);
    if (w.closed && !w.symmetric && opts.pinnedPoints.empty() && !std::getenv("LILA_QR_NO_TIP_CLIP"))
    {
        clipSharpTips(s, w.h0, opts.sharpAngle, w.tips);
        if (!w.tips.empty())
        {
            s.buildTopology();
            s.buildGeometry();
            s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * w.h0);
        }
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "sharp tips cut off: %zu\n", w.tips.size());
    }
    w.border.build(s, 0.5 * w.h0, true);
    std::vector<double> target = sizeField(s, w, opts, faces, CurvatureSizing::Triangles);
    if (std::getenv("LILA_STAGE_LOG"))
    {
        std::vector<double> sorted = target;
        std::sort(sorted.begin(), sorted.end());
        int borders = 0, corners = 0;
        for (size_t e = 0; e < s.edges.size(); ++e)
            borders += s.edgeBoundary[e];
        for (Index v = 0; v < s.vertexCount(); ++v)
            corners += s.feature[v] == FeatureKind::Corner;
        std::fprintf(stderr, "surface: %zu triangles, h0 %g, quad size p10 %g p50 %g p90 %g, mean edge %g, %zu hard, %d border edges, %d corners\n",
                     s.tris.size(), w.h0, sorted[sorted.size() / 10], sorted[sorted.size() / 2], sorted[sorted.size() * 9 / 10], s.meanEdgeLength(),
                     s.hard.size(), borders, corners);
    }
    clock.lap("density");
    for (double& t : target)
        t /= triangleRatio();
    w.iso = remeshIsotropic(s, target, *w.simpleBvh, w.creases, w.border);
    w.iso.buildTopology();
    w.iso.buildGeometry();
    // Its features are the hard set it carries over, not new creases read off the coarser triangles.
    w.iso.detectFeatures(-1.0, 75.0, 0.0);
    // The quads' own sizes, measured on the working triangles where curvature reads at about the quads' scale.
    w.isoSize = sizeField(w.iso, w, opts, faces, CurvatureSizing::Quads);
    // Creased is a crease for every fifty quads' worth of surface or so: a sculpt's few stray sharp edges don't count.
    double creased = 0.0;
    for (size_t e = 0; e < w.iso.edges.size(); ++e)
        if (w.iso.edgeFeature[e] && !w.iso.edgeBoundary[e])
            creased += glm::distance(w.iso.pos[w.iso.edges[e][0]], w.iso.pos[w.iso.edges[e][1]]);
    w.stockSizes = creased * w.h0 < 0.02 * w.iso.totalArea() && !std::getenv("LILA_QR_OWN_SIZES");
    if (std::getenv("LILA_STAGE_LOG"))
        std::fprintf(stderr, "creased %g of %g, stock sizes %d\n", creased * w.h0, 0.02 * w.iso.totalArea(), int(w.stockSizes));
    if (w.symmetric && !std::getenv("LILA_QR_HALF"))
    {
        // Laid out whole like the stock path: the half joined to its mirror image, its cut an edge chain to hold.
        std::vector<Index> mirrorOf;
        Surface whole = mirrorSurface(w.iso, w.plane, mirrorOf);
        const Index n = w.iso.vertexCount();
        for (size_t e = 0; e < w.iso.edges.size(); ++e)
        {
            const Index a = w.iso.edges[e][0], b = w.iso.edges[e][1];
            if (w.iso.edgeTriCount[e] == 1 && mirrorOf[a] == a && mirrorOf[b] == b)
                whole.hard.insert(edgeKey(a, b));
        }
        w.isoSize.resize(whole.pos.size());
        for (Index v = n; v < whole.vertexCount(); ++v)
            w.isoSize[v] = w.isoSize[mirrorOf[v]];
        w.iso = std::move(whole);
        w.iso.buildTopology();
        w.iso.buildGeometry();
        w.iso.detectFeatures(-1.0, 75.0, 0.0);
        w.wholeLayout = true;
    }
    w.stockQuads = opts.targetEdgeLength > 0.0 ? w.iso.totalArea() / (w.h0 * w.h0) : faces * (w.wholeLayout ? halves : 1.0);
    dumpWorking(w.iso);
    Budget::note(std::to_string(w.iso.tris.size()) + " working triangles for about " + std::to_string(int(faces)) + " faces");
    clock.lap("isotropic");
}

/** The working triangles in the form the field and cover solvers take. */
struct CoverInput
{
    std::vector<AutoRemesher::Vector3> vertices;
    std::vector<std::vector<size_t>> triangles;
    std::unordered_set<uint64_t> features;
    std::vector<size_t> pinned;
    std::vector<double> faceSize;
    std::vector<AutoRemesher::FaceHint> hints;
    std::vector<double> stiffness;
    std::vector<AutoRemesher::Vector3> guidance;
    /** For the stock layout: a factor on its own sizes, and the longest edge allowed, each empty when it does nothing. */
    std::vector<double> sizeFactor, sizeLimit;
    /** Per triangle, whether it is on a cap that is dropped, and on any cap at all (dropped or kept). Empty without. */
    std::vector<uint8_t> onCap, onAnyCap;
    /** The field turned through corners too sharp for it to lie along both sides, and those sides' corners held. */
    std::vector<std::pair<size_t, AutoRemesher::Vector3>> fieldOverrides;
    std::unordered_set<size_t> forcedCorners;
};

/**
 * What Lila's own size controls ask of the stock layout, per working triangle. factor: painted density, joints and
 * handles, as a multiple of the size curvature gives. limit: the longest edge gaps and thin parts allow, 0 where they
 * don't care. Each stays empty when nothing asks.
 */
void stockSizes(const Work& w, const QuadRemeshOptions& opts, std::vector<double>& factor, std::vector<double>& limit)
{
    const Surface& iso = w.iso;
    auto perTriangle = [&](const std::vector<double>& at) {
        std::vector<double> out(iso.tris.size());
        for (size_t t = 0; t < iso.tris.size(); ++t)
            out[t] = (at[iso.tris[t][0]] + at[iso.tris[t][1]] + at[iso.tris[t][2]]) / 3.0;
        return out;
    };
    bool painted = false;
    for (float d : iso.density)
        painted = painted || d != 1.0f;
    if (painted || opts.jointDensity > 0.0 || !opts.densityHandles.empty())
    {
        QuadRemeshOptions flat = opts;
        flat.adaptivity = 0.0;
        flat.resolveGaps = false;
        flat.edgesAround = 0;
        flat.targetEdgeLength = w.h0;
        const std::vector<double> size = sizeField(iso, w, flat, 1.0, CurvatureSizing::Quads);
        bool any = false;
        for (double v : size)
            any = any || std::abs(v / w.h0 - 1.0) > 0.02;
        if (any)
        {
            factor = perTriangle(size);
            for (double& f : factor)
                f /= w.h0;
        }
    }
    if (w.closed && (opts.resolveGaps || opts.edgesAround > 0))
    {
        // The stock sizes run from a third of the mean to three times it, and each fit looks only as far as the size it
        // is given and stops at a fraction of it: one from each end and the mean, the tightest of the three.
        constexpr double kLargest = 3.0;
        const std::vector<double> curvature = opts.edgesAround > 0 ? estimateCurvature(iso).magnitude : std::vector<double>();
        std::vector<double> size(iso.pos.size(), kLargest * w.h0);
        for (double from : {kLargest * w.h0, w.h0, w.h0 / kLargest})
        {
            std::vector<double> fitted(iso.pos.size(), from);
            if (opts.resolveGaps)
                fitGaps(iso, w.src, *w.bvh, fitted);
            // A little under the ask: the layout rounds the count round a part to a whole number, down as often as up.
            if (opts.edgesAround > 0)
                fitThickness(iso, w.src, *w.bvh, curvature, opts.edgesAround, 0.92, fitted);
            for (size_t v = 0; v < size.size(); ++v)
                if (fitted[v] < 0.98 * from)
                    size[v] = std::min(size[v], fitted[v]);
        }
        bool any = false;
        for (double& v : size)
        {
            any = any || v < 0.98 * kLargest * w.h0;
            if (v >= 0.98 * kLargest * w.h0)
                v = 0.0;
        }
        if (any)
        {
            // A triangle is limited by its tightest corner; a corner with no limit doesn't loosen the others.
            limit.assign(iso.tris.size(), 0.0);
            for (size_t t = 0; t < iso.tris.size(); ++t)
                for (Index v : iso.tris[t])
                    if (size[v] > 0.0)
                        limit[t] = limit[t] > 0.0 ? std::min(limit[t], size[v]) : size[v];
        }
    }
}

void turnSharpCorners(const Surface& iso, const std::vector<uint8_t>& guideEdge, CoverInput& in);
std::vector<uint8_t> guideEdges(const Work& w);

AutoRemesher::Vector3 toAr(const glm::dvec3& p) { return AutoRemesher::Vector3(p.x, p.y, p.z); }
glm::dvec3 fromAr(const AutoRemesher::Vector3& p) { return glm::dvec3(p.x(), p.y(), p.z()); }

/** Each pole's working triangle: the one with the nearest centre. Poles behind the symmetry plane drop. */
std::vector<FieldSingularity> polesOnSurface(const Work& w, const std::vector<Pole>& poles)
{
    std::vector<FieldSingularity> out;
    for (const Pole& p : poles)
    {
        if (p.valence == 4 || (w.symmetric && !w.wholeLayout && w.plane.distance(p.position) < 0.0))
            continue;
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (size_t t = 0; t < w.iso.tris.size(); ++t)
        {
            const auto& tri = w.iso.tris[t];
            const glm::dvec3 c = (w.iso.pos[tri[0]] + w.iso.pos[tri[1]] + w.iso.pos[tri[2]]) / 3.0;
            if (const double d = glm::dot(c - p.position, c - p.position); d < bestD)
            {
                bestD = d;
                best = Index(t);
            }
        }
        if (best != kInvalid)
            out.push_back({best, 4 - p.valence});
    }
    return out;
}

/**
 * The flow for a symmetric remesh: solved on the half joined to its mirror image, so it runs on across the plane as it
 * would on the whole shape instead of bending to meet the cut as a border, and held along the plane there so the
 * middle stays one edge loop. Per triangle of the half.
 */
std::vector<AutoRemesher::Vector3> symmetricFlow(const Work& w, const CoverInput& in, std::string& error)
{
    LILA_ZONE();

    const Surface& half = w.iso;
    std::vector<Index> mirrorOf;
    Surface whole = mirrorSurface(half, w.plane, mirrorOf);
    const size_t nt = half.tris.size();
    std::vector<AutoRemesher::Vector3> vertices;
    for (const glm::dvec3& p : whole.pos)
        vertices.push_back(toAr(p));
    std::vector<std::vector<size_t>> triangles;
    for (const auto& t : whole.tris)
        triangles.push_back({size_t(t[0]), size_t(t[1]), size_t(t[2])});
    // The cut is no feature of the whole; every other feature is, mirrored.
    auto onPlane = [&](Index v) { return mirrorOf[v] == v; };
    std::unordered_set<uint64_t> features;
    for (uint64_t key : in.features)
    {
        const Index a = Index(key >> 32), b = Index(key & 0xffffffffu);
        if (onPlane(a) && onPlane(b))
            continue;
        features.insert(key);
        features.insert(edgeKey(mirrorOf[a], mirrorOf[b]));
    }
    auto reflect = [&](const AutoRemesher::Vector3& d) {
        const glm::dvec3 v = fromAr(d);
        return toAr(v - 2.0 * glm::dot(v, w.plane.normal) * w.plane.normal);
    };
    std::vector<AutoRemesher::FaceHint> hints;
    for (const auto& h : in.hints)
    {
        hints.push_back(h);
        hints.push_back({h.face + nt, reflect(h.direction), h.weight});
    }
    for (size_t t = 0; t < whole.tris.size(); ++t)
    {
        const auto& f = whole.tris[t];
        for (int k = 0; k < 3; ++k)
            if (onPlane(f[k]) && onPlane(f[(k + 1) % 3]))
                hints.push_back({t, toAr(whole.pos[f[(k + 1) % 3]] - whole.pos[f[k]]), 8.0});
    }
    std::vector<double> stiffness;
    if (!in.stiffness.empty())
    {
        stiffness = in.stiffness;
        stiffness.insert(stiffness.end(), in.stiffness.begin(), in.stiffness.end());
    }
    AutoRemesher::Parameterizer pz(&vertices, &triangles, nullptr);
    pz.setFeatureEdges(&features);
    if (!hints.empty())
        pz.setFaceHints(&hints);
    if (!stiffness.empty())
        pz.setFieldStiffness(&stiffness);
    pz.setProgressHandler([](float, const char*) { Budget::checkpoint(); });
    if (!pz.solveField() || pz.field().size() != whole.tris.size())
    {
        error = "the edge flow couldn't be solved on this shape";
        return {};
    }
    return std::vector<AutoRemesher::Vector3>(pz.field().begin(), pz.field().begin() + std::ptrdiff_t(nt));
}

CoverInput coverInput(const Work& w, const QuadRemeshOptions& opts, std::string& error)
{
    LILA_ZONE();

    const Surface& iso = w.iso;
    CoverInput in;
    in.vertices.reserve(iso.pos.size());
    for (const glm::dvec3& p : iso.pos)
        in.vertices.push_back(toAr(p));
    in.triangles.reserve(iso.tris.size());
    for (const auto& t : iso.tris)
        in.triangles.push_back({size_t(t[0]), size_t(t[1]), size_t(t[2])});
    in.features = iso.hard;
    if (!opts.pinnedPoints.empty())
    {
        // Pins stay exactly where they are through the isotropic pass, so they match by position.
        std::unordered_set<uint64_t> at;
        auto key = [](const glm::dvec3& p) {
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < 3; ++i)
            {
                double d = p[i] == 0.0 ? 0.0 : p[i];
                uint64_t bits;
                std::memcpy(&bits, &d, sizeof(bits));
                h = (h ^ bits) * 1099511628211ull;
            }
            return h;
        };
        for (const glm::dvec3& p : opts.pinnedPoints)
            at.insert(key(p));
        for (size_t v = 0; v < iso.pos.size(); ++v)
            if (at.contains(key(iso.pos[v])))
                in.pinned.push_back(v);
    }
    in.faceSize.resize(iso.tris.size());
    for (size_t t = 0; t < iso.tris.size(); ++t)
    {
        const auto& f = iso.tris[t];
        in.faceSize[t] = (w.isoSize[f[0]] + w.isoSize[f[1]] + w.isoSize[f[2]]) / 3.0;
    }
    auto centre = [&](size_t t) { return (iso.pos[iso.tris[t][0]] + iso.pos[iso.tris[t][1]] + iso.pos[iso.tris[t][2]]) / 3.0; };
    if (w.stockSizes)
        stockSizes(w, opts, in.sizeFactor, in.sizeLimit);
    if (w.capsBvh)
    {
        in.onCap.resize(iso.tris.size());
        tbb::parallel_for(size_t(0), iso.tris.size(), [&](size_t t) { in.onCap[t] = w.onCap(centre(t)); });
    }
    if (w.keptCapsBvh)
    {
        in.onAnyCap.resize(iso.tris.size());
        tbb::parallel_for(size_t(0), iso.tris.size(),
                          [&](size_t t) { in.onAnyCap[t] = (!in.onCap.empty() && in.onCap[t]) || w.onKeptCap(centre(t)); });
    }
    else
        in.onAnyCap = in.onCap;

    // Flow guides: faces the curve passes over follow it.
    if (!opts.guides.empty())
    {
        // A layout made whole has the mirrored side to steer too.
        std::vector<std::vector<glm::dvec3>> guides = opts.guides;
        if (w.wholeLayout)
            for (const auto& guide : opts.guides)
            {
                guides.push_back(guide);
                for (glm::dvec3& p : guides.back())
                    p = w.plane.reflect(p);
            }
        for (const auto& guide : guides)
            for (size_t i = 0; i + 1 < guide.size(); ++i)
            {
                const glm::dvec3 a = guide[i], b = guide[i + 1], ab = b - a;
                const double len2 = glm::dot(ab, ab);
                if (len2 <= 0.0)
                    continue;
                geo::Bounds box;
                box.min = glm::min(a, b) - glm::dvec3(w.h0);
                box.max = glm::max(a, b) + glm::dvec3(w.h0);
                for (size_t t = 0; t < iso.tris.size(); ++t)
                {
                    const glm::dvec3 c = centre(t);
                    if (glm::any(glm::lessThan(c, box.min)) || glm::any(glm::greaterThan(c, box.max)))
                        continue;
                    const double u = std::clamp(glm::dot(c - a, ab) / len2, 0.0, 1.0);
                    const double d = glm::distance(c, a + u * ab);
                    const double reach = 0.75 * in.faceSize[t];
                    if (d < reach)
                        in.hints.push_back({t, toAr(ab), 4.0 * (1.0 - d / reach)});
                }
            }
    }
    // Soft creases: edges bending at least softCreaseAngle steer the faces either side along them.
    if (opts.softCreaseAngle > 0.0)
    {
        const double limit = std::cos(glm::radians(opts.softCreaseAngle));
        for (size_t e = 0; e < iso.edges.size(); ++e)
        {
            if (iso.edgeTriCount[e] != 2 || iso.hard.contains(edgeKey(iso.edges[e][0], iso.edges[e][1])))
                continue;
            const Index t0 = iso.edgeTris[e][0], t1 = iso.edgeTris[e][1];
            if (glm::dot(iso.triNormal[t0], iso.triNormal[t1]) > limit)
                continue;
            const glm::dvec3 dir = iso.pos[iso.edges[e][1]] - iso.pos[iso.edges[e][0]];
            in.hints.push_back({size_t(t0), toAr(dir), 2.0});
            in.hints.push_back({size_t(t1), toAr(dir), 2.0});
        }
    }
    // Joint Pole Avoidance: the field is stiffer in saddles (crotch, armpits, finger webs), so its poles settle beside them.
    if (opts.jointPoleAvoidance > 0.0)
    {
        const std::vector<double> saddle = saddleAmount(iso, 2.0 * w.h0);
        in.stiffness.resize(iso.tris.size());
        for (size_t t = 0; t < iso.tris.size(); ++t)
        {
            const auto& f = iso.tris[t];
            const double s = std::max({saddle[f[0]], saddle[f[1]], saddle[f[2]]});
            in.stiffness[t] = 1.0 + opts.jointPoleAvoidance * s;
        }
    }
    // Poles asked for: the field is solved turning exactly at them, and the cover keeps to it as it is.
    if (opts.poles)
    {
        FieldOptions fo;
        fo.scale = w.h0;
        fo.jointStiffness = opts.jointPoleAvoidance;
        PrescribedTwist pt = prescribeSingularities(iso, polesOnSurface(w, *opts.poles));
        if (!pt.error.empty())
        {
            error = pt.error;
            return in;
        }
        fo.twist = std::move(pt.twist);
        const std::vector<glm::dvec3> q = solveCrossField(iso, fo);
        in.guidance.resize(iso.tris.size());
        for (size_t t = 0; t < iso.tris.size(); ++t)
        {
            const auto& f = iso.tris[t];
            const glm::dvec3 n = iso.triNormal[t];
            glm::dvec3 d = q[f[0]] - n * glm::dot(n, q[f[0]]);
            glm::dvec3 sum = d;
            for (int k = 1; k < 3; ++k)
            {
                glm::dvec3 o = matchQuarterRotation(d, q[f[k]], n);
                sum += o - n * glm::dot(n, o);
            }
            const double len = glm::length(sum);
            in.guidance[t] = toAr(len > 0.0 ? sum / len : anyTangent(n));
        }
    }
    else if (w.symmetric && !w.wholeLayout)
        in.guidance = symmetricFlow(w, in, error);
    if (in.guidance.empty() && !w.stock && !std::getenv("LILA_QR_NO_CORNERS"))
        turnSharpCorners(iso, guideEdges(w), in);
    if (std::getenv("LILA_STAGE_LOG"))
        std::fprintf(stderr, "sharp corners: %zu triangles turned, %zu sides held, %zu curve segments\n", in.fieldOverrides.size(),
                     in.forcedCorners.size(), w.guideSegs.size());
    return in;
}

/**
 * The stock sizes as they come pinch rows of thin quads together at the end of a sharp crease (a nostril's): the size
 * jumps from triangle to triangle and the quads stretch up to 2.3 to 1 along it. Smoothed a little and held to 1.5 to 1,
 * the head and Mochi come out with fewer poles and folds at every size tried.
 *
 * Pole pairs cancel across twice the stock reach: fewer poles on the head at every size with the surface no further
 * off. At four times it starts to fold and drift, and at eight it flattens the nose.
 */
void tameSizes(AutoRemesher::Parameterizer& pz)
{
    pz.setMaximumSingularityPairDistance(12);
    pz.setScalingSmoothing(3);
    pz.setMaxAspectRatio(1.5);
}

/** Per edge of the working triangles, whether it lies along an imprinted edge loop curve (on either side of a whole layout). */
std::vector<uint8_t> guideEdges(const Work& w)
{
    const Surface& iso = w.iso;
    std::vector<uint8_t> out(iso.edges.size(), 0);
    if (w.guideSegs.empty())
        return out;
    tbb::parallel_for(size_t(0), iso.edges.size(), [&](size_t e) {
        if (!iso.edgeFeature[e] || iso.edgeTriCount[e] != 2)
            return;
        glm::dvec3 a = iso.pos[iso.edges[e][0]], b = iso.pos[iso.edges[e][1]];
        if (w.wholeLayout && w.plane.distance(0.5 * (a + b)) < 0.0)
        {
            a = w.plane.reflect(a);
            b = w.plane.reflect(b);
        }
        // Ends and middle: a crease leaving the curve at a slant has its far end off it.
        const double reach = 0.05 * glm::distance(a, b);
        auto on = [&](const glm::dvec3& p) { return glm::distance(closestOnSegments(p, w.guideSegs), p) < reach; };
        out[e] = on(a) && on(b) && on(0.5 * (a + b));
    });
    return out;
}

/**
 * Corners where two feature edges meet at under 45 degrees and no third joins them: turned at a prism's tip, where a
 * third crease runs down the side, the star prism came out with half as many poles again. A quad corner there has one side along U and the other
 * along V, but a field lying along both sides takes them for the same direction and the tip collapses. Across each
 * such corner's fan of triangles the field is turned from along the first side to square to the second (the long way
 * round, a quarter turn short of the corner's own angle), and the two sides are held to whichever direction that
 * makes them nearer.
 */
void turnSharpCorners(const Surface& iso, const std::vector<uint8_t>& guideEdge, CoverInput& in)
{
    constexpr double kSharpest = glm::radians(45.0);
    const Index n = iso.vertexCount();
    // Each vertex's triangles, and per triangle the vertex after and before it going round.
    std::vector<std::vector<Index>> around(n);
    for (Index t = 0; t < Index(iso.tris.size()); ++t)
        for (Index v : iso.tris[t])
            around[v].push_back(t);
    // Only creases and borders are a corner's sides: an edge loop curve crossing one at a slant makes a sharp sector
    // that is no corner.
    auto side = [&](Index e) { return iso.edgeFeature[e] != 0 && !guideEdge[e]; };
    auto featureBetween = [&](Index a, Index b) {
        for (Index k = iso.adjStart[a]; k < iso.adjStart[a + 1]; ++k)
            if (iso.adj[k] == b)
                return side(iso.adjEdge[k]);
        return false;
    };
    for (Index v = 0; v < n; ++v)
    {
        // The fan in order: each triangle (v, a, b) leads to the one whose first side is (v, b).
        std::unordered_map<Index, Index> byFirst;
        auto sides = [&](Index t, Index& a, Index& b) {
            const auto& tri = iso.tris[t];
            const int k = tri[0] == v ? 0 : tri[1] == v ? 1 : 2;
            a = tri[(k + 1) % 3];
            b = tri[(k + 2) % 3];
            return k;
        };
        int features = 0;
        for (Index t : around[v])
        {
            Index a, b;
            sides(t, a, b);
            byFirst[a] = t;
        }
        for (Index k = iso.adjStart[v]; k < iso.adjStart[v + 1]; ++k)
            features += side(iso.adjEdge[k]);
        if (features != 2)
            continue;
        for (Index start : around[v])
        {
            Index a, b;
            sides(start, a, b);
            if (!featureBetween(v, a))
                continue;
            // From this feature side round to the next.
            std::vector<Index> fan;
            double angle = 0.0;
            Index t = start;
            bool closed = false;
            for (size_t guard = 0; guard < around[v].size(); ++guard)
            {
                sides(t, a, b);
                fan.push_back(t);
                angle += std::acos(std::clamp(glm::dot(glm::normalize(iso.pos[a] - iso.pos[v]), glm::normalize(iso.pos[b] - iso.pos[v])), -1.0, 1.0));
                if (featureBetween(v, b))
                {
                    closed = true;
                    break;
                }
                auto next = byFirst.find(b);
                if (next == byFirst.end())
                    break;
                t = next->second;
            }
            if (!closed || angle >= kSharpest || angle <= 0.0)
                continue;
            double before = 0.0;
            for (size_t i = 0; i < fan.size(); ++i)
            {
                const int k = sides(fan[i], a, b);
                const glm::dvec3 first = glm::normalize(iso.pos[a] - iso.pos[v]);
                const glm::dvec3 normal = iso.triNormal[fan[i]];
                const double own = std::acos(std::clamp(glm::dot(first, glm::normalize(iso.pos[b] - iso.pos[v])), -1.0, 1.0));
                // At the middle of this triangle, measured from its own first side.
                const double middle = before + 0.5 * own;
                const double turn = middle / angle * (angle - 1.5707963267948966) - before;
                const glm::dvec3 dir = std::cos(turn) * first + std::sin(turn) * glm::cross(normal, first);
                in.fieldOverrides.push_back({size_t(fan[i]), toAr(dir)});
                if (i == 0)
                    in.forcedCorners.insert(3 * size_t(fan[i]) + size_t(k));
                if (i + 1 == fan.size())
                    in.forcedCorners.insert(3 * size_t(fan[i]) + size_t((k + 2) % 3));
                before += own;
            }
        }
    }
}

void setUp(AutoRemesher::Parameterizer& pz, const CoverInput& in, const Work& w)
{
    double edge = 0.0;
    size_t count = 0;
    for (const auto& t : in.triangles)
        for (int k = 0; k < 3; ++k, ++count)
            edge += glm::distance(w.iso.pos[t[k]], w.iso.pos[t[(k + 1) % 3]]);
    edge /= std::max<size_t>(1, count);
    // The cover's unit is scaling times the mean edge; with this it is one, so the face sizes are lengths.
    if (w.stock)
    {
        pz.setGradientAdaptivity(4.0 * w.adaptivity);
        tameSizes(pz);
        pz.setTargetQuadCount(w.stockQuads);
        if (!in.onAnyCap.empty())
            pz.setUnsizedFaces(&in.onAnyCap);
        if (!in.onCap.empty())
            pz.setUncountedFaces(&in.onCap);
        pz.setFeatureEdges(&in.features);
        if (!in.sizeFactor.empty())
            pz.setFaceScalingMultiplier(&in.sizeFactor);
        if (!in.sizeLimit.empty())
            pz.setFaceSizeLimit(&in.sizeLimit);
        if (!in.hints.empty())
            pz.setFaceHints(&in.hints);
        if (!in.stiffness.empty())
            pz.setFieldStiffness(&in.stiffness);
        pz.setProgressHandler([](float, const char*) { Budget::checkpoint(); });
        return;
    }
    if (!w.stockSizes)
    {
        pz.setScaling(1.0 / std::max(edge, 1e-12));
        pz.setFaceScaling(&in.faceSize);
    }
    else
    {
        pz.setGradientAdaptivity(4.0 * w.adaptivity);
        tameSizes(pz);
        pz.setTargetQuadCount(w.stockQuads);
        if (!in.sizeFactor.empty())
            pz.setFaceScalingMultiplier(&in.sizeFactor);
        if (!in.sizeLimit.empty())
            pz.setFaceSizeLimit(&in.sizeLimit);
    }
    pz.setFeatureEdges(&in.features);
    if (!in.fieldOverrides.empty())
    {
        pz.setFieldOverrides(&in.fieldOverrides);
        pz.setForcedCorners(&in.forcedCorners);
    }
    if (!in.pinned.empty())
        pz.setPinnedVertices(&in.pinned);
    if (!in.hints.empty())
        pz.setFaceHints(&in.hints);
    if (!in.stiffness.empty())
        pz.setFieldStiffness(&in.stiffness);
    if (!in.guidance.empty())
        pz.setSingularitySimplification(false);
    pz.setProgressHandler([](float, const char*) { Budget::checkpoint(); });
}

/** Per point, the feature it lies on: a corner, a crease or border line, or none. Only border points take a border. */
std::vector<FeatureKind> pointFeatures(const geo::Mesh& m, const FeatureLines& creases, const FeatureLines& border, double reach)
{
    std::vector<FeatureKind> feature(m.pointCapacity(), FeatureKind::None);
    tbb::parallel_for(Index(0), m.pointCapacity(), [&](Index p) {
        if (!m.pointValid(p))
            return;
        const bool onBorder = m.isBoundaryPoint(p);
        const FeatureLines& lines = onBorder ? border : creases;
        glm::dvec3 q;
        if (lines.closestCorner(m.position(p), reach, q))
            feature[p] = FeatureKind::Corner;
        else if (lines.closest(m.position(p), onBorder ? kBorderReach * reach : reach, q))
            feature[p] = FeatureKind::Line;
    });
    return feature;
}

/** The polygon two faces sharing exactly one edge make together, or empty when they share more or none. */
std::vector<Index> mergedPolygon(const std::vector<Index>& f, const std::vector<Index>& g)
{
    int shared = 0;
    size_t fi = 0, gi = 0;
    for (size_t i = 0; i < f.size(); ++i)
        for (size_t j = 0; j < g.size(); ++j)
            if (f[i] == g[(j + 1) % g.size()] && f[(i + 1) % f.size()] == g[j])
            {
                ++shared;
                fi = i;
                gi = j;
            }
    if (shared != 1)
        return {};
    // f from the end of the shared edge round to its start, then the rest of g.
    std::vector<Index> out;
    for (size_t k = 1; k <= f.size(); ++k)
        out.push_back(f[(fi + k) % f.size()]);
    for (size_t k = 2; k < g.size(); ++k)
        out.push_back(g[(gi + k) % g.size()]);
    std::vector<Index> sorted = out;
    std::sort(sorted.begin(), sorted.end());
    if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return {};
    return out;
}

/** How far a polygon's corners are from square: the sum of their cosines squared, and a lot for a corner folded back. */
double squareness(const std::vector<glm::dvec3>& points, const std::vector<Index>& f)
{
    double cost = 0.0;
    for (size_t k = 0; k < f.size(); ++k)
    {
        const glm::dvec3 p = points[f[k]], u = points[f[(k + f.size() - 1) % f.size()]] - p, v = points[f[(k + 1) % f.size()]] - p;
        const double lu = glm::length(u), lv = glm::length(v);
        if (lu <= 0.0 || lv <= 0.0)
            return 1e9;
        const double c = glm::dot(u, v) / (lu * lv);
        cost += c * c + (c < -0.9 ? 10.0 : 0.0);
    }
    return cost;
}

/** An even polygon as a ladder of quads, the squarest of its offsets. */
std::vector<std::vector<Index>> ladder(const std::vector<glm::dvec3>& points, const std::vector<Index>& loop)
{
    const size_t n = loop.size(), m = n / 2;
    double bestCost = std::numeric_limits<double>::max();
    std::vector<std::vector<Index>> best;
    for (size_t s = 0; s < m; ++s)
    {
        std::vector<std::vector<Index>> quads;
        double cost = 0.0;
        for (size_t i = 0; i + 1 < m; ++i)
        {
            quads.push_back({loop[(s + i) % n], loop[(s + i + 1) % n], loop[(s + n - 2 - i) % n], loop[(s + n - 1 - i) % n]});
            cost += squareness(points, quads.back());
        }
        if (cost < bestCost)
        {
            bestCost = cost;
            best = std::move(quads);
        }
    }
    return best;
}

/**
 * Splits poly into a quad and an odd face of oddSize points holding the edge (x, y), so the odd face moves on across
 * that edge: the squarer of the ways to do it.
 */
bool splitTowards(const std::vector<glm::dvec3>& points, const std::vector<Index>& poly, Index x, Index y, size_t oddSize,
                  std::vector<Index>& quad, std::vector<Index>& odd)
{
    const size_t n = poly.size();
    if (n != oddSize + 2)
        return false;
    size_t at = n;
    for (size_t k = 0; k < n; ++k)
        if ((poly[k] == x && poly[(k + 1) % n] == y) || (poly[k] == y && poly[(k + 1) % n] == x))
            at = k;
    if (at == n)
        return false;
    double bestCost = std::numeric_limits<double>::max();
    // The odd face is oddSize consecutive points covering positions at and at + 1; the quad is the rest plus its two ends.
    for (size_t first = at + n - (oddSize - 2); first <= at + n; ++first)
    {
        std::vector<Index> run, rest;
        for (size_t k = 0; k < oddSize; ++k)
            run.push_back(poly[(first + k) % n]);
        rest.push_back(run.back());
        for (size_t k = oddSize; k < n; ++k)
            rest.push_back(poly[(first + k) % n]);
        rest.push_back(run.front());
        if (rest.size() != 4)
            continue;
        const double cost = squareness(points, rest) + squareness(points, run);
        if (cost < bestCost)
        {
            bestCost = cost;
            quad = rest;
            odd = run;
        }
    }
    return bestCost < std::numeric_limits<double>::max();
}

/**
 * Pairs up the faces that aren't quads (triangles and pentagons) through the quads between them: each odd face walks
 * to a nearby other one, re-splitting itself and the next quad on the way so it moves one quad at a time, and the two
 * become quads where they meet. With seam, a triangle may instead walk onto an edge of the half's cut, where its mirror
 * image meets it into a quad once mirrored. Returns how many odd faces are left.
 */
size_t pairOddFaces(std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces, const std::function<bool(Index, Index)>& seam = {},
                    size_t maxPath = 120, double unsquare = 3.0)
{
    LILA_ZONE();

    auto seamEdgeOf = [&](size_t f, Index& x, Index& y) {
        if (!seam)
            return false;
        const auto& face = faces[f];
        for (size_t k = 0; k < face.size(); ++k)
            if (seam(face[k], face[(k + 1) % face.size()]))
            {
                x = face[k];
                y = face[(k + 1) % face.size()];
                return true;
            }
        return false;
    };
    auto isOdd = [&](size_t f) {
        Index x, y;
        return faces[f].size() == 5 || (faces[f].size() == 3 && !seamEdgeOf(f, x, y));
    };
    for (int round = 0; round < 4; ++round)
    {
        std::unordered_map<uint64_t, std::vector<size_t>> byEdge;
        for (size_t f = 0; f < faces.size(); ++f)
            for (size_t k = 0; k < faces[f].size(); ++k)
                byEdge[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])].push_back(f);
        auto neighbours = [&](size_t f) {
            std::vector<size_t> out;
            for (size_t k = 0; k < faces[f].size(); ++k)
            {
                const auto& list = byEdge[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])];
                if (list.size() == 2)
                    out.push_back(list[0] == f ? list[1] : list[0]);
            }
            return out;
        };
        std::vector<uint8_t> used(faces.size(), 0);
        bool any = false;
        for (size_t start = 0; start < faces.size(); ++start)
        {
            if (faces[start].empty() || used[start] || !isOdd(start))
                continue;
            // Cheapest first through quads, where going straight on across a quad costs one and turning in it costs
            // more: an odd face carried along one row leaves no poles behind it, and every turn leaves a pair. The first
            // few odd faces reached (or, for a triangle, quads on the seam) are candidates.
            constexpr double kTurn = 4.0;
            using State = std::pair<size_t, size_t>; // a face, and the face it was entered from
            std::map<State, std::pair<double, State>> reached;
            std::priority_queue<std::pair<double, State>, std::vector<std::pair<double, State>>, std::greater<>> open;
            const State origin{start, SIZE_MAX};
            reached[origin] = {0.0, origin};
            open.push({0.0, origin});
            std::vector<State> candidates;
            auto sharedEdge = [&](size_t f, size_t g) {
                for (size_t k = 0; k < faces[f].size(); ++k)
                {
                    const auto& list = byEdge[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])];
                    if (list.size() == 2 && (list[0] == g || list[1] == g))
                        return k;
                }
                return SIZE_MAX;
            };
            while (!open.empty() && candidates.size() < 6)
            {
                const auto [cost, at] = open.top();
                open.pop();
                if (cost > reached[at].first || cost > double(maxPath))
                    continue;
                const size_t f = at.first;
                const size_t cameBy = at.second == SIZE_MAX ? SIZE_MAX : sharedEdge(f, at.second);
                for (size_t k = 0; k < faces[f].size(); ++k)
                {
                    const auto& list = byEdge[edgeKey(faces[f][k], faces[f][(k + 1) % faces[f].size()])];
                    if (list.size() != 2)
                        continue;
                    const size_t g = list[0] == f ? list[1] : list[0];
                    if (used[g] || g == at.second || g == start)
                        continue;
                    const bool straight = cameBy == SIZE_MAX || faces[f].size() != 4 || (k + 4 - cameBy) % 4 == 2;
                    const double to = cost + 1.0 + (straight ? 0.0 : kTurn);
                    const State next{g, f};
                    auto it = reached.find(next);
                    if (it != reached.end() && it->second.first <= to)
                        continue;
                    reached[next] = {to, at};
                    Index sx, sy;
                    if (isOdd(g))
                        candidates.push_back(next);
                    else if (faces[g].size() == 4)
                    {
                        if (faces[start].size() == 3 && seamEdgeOf(g, sx, sy))
                            candidates.push_back(next);
                        open.push({to, next});
                    }
                }
            }
            for (const State& end : candidates)
            {
                const size_t found = end.first;
                std::vector<size_t> path;
                for (State at = end;; at = reached[at].second)
                {
                    path.push_back(at.first);
                    if (at == origin || path.size() > 4 * maxPath)
                        break;
                }
                std::reverse(path.begin(), path.end());
                std::vector<size_t> once = path;
                std::sort(once.begin(), once.end());
                if (path.front() != start || std::adjacent_find(once.begin(), once.end()) != once.end())
                    continue;
                bool toSeam = !isOdd(found);
                // The odd face walks along copies, so a step that fails leaves the faces as they were.
                std::vector<std::vector<Index>> work;
                for (size_t f : path)
                    work.push_back(faces[f]);
                bool ok = true;
                const size_t steps = toSeam ? work.size() - 1 : work.size() - 2;
                for (size_t i = 0; ok && i < steps; ++i)
                {
                    const std::vector<Index> poly = mergedPolygon(work[i], work[i + 1]);
                    // The edge the odd face moves up to: the one the next quad shares with the face after it, or its seam edge.
                    Index x = kInvalid, y = kInvalid;
                    const auto& q = work[i + 1];
                    if (i + 2 < work.size())
                    {
                        const auto& after = work[i + 2];
                        for (size_t k = 0; k < q.size() && x == kInvalid; ++k)
                            for (size_t j = 0; j < after.size(); ++j)
                                if (q[k] == after[(j + 1) % after.size()] && q[(k + 1) % q.size()] == after[j])
                                {
                                    x = q[k];
                                    y = q[(k + 1) % q.size()];
                                    break;
                                }
                    }
                    else
                        for (size_t k = 0; k < q.size(); ++k)
                            if (seam(q[k], q[(k + 1) % q.size()]))
                            {
                                x = q[k];
                                y = q[(k + 1) % q.size()];
                                break;
                            }
                    std::vector<Index> quad, odd;
                    ok = !poly.empty() && x != kInvalid && splitTowards(points, poly, x, y, work[i].size(), quad, odd);
                    if (ok)
                    {
                        work[i] = std::move(quad);
                        work[i + 1] = std::move(odd);
                    }
                }
                std::vector<std::vector<Index>> ending;
                if (ok && !toSeam)
                {
                    const std::vector<Index> poly = mergedPolygon(work[work.size() - 2], work.back());
                    ok = !poly.empty() && poly.size() % 2 == 0;
                    if (ok)
                        ending = poly.size() == 4 ? std::vector<std::vector<Index>>{poly} : ladder(points, poly);
                }
                // Two odd faces side by side always merge: an odd-shaped quad beats two faces that aren't quads.
                for (const auto& q : ending)
                    ok = ok && (path.size() == 2 ? squareness(points, q) < 1e9 : squareness(points, q) < unsquare);
                for (size_t i = 0; ok && i < steps; ++i)
                    ok = squareness(points, work[i]) < unsquare;
                if (!ok)
                    continue;
                for (size_t i = 0; i < steps; ++i)
                    faces[path[i]] = std::move(work[i]);
                if (toSeam)
                    faces[path.back()] = std::move(work.back());
                else
                {
                    faces[path[path.size() - 2]].clear();
                    faces[path.back()].clear();
                }
                for (size_t f : path)
                    used[f] = 1;
                for (auto& q : ending)
                {
                    faces.push_back(std::move(q));
                    used.push_back(1);
                }
                any = true;
                break;
            }
        }
        faces.erase(std::remove_if(faces.begin(), faces.end(), [](const std::vector<Index>& f) { return f.empty(); }), faces.end());
        if (!any)
            break;
    }
    size_t odd = 0;
    for (size_t f = 0; f < faces.size(); ++f)
        odd += isOdd(f);
    return odd;
}

/**
 * Collapses edges shorter than a tenth of a quad and drops the faces that leaves with fewer than three points, plus any
 * face repeating another's points: the extraction now and then squashes a quad onto a line or lays one twice. With no
 * h, a quad is as long as the face's own mean edge, for a layout whose sizes vary.
 */
void cleanSoup(std::vector<std::vector<Index>>& faces, const std::vector<glm::dvec3>& points, double h)
{
    const bool local = h <= 0.0;
    std::vector<Index> to(points.size());
    for (Index p = 0; p < Index(points.size()); ++p)
        to[p] = p;
    auto find = [&](Index p) {
        while (to[p] != p)
            p = to[p] = to[to[p]];
        return p;
    };
    for (const auto& f : faces)
    {
        if (local)
        {
            h = 0.0;
            for (size_t k = 0; k < f.size(); ++k)
                h += glm::distance(points[f[k]], points[f[(k + 1) % f.size()]]) / double(f.size());
        }
        for (size_t k = 0; k < f.size(); ++k)
        {
            const Index a = f[k], b = f[(k + 1) % f.size()];
            if (glm::distance(points[a], points[b]) < 0.1 * h)
            {
                const Index ra = find(a), rb = find(b);
                if (ra != rb)
                    to[std::max(ra, rb)] = std::min(ra, rb);
            }
        }
    }
    std::unordered_set<std::string> seen;
    for (auto& f : faces)
    {
        std::vector<Index> g;
        for (Index p : f)
        {
            const Index q = find(p);
            if (std::find(g.begin(), g.end(), q) == g.end())
                g.push_back(q);
        }
        if (g.size() < 3)
        {
            f.clear();
            continue;
        }
        std::vector<Index> key = g;
        std::sort(key.begin(), key.end());
        if (!seen.insert(std::string(reinterpret_cast<const char*>(key.data()), key.size() * sizeof(Index))).second)
        {
            f.clear();
            continue;
        }
        f = std::move(g);
    }
    faces.erase(std::remove_if(faces.begin(), faces.end(), [](const std::vector<Index>& f) { return f.empty(); }), faces.end());
}

/**
 * Merges each pair of faces that share two edges round a point of their own (a point with only those two faces) into
 * one face without it: two quads make a quad. Pairing odd faces leaves a few.
 */
void mergeDoublets(std::vector<std::vector<Index>>& faces, size_t pointCount)
{
    for (bool again = true; again;)
    {
        again = false;
        std::vector<std::array<size_t, 2>> at(pointCount, {SIZE_MAX, SIZE_MAX});
        std::vector<int> count(pointCount, 0);
        std::unordered_map<uint64_t, int> uses;
        for (size_t f = 0; f < faces.size(); ++f)
            for (size_t k = 0; k < faces[f].size(); ++k)
            {
                const Index p = faces[f][k];
                if (count[p] < 2)
                    at[p][count[p]] = f;
                ++count[p];
                ++uses[edgeKey(p, faces[f][(k + 1) % faces[f].size()])];
            }
        std::vector<uint8_t> touched(faces.size(), 0);
        for (Index p = 0; p < Index(pointCount); ++p)
        {
            if (count[p] != 2 || touched[at[p][0]] || touched[at[p][1]])
                continue;
            std::vector<Index>& f = faces[at[p][0]];
            std::vector<Index>& g = faces[at[p][1]];
            const size_t nf = f.size(), ng = g.size();
            if (nf + ng - 4 < 3 || nf + ng - 4 > 5)
                continue;
            const size_t kf = size_t(std::find(f.begin(), f.end(), p) - f.begin()), kg = size_t(std::find(g.begin(), g.end(), p) - g.begin());
            const Index a = f[(kf + 1) % nf], b = f[(kf + nf - 1) % nf];
            // Both of p's edges are shared by the two faces, so p is no border point and the faces close round it.
            if (g[(kg + 1) % ng] != b || g[(kg + ng - 1) % ng] != a || uses[edgeKey(p, a)] != 2 || uses[edgeKey(p, b)] != 2)
                continue;
            std::vector<Index> merged;
            for (size_t k = 1; k < nf; ++k)
                merged.push_back(f[(kf + k) % nf]);
            for (size_t k = 2; k + 1 < ng; ++k)
                merged.push_back(g[(kg + k) % ng]);
            std::vector<Index> sorted = merged;
            std::sort(sorted.begin(), sorted.end());
            if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            {
                // The same points both ways round: a pocket of two faces back to back, closed on itself.
                std::vector<Index> sf = f, sg = g;
                std::sort(sf.begin(), sf.end());
                std::sort(sg.begin(), sg.end());
                if (sf != sg)
                    continue;
                f.clear();
            }
            else
                f = std::move(merged);
            g.clear();
            touched[at[p][0]] = touched[at[p][1]] = 1;
            again = true;
        }
        faces.erase(std::remove_if(faces.begin(), faces.end(), [](const std::vector<Index>& f) { return f.empty(); }), faces.end());
    }
}

/**
 * Keeps the positive half of a layout made whole, for mirroring. The points along the cut go onto the plane, and a
 * triangle with an edge there (where the layout broke its middle edge loop) closes up along that edge, so the loop runs
 * on through one point instead of round a kite.
 */
void keepPositiveHalf(std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces, const geo::SymmetryPlane& plane)
{
    auto edgeUses = [&] {
        std::unordered_map<uint64_t, int> uses;
        for (const auto& f : faces)
            for (size_t k = 0; k < f.size(); ++k)
                ++uses[edgeKey(f[k], f[(k + 1) % f.size()])];
        return uses;
    };
    const std::unordered_map<uint64_t, int> whole = edgeUses();
    auto behind = [&](const std::vector<Index>& f) {
        glm::dvec3 c(0.0);
        for (Index v : f)
            c += points[v];
        return plane.distance(c / double(f.size())) < 0.0;
    };
    faces.erase(std::remove_if(faces.begin(), faces.end(), behind), faces.end());
    // The cut is the border the half has that the whole didn't: a border of the source's own stays where it is.
    std::unordered_map<uint64_t, int> uses = edgeUses();
    std::vector<uint8_t> onCut(points.size(), 0);
    for (auto& [key, n] : uses)
    {
        if (n == 1 && whole.at(key) == 2)
            onCut[key >> 32] = onCut[key & 0xffffffffu] = 1;
        else if (n == 1)
            n = 0;
    }
    for (Index p = 0; p < Index(points.size()); ++p)
        if (onCut[p])
            points[p] = plane.project(points[p]);
    std::vector<Index> to(points.size());
    for (Index p = 0; p < Index(points.size()); ++p)
        to[p] = p;
    auto find = [&](Index p) {
        while (to[p] != p)
            p = to[p] = to[to[p]];
        return p;
    };
    // A face with a corner on the cut and no other face at it (the loop turned a corner there) would share both edges
    // with its mirror image: the corner's two neighbours close onto it.
    std::vector<int> faceCount(points.size(), 0);
    for (const auto& f : faces)
        for (Index p : f)
            ++faceCount[p];
    for (const auto& f : faces)
        for (size_t k = 0; k < f.size(); ++k)
        {
            const Index a = f[(k + f.size() - 1) % f.size()], p = f[k], b = f[(k + 1) % f.size()];
            if (onCut[a] && onCut[p] && onCut[b] && faceCount[p] == 1)
                to[find(a)] = to[find(b)] = find(p);
        }
    for (auto& f : faces)
    {
        if (f.size() != 3)
            continue;
        for (size_t k = 0; k < 3; ++k)
        {
            const Index a = find(f[k]), b = find(f[(k + 1) % 3]);
            if (a != b && onCut[a] && onCut[b] && uses[edgeKey(f[k], f[(k + 1) % 3])] == 1)
            {
                points[a] = 0.5 * (points[a] + points[b]);
                to[b] = a;
                break;
            }
        }
    }
    for (auto& f : faces)
    {
        std::vector<Index> g;
        for (Index v : f)
            if (const Index q = find(v); std::find(g.begin(), g.end(), q) == g.end())
                g.push_back(q);
        f = g.size() >= 3 ? std::move(g) : std::vector<Index>{};
    }
    faces.erase(std::remove_if(faces.begin(), faces.end(), [](const std::vector<Index>& f) { return f.empty(); }), faces.end());
}

/** Drops the points no face uses. */
void compactSoup(std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces)
{
    std::vector<Index> id(points.size(), kInvalid);
    std::vector<glm::dvec3> kept;
    for (auto& f : faces)
        for (Index& v : f)
        {
            if (id[v] == kInvalid)
            {
                id[v] = Index(kept.size());
                kept.push_back(points[v]);
            }
            v = id[v];
        }
    points = std::move(kept);
}

/**
 * The half's border on the mirror plane: its points go onto the plane and points that meet there become one, so a sliver
 * row the layout left along the cut closes up instead of turning into faces lying flat in the plane.
 */
void weldSeam(std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces, const geo::SymmetryPlane& plane, int axis, double h)
{
    std::unordered_map<uint64_t, int> uses;
    for (const auto& f : faces)
        for (size_t k = 0; k < f.size(); ++k)
            ++uses[edgeKey(f[k], f[(k + 1) % f.size()])];
    std::vector<uint8_t> onSeam(points.size(), 0);
    for (const auto& [key, n] : uses)
        if (n == 1)
            for (Index p : {Index(key >> 32), Index(key & 0xffffffffu)})
                if (std::abs(plane.distance(points[p])) < 1.0 * h)
                {
                    points[p][axis] = plane.offset;
                    onSeam[p] = 1;
                }
    // Seam points closer than a fifth of a quad along the seam merge.
    std::vector<Index> seam;
    for (Index p = 0; p < Index(points.size()); ++p)
        if (onSeam[p])
            seam.push_back(p);
    std::vector<Index> to(points.size());
    for (Index p = 0; p < Index(points.size()); ++p)
        to[p] = p;
    auto find = [&](Index p) {
        while (to[p] != p)
            p = to[p] = to[to[p]];
        return p;
    };
    for (size_t i = 0; i < seam.size(); ++i)
        for (size_t j = i + 1; j < seam.size(); ++j)
            if (glm::distance(points[seam[i]], points[seam[j]]) < 0.2 * h)
            {
                const Index a = find(seam[i]), b = find(seam[j]);
                if (a != b)
                    to[std::max(a, b)] = std::min(a, b);
            }
    for (auto& f : faces)
    {
        std::vector<Index> g;
        for (Index p : f)
        {
            const Index q = find(p);
            if (g.empty() || g.back() != q)
                g.push_back(q);
        }
        while (g.size() > 1 && g.front() == g.back())
            g.pop_back();
        bool flat = true;
        for (Index p : g)
            flat = flat && onSeam[p];
        f = g.size() >= 3 && !flat ? std::move(g) : std::vector<Index>{};
    }
    faces.erase(std::remove_if(faces.begin(), faces.end(), [](const std::vector<Index>& f) { return f.empty(); }), faces.end());
    // A border that leaves the plane for one or two points and comes back is a quad (or triangle) the layout missed.
    {
        std::unordered_map<uint64_t, int> directed;
        auto dkey = [](Index a, Index b) { return (uint64_t(a) << 32) | b; };
        for (const auto& f : faces)
            for (size_t k = 0; k < f.size(); ++k)
                directed[dkey(f[k], f[(k + 1) % f.size()])] = 1;
        std::unordered_map<Index, Index> next;
        for (const auto& f : faces)
            for (size_t k = 0; k < f.size(); ++k)
            {
                const Index a = f[k], b = f[(k + 1) % f.size()];
                if (!directed.contains(dkey(b, a)))
                    next[a] = b;
            }
        std::vector<std::vector<Index>> added;
        for (const auto& [start, first] : next)
        {
            if (!onSeam[start] || onSeam[first])
                continue;
            std::vector<Index> chain{start, first};
            Index at = first;
            while (chain.size() <= 4)
            {
                auto it = next.find(at);
                if (it == next.end())
                    break;
                at = it->second;
                chain.push_back(at);
                if (onSeam[at])
                    break;
            }
            if (!onSeam[chain.back()] || chain.size() < 3 || chain.size() > 4 || chain.back() == start)
                continue;
            std::reverse(chain.begin(), chain.end());
            added.push_back(chain);
        }
        faces.insert(faces.end(), added.begin(), added.end());
    }
    // A seam point inside one face only, between two other seam points, would fold that face onto its mirror image: it
    // goes, and the face becomes a triangle that pairs with its mirror into a quad.
    std::vector<int> faceCount(points.size(), 0);
    for (const auto& f : faces)
        for (Index p : f)
            ++faceCount[p];
    for (auto& f : faces)
        for (bool again = true; again && f.size() > 3;)
        {
            again = false;
            for (size_t k = 0; k < f.size(); ++k)
            {
                const Index a = f[(k + f.size() - 1) % f.size()], b = f[k], c = f[(k + 1) % f.size()];
                if (onSeam[b] && onSeam[a] && onSeam[c] && faceCount[b] == 1)
                {
                    f.erase(f.begin() + std::ptrdiff_t(k));
                    again = true;
                    break;
                }
            }
        }
}

/** Dissolves the edges on the plane between two triangles: a triangle and its mirror image make one quad. */
void joinSeamTriangles(geo::Mesh& m, const geo::SymmetryPlane& plane, double h)
{
    std::vector<Index> edges;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
    {
        if (!m.edgeValid(e) || m.isBoundaryEdge(e))
            continue;
        if (std::abs(plane.distance(m.position(m.edgePoint(e, 0)))) > 1e-6 * h || std::abs(plane.distance(m.position(m.edgePoint(e, 1)))) > 1e-6 * h)
            continue;
        bool triangles = true;
        int faces = 0;
        m.forEachEdgeCorner(e, [&](Index c) {
            ++faces;
            triangles = triangles && m.faceSize(m.cornerFace(c)) == 3;
        });
        if (faces == 2 && triangles)
            edges.push_back(e);
    }
    if (!edges.empty())
        geo::dissolveEdges(m, edges);
}

/**
 * The quads traced straight from the cover's integer grid, as a polygon soup; empty when the grid leaves a hole off
 * the source's borders (a fold in the cover), which the library's extraction mends instead.
 */
/**
 * How much of the working surface the result leaves out, in quads' worth: a part whose layout folded through and
 * through (a thin arm at too coarse a size) drops out whole and leaves no hole to show for it.
 */
int uncovered(const geo::Mesh& m, const Work& w)
{
    if (m.numFaces() == 0)
        return 0;
    const geo::MeshBvh result(m);
    const Surface& iso = w.iso;
    double missing = 0.0;
    for (Index v = 0; v < iso.vertexCount(); ++v)
    {
        glm::dvec3 at = iso.pos[v];
        if ((w.symmetric && w.plane.distance(at) < 0.0) || w.onCap(at))
            continue;
        geo::BvhClosest c;
        if (!result.closestPoint(at, c, 2.0 * w.h0))
            missing += iso.area[v];
    }
    const int quads = int(missing / std::max(w.h0 * w.h0, 1e-12));
    return quads >= 2 ? quads : 0;
}

/** A face with less area than this (times the spacing squared) lying along a crease is a flap. */
constexpr double kFlapArea = 0.08;
/** How far (in spacings) on average an edge loop may lie from a guide and still be laid onto it. */
constexpr double kLoopReach = 0.6;

/** Longest hole, in edges, the grid's holes are filled across; a longer one means the cover folded over a region. */
constexpr size_t kMaxGridHole = 24;

/** One way of closing a hole: its faces, the points it adds inside (indexed on from the soup's own), and what it costs. */
struct HoleFill
{
    std::vector<std::vector<Index>> faces;
    std::vector<glm::dvec3> added;
    /**
     * The irregular points it leaves: its own inner pole and the loop points that end up with three or five faces, with
     * any other count but four weighing as four of those.
     */
    int poles = 0;
    double unsquare = 0.0;

    bool better(const HoleFill& o) const
    {
        // A fill with a corner folded back loses to any without.
        const bool folded = unsquare >= 10.0, otherFolded = o.unsquare >= 10.0;
        if (folded != otherFolded)
            return otherFolded;
        // A pole weighs as two corners 30 degrees off square: weighed any heavier, the fills that save one start to fold.
        return 0.5 * double(poles) + unsquare < 0.5 * double(o.poles) + o.unsquare;
    }
};

/** Builds a hole's fills over the soup's points, with the fill's own points indexed after them. */
struct HoleFiller
{
    const std::vector<glm::dvec3>& points;
    const std::vector<Index>& loop;
    /** Faces each loop point has before the fill. */
    const std::vector<int>& have;

    glm::dvec3 at(const HoleFill& fill, Index p) const { return p < Index(points.size()) ? points[p] : fill.added[p - points.size()]; }

    Index add(HoleFill& fill, const glm::dvec3& p) const
    {
        fill.added.push_back(p);
        return Index(points.size() + fill.added.size() - 1);
    }

    void rate(HoleFill& fill, int innerPoles) const
    {
        std::unordered_map<Index, int> got;
        for (const auto& f : fill.faces)
            for (Index p : f)
                ++got[p];
        fill.poles = innerPoles;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            // Two faces round a point is a doublet and six or more a fan, each worse than several plain poles.
            const int around = have[i] + got[loop[i]];
            fill.poles += around == 4 ? 0 : around == 3 || around == 5 ? 1 : 4;
        }
        fill.unsquare = 0.0;
        for (const auto& f : fill.faces)
            for (size_t k = 0; k < f.size(); ++k)
            {
                const glm::dvec3 p = at(fill, f[k]), u = at(fill, f[(k + f.size() - 1) % f.size()]) - p, v = at(fill, f[(k + 1) % f.size()]) - p;
                const double lu = glm::length(u), lv = glm::length(v);
                const double c = lu > 0.0 && lv > 0.0 ? glm::dot(u, v) / (lu * lv) : -1.0;
                fill.unsquare += c * c + (c < -0.9 ? 10.0 : 0.0);
            }
    }

    /**
     * A block of quads between four runs of points going round it, each starting where the one before ends; opposite
     * runs are as long as each other. The points inside are blended from the four runs.
     */
    void block(HoleFill& fill, const std::vector<Index>& s0, const std::vector<Index>& s1, const std::vector<Index>& s2,
               const std::vector<Index>& s3) const
    {
        const size_t m = s0.size() - 1, n = s1.size() - 1;
        std::vector<Index> grid((m + 1) * (n + 1));
        const glm::dvec3 c00 = at(fill, s0[0]), c10 = at(fill, s0[m]), c11 = at(fill, s1[n]), c01 = at(fill, s3[0]);
        for (size_t j = 0; j <= n; ++j)
            for (size_t i = 0; i <= m; ++i)
            {
                Index& g = grid[j * (m + 1) + i];
                if (j == 0)
                    g = s0[i];
                else if (i == m)
                    g = s1[j];
                else if (j == n)
                    g = s2[m - i];
                else if (i == 0)
                    g = s3[n - j];
                else
                {
                    const double u = double(i) / double(m), v = double(j) / double(n);
                    g = add(fill, (1.0 - v) * at(fill, s0[i]) + v * at(fill, s2[m - i]) + (1.0 - u) * at(fill, s3[n - j]) + u * at(fill, s1[j]) -
                                      ((1.0 - u) * (1.0 - v) * c00 + u * (1.0 - v) * c10 + u * v * c11 + (1.0 - u) * v * c01));
                }
            }
        for (size_t j = 0; j < n; ++j)
            for (size_t i = 0; i < m; ++i)
                fill.faces.push_back({grid[j * (m + 1) + i], grid[j * (m + 1) + i + 1], grid[(j + 1) * (m + 1) + i + 1], grid[(j + 1) * (m + 1) + i]});
    }

    /** The loop's points from position a on round to position b. */
    std::vector<Index> run(size_t a, size_t b) const
    {
        std::vector<Index> out{loop[a % loop.size()]};
        for (size_t k = a % loop.size(); k != b % loop.size();)
        {
            k = (k + 1) % loop.size();
            out.push_back(loop[k]);
        }
        return out;
    }

    /**
     * The fill the hole's own corners ask for, when it has only corners (three faces there already) and straight points
     * (two): a block of quads for four corners with opposite sides alike, and for three, five or six a block at each
     * corner round one pole in the middle. False when the sides' lengths don't allow it.
     */
    bool patch(HoleFill& fill) const
    {
        std::vector<size_t> corners;
        for (size_t i = 0; i < loop.size(); ++i)
        {
            if (have[i] == 3)
                corners.push_back(i);
            else if (have[i] != 2)
                return false;
        }
        const size_t c = corners.size(), n = loop.size();
        if (c < 3 || c > 6)
            return false;
        std::vector<int> side(c);
        for (size_t i = 0; i < c; ++i)
            side[i] = int((corners[(i + 1) % c] + n - corners[i]) % n);
        if (c == 4)
        {
            if (side[0] != side[2] || side[1] != side[3])
                return false;
            block(fill, run(corners[0], corners[1]), run(corners[1], corners[2]), run(corners[2], corners[3]), run(corners[3], corners[0]));
            rate(fill, 0);
            return true;
        }
        // Side i is spoke i - 1's length then spoke i + 1's. Stepping by two that closes on itself after an odd count
        // (all the spokes for three or five corners, every other one for six), which leaves one answer.
        std::vector<int> spoke(c, 0);
        for (size_t start = 0; start < (c % 2 ? 1u : 2u); ++start)
        {
            int k = 0;
            size_t j = start;
            do
            {
                k = side[(j + 1) % c] - k;
                j = (j + 2) % c;
            } while (j != start);
            if (k % 2 != 0)
                return false;
            spoke[start] = k / 2;
            for (j = start; (j + 2) % c != start; j = (j + 2) % c)
                spoke[(j + 2) % c] = side[(j + 1) % c] - spoke[j];
        }
        for (int s : spoke)
            if (s < 1)
                return false;
        glm::dvec3 middle(0.0);
        for (Index p : loop)
            middle += points[p] / double(n);
        const Index centre = add(fill, middle);
        // Each spoke from its side's split point in to the centre.
        std::vector<size_t> split(c);
        std::vector<std::vector<Index>> spokes(c);
        for (size_t i = 0; i < c; ++i)
        {
            split[i] = (corners[i] + size_t(spoke[(i + c - 1) % c])) % n;
            spokes[i].push_back(loop[split[i]]);
            for (int k = 1; k < spoke[i]; ++k)
                spokes[i].push_back(add(fill, glm::mix(points[loop[split[i]]], middle, double(k) / double(spoke[i]))));
            spokes[i].push_back(centre);
        }
        for (size_t i = 0; i < c; ++i)
        {
            const size_t before = (i + c - 1) % c;
            std::vector<Index> back(spokes[before].rbegin(), spokes[before].rend());
            block(fill, run(corners[i], split[i]), spokes[i], back, run(split[before], corners[i]));
        }
        rate(fill, 1);
        return true;
    }

    /** Ladders of quads across an even loop, one per offset; the best joins `fill`'s faces when it beats it. */
    void ladders(const std::vector<Index>& ring, const HoleFill& prefix, HoleFill& best, bool& any) const
    {
        const size_t n = ring.size(), m = n / 2;
        for (size_t s = 0; s < m; ++s)
        {
            HoleFill fill = prefix;
            if (n == 4)
                fill.faces.push_back(ring);
            else
                for (size_t i = 0; i + 1 < m; ++i)
                    fill.faces.push_back({ring[(s + i) % n], ring[(s + i + 1) % n], ring[(s + n - 2 - i) % n], ring[(s + n - 1 - i) % n]});
            rate(fill, 0);
            if (!any || fill.better(best))
                best = std::move(fill);
            any = true;
            if (n == 4)
                break;
        }
    }

    /**
     * The fill that leaves the fewest irregular points, the squarest among those: the hole's own patch, or a ladder of
     * quads at any offset, with a triangle off any one point first when the loop is odd, for the pairing to walk off.
     */
    HoleFill best() const
    {
        HoleFill out;
        bool any = false;
        if (HoleFill own; patch(own))
        {
            out = std::move(own);
            any = true;
        }
        if (loop.size() % 2 == 0)
            ladders(loop, HoleFill(), out, any);
        else
            for (size_t r = 0; r < loop.size(); ++r)
            {
                const size_t n = loop.size();
                HoleFill ear;
                ear.faces.push_back({loop[r], loop[(r + 1) % n], loop[(r + 2) % n]});
                std::vector<Index> ring;
                for (size_t k = 0; k < n; ++k)
                    if (k != (r + 1) % n)
                        ring.push_back(loop[k]);
                if (ring.size() < 4)
                {
                    rate(ear, 0);
                    if (!any || ear.better(out))
                        out = std::move(ear);
                    any = true;
                }
                else
                    ladders(ring, ear, out, any);
            }
        return out;
    }
};

/**
 * Closes the grid's holes off the source's borders, each with the fill that leaves the fewest irregular points (see
 * HoleFiller::best). False when a hole is too long or can't be traced as one loop.
 */
bool fillGridHoles(std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces, const Work& w)
{
    const auto why = [](int check) {
        if (std::getenv("LILA_HOLE_LOG"))
            std::fprintf(stderr, "hole fill: check %d failed\n", check);
        return false;
    };
    std::vector<int> faceCount(points.size(), 0);
    for (const auto& f : faces)
        for (Index p : f)
            ++faceCount[p];
    std::unordered_set<uint64_t> half;
    auto dkey = [](Index a, Index b) { return (uint64_t(a) << 32) | b; };
    for (const auto& f : faces)
        for (size_t k = 0; k < f.size(); ++k)
            half.insert(dkey(f[k], f[(k + 1) % f.size()]));
    // A hole's loop runs against the faces' winding: each border half edge a -> b gives b -> a.
    std::unordered_map<Index, Index> next;
    glm::dvec3 q;
    for (const auto& f : faces)
        for (size_t k = 0; k < f.size(); ++k)
        {
            const Index a = f[k], b = f[(k + 1) % f.size()];
            if (half.contains(dkey(b, a)) || w.border.closest(0.5 * (points[a] + points[b]), 0.25 * w.h0, q))
                continue;
            if (!next.emplace(b, a).second)
                return why(1);
        }
    // A gap cut right across a strip, or a notch in the border, leaves chains that start and end on the source's border
    // instead of loops. Each end goes on to the nearest chain start along the border, which closes them.
    std::unordered_set<Index> bridged;
    {
        std::unordered_set<Index> entered;
        for (const auto& [from, to] : next)
            entered.insert(to);
        std::vector<Index> starts, ends;
        for (const auto& [from, to] : next)
        {
            if (!entered.contains(from))
                starts.push_back(from);
            if (!next.contains(to))
                ends.push_back(to);
        }
        // Each chain's end by its start, and its length in points.
        std::unordered_map<Index, std::pair<Index, int>> chainOf;
        for (Index st : starts)
        {
            Index at = st;
            int length = 1;
            for (auto it = next.find(at); it != next.end() && length < 4096; it = next.find(at), ++length)
                at = it->second;
            chainOf[at] = {st, length};
        }
        // Along the border from the end to the start: their chord's middle is near it (cutting a corner of it at most),
        // where a chord across the strip has its middle half its length from either side. A short chain straight across
        // a strip is as near its own start as the next chain's, and is no loop by itself.
        std::vector<std::tuple<double, Index, Index>> links;
        for (Index e : ends)
            for (Index st : starts)
            {
                const double d = glm::distance(points[e], points[st]);
                if (d >= 4.0 * w.h0 || !w.border.closest(0.5 * (points[e] + points[st]), 0.25 * w.h0 + 0.3 * d, q))
                    continue;
                if (const auto own = chainOf.find(e); own != chainOf.end() && own->second.first == st && own->second.second < 4)
                    continue;
                links.push_back({d, e, st});
            }
        std::sort(links.begin(), links.end());
        std::unordered_set<Index> startTaken;
        for (const auto& [d, e, st] : links)
            if (!next.contains(e) && !startTaken.contains(st))
            {
                next[e] = st;
                startTaken.insert(st);
                bridged.insert(e);
                bridged.insert(st);
            }
    }
    std::unordered_set<Index> seen, unclosed;
    for (const auto& [start, unused] : next)
    {
        if (seen.contains(start))
            continue;
        std::vector<Index> loop;
        Index at = start;
        bool open = false;
        while (!open && !seen.contains(at))
        {
            seen.insert(at);
            loop.push_back(at);
            auto it = next.find(at);
            if (it == next.end())
                open = true;
            else
                at = it->second;
        }
        // A chain nothing closed (its other side is too far along the border) stays a gap in the border.
        if (open || unclosed.contains(at))
        {
            unclosed.insert(loop.begin(), loop.end());
            continue;
        }
        if (at != start || loop.size() < 3 || loop.size() > kMaxGridHole)
            return why(3);
        // A bridged chain with next to no area between it and the border is the border itself, a little off where it
        // was looked for: filled, it would be faces lying flat along it.
        {
            bool bridge = false;
            glm::dvec3 area(0.0);
            double round = 0.0;
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const glm::dvec3 a = points[loop[i]], b = points[loop[(i + 1) % loop.size()]];
                bridge = bridge || bridged.contains(loop[i]);
                area += 0.5 * glm::cross(a - points[loop[0]], b - points[loop[0]]);
                round += glm::distance(a, b);
            }
            if (bridge && glm::length(area) < 0.02 * w.h0 * round)
                continue;
        }
        std::vector<int> have(loop.size());
        // A point on the border is whole with two faces round it, not four.
        for (size_t i = 0; i < loop.size(); ++i)
            have[i] = faceCount[loop[i]] + (bridged.contains(loop[i]) ? 2 : 0);
        HoleFill fill;
        if (std::getenv("LILA_QR_LADDER_HOLES"))
        {
            if (loop.size() % 2 == 1)
            {
                fill.faces.push_back({loop[0], loop[1], loop[2]});
                loop.erase(loop.begin() + 1);
            }
            if (loop.size() == 4)
                fill.faces.push_back(loop);
            else if (loop.size() > 4)
                for (auto& quad : ladder(points, loop))
                    fill.faces.push_back(std::move(quad));
        }
        else
            fill = HoleFiller{points, loop, have}.best();
        if (std::getenv("LILA_HOLE_LOG"))
        {
            std::string shape;
            for (int n : have)
                shape += char('0' + std::min(n, 9));
            shape += " crease ";
            glm::dvec3 on, middle(0.0);
            for (Index p : loop)
            {
                shape += w.creases.closestCorner(points[p], 0.3 * w.h0, on) ? 'C' : w.creases.closest(points[p], 0.3 * w.h0, on) ? 'x' : '.';
                middle += points[p] / double(loop.size());
            }
            int corners = 0;
            double radius = 0.0;
            for (Index p : loop)
                radius = std::max(radius, glm::distance(points[p], middle));
            for (const glm::dvec3& c : w.creases.corners())
                corners += glm::distance(c, middle) < 1.5 * radius;
            shape += " corners near " + std::to_string(corners);
            std::fprintf(stderr, "hole %zu: %s, %zu faces, %zu points inside, %d irregular\n", have.size(), shape.c_str(), fill.faces.size(),
                         fill.added.size(), fill.poles);
        }
        points.insert(points.end(), fill.added.begin(), fill.added.end());
        faceCount.resize(points.size(), 0);
        for (auto& f : fill.faces)
        {
            for (Index p : f)
                ++faceCount[p];
            faces.push_back(std::move(f));
        }
    }
    // A gap left open has to be shallow enough for its points to be taken onto the border.
    for (Index p : unclosed)
        if (!w.border.closest(points[p], w.h0, q))
            return why(4);
    return true;
}

void gridSoup(const Work& w, const std::vector<std::vector<AutoRemesher::Vector2>>& uvs, std::vector<glm::dvec3>& points,
              std::vector<std::vector<Index>>& faces)
{
    LILA_ZONE();

    std::vector<Chart> charts(uvs.size());
    for (size_t t = 0; t < uvs.size(); ++t)
        for (int k = 0; k < 3; ++k)
            charts[t][k] = glm::dvec2(uvs[t][k].x(), uvs[t][k].y());
    Extracted ex = extractIntegerGrid(w.iso, charts);
    const geo::Mesh& g = ex.mesh;
    if (g.numFaces() == 0)
        return;
    glm::dvec3 q;
    int holes = 0;
    for (Index e = 0; e < g.edgeCapacity(); ++e)
        if (g.edgeValid(e) && g.isBoundaryEdge(e) &&
            !w.border.closest(0.5 * (g.position(g.edgePoint(e, 0)) + g.position(g.edgePoint(e, 1))), 0.25 * w.h0, q))
            ++holes;
    if (std::getenv("LILA_STAGE_LOG"))
        std::fprintf(stderr, "integer grid: %u faces, %d hole edges\n", unsigned(g.numFaces()), holes);
    std::vector<Index> id(g.pointCapacity(), kInvalid);
    for (Index p = 0; p < g.pointCapacity(); ++p)
        if (g.pointValid(p))
        {
            id[p] = Index(points.size());
            points.push_back(g.position(p));
        }
    for (Index f = 0; f < g.faceCapacity(); ++f)
        if (g.faceValid(f))
        {
            std::vector<Index> face;
            for (Index p : g.facePoints(f))
                face.push_back(id[p]);
            faces.push_back(std::move(face));
        }
    if (holes > 0 && !fillGridHoles(points, faces, w))
    {
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "integer grid: holes not closed, left to the library's extraction\n");
        if (const char* path = std::getenv("LILA_GRID_DUMP"))
            if (FILE* f = std::fopen(path, "w"))
            {
                for (const glm::dvec3& p : points)
                    std::fprintf(f, "v %.9g %.9g %.9g\n", p.x, p.y, p.z);
                for (const auto& face : faces)
                {
                    std::fprintf(f, "f");
                    for (Index v : face)
                        std::fprintf(f, " %d", int(v + 1));
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        points.clear();
        faces.clear();
    }
}

/**
 * Frees the points of a face laid flat along a crease: when a row of points beside a crease was taken onto it, the face
 * between that row and the crease's own edge lies on the line with no area. The points off its crease edge (the edge it
 * shares with a face across the crease, else its longest) go back to their other neighbours' mean on the surface.
 */
void freeCreaseFlaps(geo::Mesh& m, std::vector<FeatureKind>& feature, const geo::MeshBvh& surface, double h)
{
    const auto nb = pointNeighbours(m);
    std::unordered_set<Index> freed;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<Index> pts = m.facePoints(f);
        if (pts.size() < 4 || glm::length(m.faceAreaVector(f)) > kFlapArea * h * h)
            continue;
        if (std::any_of(pts.begin(), pts.end(), [&](Index p) { return feature[p] == FeatureKind::None; }))
            continue;
        // Its crease edge: the one shared with a face turned well away, else the longest.
        const glm::dvec3 n = m.faceNormal(f);
        Index keepA = kInvalid, keepB = kInvalid;
        double best = -1.0;
        for (Index c : m.faceCorners(f))
        {
            const Index e = m.cornerEdge(c);
            const Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
            double score = glm::distance(m.position(a), m.position(b));
            const Index r = m.cornerRadial(c);
            if (r != c)
                if (const Index g = m.cornerFace(r); g != f && std::abs(glm::dot(m.faceNormal(g), n)) < 0.7)
                    score += 1e6 * h;
            if (score > best)
            {
                best = score;
                keepA = a;
                keepB = b;
            }
        }
        for (Index p : pts)
            if (p != keepA && p != keepB && feature[p] == FeatureKind::Line && !m.isBoundaryPoint(p))
                freed.insert(p);
    }
    if (freed.empty())
        return;
    for (Index p : freed)
        feature[p] = FeatureKind::None;
    for (int round = 0; round < 4; ++round)
        for (Index p : freed)
        {
            glm::dvec3 avg(0.0);
            int count = 0;
            for (Index o : nb[p])
                if (!freed.contains(o) || round > 0)
                {
                    avg += m.position(o);
                    ++count;
                }
            if (count == 0)
                continue;
            geo::BvhClosest c;
            avg /= double(count);
            m.setPosition(p, surface.closestPoint(avg, c) ? c.point : avg);
        }
}

/**
 * The closed edge loop of a quad mesh lying closest along a closed guide, traced straight through four-edge points from
 * the points nearest the guide; empty when none closes within reach (mean distance under kLoopReach spacings).
 */
std::vector<Index> nearestEdgeLoop(const geo::Mesh& m, const std::vector<std::array<glm::dvec3, 2>>& segs, double h)
{
    std::vector<std::pair<double, Index>> near;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && !m.isBoundaryPoint(p))
            near.push_back({glm::distance(closestOnSegments(m.position(p), segs), m.position(p)), p});
    std::sort(near.begin(), near.end());
    near.resize(std::min<size_t>(near.size(), 12));
    double guideLength = 0.0;
    for (const auto& sg : segs)
        guideLength += glm::distance(sg[0], sg[1]);
    const size_t most = size_t(4.0 * guideLength / std::max(h, 1e-12)) + 8;
    auto facesOf = [&](Index e) {
        std::vector<Index> out;
        const Index c0 = m.edgeCorner(e);
        if (c0 == kInvalid)
            return out;
        Index c = c0;
        do
        {
            out.push_back(m.cornerFace(c));
            c = m.cornerRadial(c);
        } while (c != c0 && out.size() < 8);
        return out;
    };
    std::vector<Index> best;
    double bestScore = kLoopReach * h;
    for (const auto& [d0, start] : near)
    {
        if (m.pointValence(start) != 4)
            continue;
        std::vector<Index> edges;
        m.forEachPointEdge(start, [&](Index e) { edges.push_back(e); });
        for (Index first : edges)
        {
            std::vector<Index> loop{start};
            Index at = start, via = first;
            bool closed = false;
            for (size_t step = 0; step < most; ++step)
            {
                const Index next = m.edgeOtherPoint(via, at);
                if (next == start)
                {
                    closed = true;
                    break;
                }
                if (m.isBoundaryPoint(next) || m.pointValence(next) != 4)
                    break;
                loop.push_back(next);
                // Straight on: the edge at next sharing no face with the one we came in by.
                Index out = kInvalid;
                m.forEachPointEdge(next, [&](Index e) {
                    if (e == via)
                        return;
                    bool sharesFace = false;
                    for (Index f : facesOf(via))
                        for (Index g : facesOf(e))
                            sharesFace |= f == g;
                    if (!sharesFace)
                        out = e;
                });
                if (out == kInvalid)
                    break;
                at = next;
                via = out;
            }
            if (!closed || loop.size() < 3)
                continue;
            double mean = 0.0;
            for (Index p : loop)
                mean += glm::distance(closestOnSegments(m.position(p), segs), m.position(p));
            mean /= double(loop.size());
            if (mean < bestScore)
            {
                bestScore = mean;
                best = loop;
            }
        }
    }
    return best;
}

/**
 * Lays an edge loop onto each closed guide: the nearest closed loop's points move onto the guide (and back onto the
 * surface), and their neighbours ease halfway after them so the quads beside stay even.
 */
void layLoopsOnGuides(geo::Mesh& m, const std::vector<std::vector<glm::dvec3>>& guides, const geo::MeshBvh& bvh, double h)
{
    const auto nb = pointNeighbours(m);
    for (const auto& guide : guides)
    {
        if (guide.size() < 3 || glm::distance(guide.front(), guide.back()) > 1e-6 * h)
            continue;
        std::vector<std::array<glm::dvec3, 2>> segs;
        for (size_t i = 0; i + 1 < guide.size(); ++i)
            segs.push_back({guide[i], guide[i + 1]});
        const std::vector<Index> loop = nearestEdgeLoop(m, segs, h);
        if (loop.empty())
            continue;
        std::unordered_map<Index, glm::dvec3> moved;
        for (Index p : loop)
        {
            const glm::dvec3 q = closestOnSegments(m.position(p), segs);
            geo::BvhClosest c;
            moved[p] = bvh.closestPoint(q, c) ? c.point : q;
        }
        std::unordered_map<Index, glm::dvec3> eased;
        for (Index p : loop)
        {
            const glm::dvec3 shift = moved[p] - m.position(p);
            for (Index o : nb[p])
                if (!moved.contains(o))
                {
                    auto [it, fresh] = eased.try_emplace(o, glm::dvec3(0.0));
                    it->second += 0.5 * shift;
                }
        }
        for (const auto& [p, at] : moved)
            m.setPosition(p, at);
        for (const auto& [p, shift] : eased)
        {
            geo::BvhClosest c;
            const glm::dvec3 q = m.position(p) + shift * 0.5;
            m.setPosition(p, bvh.closestPoint(q, c) ? c.point : q);
        }
    }
}

/**
 * Smooths the points round faces that face against the source until they don't: each free point of a folded face and
 * its neighbours moves to its neighbours' mean on the surface. Creases, corners and seam points stay.
 */
void unfold(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::Mesh& src, const geo::MeshBvh& bvh, const geo::MeshBvh& surface,
            int seamAxis, double h)
{
    auto nb = pointNeighbours(m);
    for (int round = 0; round < 8; ++round)
    {
        const std::vector<Index> folded = foldedFaces(m, src, bvh);
        if (folded.empty())
            return;
        std::unordered_set<Index> moving;
        for (Index f : folded)
            for (Index p : m.facePoints(f))
            {
                moving.insert(p);
                for (Index o : nb[p])
                    moving.insert(o);
            }
        for (Index p : moving)
        {
            if (feature[p] != FeatureKind::None || m.isBoundaryPoint(p) || nb[p].empty() ||
                (seamAxis >= 0 && std::abs(m.position(p)[seamAxis]) < 0.35 * h))
                continue;
            glm::dvec3 avg(0.0);
            for (Index o : nb[p])
                avg += m.position(o);
            avg /= double(nb[p].size());
            geo::BvhClosest c;
            m.setPosition(p, surface.closestPoint(avg, c) ? c.point : avg);
        }
    }
}

/**
 * One connected part's quads as a polygon soup: the cover solved over the working triangles, traced from its integer
 * grid (or the library's extraction where the grid leaves a hole).
 */
bool coverSoup(Work& w, const QuadRemeshOptions& opts, std::vector<glm::dvec3>& points, std::vector<std::vector<Index>>& faces, std::string& error,
               Stopwatch& clock)
{
    {
        CoverInput in = coverInput(w, opts, error);
        if (!error.empty())
            return false;
        AutoRemesher::Parameterizer pz(&in.vertices, &in.triangles, in.guidance.empty() ? nullptr : &in.guidance);
        setUp(pz, in, w);
        if (!pz.parameterize())
        {
            error = "the quad layout couldn't be solved on this shape";
            return false;
        }
        clock.lap("cover");
        if (std::getenv("LILA_STAGE_LOG"))
        {
            int flipped = 0;
            for (const auto& uv : pz.originalTriangleUvs())
                flipped += (uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y()) - (uv[1].y() - uv[0].y()) * (uv[2].x() - uv[0].x()) < 0.0;
            std::fprintf(stderr, "cover: %zu triangles, %d flipped, %zu feature edges, %zu singular\n", in.triangles.size(), flipped,
                         in.features.size(), pz.singularVertexIndices().size());
        }
        std::unique_ptr<std::vector<std::vector<AutoRemesher::Vector2>>> uvs = pz.takeTriangleUvs();
        std::vector<std::vector<AutoRemesher::Vector2>> originalUvs = pz.originalTriangleUvs();
        std::vector<size_t> singular = pz.singularVertexIndices();
        // LILA_UV_DUMP: per working triangle its centre, its area and its area in the cover (signed).
        if (const char* path = std::getenv("LILA_UV_DUMP"))
            if (FILE* f = std::fopen(path, "w"))
            {
                for (size_t t = 0; t < originalUvs.size(); ++t)
                {
                    const auto& uv = originalUvs[t];
                    const auto& tri = w.iso.tris[t];
                    const glm::dvec3 c = (w.iso.pos[tri[0]] + w.iso.pos[tri[1]] + w.iso.pos[tri[2]]) / 3.0;
                    std::fprintf(f, "%g %g %g %g %g %g %g %g %g %g %g\n", c.x, c.y, c.z, w.iso.triArea[t],
                                 0.5 * ((uv[1].x() - uv[0].x()) * (uv[2].y() - uv[0].y()) - (uv[1].y() - uv[0].y()) * (uv[2].x() - uv[0].x())),
                                 uv[0].x(), uv[0].y(), uv[1].x(), uv[1].y(), uv[2].x(), uv[2].y());
                }
                std::fclose(f);
            }
        const FeatureLines& border = w.border;
        const double h0 = w.h0;
        if (!std::getenv("LILA_AR_EXTRACT") && !w.stock)
            gridSoup(w, originalUvs, points, faces);
        if (faces.empty())
        {
            // The library's extraction mends a layout the integer grid can't close, at the cost of a few more poles.
            AutoRemesher::QuadExtractor qe(&in.vertices, &in.triangles, uvs.get());
            qe.setOriginalTriangleUvs(&originalUvs);
            qe.setSingularVertices(&singular);
            qe.setProgressHandler([](float, const char*) { Budget::checkpoint(); });
            // A loop along the source's own border (or the symmetry cut) stays open.
            qe.setKeepHole([&](const std::vector<AutoRemesher::Vector3>& loop) {
                if (border.empty())
                    return false;
                glm::dvec3 q;
                for (const auto& p : loop)
                    if (!border.closest(fromAr(p), 0.75 * h0, q))
                        return false;
                return true;
            });
            if (!qe.extract() || qe.remeshedQuads().empty())
            {
                error = "extraction produced no faces";
                return false;
            }
            for (const auto& p : qe.remeshedVertices())
                points.push_back(fromAr(p));
            for (const auto& f : qe.remeshedQuads())
            {
                std::vector<Index> face;
                for (size_t v : f)
                    face.push_back(Index(v));
                faces.push_back(std::move(face));
            }
        }
        clock.lap("extract");

    }
    return true;
}

/**
 * Removes faces left lying flat along a border, hung on it by one edge: a quad that straddled the border of a cap has
 * its two outer points taken onto the border with the rest, and no area left.
 */
void dropBorderFlaps(geo::Mesh& m, double h)
{
    std::vector<Index> hung;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceArea(f) > 0.02 * h * h)
            continue;
        std::vector<Index> loose;
        bool onBorder = true;
        for (Index p : m.facePoints(f))
        {
            onBorder = onBorder && m.isBoundaryPoint(p);
            if (m.pointValence(p) == 2)
                loose.push_back(p);
        }
        if (onBorder && loose.size() >= 2)
            hung.insert(hung.end(), loose.begin(), loose.end());
    }
    for (Index p : hung)
        if (m.pointValid(p))
            m.removePoint(p);
    if (!hung.empty())
        m.removeAllLoose();
}

/**
 * Closes up the faces left with no area once their points are on the creases: points taken onto the same spot become
 * one, and a quad squashed onto a line (two opposite corners together between the other two) goes, those corners one.
 */
void closeFlatFaces(geo::Mesh& m, double h)
{
    const double together = 1e-6 * h;
    std::vector<Index> target(m.pointCapacity(), kInvalid), squashed;
    auto root = [&](Index p) {
        while (target[p] != kInvalid && target[p] != p)
            p = target[p];
        return p;
    };
    bool any = false;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f) || m.faceArea(f) > 1e-6 * h * h)
            continue;
        const std::vector<Index> pts = m.facePoints(f);
        bool closed = false;
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const Index a = root(pts[k]), b = root(pts[(k + 1) % pts.size()]);
            if (a != b && glm::distance(m.position(a), m.position(b)) < together)
            {
                target[b] = a;
                closed = any = true;
            }
        }
        if (closed || pts.size() != 4)
            continue;
        const double d02 = glm::distance(m.position(pts[0]), m.position(pts[2])), d13 = glm::distance(m.position(pts[1]), m.position(pts[3]));
        const Index a = root(d02 < d13 ? pts[0] : pts[1]), b = root(d02 < d13 ? pts[2] : pts[3]);
        if (std::min(d02, d13) > 0.25 * h || a == b || m.isBoundaryPoint(a) != m.isBoundaryPoint(b))
            continue;
        target[b] = a;
        squashed.push_back(f);
        any = true;
    }
    if (!any)
        return;
    for (Index f : squashed)
        m.removeFace(f);
    for (Index p = 0; p < Index(target.size()); ++p)
        if (target[p] != kInvalid)
            target[p] = root(p);
    geo::weldPoints(m, target);
    m.removeAllLoose();
}

/**
 * Takes the nearest border point onto each corner of the source's border the result's border stops short of: the grid
 * drops the part cells at a corner its lines don't quite meet in, and the corner comes out cut off. Returns how many.
 */
int reachBorderCorners(geo::Mesh& m, const FeatureLines& border, double h)
{
    // As far as six quads: a sharp tip is cut by one chord, and its nearest border point is that chord's end however far.
    // At ten a thin strip starts taking points from the next turn of itself.
    constexpr double kMissed = 0.3, kReach = 6.0;
    std::vector<Index> onBorder;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p) && m.isBoundaryPoint(p))
            onBorder.push_back(p);
    int moved = 0;
    for (const glm::dvec3& corner : border.corners())
    {
        size_t best = onBorder.size();
        double bestD = kReach * h;
        for (size_t i = 0; i < onBorder.size(); ++i)
            if (const double d = glm::distance(m.position(onBorder[i]), corner); d < bestD)
            {
                bestD = d;
                best = i;
            }
        if (best == onBorder.size() || bestD < kMissed * h)
            continue;
        m.setPosition(onBorder[best], corner);
        onBorder[best] = onBorder.back();
        onBorder.pop_back();
        ++moved;
    }
    return moved;
}

/** The connected parts of a surface, each with its share of the per-vertex sizes. */
std::vector<std::pair<Surface, std::vector<double>>> surfaceParts(const Surface& s, const std::vector<double>& size)
{
    const Index n = s.vertexCount();
    std::vector<Index> part(n, kInvalid);
    Index parts = 0;
    for (Index v = 0; v < n; ++v)
    {
        if (part[v] != kInvalid || s.adjStart[v] == s.adjStart[v + 1])
            continue;
        std::vector<Index> stack{v};
        part[v] = parts;
        while (!stack.empty())
        {
            const Index u = stack.back();
            stack.pop_back();
            for (Index k = s.adjStart[u]; k < s.adjStart[u + 1]; ++k)
                if (part[s.adj[k]] == kInvalid)
                {
                    part[s.adj[k]] = parts;
                    stack.push_back(s.adj[k]);
                }
        }
        ++parts;
    }
    std::vector<std::pair<Surface, std::vector<double>>> out(parts);
    if (parts == 1)
    {
        out[0] = {s, size};
        return out;
    }
    std::vector<Index> local(n, kInvalid);
    for (Index v = 0; v < n; ++v)
        if (part[v] != kInvalid)
        {
            auto& [ps, psize] = out[part[v]];
            local[v] = Index(ps.pos.size());
            ps.pos.push_back(s.pos[v]);
            ps.density.push_back(v < Index(s.density.size()) ? s.density[v] : 1.0f);
            psize.push_back(size[v]);
        }
    for (const auto& t : s.tris)
        out[part[t[0]]].first.tris.push_back({local[t[0]], local[t[1]], local[t[2]]});
    for (uint64_t key : s.hard)
    {
        const Index a = Index(key >> 32), b = Index(key & 0xffffffffu);
        out[part[a]].first.hard.insert(edgeKey(local[a], local[b]));
    }
    for (auto& [ps, psize] : out)
    {
        ps.pins = s.pins;
        ps.buildTopology();
        ps.buildGeometry();
        ps.detectFeatures(-1.0, 75.0, 0.0);
    }
    return out;
}

/** The quads laid out over the working triangles, cleaned up on the source: features snapped, relaxed, mirrored. */
QuadRemeshResult coverRemesh(Work& w, const QuadRemeshOptions& opts, Stopwatch& clock)
{
    LILA_ZONE();

    QuadRemeshResult out;
    out.layout = "flow";
    std::vector<glm::dvec3> points;
    std::vector<std::vector<Index>> faces;
    {
        // Each connected part gets its own cover: one solve over several leaves all but the first unanchored.
        Surface whole = std::move(w.iso);
        std::vector<double> wholeSize = std::move(w.isoSize);
        auto parts = surfaceParts(whole, wholeSize);
        const double wholeArea = whole.totalArea(), wholeQuads = w.stockQuads;
        for (auto& [part, size] : parts)
        {
            w.iso = std::move(part);
            w.isoSize = std::move(size);
            // Each part's share of the stock layout's quads goes by its area.
            if (parts.size() > 1 && wholeArea > 0.0)
                w.stockQuads = wholeQuads * w.iso.totalArea() / wholeArea;
            std::vector<glm::dvec3> partPoints;
            std::vector<std::vector<Index>> partFaces;
            if (!coverSoup(w, opts, partPoints, partFaces, out.error, clock))
                break;
            const Index base = Index(points.size());
            points.insert(points.end(), partPoints.begin(), partPoints.end());
            for (auto& f : partFaces)
            {
                for (Index& v : f)
                    v += base;
                faces.push_back(std::move(f));
            }
        }
        w.iso = std::move(whole);
        w.isoSize = std::move(wholeSize);
        w.stockQuads = wholeQuads;
        if (!out.ok())
            return out;
    }
    const double h0 = w.h0;
    if (const char* path = std::getenv("LILA_RAW_DUMP"))
        if (FILE* f = std::fopen(path, "w"))
        {
            for (const glm::dvec3& p : points)
                std::fprintf(f, "v %.9g %.9g %.9g\n", p.x, p.y, p.z);
            for (const auto& face : faces)
            {
                std::fprintf(f, "f");
                for (Index v : face)
                    std::fprintf(f, " %d", int(v + 1));
                std::fprintf(f, "\n");
            }
            std::fclose(f);
        }
    if (w.capsBvh)
    {
        auto capped = [&](const std::vector<Index>& f) {
            glm::dvec3 c(0.0);
            for (Index v : f)
                c += points[v];
            return w.onCap(c / double(f.size()));
        };
        faces.erase(std::remove_if(faces.begin(), faces.end(), capped), faces.end());
    }
    const bool refine = !std::getenv("LILA_QR_RAW");
    if (refine)
        cleanSoup(faces, points, w.stock ? 0.0 : w.h0);
    // Dropping faces left lying in the plane can bare another row of border points next to it, so twice.
    if (w.wholeLayout)
        keepPositiveHalf(points, faces, w.plane);
    else if (w.symmetric)
        for (int pass = 0; pass < 2; ++pass)
            weldSeam(points, faces, w.plane, opts.symmetryAxis, w.h0);
    if (opts.pureQuads && refine)
    {
        std::function<bool(Index, Index)> seam;
        std::unordered_set<uint64_t> border;
        // A triangle walked onto the cut meets its mirror image in a kite across the middle edge loop, which a whole
        // layout has no need to break.
        if (w.symmetric && !w.wholeLayout)
        {
            std::unordered_map<uint64_t, int> uses;
            for (const auto& f : faces)
                for (size_t k = 0; k < f.size(); ++k)
                    ++uses[edgeKey(f[k], f[(k + 1) % f.size()])];
            for (const auto& [key, n] : uses)
                if (n == 1)
                    border.insert(key);
            seam = [&](Index a, Index b) {
                return border.contains(edgeKey(a, b)) && std::abs(w.plane.distance(points[a])) < 1e-9 * w.h0 &&
                       std::abs(w.plane.distance(points[b])) < 1e-9 * w.h0;
            };
        }
        // The squarest pairings first; what is left pairs through quads less square, which the relax evens out after.
        for (double unsquare : {3.0, 6.0, 12.0})
            if (pairOddFaces(points, faces, seam, 120, unsquare) == 0)
                break;
        mergeDoublets(faces, points.size());
    }
    compactSoup(points, faces);
    std::vector<Index> sizes, indices;
    for (const auto& f : faces)
    {
        sizes.push_back(Index(f.size()));
        indices.insert(indices.end(), f.begin(), f.end());
    }
    geo::Mesh m = geo::Mesh::fromPolygons(points, sizes, indices);
    m.compact();
    const geo::Mesh& src = w.src;
    const geo::MeshBvh& bvh = *w.bvh;
    geo::conformNormals(m);
    // Conforming only makes the winding consistent; the source's own decides which way round it goes.
    if (2 * foldedFaces(m, src, bvh).size() > size_t(m.numFaces()))
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                m.flipFace(f);

    const int axis = w.symmetric ? opts.symmetryAxis : -1;
    if (w.symmetric)
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && m.isBoundaryPoint(p) && std::abs(w.plane.distance(m.position(p))) < 0.35 * h0)
            {
                glm::dvec3 at = m.position(p);
                at[axis] = 0.0;
                m.setPosition(p, at);
            }
    if (refine && !w.stock && !std::getenv("LILA_QR_NO_CORNER_REACH"))
    {
        const int reached = reachBorderCorners(m, w.border, h0);
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "border corners reached: %d\n", reached);
    }
    std::vector<FeatureKind> feature = pointFeatures(m, w.creases, w.border, 0.3 * h0);
    if (refine)
    {
        projectPoints(m, feature, *w.simpleBvh, w.creases, w.border, 0.3 * h0);
        freeCreaseFlaps(m, feature, *w.simpleBvh, h0);
        relaxAlongFeatures(m, feature, *w.simpleBvh, w.creases, w.border, h0, opts.relaxIterations, axis);
        unfold(m, feature, src, bvh, *w.simpleBvh, axis, h0);
        closeFlatFaces(m, h0);
        dropBorderFlaps(m, h0);
    }
    // Once the rims of the cuts are on their planes: before, a rim wanders along the tip's creases and its rungs fold flat.
    if (!w.tips.empty())
    {
        const int closed = closeTips(m, w.tips, h0, *w.simpleBvh);
        if (std::getenv("LILA_STAGE_LOG"))
            std::fprintf(stderr, "sharp tips built back: %d of %zu\n", closed, w.tips.size());
    }
    clock.lap("refine");

    if (w.symmetric)
    {
        // Seam points sit exactly on the plane so the mirror welds them and the halves match bit for bit.
        for (Index p = 0; p < m.pointCapacity(); ++p)
            if (m.pointValid(p) && m.isBoundaryPoint(p) && std::abs(w.plane.distance(m.position(p))) < 0.35 * h0)
            {
                glm::dvec3 at = m.position(p);
                at[axis] = 0.0;
                m.setPosition(p, at);
            }
        geo::MirrorOptions mo;
        mo.plane = w.plane;
        mo.weld = true;
        mo.tolerance = std::max(1e-9, h0 * 1e-6);
        geo::mirrorMesh(m, mo);
        m.compact();
        joinSeamTriangles(m, w.plane, h0);
        m.compact();
        if (opts.middleSmoothing > 0.0)
            smoothMiddle(m, axis, opts.middleSmoothing);
    }
    if (!opts.featureGuides.empty())
        layLoopsOnGuides(m, opts.featureGuides, bvh, h0);
    if (opts.sharpAngle > 0.0 && !w.creases.empty())
        geo::setHardEdgesByAngle(m, opts.sharpAngle);
    out.metrics = measureQuality(m, &bvh);
    out.defects = int(defects(m, src, bvh, w.closed)) + uncovered(m, w);
    out.mesh = std::move(m);
    return out;
}

/** The mirrored half's asymmetry: the share of points whose mirror image lies off the mesh by over 1% of its size. */
double asymmetry(const geo::Mesh& m, const geo::MeshBvh& bvh, int axis)
{
    const geo::Bounds box = m.bounds();
    const double tol = 0.01 * glm::length(box.max - box.min);
    std::vector<Index> points;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
            points.push_back(p);
    if (points.empty())
        return 0.0;
    const size_t step = std::max<size_t>(1, points.size() / 2000);
    int far = 0, n = 0;
    for (size_t i = 0; i < points.size(); i += step, ++n)
    {
        glm::dvec3 q = m.position(points[i]);
        q[axis] = -q[axis];
        geo::BvhClosest c;
        far += !bvh.closestPoint(q, c) || glm::length(c.point - q) > tol;
    }
    return double(far) / double(n);
}

/** keepCaps false lays a body with flat faces out as any creased shape, without the stock stages. */
QuadRemeshResult quadRemeshOnce(const geo::Mesh& source, const QuadRemeshOptions& opts, bool keepCaps = true)
{
    LILA_ZONE();

    QuadRemeshResult result;
    Stopwatch clock(result.timings);
    Work w;
    prepareWork(w, source, opts, clock, keepCaps);
    if (!w.error.empty())
    {
        result.error = w.error;
        return result;
    }
    if (w.symmetric)
        if (const double off = asymmetry(w.src, *w.bvh, opts.symmetryAxis); off > 0.05)
            result.notes.push_back("the mesh isn't symmetric across " + std::string(1, char('X' + opts.symmetryAxis)) + " (" +
                                   std::to_string(int(std::lround(100.0 * off))) + "% of it has no mirror image), so this mirrors its +" +
                                   std::string(1, char('X' + opts.symmetryAxis)) + " half; set Symmetry to Off for it");
    result.notes.insert(result.notes.end(), w.notes.begin(), w.notes.end());
    QuadRemeshResult r = coverRemesh(w, opts, clock);
    if (!r.ok() && !opts.featureGuides.empty())
    {
        // Edge loop curves that can't be laid in (a curve the layout can't follow) shouldn't cost the whole remesh.
        QuadRemeshOptions plain = opts;
        plain.featureGuides.clear();
        QuadRemeshResult again = quadRemeshOnce(source, plain);
        if (again.ok())
            again.notes.insert(again.notes.begin(), "the edge loop curves couldn't be laid into the layout (" + r.error + "), so this follows the flow without them");
        return again;
    }
    if (!r.ok())
    {
        result.error = r.error;
        return result;
    }
    // The stock extraction now and then folds along a kept cap's rim or in a tight spot the grid traces cleanly.
    constexpr int kFewFlaws = 3;
    if (w.keptCapsBvh && r.defects > kFewFlaws)
    {
        QuadRemeshResult own = quadRemeshOnce(source, opts, false);
        if (own.ok() && own.defects < r.defects)
        {
            own.attempts.insert(own.attempts.begin(), {r.layout, r.mesh.numFaces(), r.defects, r.metrics.irregular});
            own.notes.push_back("laid out as a creased shape: with its flat faces as caps the layout left " + std::to_string(r.defects) +
                                " folds, this way " + std::to_string(own.defects));
            return own;
        }
    }
    result.attempts.push_back({r.layout, r.mesh.numFaces(), r.defects, r.metrics.irregular});
    result.mesh = std::move(r.mesh);
    result.metrics = r.metrics;
    result.layout = r.layout;
    result.defects = r.defects;
    clock.lap("finish");
    return result;
}

/**
 * Folds, slivers, pinched edges, holes in a closed source, and fans of many faces round one point: what makes a lattice
 * too coarse for the shape. None in a clean result.
 */
Index defects(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh, bool sourceClosed)
{
    const std::vector<Index> folded = foldedFaces(m, src, bvh);
    Index count = Index(folded.size() + m.validate(geo::kValidateManifold).size());
    if (std::getenv("LILA_DEFECT_LIST"))
    {
        for (Index f : folded)
        {
            const glm::dvec3 c = m.faceCentroid(f);
            std::fprintf(stderr, "defect fold %.3f %.3f %.3f\n", c.x, c.y, c.z);
        }
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            if (sourceClosed && m.edgeValid(e) && m.isBoundaryEdge(e))
            {
                const glm::dvec3 c = m.position(m.edgePoint(e, 0));
                std::fprintf(stderr, "defect open %.3f %.3f %.3f\n", c.x, c.y, c.z);
            }
    }
    const double size = glm::length(src.bounds().size());
    for (Index f = 0; f < m.faceCapacity(); ++f)
        count += m.faceValid(f) && m.faceArea(f) <= 1e-9 * size * size;
    if (sourceClosed)
        for (Index e = 0; e < m.edgeCapacity(); ++e)
            count += m.edgeValid(e) && m.isBoundaryEdge(e);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        count += m.pointValid(p) && m.pointValence(p) > 8;
    // A hole filled in one piece: a face of many sides, or one far bigger than the faces round it.
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        if (m.faceSize(f) > 5)
        {
            ++count;
            continue;
        }
        double around = 0.0;
        int n = 0;
        m.forEachFaceCorner(f, [&](Index c) {
            m.forEachEdgeCorner(m.cornerEdge(c), [&](Index d) {
                if (const Index g = m.cornerFace(d); g != f)
                {
                    around += m.faceArea(g);
                    ++n;
                }
            });
        });
        const bool big = n > 0 && m.faceArea(f) > 6.0 * around / double(n);
        if (big && std::getenv("LILA_DEFECT_LIST"))
        {
            const glm::dvec3 c = m.faceCentroid(f);
            std::fprintf(stderr, "defect big face %.3f %.3f %.3f\n", c.x, c.y, c.z);
        }
        count += big;
    }
    return count;
}

/** Faces of more than five sides, which only hole filling leaves. */
Index largeFaces(const geo::Mesh& m)
{
    Index count = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        count += m.faceValid(f) && m.faceSize(f) > 5;
    return count;
}
/** Of two results, the one that worked with fewer defects. */
bool better(const QuadRemeshResult& a, int defectsA, const QuadRemeshResult& b, int defectsB)
{
    return a.ok() && (!b.ok() || defectsA < defectsB);
}
} // namespace

static PoleResult findPolesWithin(const geo::Mesh& source, const QuadRemeshOptions& opts);
static QuadRemeshResult quadRemeshWithin(const geo::Mesh& source, const QuadRemeshOptions& opts);

PoleResult findPoles(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    progress::Scope scope("Finding poles", 1.0, progress::Cancellable);
    Budget budget(opts.timeout, &scope);
    try
    {
        return findPolesWithin(source, opts);
    }
    catch (const Interrupted& e)
    {
        PoleResult out;
        out.error = e.what();
        return out;
    }
}

static PoleResult findPolesWithin(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    PoleResult out;
    std::vector<std::pair<std::string, double>> timings;
    Stopwatch clock(timings);
    Work w;
    prepareWork(w, source, opts, clock);
    if (!w.error.empty())
    {
        out.error = w.error;
        return out;
    }
    CoverInput in = coverInput(w, opts, out.error);
    if (!out.ok())
        return out;
    AutoRemesher::Parameterizer pz(&in.vertices, &in.triangles, in.guidance.empty() ? nullptr : &in.guidance);
    setUp(pz, in, w);
    if (!pz.solveField())
    {
        out.error = "the edge flow couldn't be solved on this shape";
        return out;
    }
    const std::vector<size_t>& at = pz.singularVertexIndices();
    const std::vector<int>& charge = pz.singularVertexCharges();
    for (size_t i = 0; i < at.size(); ++i)
    {
        glm::dvec3 c = w.iso.pos[at[i]];
        // A layout made whole turns on both sides; the result mirrors its positive half.
        if (w.wholeLayout && w.plane.distance(c) < -1e-9)
            continue;
        out.poles.push_back({c, 4 - charge[i]});
        if (w.symmetric && std::abs(w.plane.distance(c)) > 1e-9)
        {
            c[opts.symmetryAxis] = -c[opts.symmetryAxis];
            out.poles.push_back({c, 4 - charge[i]});
        }
    }
    for (int total : shellIndexTotals(w.iso).indexTotal)
        out.shellTotals.push_back(total == INT_MIN ? std::optional<int>() : std::optional<int>(total));
    return out;
}

QuadRemeshResult quadRemesh(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    progress::Scope scope("Quad remesh", 1.0, progress::Cancellable);
    Budget budget(opts.timeout, &scope);
    try
    {
        return quadRemeshWithin(source, opts);
    }
    catch (const Interrupted& e)
    {
        QuadRemeshResult out;
        out.error = e.what();
        return out;
    }
}

namespace
{
double secondsSince(std::chrono::steady_clock::time_point t)
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

/** Keeps `again` in place of `kept` when take is set, and the attempts of both either way. */
void keepOrMerge(QuadRemeshResult& kept, QuadRemeshResult&& again, bool take)
{
    std::vector<RemeshAttempt> attempts = std::move(kept.attempts);
    attempts.insert(attempts.end(), again.attempts.begin(), again.attempts.end());
    if (take)
        kept = std::move(again);
    kept.attempts = std::move(attempts);
}

/**
 * The layout at the size asked for; one with many flaws (folds or holes in over 1% of its faces, usually a thin part too
 * narrow for the quads) tries again up to twice as fine, and the cleanest is kept.
 */
QuadRemeshResult cleanest(const geo::Mesh& source, const QuadRemeshOptions& opts, int& asked)
{
    auto started = std::chrono::steady_clock::now();
    QuadRemeshResult result = quadRemeshOnce(source, opts);
    double lastTook = secondsSince(started);
    asked = opts.targetFaces;
    auto many = [](const QuadRemeshResult& r) { return r.defects > std::max(8, int(r.mesh.numFaces() / 100)); };
    if (source.numFaces() == 0 || !opts.pinnedPoints.empty() || (result.ok() && !many(result)))
        return result;
    QuadRemeshOptions finer = opts;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (result.ok() && (Budget::remaining() < 2.0 * lastTook || std::getenv("LILA_NO_FINER")))
            break;
        if (opts.targetEdgeLength > 0.0)
            finer.targetEdgeLength *= 0.7;
        else
            finer.targetFaces = int(std::lround(finer.targetFaces / (0.7 * 0.7)));
        started = std::chrono::steady_clock::now();
        QuadRemeshResult again = quadRemeshOnce(source, finer);
        lastTook = secondsSince(started);
        const bool take = better(again, again.defects, result, result.defects);
        if (take)
            asked = finer.targetFaces;
        keepOrMerge(result, std::move(again), take);
        if (take)
            result.notes.insert(result.notes.begin(), "at " + std::to_string(opts.targetFaces) +
                                                          " faces the layout folded at thin parts or tight spots, so it was made finer (" +
                                                          std::to_string(finer.targetFaces) + " asked)");
        if (result.ok() && !many(result))
            break;
    }
    return result;
}

/**
 * Steers the face count onto the target: the count follows the faces asked for closely but not exactly (creases, gaps
 * and borders add their own), so ask again in proportion. A retry is kept only when it is closer and no less clean.
 */
void fitCount(const geo::Mesh& source, const QuadRemeshOptions& opts, QuadRemeshResult& result, int asked)
{
    if (opts.countTolerance <= 0.0 || opts.targetEdgeLength > 0.0 || !opts.pinnedPoints.empty() || !result.ok())
        return;
    const double target = opts.targetFaces;
    const auto off = [&](const QuadRemeshResult& r) { return std::abs(double(r.mesh.numFaces()) / target - 1.0); };
    QuadRemeshOptions again = opts;
    double lastTook = 0.0;
    for (int attempt = 0; attempt < 3 && off(result) > opts.countTolerance; ++attempt)
    {
        if (lastTook > 0.0 && Budget::remaining() < 2.0 * lastTook)
            break;
        // Within reason: thin parts and gaps can need more than the target, and asking for far less to pay for them
        // starves the rest.
        again.targetFaces = std::clamp(int(std::lround(asked * target / double(result.mesh.numFaces()))), std::max(8, int(target / 2)),
                                       int(target * 4));
        if (again.targetFaces == asked)
            break;
        const auto started = std::chrono::steady_clock::now();
        QuadRemeshResult r = quadRemeshOnce(source, again);
        lastTook = secondsSince(started);
        const auto nonQuads = [](const QuadRemeshResult& x) { return x.metrics.triangles + x.metrics.ngons; };
        const bool take = r.ok() && r.defects <= std::max(result.defects + 2, result.defects * 5 / 4) && nonQuads(r) <= nonQuads(result) + 2 &&
                          off(r) < off(result);
        if (take)
            asked = again.targetFaces;
        keepOrMerge(result, std::move(r), take);
        if (!take)
            break;
    }
    if (off(result) > opts.countTolerance)
        result.notes.push_back(std::to_string(result.mesh.numFaces()) + " faces, outside the count tolerance: no closer count came out as clean");
}

/** For each requested pole, the nearest irregular inner point within a few faces of it. */
void matchPoles(const QuadRemeshOptions& opts, QuadRemeshResult& result)
{
    if (!opts.poles || !result.ok())
        return;
    const geo::Mesh& m = result.mesh;
    double edge = 0.0;
    Index edges = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            edge += m.edgeLength(e);
            ++edges;
        }
    const double reach = 2.5 * (edges ? edge / double(edges) : 0.0);
    std::vector<uint8_t> taken(m.pointCapacity(), 0);
    for (const Pole& pole : *opts.poles)
    {
        PoleMatch match;
        double best = reach;
        // A point of the asked valence wins over a nearer one of another.
        for (bool wantValence : {true, false})
        {
            for (Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p) || taken[p] || m.isBoundaryPoint(p) || m.pointValence(p) == 4)
                    continue;
                if (wantValence && int(m.pointValence(p)) != pole.valence)
                    continue;
                if (const double d = glm::distance(m.position(p), pole.position); d <= best)
                {
                    best = d;
                    match.point = p;
                }
            }
            if (match.point != geo::kInvalid)
                break;
        }
        if (match.point != geo::kInvalid)
        {
            taken[match.point] = 1;
            match.valence = int(m.pointValence(match.point));
            match.distance = best;
            match.valenceMatched = match.valence == pole.valence;
        }
        result.poleMatches.push_back(match);
    }
}
} // namespace

static QuadRemeshResult quadRemeshWithin(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    QuadRemeshResult result;
    QuadRemeshOptions laid = opts;
    std::string gridNote;
    if (opts.method == RemeshMethod::Grid)
    {
        result = gridRemesh(source, opts);
        if (result.ok())
            return result;
        gridNote = "Grid takes an open sheet seen from one side (" + result.error + "), so this follows the flow instead";
        result = {};
        laid.method = RemeshMethod::Flow;
    }
    int asked = laid.targetFaces;
    keepOrMerge(result, cleanest(source, laid, asked), true);
    fitCount(source, laid, result, asked);
    if (!gridNote.empty() && result.ok())
        result.notes.insert(result.notes.begin(), gridNote);
    matchPoles(laid, result);
    return result;
}
} // namespace lila::retopo
