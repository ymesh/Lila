#include "retopo/Retopo.h"

#include "core/Profile.h"
#include "core/Progress.h"
#include "geo/Normals.h"
#include "geo/Ops.h"
#include "geo/Topology.h"
#include "retopo/Internal.h"
#include "retopo/Patch.h"

#include <tbb/parallel_for.h>

#include <algorithm>
#include <map>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <optional>
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
        static const std::pair<const char*, double> kAt[] = {{"bvh", 0.03}, {"surface", 0.12}, {"density", 0.18}, {"cross field", 0.3},
                                                             {"seamless", 0.6}, {"patches", 0.85}, {"local", 0.9}, {"finish", 1.0}};
        double at = mAt;
        for (auto [stage, f] : kAt)
            if (std::string_view(stage) == name)
                at = f;
        mAt = at;
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

/** Snaps each point to its feature (corner, crease line) or the surface; demotes points whose feature isn't near. */
void projectPoints(geo::Mesh& m, std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& lines, double reach)
{
    LILA_ZONE();

    // One inner point per corner, the nearest: two snapped onto it would pinch the mesh there.
    std::map<std::array<double, 3>, std::pair<double, Index>> claimed;
    std::vector<std::pair<Index, std::array<double, 3>>> claims;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        glm::dvec3 q;
        if (!m.pointValid(p) || feature[p] != FeatureKind::Corner || m.isBoundaryPoint(p) || !lines.closestCorner(m.position(p), reach, q))
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
            if (lines.closest(at, reach, q))
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
void relaxAlongFeatures(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, const FeatureLines& lines, double h,
                    int iterations, int seamAxis = -1, const SizeField* sizes = nullptr)
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

/** A face squashed onto a line, like a quad whose points all landed on one crease. */
bool isSliver(const geo::Mesh& m, Index f)
{
    double longest = 0.0;
    m.forEachFaceCorner(f, [&](Index c) { longest = std::max(longest, m.edgeLength(m.cornerEdge(c))); });
    return m.faceArea(f) < 1e-3 * longest * longest;
}

/**
 * The points of a sliver other than the ends of its longest edge: they were snapped onto the line the face lies along
 * and have to come off it for the face to open up.
 */
void releaseSliver(const geo::Mesh& m, Index f, std::vector<FeatureKind>& feature)
{
    Index longest = geo::kInvalid;
    m.forEachFaceCorner(f, [&](Index c) {
        const Index e = m.cornerEdge(c);
        if (longest == geo::kInvalid || m.edgeLength(e) > m.edgeLength(longest))
            longest = e;
    });
    const Index a = m.edgePoint(longest, 0), b = m.edgePoint(longest, 1);
    for (Index p : m.facePoints(f))
        if (p != a && p != b && feature[p] == FeatureKind::Line)
            feature[p] = FeatureKind::None;
}

/**
 * Smooths out folds the snapping and relaxing left: points of faces that face against the source, or are squashed flat,
 * move to the middle of their neighbours, back on the surface (or their crease), until nothing is folded. Corners stay.
 */
void untangle(geo::Mesh& m, std::vector<FeatureKind>& feature, const geo::Mesh& src, const geo::MeshBvh& bvh,
              const FeatureLines& lines, double h)
{
    LILA_ZONE();

    for (int it = 0; it < 20; ++it)
    {
        Budget::checkpoint();
        std::vector<Index> folded = foldedFaces(m, src, bvh);
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f) && isSliver(m, f))
            {
                releaseSliver(m, f, feature);
                folded.push_back(f);
            }
        if (folded.empty())
            return;
        // A corner with no crease through it (a cone tip, a saddle) only marks the shape; after enough tries a fold it
        // pins may move it rather than stay.
        auto pinned = [&](Index p) {
            if (feature[p] != FeatureKind::Corner)
                return false;
            if (it < 10)
                return true;
            bool onCrease = false;
            m.forEachPointEdge(p, [&](Index e) { onCrease = onCrease || feature[m.edgeOtherPoint(e, p)] != FeatureKind::None; });
            return onCrease;
        };
        std::vector<Index> pts;
        for (Index f : folded)
            for (Index p : m.facePoints(f))
                if (!pinned(p))
                    pts.push_back(p);
        std::sort(pts.begin(), pts.end());
        pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        // Widen to the ring once it has had a few tries, so a fold pinned between corners can still open up.
        if (it >= 5)
        {
            const std::vector<Index> ring = pts;
            for (Index p : ring)
                m.forEachPointEdge(p, [&](Index e) {
                    const Index q = m.edgeOtherPoint(e, p);
                    if (feature[q] != FeatureKind::Corner)
                        pts.push_back(q);
                });
            std::sort(pts.begin(), pts.end());
            pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
        }
        std::vector<glm::dvec3> next(pts.size());
        for (size_t k = 0; k < pts.size(); ++k)
        {
            const Index p = pts[k];
            glm::dvec3 avg(0.0);
            int count = 0;
            m.forEachPointEdge(p, [&](Index e) {
                const Index q = m.edgeOtherPoint(e, p);
                // Along a crease only its own points pull.
                if (feature[p] == FeatureKind::None || feature[p] == FeatureKind::Corner || feature[q] != FeatureKind::None)
                {
                    avg += m.position(q);
                    ++count;
                }
            });
            next[k] = m.position(p);
            if (count == 0)
                continue;
            const glm::dvec3 target = avg / double(count);
            glm::dvec3 q;
            geo::BvhClosest c;
            if (feature[p] == FeatureKind::Line && lines.closest(target, h, q))
                next[k] = q;
            else if (feature[p] != FeatureKind::Line && bvh.closestPoint(target, c))
                next[k] = c.point;
        }
        for (size_t k = 0; k < pts.size(); ++k)
            m.setPosition(pts[k], next[k]);
    }
}

/** Interior points with two edges just bend an edge: take them out of both faces. */
void removeValence2(geo::Mesh& m, std::vector<FeatureKind>& feature)
{
    LILA_ZONE();

    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || m.isBoundaryPoint(p) || m.pointValence(p) != 2)
            continue;
        std::vector<Index> faces = m.pointFaces(p);
        std::vector<std::vector<Index>> rebuilt;
        bool ok = true;
        for (Index f : faces)
        {
            std::vector<Index> pts;
            for (Index q : m.facePoints(f))
                if (q != p)
                    pts.push_back(q);
            ok &= pts.size() >= 3;
            rebuilt.push_back(std::move(pts));
        }
        if (!ok)
            continue;
        for (Index f : faces)
            m.removeFace(f);
        m.removePoint(p);
        for (const auto& pts : rebuilt)
            m.addFace(pts);
        feature[p] = FeatureKind::None;
    }
}

/**
 * Splits each inner point of more than eight edges (close poles merged into one) into two, with a quad between them
 * across the middle of its fan: valence 10 becomes two of 6, and the two spokes the quad spans gain one edge each.
 */
void splitHighValence(geo::Mesh& m, std::vector<FeatureKind>& feature)
{
    LILA_ZONE();

    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || m.isBoundaryPoint(p) || m.pointValence(p) <= 8 || (p < feature.size() && feature[p] != FeatureKind::None))
            continue;
        // The fan in order: each face's corner after p is the spoke the next face starts from.
        const std::vector<Index> faces = m.pointFaces(p);
        std::vector<std::vector<Index>> pts;
        bool quads = true;
        for (Index f : faces)
        {
            pts.push_back(m.facePoints(f));
            quads = quads && pts.back().size() == 4;
        }
        if (!quads)
            continue;
        auto around = [&](size_t i, int step) {
            const std::vector<Index>& fp = pts[i];
            const size_t at = size_t(std::find(fp.begin(), fp.end(), p) - fp.begin());
            return fp[(at + 4 + step) % 4];
        };
        std::vector<size_t> order = {0};
        for (size_t guard = 0; order.size() < faces.size() && guard < faces.size(); ++guard)
        {
            const Index spoke = around(order.back(), 1);
            for (size_t j = 0; j < faces.size(); ++j)
                if (around(j, -1) == spoke && std::find(order.begin(), order.end(), j) == order.end())
                {
                    order.push_back(j);
                    break;
                }
        }
        if (order.size() != faces.size())
            continue;
        const size_t k = order.size(), half = k / 2;
        const Index n0 = around(order[0], -1), nm = around(order[half], -1);
        glm::dvec3 sideA(0.0), sideB(0.0);
        for (size_t i = 0; i < k; ++i)
            (i < half ? sideA : sideB) += m.position(around(order[i], 1));
        const glm::dvec3 at = m.position(p);
        const Index q = m.addPoint(at);
        if (feature.size() < m.pointCapacity())
            feature.resize(m.pointCapacity(), FeatureKind::None);
        std::vector<std::vector<Index>> rebuilt;
        for (size_t i = 0; i < k; ++i)
        {
            std::vector<Index> fp = pts[order[i]];
            if (i >= half)
                std::replace(fp.begin(), fp.end(), p, q);
            rebuilt.push_back(std::move(fp));
        }
        for (Index f : faces)
            m.removeFace(f);
        for (const auto& fp : rebuilt)
            m.addFace(fp);
        m.addFace({p, n0, q, nm});
        // Removing a face leaves its edges: p's spokes to the faces that moved to q are loose now.
        std::vector<Index> loose;
        m.forEachPointEdge(p, [&](Index e) {
            if (m.edgeFaceCount(e) == 0)
                loose.push_back(e);
        });
        m.removeLoose(loose, {});
        m.setPosition(p, glm::mix(at, sideA / double(half), 0.25));
        m.setPosition(q, glm::mix(at, sideB / double(k - half), 0.25));
    }
}

void compactTagged(geo::Mesh& m, std::vector<FeatureKind>& feature, std::vector<uint8_t>& boundary);

/**
 * On the half of a symmetric remesh, folds each quad with three corners in a row on the seam (the middle one with no
 * other edge) into its fourth corner's row: the outer two seam corners merge at their midpoint. Left in, the middle
 * corner mirrors into a valence 2 point and the fourth into a valence 6 pair, a diamond on the seam. The rows round each
 * merge relax again, so they don't pinch in to it.
 */
void collapseSeamWedges(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, int axis, double tolerance)
{
    LILA_ZONE();

    std::vector<Index> merged;
    auto onSeam = [&](Index p) { return m.isBoundaryPoint(p) && std::abs(m.position(p)[axis]) <= tolerance; };
    for (bool changed = true; changed;)
    {
        changed = false;
        for (Index b = 0; b < m.pointCapacity(); ++b)
        {
            if (!m.pointValid(b) || m.pointValence(b) != 2 || !onSeam(b))
                continue;
            const std::vector<Index> faces = m.pointFaces(b);
            if (faces.size() != 1 || m.faceSize(faces[0]) != 4)
                continue;
            const Index wedge = faces[0];
            std::vector<Index> pts = m.facePoints(wedge);
            const size_t at = size_t(std::find(pts.begin(), pts.end(), b) - pts.begin());
            const Index a = pts[(at + 3) % 4], c = pts[(at + 1) % 4];
            if (!onSeam(a) || !onSeam(c))
                continue;
            // c's faces are rebuilt on a; one already holding a would come out with a repeated corner.
            std::vector<std::vector<Index>> rebuilt;
            std::vector<Index> cFaces = m.pointFaces(c);
            bool ok = true;
            for (Index f : cFaces)
            {
                if (f == wedge)
                    continue;
                std::vector<Index> fp = m.facePoints(f);
                ok &= std::find(fp.begin(), fp.end(), a) == fp.end();
                std::replace(fp.begin(), fp.end(), c, a);
                rebuilt.push_back(std::move(fp));
            }
            if (!ok)
                continue;
            const glm::dvec3 mid = 0.5 * (m.position(a) + m.position(c));
            m.removeFace(wedge);
            for (Index f : cFaces)
                if (f != wedge)
                    m.removeFace(f);
            m.removePoint(b);
            m.removePoint(c);
            for (const auto& fp : rebuilt)
                m.addFace(fp);
            m.setPosition(a, mid);
            merged.push_back(a);
            changed = true;
        }
    }
    if (merged.empty())
        return;
    std::vector<Index> region;
    std::vector<uint8_t> in(m.pointCapacity(), 0);
    for (Index p : merged)
        if (m.pointValid(p) && !in[p])
        {
            in[p] = 1;
            region.push_back(p);
        }
    for (int ring = 0; ring < 8; ++ring)
        for (size_t i = 0, n = region.size(); i < n; ++i)
            m.forEachPointEdge(region[i], [&](Index e) {
                const Index q = m.edgeOtherPoint(e, region[i]);
                if (!in[q])
                {
                    in[q] = 1;
                    region.push_back(q);
                }
            });
    const std::vector<glm::vec3> normals = geo::pointNormals(m);
    for (int it = 0; it < 20; ++it)
        for (Index p : region)
        {
            if (p < feature.size() && feature[p] != FeatureKind::None)
                continue;
            const bool seam = onSeam(p);
            glm::dvec3 avg(0.0);
            double weight = 0.0;
            m.forEachPointEdge(p, [&](Index e) {
                const Index q = m.edgeOtherPoint(e, p);
                const double w = seam && !onSeam(q) ? 2.0 : 1.0;
                avg += w * m.position(q);
                weight += w;
            });
            if (weight == 0.0)
                continue;
            glm::dvec3 target = glm::mix(m.position(p), avg / weight, 0.5);
            if (!seam)
            {
                const glm::dvec3 n(normals[p]);
                target -= n * glm::dot(n, target - m.position(p));
            }
            geo::BvhClosest c;
            if (bvh.closestPoint(target, c))
                target = c.point;
            if (seam)
                target[axis] = 0.0;
            m.setPosition(p, target);
        }
}

/**
 * On the half of a symmetric remesh, each seam point with one edge off the seam moves to the point of its two seam edges
 * nearest that edge's other end, back on the surface, so the row meets the seam square and crosses it straight instead
 * of in a V. Staying on its own seam edges keeps the seam's order.
 */
void straightenSeamRows(geo::Mesh& m, const std::vector<FeatureKind>& feature, const geo::MeshBvh& bvh, int axis, double tolerance)
{
    LILA_ZONE();

    auto onSeam = [&](Index p) { return m.isBoundaryPoint(p) && std::abs(m.position(p)[axis]) <= tolerance; };
    std::vector<std::pair<Index, glm::dvec3>> moves;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || !onSeam(p))
            continue;
        Index across = geo::kInvalid;
        std::vector<Index> along;
        m.forEachPointEdge(p, [&](Index e) {
            const Index q = m.edgeOtherPoint(e, p);
            if (onSeam(q))
                along.push_back(q);
            else
                across = across == geo::kInvalid ? q : geo::kInvalid - 1;
        });
        // The seam's own bends read as corners; a crease leaving across the seam is a real one and stays.
        if (along.size() != 2 || across >= geo::kInvalid - 1 || (across < feature.size() && feature[across] != FeatureKind::None))
            continue;
        const glm::dvec3 at = m.position(p), q = m.position(across);
        glm::dvec3 best = at;
        double bestDist = glm::length(q - at);
        for (Index s : along)
        {
            // Only the half of each seam edge nearest p, so neighbours moving the same way can't cross.
            const glm::dvec3 d = 0.5 * (m.position(s) - at);
            const double t = std::clamp(glm::dot(q - at, d) / std::max(glm::dot(d, d), 1e-30), 0.0, 0.8);
            const glm::dvec3 c = at + t * d;
            if (glm::length(q - c) < bestDist)
            {
                bestDist = glm::length(q - c);
                best = c;
            }
        }
        geo::BvhClosest c;
        if (bvh.closestPoint(best, c))
            best = c.point;
        best[axis] = 0.0;
        moves.push_back({p, best});
    }
    // A move that turns a face round the point over (a quad bent across its diagonal) stays undone.
    for (const auto& [p, at] : moves)
    {
        const glm::dvec3 was = m.position(p);
        std::vector<std::pair<Index, glm::dvec3>> before;
        for (Index f : m.pointFaces(p))
            before.push_back({f, m.faceAreaVector(f)});
        m.setPosition(p, at);
        for (const auto& [f, n] : before)
            if (glm::dot(m.faceAreaVector(f), n) < 0.25 * glm::dot(n, n))
            {
                m.setPosition(p, was);
                break;
            }
    }
}

/**
 * Closes the holes extraction leaves where the field was too tangled to trace a face. Boundary edges are walked
 * against their faces, and a walk that comes back through a point it already passed is cut there, so a hole that
 * touches itself or another hole still comes out as simple loops. Loops on the source's own border stay open.
 */
/** A quad patch across a hole loop wider than a face, or false when no pattern fits its sides. */
bool patchHole(geo::Mesh& m, std::vector<Index>& loop, const geo::MeshBvh* surface)
{
    // Quads need an even loop: an odd one gets a point in its longest edge, turning the face beyond it into a pentagon
    // that the pure-quad pass splits later.
    if (loop.size() >= 5 && loop.size() % 2 != 0)
    {
        size_t longest = 0;
        double length = -1.0;
        for (size_t k = 0; k < loop.size(); ++k)
            if (const double d = glm::distance(m.position(loop[k]), m.position(loop[(k + 1) % loop.size()])); d > length)
            {
                length = d;
                longest = k;
            }
        const Index e = m.findEdge(loop[longest], loop[(longest + 1) % loop.size()]);
        if (e == kInvalid)
            return false;
        const Index p = m.splitEdge(e, 0.5);
        loop.insert(loop.begin() + std::ptrdiff_t(longest + 1), p);
    }
    if (loop.size() < 6 || loop.size() % 2 != 0)
        return false;
    PatchBoundary b;
    for (Index p : loop)
        b.points.push_back(m.position(p));
    for (int sides : {4, 6, 5, 3})
    {
        b.corners = patchCorners(b.points, sides);
        if (b.corners.size() != size_t(sides) || !patchSidesFillable(patchSideCounts(b)))
            continue;
        const PatchResult r = fillPatch(b, {}, surface);
        if (!r.ok())
            continue;
        std::vector<Index> id(r.positions.size());
        for (size_t i = 0; i < loop.size(); ++i)
            id[i] = loop[i];
        for (size_t i = loop.size(); i < r.positions.size(); ++i)
            id[i] = m.addPoint(r.positions[i]);
        for (const auto& f : r.faces)
        {
            std::vector<Index> pts;
            for (Index k : f)
                pts.push_back(id[k]);
            m.addFace(pts);
        }
        return true;
    }
    return false;
}

void fillHoles(geo::Mesh& m, const std::vector<uint8_t>& sourceBoundary, const geo::MeshBvh* surface = nullptr)
{
    LILA_ZONE();

    std::vector<std::vector<Index>> out(m.pointCapacity());
    Index count = 0;
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e) && m.edgeFaceCount(e) == 1)
        {
            const Index c = m.edgeCorner(e);
            out[m.cornerNextPoint(c)].push_back(m.cornerPoint(c));
            ++count;
        }
    std::vector<std::vector<Index>> loops;
    std::vector<Index> walk, at(m.pointCapacity(), kInvalid);
    for (Index start = 0; start < m.pointCapacity() && count > 0; ++start)
        while (!out[start].empty())
        {
            walk.clear();
            Index p = start;
            for (;;)
            {
                if (const Index from = at[p]; from != kInvalid)
                {
                    // Back at a point of this walk: the stretch since then is one loop.
                    loops.emplace_back(walk.begin() + from, walk.end());
                    for (size_t k = from; k < walk.size(); ++k)
                        at[walk[k]] = kInvalid;
                    walk.resize(from);
                }
                if (out[p].empty())
                    break;
                at[p] = Index(walk.size());
                walk.push_back(p);
                const Index q = out[p].back();
                out[p].pop_back();
                --count;
                p = q;
            }
            for (Index q : walk)
                at[q] = kInvalid;
        }
    const auto onBorder = [&](Index p) { return p < sourceBoundary.size() && sourceBoundary[p]; };
    for (const std::vector<Index>& loop : loops)
    {
        if (loop.size() < 3)
            continue;
        std::vector<size_t> border;
        for (size_t k = 0; k < loop.size(); ++k)
            if (onBorder(loop[k]))
                border.push_back(k);
        if (border.size() < 2)
        {
            std::vector<Index> hole = loop;
            if (!(surface && patchHole(m, hole, surface)))
                m.addFace(hole);
            continue;
        }
        // A hole that reaches the source's border is a bay in it: fill each run of inner points between two border
        // points, closing it along the border.
        for (size_t i = 0; i < border.size(); ++i)
        {
            const size_t a = border[i], b = border[(i + 1) % border.size()];
            std::vector<Index> bay;
            for (size_t k = a;; k = (k + 1) % loop.size())
            {
                bay.push_back(loop[k]);
                if (k == b && bay.size() > 1)
                    break;
                if (bay.size() > loop.size())
                    break;
            }
            if (bay.size() >= 3 && bay.front() != bay.back())
                m.addFace(bay);
        }
    }
}

/** Moving p to `to` leaves every face around it facing the way it did. */
bool movesCleanly(const geo::Mesh& m, Index p, const glm::dvec3& to)
{
    bool ok = true;
    m.forEachPointCorner(p, [&](Index c) {
        if (!ok)
            return;
        const Index f = m.cornerFace(c);
        const glm::dvec3 before = m.faceAreaVector(f);
        glm::dvec3 after(0.0);
        const Index c0 = m.faceCorner(f);
        const auto at = [&](Index k) { return m.cornerPoint(k) == p ? to : m.position(m.cornerPoint(k)); };
        const glm::dvec3 o = at(c0);
        for (Index k = m.cornerNext(c0); m.cornerNext(k) != c0; k = m.cornerNext(k))
            after += glm::cross(at(k) - o, at(m.cornerNext(k)) - o);
        ok = glm::dot(after, before) > 0.1 * glm::length(after) * glm::length(before);
    });
    return ok;
}

/**
 * Every source corner gets an output point. The nearest free point within reach moves onto it if no face turns over;
 * otherwise the nearest edge (a border edge, for a corner on the border) is split there. Either way it stays put after.
 */
void snapCorners(geo::Mesh& m, std::vector<FeatureKind>& feature, const Surface& s, double reach)
{
    LILA_ZONE();

    const double same = 1e-9 * (1.0 + reach);
    std::vector<Index> corners;
    const std::vector<uint8_t> tip = s.saddleTips();
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.feature[v] == FeatureKind::Corner && !tip[v])
            corners.push_back(v);
    std::vector<uint8_t> taken(m.pointCapacity(), 0);
    std::vector<std::pair<double, Index>> near;
    for (Index v : corners)
    {
        const glm::dvec3 target = s.pos[v];
        const bool onBorder = s.onBoundary[v] != 0;
        near.clear();
        bool present = false;
        for (Index p = 0; p < m.pointCapacity() && !present; ++p)
        {
            if (!m.pointValid(p))
                continue;
            const double d = glm::distance(m.position(p), target);
            present = d < same;
            if (d < reach && !taken[p] && onBorder == m.isBoundaryPoint(p))
                near.push_back({d, p});
        }
        if (present)
            continue;
        std::sort(near.begin(), near.end());
        Index chosen = kInvalid;
        for (const auto& [d, p] : near)
            if (movesCleanly(m, p, target))
            {
                chosen = p;
                break;
            }
        if (chosen == kInvalid)
        {
            // A border corner has to be there however far the outline fell short; an inner one only nearby.
            Index edge = kInvalid;
            double bestD = onBorder ? 3.0 * reach : reach, bestT = 0.5;
            for (Index e = 0; e < m.edgeCapacity(); ++e)
            {
                if (!m.edgeValid(e) || (onBorder && !m.isBoundaryEdge(e)))
                    continue;
                const glm::dvec3 a = m.position(m.edgePoint(e, 0)), ab = m.position(m.edgePoint(e, 1)) - a;
                const double len2 = glm::dot(ab, ab);
                const double t = len2 > 0.0 ? std::clamp(glm::dot(target - a, ab) / len2, 0.1, 0.9) : 0.5;
                const double d = glm::distance(a + ab * t, target);
                if (d < bestD)
                {
                    bestD = d;
                    bestT = t;
                    edge = e;
                }
            }
            if (edge == kInvalid)
                continue;
            chosen = m.splitEdge(edge, bestT);
            feature.resize(m.pointCapacity(), FeatureKind::None);
            taken.resize(m.pointCapacity(), 0);
            if (!movesCleanly(m, chosen, target))
            {
                feature[chosen] = FeatureKind::None;
                continue;
            }
        }
        m.setPosition(chosen, target);
        feature[chosen] = FeatureKind::Corner;
        taken[chosen] = 1;
    }
}

/** Output border points go onto the source's border, so open edges keep their outline. */
void snapBorder(geo::Mesh& m, std::vector<FeatureKind>& feature, const FeatureLines& border, double reach)
{
    LILA_ZONE();

    if (border.empty())
        return;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p) || !m.isBoundaryPoint(p) || feature[p] == FeatureKind::Corner)
            continue;
        glm::dvec3 q;
        if (border.closest(m.position(p), reach, q))
        {
            m.setPosition(p, q);
            feature[p] = FeatureKind::Line;
        }
    }
}

/**
 * Splits every n-gon into n quads around its centroid (no smoothing), so any polygon mix comes out all quads. Edge
 * midpoints along a crease stay on it.
 */
geo::Mesh splitToQuads(const geo::Mesh& m, std::vector<FeatureKind>& feature, std::vector<uint8_t>& boundary, const FeatureLines& lines,
                       double h)
{
    LILA_ZONE();

    std::vector<glm::dvec3> pts;
    std::vector<FeatureKind> outFeature;
    std::vector<uint8_t> outBoundary;
    std::vector<Index> pointId(m.pointCapacity(), kInvalid);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            pointId[p] = Index(pts.size());
            pts.push_back(m.position(p));
            outFeature.push_back(feature[p]);
            outBoundary.push_back(boundary[p]);
        }
    std::vector<Index> edgeId(m.edgeCapacity(), kInvalid);
    for (Index e = 0; e < m.edgeCapacity(); ++e)
        if (m.edgeValid(e))
        {
            Index a = m.edgePoint(e, 0), b = m.edgePoint(e, 1);
            glm::dvec3 mid = 0.5 * (m.position(a) + m.position(b));
            FeatureKind k = FeatureKind::None;
            if (feature[a] != FeatureKind::None && feature[b] != FeatureKind::None)
            {
                glm::dvec3 q, t;
                glm::dvec3 dir = glm::normalize(m.position(b) - m.position(a));
                if (lines.closest(mid, 0.3 * h, q, &t) && std::abs(glm::dot(t, dir)) > 0.8)
                    k = FeatureKind::Line;
            }
            edgeId[e] = Index(pts.size());
            pts.push_back(mid);
            outFeature.push_back(k);
            outBoundary.push_back(m.isBoundaryEdge(e));
        }
    std::vector<Index> sizes, idx;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        Index centre = Index(pts.size());
        pts.push_back(m.faceCentroid(f));
        outFeature.push_back(FeatureKind::None);
        outBoundary.push_back(0);
        std::vector<Index> cs = m.faceCorners(f);
        for (size_t k = 0; k < cs.size(); ++k)
        {
            Index c = cs[k], prev = cs[(k + cs.size() - 1) % cs.size()];
            sizes.push_back(4);
            idx.insert(idx.end(), {pointId[m.cornerPoint(c)], edgeId[m.cornerEdge(c)], centre, edgeId[m.cornerEdge(prev)]});
        }
    }
    geo::Mesh out = geo::Mesh::fromPolygons(pts, sizes, idx);
    feature = std::move(outFeature);
    boundary = std::move(outBoundary);
    return out;
}

/** Compacts the mesh and the per-point tags along with it. */
void compactTagged(geo::Mesh& m, std::vector<FeatureKind>& feature, std::vector<uint8_t>& boundary)
{
    geo::MeshRemap remap;
    m.compact(&remap);
    std::vector<FeatureKind> f(m.pointCapacity(), FeatureKind::None);
    std::vector<uint8_t> b(m.pointCapacity(), 0);
    for (Index p = 0; p < Index(remap.points.size()); ++p)
        if (remap.points[p] != kInvalid)
        {
            f[remap.points[p]] = p < feature.size() ? feature[p] : FeatureKind::None;
            b[remap.points[p]] = p < boundary.size() ? boundary[p] : 0;
        }
    feature = std::move(f);
    boundary = std::move(b);
}
} // namespace

namespace
{
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
 * Lays each feature guide onto the working surface as hard edges: the surface vertex path nearest the polyline, with its
 * vertices moved onto the polyline where that leaves their triangles facing the same way, so the crease runs straight.
 */
void embedFeatureGuides(Surface& s, const std::vector<std::vector<glm::dvec3>>& guides, const geo::MeshBvh& bvh)
{
    LILA_ZONE();

    if (guides.empty())
        return;
    s.buildTopology();
    s.buildGeometry();
    const Index n = s.vertexCount();
    std::vector<std::vector<Index>> vertexTris(n);
    for (size_t t = 0; t < s.tris.size(); ++t)
        for (Index v : s.tris[t])
            vertexTris[v].push_back(Index(t));
    std::vector<uint8_t> border(n, 0);
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeTriCount[e] == 1)
            border[s.edges[e][0]] = border[s.edges[e][1]] = 1;
    const auto nearestVertex = [&](const glm::dvec3& p) {
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (Index v = 0; v < n; ++v)
            if (const double d = glm::dot(s.pos[v] - p, s.pos[v] - p); d < bestD)
            {
                bestD = d;
                best = v;
            }
        return best;
    };
    const double step = std::max(s.meanEdgeLength() * 0.5, 1e-12);
    std::vector<double> dist(n, std::numeric_limits<double>::max());
    std::vector<Index> from(n, kInvalid), touched;
    for (const auto& guide : guides)
    {
        std::vector<glm::dvec3> samples;
        for (size_t i = 0; i + 1 < guide.size(); ++i)
        {
            const int count = std::max(1, int(std::ceil(glm::distance(guide[i], guide[i + 1]) / step)));
            for (int k = 0; k < count; ++k)
                samples.push_back(glm::mix(guide[i], guide[i + 1], double(k) / count));
        }
        if (!guide.empty())
            samples.push_back(guide.back());
        std::vector<Index> path;
        for (const glm::dvec3& p : samples)
        {
            const Index v = nearestVertex(p);
            if (v == kInvalid || (!path.empty() && path.back() == v))
                continue;
            if (path.empty())
            {
                path.push_back(v);
                continue;
            }
            // Shortest edge path from the last vertex, which is a few edges away at most.
            const Index a = path.back();
            for (Index t : touched)
            {
                dist[t] = std::numeric_limits<double>::max();
                from[t] = kInvalid;
            }
            touched.assign({a});
            dist[a] = 0.0;
            std::vector<std::pair<double, Index>> heap{{0.0, a}};
            while (!heap.empty())
            {
                std::pop_heap(heap.begin(), heap.end(), std::greater<>());
                auto [d, u] = heap.back();
                heap.pop_back();
                if (u == v || d > dist[u])
                    break;
                for (Index k = s.adjStart[u]; k < s.adjStart[u + 1]; ++k)
                {
                    const Index w = s.adj[k];
                    const double nd = d + glm::distance(s.pos[u], s.pos[w]);
                    if (nd < dist[w] && nd < 8.0 * step)
                    {
                        if (dist[w] == std::numeric_limits<double>::max())
                            touched.push_back(w);
                        dist[w] = nd;
                        from[w] = u;
                        heap.push_back({nd, w});
                        std::push_heap(heap.begin(), heap.end(), std::greater<>());
                    }
                }
            }
            if (from[v] == kInvalid)
            {
                path.push_back(v);
                continue;
            }
            std::vector<Index> leg;
            for (Index u = v; u != a; u = from[u])
                leg.push_back(u);
            path.insert(path.end(), leg.rbegin(), leg.rend());
        }
        for (Index v : path)
        {
            glm::dvec3 best = s.pos[v];
            double bestD = std::numeric_limits<double>::max();
            for (size_t i = 0; i + 1 < guide.size(); ++i)
            {
                const glm::dvec3 a = guide[i], ab = guide[i + 1] - a;
                const double len2 = glm::dot(ab, ab);
                const glm::dvec3 q = a + ab * (len2 > 0.0 ? std::clamp(glm::dot(s.pos[v] - a, ab) / len2, 0.0, 1.0) : 0.0);
                if (const double d = glm::dot(q - s.pos[v], q - s.pos[v]); d < bestD)
                {
                    bestD = d;
                    best = q;
                }
            }
            geo::BvhClosest c;
            if (bvh.closestPoint(best, c))
                best = c.point;
            bool keeps = !border[v];
            for (Index t : vertexTris[v])
            {
                std::array<glm::dvec3, 3> p;
                for (int k = 0; k < 3; ++k)
                    p[k] = s.tris[t][k] == v ? best : s.pos[s.tris[t][k]];
                keeps &= glm::dot(glm::cross(p[1] - p[0], p[2] - p[0]), s.triNormal[t]) > 0.0;
            }
            if (keeps)
                s.pos[v] = best;
        }
        for (size_t i = 0; i + 1 < path.size(); ++i)
            s.hard.insert(edgeKey(path[i], path[i + 1]));
    }
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

/** The working surface and lattice spacing everything after it shares. Not movable: the bvh points into src. */
struct Work
{
    geo::Mesh src;
    std::optional<geo::MeshBvh> bvh;
    Surface s;
    bool symmetric = false;
    geo::SymmetryPlane plane;
    double h = 0.0, h0 = 0.0, cf = 1.0;
    std::vector<double> scale;
    /** The spacing before gaps and thin parts capped it. */
    std::vector<double> uncapped;
    FieldOptions fo;
    std::string error;

    Work() = default;
    Work(const Work&) = delete;
};

/** Edges whose two triangles face apart (debug: LILA_SURFACE_DEBUG). */
int foldedSurfaceEdges(const Surface& s)
{
    std::unordered_map<uint64_t, std::vector<Index>> by;
    for (Index t = 0; t < Index(s.tris.size()); ++t)
        for (int k = 0; k < 3; ++k)
            by[edgeKey(s.tris[t][k], s.tris[t][(k + 1) % 3])].push_back(t);
    auto normal = [&](Index t) {
        const auto& f = s.tris[t];
        return glm::cross(s.pos[f[1]] - s.pos[f[0]], s.pos[f[2]] - s.pos[f[0]]);
    };
    int n = 0;
    for (const auto& [k, ts] : by)
        if (ts.size() == 2)
        {
            const glm::dvec3 a = normal(ts[0]), b = normal(ts[1]);
            n += glm::dot(a, b) < -0.5 * glm::length(a) * glm::length(b);
        }
    return n;
}

void prepareWork(Work& w, const geo::Mesh& source, const QuadRemeshOptions& opts, Stopwatch& clock)
{
    LILA_ZONE();

    if (source.numFaces() == 0)
    {
        w.error = "source has no faces";
        return;
    }
    w.src = source;
    w.src.compact();
    w.bvh.emplace(w.src);
    const geo::MeshBvh& bvh = *w.bvh;
    clock.lap("bvh");

    Surface& s = w.s;
    s = surfaceFromMesh(w.src, opts.useHardEdges);
    s.pins = opts.pinnedPoints;
    if (std::getenv("LILA_SURFACE_DEBUG"))
        std::fprintf(stderr, "surface %s: %zu tris, %d folded edges\n", "source", s.tris.size(), foldedSurfaceEdges(s));
    w.symmetric = opts.symmetryAxis >= 0 && opts.symmetryAxis < 3;
    const bool symmetric = w.symmetric;
    if (symmetric)
    {
        w.plane.normal = glm::dvec3(0.0);
        w.plane.normal[opts.symmetryAxis] = 1.0;
    }
    // Sizes are for the half that is remeshed; the shape is simplified whole and clipped after, since simplifying the
    // clipped half folds triangles beside its cut border.
    const double halves = symmetric ? 2.0 : 1.0;
    const double area = s.totalArea() / halves;
    const double faces = std::max(8.0, double(opts.targetFaces) / halves);
    const double h0 = opts.targetEdgeLength > 0.0 ? opts.targetEdgeLength : std::sqrt(area / faces);
    const double cf = opts.pureQuads ? 2.0 : 1.0;
    w.cf = cf;
    w.h0 = h0;
    double workLength = cf * h0 / 4.0;
    const double workTris = std::max(64.0, area / (0.433 * workLength * workLength));
    bool decimated = double(s.tris.size()) > 1.5 * halves * workTris;
    if (decimated)
        decimateSurface(s, Index(halves * workTris));
    if (std::getenv("LILA_SURFACE_DEBUG"))
        std::fprintf(stderr, "surface %s: %zu tris, %d folded edges\n", "decimated", s.tris.size(), foldedSurfaceEdges(s));
    if (symmetric)
    {
        clipSurface(s, w.plane, opts.symmetryAxis);
        if (s.tris.empty())
        {
            w.error = "nothing on the positive side of the symmetry plane";
            return;
        }
    }
    refineSurface(s, 1.5 * workLength);
    embedFeatureGuides(s, opts.featureGuides, bvh);
    if (std::getenv("LILA_SURFACE_DEBUG"))
        std::fprintf(stderr, "surface %s: %zu tris, %d folded edges\n", "refined", s.tris.size(), foldedSurfaceEdges(s));
    auto prepare = [&]() {
        s.buildTopology();
        s.buildGeometry();
        s.detectFeatures(opts.sharpAngle, 75.0, 2.0 * h0);
    };
    prepare();
    if (decimated)
    {
        // Simplification leaves slivers that make the fields noisy.
        smoothSurface(s, bvh, 8);
        prepare();
    if (std::getenv("LILA_SURFACE_DEBUG"))
        std::fprintf(stderr, "surface %s: %zu tris, %d folded edges\n", "smoothed", s.tris.size(), foldedSurfaceEdges(s));
    }
    clock.lap("surface");

    // Gaps across air only bridge on a closed shape; an open sheet's folds remesh fine as they are.
    const bool closedSource = isClosed(w.src);

    // Lattice spacing per vertex from painted density and curvature, normalized to the face budget.
    std::vector<double>& scale = w.scale;
    double h = h0;
    for (int pass = 0; pass < 2; ++pass)
    {
        const Index n = s.vertexCount();
        Curvature curv = estimateCurvature(s);
        std::vector<double> rho(n);
        for (Index v = 0; v < n; ++v)
        {
            double r = s.density[v];
            if (opts.adaptivity > 0.0)
                r *= std::pow(1.0 + 2.0 * curv.magnitude[v] * h0, opts.adaptivity);
            rho[v] = std::clamp(r, 1.0 / 8.0, 8.0);
        }
        // Spread far enough that the spacing changes over several lattice cells; extraction needs it slowly varying.
        const int spread = int(std::clamp(3.0 * cf * h0 / std::max(workLength, 1e-12), 6.0, 60.0));
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
        // Joints get theirs after the spread, which would flatten a fold's narrow peak. A fold reads as a saddle only along
        // its crease, so the band widens from there, halving every two cells.
        if (opts.jointDensity > 0.0)
        {
            std::vector<double> joint = saddleAmount(s, cf * h0);
            const double perCell = std::max(1.0, cf * h0 / std::max(s.meanEdgeLength(), 1e-12));
            const double decay = std::pow(0.5, 1.0 / (2.0 * perCell));
            const int rings = int(std::min(80.0, opts.jointDensityReach * perCell));
            for (int it = 0; it < rings; ++it)
            {
                std::vector<double> next = joint;
                for (Index v = 0; v < n; ++v)
                    for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
                        next[v] = std::max(next[v], joint[s.adj[k]] * decay);
                joint.swap(next);
            }
            for (Index v = 0; v < n; ++v)
                rho[v] = std::clamp(rho[v] * (1.0 + 2.0 * opts.jointDensity * joint[v]), 1.0 / 8.0, 8.0);
        }
        // Handles ease out over their own radius already; spreading them as well would flatten their peak.
        if (!opts.densityHandles.empty())
            for (Index v = 0; v < n; ++v)
                rho[v] = std::clamp(rho[v] * handleDensity(s.pos[v], opts.densityHandles), 1.0 / 8.0, 8.0);
        if (opts.targetEdgeLength <= 0.0)
        {
            double weighted = 0.0;
            for (Index v = 0; v < n; ++v)
                weighted += s.area[v] * rho[v];
            h = std::sqrt(weighted / faces);
        }
        scale.resize(n);
        for (Index v = 0; v < n; ++v)
            scale[v] = cf * h / std::sqrt(rho[v]);
        w.uncapped = scale;
        if (opts.resolveGaps && closedSource)
            fitGaps(s, w.src, bvh, scale);
        if (opts.edgesAround > 0 && closedSource)
            fitThickness(s, w.src, bvh, curv.magnitude, opts.edgesAround, cf, scale);
        double minScale = std::numeric_limits<double>::max();
        for (Index v = 0; v < n; ++v)
            minScale = std::min(minScale, scale[v]);
        if (pass == 1 || minScale >= 2.5 * workLength)
            break;
        // Denser spots need a finer working mesh to resolve their lattice.
        workLength = minScale / 2.5;
        refineSurface(s, 1.5 * workLength);
        prepare();
    }
    w.h = h;
    clock.lap("density");

    {
        std::vector<double> sorted = scale;
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
        w.fo.scale = sorted[sorted.size() / 2];
    }
    w.fo.guides = opts.guides;
    w.fo.softCreaseAngle = opts.softCreaseAngle;
    w.fo.jointStiffness = opts.jointPoleAvoidance;
    s.thinCorners(scale);
    Budget::note(std::to_string(s.tris.size()) + " working triangles for about " + std::to_string(int(faces)) + " faces");
}

/** Each pole's triangle on the working surface: the one with the nearest centre. Poles behind the symmetry plane drop. */
std::vector<FieldSingularity> polesOnSurface(const Work& w, const std::vector<Pole>& poles)
{
    std::vector<FieldSingularity> out;
    for (const Pole& p : poles)
    {
        if (p.valence == 4 || (w.symmetric && w.plane.distance(p.position) < 0.0))
            continue;
        Index best = kInvalid;
        double bestD = std::numeric_limits<double>::max();
        for (size_t t = 0; t < w.s.tris.size(); ++t)
        {
            const auto& tri = w.s.tris[t];
            glm::dvec3 c = (w.s.pos[tri[0]] + w.s.pos[tri[1]] + w.s.pos[tri[2]]) / 3.0;
            double d = glm::dot(c - p.position, c - p.position);
            if (d < bestD)
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
 * The cross field of a symmetric shape, solved over the whole shape (the half joined to its reflection) and the same
 * either side by construction: the seam is held along the plane, so it can be a straight edge loop, and each vertex
 * takes the mean of its own cross and its reflection's. Its poles are placed once: close opposite pairs cancel, and the
 * rest move at least two quads off the seam and out of saddles (crotch, armpit, finger webs), mirrored pole for pole.
 * Returns the field on the half's vertices; s's seam border loses the corners clipping made there.
 */
/** Corners under 30 degrees plus crowded poles above which a patch layout is laid out again in the seamless charts. */
constexpr Index kRescueCrowding = 40;

/** Saddle amount (saddleAmount at the field's spacing) above which a pole counts as in a joint. */
constexpr double kPoleSaddle = 0.15;

std::vector<glm::dvec3> symmetricField(Work& w, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    Surface& s = w.s;
    const geo::SymmetryPlane& plane = w.plane;
    std::vector<Index> mirrorOf;
    Surface whole = mirrorSurface(s, plane, mirrorOf);
    whole.buildTopology();
    whole.buildGeometry();
    whole.detectFeatures(opts.sharpAngle, 75.0, 2.0 * w.h0);
    const Index n = s.vertexCount(), nt = Index(s.tris.size());
    if (const char* dump = std::getenv("LILA_WHOLE_DUMP"))
        if (FILE* f = std::fopen(dump, "w"))
        {
            for (Index v = 0; v < whole.vertexCount(); ++v)
                std::fprintf(f, "v %.9g %.9g %.9g\n", whole.pos[v].x, whole.pos[v].y, whole.pos[v].z);
            for (const auto& t : whole.tris)
                std::fprintf(f, "f %d %d %d\n", int(t[0] + 1), int(t[1] + 1), int(t[2] + 1));
            for (Index v = 0; v < whole.vertexCount(); ++v)
                if (whole.feature[v] != FeatureKind::None)
                    std::fprintf(f, "# feature %d %d\n", int(v), int(whole.feature[v]));
            std::fclose(f);
        }
    std::vector<double> scale(whole.vertexCount());
    for (Index v = 0; v < n; ++v)
        scale[v] = scale[mirrorOf[v]] = w.scale[v];

    auto onSeam = [&](Index v) { return mirrorOf[v] == v; };
    auto seamTangent = [&](Index v) { return glm::normalize(glm::cross(plane.normal, whole.normal[v])); };
    FieldOptions fo = w.fo;
    for (Index v = 0; v < n; ++v)
        if (onSeam(v) && whole.feature[v] == FeatureKind::None)
            fo.aligned.push_back({v, seamTangent(v)});

    auto reflectDir = [&](const glm::dvec3& d) { return d - 2.0 * glm::dot(d, plane.normal) * plane.normal; };
    auto symmetrize = [&](std::vector<glm::dvec3>& q) {
        for (Index v = 0; v < n; ++v)
        {
            const glm::dvec3 nv = whole.normal[v], e1 = anyTangent(nv), e2 = glm::cross(nv, e1);
            auto power = [&](glm::dvec3 d) {
                d -= nv * glm::dot(nv, d);
                return std::polar(1.0, 4.0 * std::atan2(glm::dot(d, e2), glm::dot(d, e1)));
            };
            const std::complex<double> z = power(q[v]) + power(reflectDir(q[mirrorOf[v]]));
            if (std::abs(z) < 1e-12)
                continue;
            const double a = std::arg(z) / 4.0;
            q[v] = std::cos(a) * e1 + std::sin(a) * e2;
            q[mirrorOf[v]] = reflectDir(q[v]);
        }
    };
    std::vector<glm::dvec3> q = solveCrossField(whole, fo);
    symmetrize(q);

    // Two quads off the seam (with pure quads the field's spacing is two quads) and out of saddles.
    const std::vector<double> saddle = saddleAmount(whole, w.fo.scale);
    auto centre = [&](Index t) { return (whole.pos[whole.tris[t][0]] + whole.pos[whole.tris[t][1]] + whole.pos[whole.tris[t][2]]) / 3.0; };
    auto allowed = [&](Index t) {
        const auto& tri = whole.tris[t];
        const double cell = (scale[tri[0]] + scale[tri[1]] + scale[tri[2]]) / 3.0;
        const double sad = std::max({saddle[tri[0]], saddle[tri[1]], saddle[tri[2]]});
        return t < nt && plane.distance(centre(t)) >= 2.0 * cell / w.cf && sad < kPoleSaddle;
    };
    std::vector<FieldSingularity> half;
    for (const FieldSingularity& p : fieldSingularities(whole, q))
        if (p.tri < nt)
            half.push_back(p);
    const size_t found = half.size();
    half = spreadPoles(whole, cancelClosePoles(whole, half, scale, 1.5), scale, 1.0, allowed);
    std::vector<FieldSingularity> poles = half;
    for (const FieldSingularity& p : half)
        poles.push_back({p.tri + nt, p.index});
    PrescribedTwist pt = prescribeSingularities(whole, poles);
    if (pt.error.empty())
    {
        // Weaker pull to the curvature lets the field keep to the poles it is given instead of growing new ones beside them.
        fo.curvatureWeight *= std::getenv("LILA_PRESCRIBED_ALIGN") ? std::atof(std::getenv("LILA_PRESCRIBED_ALIGN")) : 0.3;
        fo.twist = std::move(pt.twist);
        q = solveCrossField(whole, fo);
        symmetrize(q);
    }
    else
        Budget::note("pole placement: " + pt.error);
    const size_t after = fieldSingularities(whole, q).size();
    Budget::note(std::to_string(found) + " poles on the half, " + std::to_string(half.size()) + " placed, " + std::to_string(after) + " in the whole field");
    if (std::getenv("LILA_POLE_DEBUG"))
    {
        std::fprintf(stderr, "symmetric field: %zu half poles found, %zu placed, %zu in the final whole field\n", found, half.size(), after);
        std::unordered_map<Index, int> want;
        for (const FieldSingularity& p : poles)
            want[p.tri] += p.index;
        for (const FieldSingularity& p : fieldSingularities(whole, q))
        {
            auto it = want.find(p.tri);
            if (it != want.end() && it->second == p.index)
            {
                want.erase(it);
                continue;
            }
            const auto& tri = whole.tris[p.tri];
            int feat = 0;
            for (Index v : tri)
                feat = std::max(feat, int(whole.feature[v]) + (mirrorOf[v] == v ? 10 : 0));
            const glm::dvec3 c = centre(p.tri);
            std::fprintf(stderr, "  extra pole %+d at %.1f %.1f %.1f feature %d\n", p.index, c.x, c.y, c.z, feat);
        }
        for (Index v = 0; v < n; ++v)
            if (onSeam(v) && whole.feature[v] != FeatureKind::None)
                std::fprintf(stderr, "  seam feature %d at %.2f %.2f %.2f\n", int(whole.feature[v]), whole.pos[v].x, whole.pos[v].y, whole.pos[v].z);
        for (const auto& [t, k] : want)
            if (k != 0)
            {
                const glm::dvec3 c = centre(t);
                std::fprintf(stderr, "  missing pole %+d at %.1f %.1f %.1f\n", k, c.x, c.y, c.z);
            }
    }

    // The half's seam is its border: a line along the plane except where a crease of the whole crosses it.
    for (Index v = 0; v < n; ++v)
    {
        if (!onSeam(v))
            continue;
        const glm::dvec3 t = seamTangent(v);
        const bool crossing = whole.feature[v] == FeatureKind::Corner ||
                              (whole.feature[v] == FeatureKind::Line && std::abs(glm::dot(whole.featureDir[v], t)) < 0.7);
        if (crossing || s.pinned[v])
            s.feature[v] = FeatureKind::Corner;
        else
        {
            s.feature[v] = FeatureKind::Line;
            s.featureDir[v] = t;
        }
    }
    q.resize(n);
    return q;
}

/** Whether the field is solved on the half joined to its reflection, with its poles cancelled and spread once there. */
bool usesSymmetricField(const Work& w, const QuadRemeshOptions& opts) { return w.symmetric && !opts.poles && !std::getenv("LILA_HALF_FIELD"); }

/** The cross field, turning exactly at opts.poles when given. */
std::vector<glm::dvec3> crossField(Work& w, const QuadRemeshOptions& opts, std::string& error)
{
    if (usesSymmetricField(w, opts))
        return symmetricField(w, opts);
    if (opts.poles)
    {
        PrescribedTwist pt = prescribeSingularities(w.s, polesOnSurface(w, *opts.poles));
        if (!pt.error.empty())
        {
            error = pt.error;
            return {};
        }
        w.fo.twist = std::move(pt.twist);
    }
    return solveCrossField(w.s, w.fo);
}

Index defects(const geo::Mesh& m, const geo::Mesh& src, const geo::MeshBvh& bvh, bool sourceClosed);

/** The share of the mesh's points whose mirror image across the axis plane lies off the mesh, by over 1% of its size. */
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

/** Relax passes on a patch layout per Relax Iteration asked for. */
constexpr int kPatchRelaxPerIteration = 5;

/** Debug dumps: the mesh as an obj. */
void dumpObj(const geo::Mesh& m, const std::string& path)
{
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        return;
    std::vector<Index> id(m.pointCapacity(), 0);
    Index next = 1;
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            std::fprintf(f, "v %.9g %.9g %.9g\n", m.position(p).x, m.position(p).y, m.position(p).z);
            id[p] = next++;
        }
    for (Index fc = 0; fc < m.faceCapacity(); ++fc)
        if (m.faceValid(fc))
        {
            std::fprintf(f, "f");
            for (Index p : m.facePoints(fc))
                std::fprintf(f, " %d", int(id[p]));
            std::fprintf(f, "\n");
        }
    std::fclose(f);
}
Index largeFaces(const geo::Mesh& m);
Index sharpCorners(const geo::Mesh& m);
Index clusteredPoles(const geo::Mesh& m);
bool better(const QuadRemeshResult& a, int defectsA, const QuadRemeshResult& b, int defectsB);

QuadRemeshResult quadRemeshOnce(const geo::Mesh& source, const QuadRemeshOptions& opts)
{
    LILA_ZONE();

    QuadRemeshResult result;
    Stopwatch clock(result.timings);
    Work w;
    prepareWork(w, source, opts, clock);
    if (!w.error.empty())
    {
        result.error = w.error;
        return result;
    }
    const geo::Mesh& src = w.src;
    const geo::MeshBvh& bvh = *w.bvh;
    Surface& s = w.s;
    const bool symmetric = w.symmetric;
    if (symmetric)
        if (const double off = asymmetry(src, bvh, opts.symmetryAxis); off > 0.05)
            result.notes.push_back("the mesh isn't symmetric across " + std::string(1, char('X' + opts.symmetryAxis)) + " (" +
                                   std::to_string(int(std::lround(100.0 * off))) + "% of it has no mirror image), so this mirrors its +" +
                                   std::string(1, char('X' + opts.symmetryAxis)) + " half; set Symmetry to Off for it");
    const geo::SymmetryPlane& plane = w.plane;
    const double h = w.h, cf = w.cf;
    const std::vector<double>& scale = w.scale;
    const FieldOptions& fo = w.fo;

    std::vector<glm::dvec3> q = crossField(w, opts, result.error);
    if (!result.ok())
        return result;
    Budget::note(std::to_string(fieldSingularities(s, q).size()) + " poles");
    clock.lap("cross field");

    // Extraction and cleanup for one lattice layout.
    auto buildFrom = [&](Extracted ex, const char* layout) {
        QuadRemeshResult out;
        out.layout = layout;
        if (ex.mesh.numFaces() == 0)
        {
            out.error = "extraction produced no faces";
            return out;
        }
        clock.lap("extract");

        FeatureLines lines;
        lines.build(s, fo.scale * 0.5);
        geo::Mesh& m = ex.mesh;
        auto seamDebug = [&](const char* stage) {
            if (!symmetric || !std::getenv("LILA_SEAM_DEBUG"))
                return;
            for (Index p = 0; p < m.pointCapacity(); ++p)
            {
                if (!m.pointValid(p) || !m.isBoundaryPoint(p) || std::abs(plane.distance(m.position(p))) > 0.35 * h)
                    continue;
                int inner = 0;
                m.forEachPointEdge(p, [&](Index e) { inner += !m.isBoundaryEdge(e); });
                if (inner != 1)
                    std::fprintf(stderr, "seam %s %s inner %d at %.3f %.3f %.3f\n", layout, stage, inner, m.position(p).x, m.position(p).y, m.position(p).z);
            }
        };
        seamDebug("extracted");
        // LILA_STAGE_DUMP=<dir>: the mesh after each cleanup step, as <layout>_<step>.obj.
        auto stageDump = [&](const char* stage) {
            if (const char* dir = std::getenv("LILA_STAGE_DUMP"))
                dumpObj(m, std::string(dir) + "/" + layout + "_" + stage + ".obj");
        };
        std::vector<FeatureKind>& feature = ex.feature;
        std::vector<uint8_t>& boundary = ex.boundary;
        feature.resize(m.pointCapacity(), FeatureKind::None);
        boundary.resize(m.pointCapacity(), 0);
        removeValence2(m, feature);
        seamDebug("v2");
        stageDump("1v2");
        // Every border point on the plane is the seam's, however it was traced, so no hole fill closes a bay along it.
        if (symmetric)
        {
            boundary.resize(m.pointCapacity(), 0);
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p) && m.isBoundaryPoint(p) && std::abs(plane.distance(m.position(p))) < 1e-6 * h)
                    boundary[p] = 1;
        }
        geo::conformNormals(m);
        fillHoles(m, boundary, &bvh);
        seamDebug("holes");
        {
            std::vector<Index> tris;
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f) && m.faceSize(f) == 3)
                    tris.push_back(f);
            geo::QuadrangulateOptions qo;
            qo.maxFaceAngle = 60.0;
            geo::quadrangulateFaces(m, tris, qo);
        }
        seamDebug("quads");
        feature.resize(m.pointCapacity(), FeatureKind::None);
        removeValence2(m, feature);
        boundary.resize(m.pointCapacity(), 0);
        compactTagged(m, feature, boundary);
        seamDebug("compact");
        stageDump("2compact");
        geo::conformNormals(m);
        // Conforming only makes the winding consistent; the source's own decides which way round it goes.
        if (2 * foldedFaces(m, src, bvh).size() > size_t(m.numFaces()))
            for (Index f = 0; f < m.faceCapacity(); ++f)
                if (m.faceValid(f))
                    m.flipFace(f);
        const double coarseH = cf * h;
        FeatureLines border;
        border.build(s, fo.scale * 0.5, true);
        snapCorners(m, feature, s, 1.25 * coarseH);
        boundary.resize(m.pointCapacity(), 0);
        seamDebug("presnap");
        stageDump("3corners");
        snapBorder(m, feature, border, 1.5 * coarseH);
        seamDebug("snapped");
        stageDump("4border");
        projectPoints(m, feature, bvh, lines, 0.6 * coarseH);
        relaxAlongFeatures(m, feature, bvh, lines, coarseH, std::max(1, opts.relaxIterations / 2), symmetric ? opts.symmetryAxis : -1);
        clock.lap("cleanup");
        stageDump("5relax");

        // The patch layout is built at the final density.
        if (opts.pureQuads && std::string_view(layout) != "patches")
        {
            m = splitToQuads(m, feature, boundary, lines, coarseH);
            seamDebug("split");
            snapBorder(m, feature, border, coarseH);
            seamDebug("snapped2");
            projectPoints(m, feature, bvh, lines, 0.4 * coarseH);
        }
        // Patch grids are filled flat between their corners, so they take longer to even out over the surface.
        const bool patches = std::string_view(layout) == "patches";
        int relaxIterations = patches ? kPatchRelaxPerIteration * opts.relaxIterations : opts.relaxIterations;
        if (const char* e = std::getenv("LILA_PATCH_RELAX"); e && patches)
            relaxIterations = std::atoi(e);
        if (!patches)
            splitHighValence(m, feature);
        stageDump("6split");
        const std::optional<SizeField> sizes = patches ? std::optional<SizeField>(std::in_place, s, scale) : std::nullopt;
        relaxAlongFeatures(m, feature, bvh, lines, h, relaxIterations, symmetric ? opts.symmetryAxis : -1, sizes ? &*sizes : nullptr);
        stageDump("7relax");
        untangle(m, feature, src, bvh, lines, h);
        stageDump("8untangle");
        clock.lap("refine");

        if (symmetric)
        {
            // Seam points sit exactly on the plane so the mirror welds them and the halves match bit for bit.
            const int axis = opts.symmetryAxis;
            for (Index p = 0; p < m.pointCapacity(); ++p)
                if (m.pointValid(p) && m.isBoundaryPoint(p) && std::abs(plane.distance(m.position(p))) < 0.35 * h)
                {
                    glm::dvec3 at = m.position(p);
                    at[axis] = 0.0;
                    m.setPosition(p, at);
                }
            seamDebug("premerge");
            // The patch layout's center line is a traced line, an edge loop already.
            if (!patches)
            {
                collapseSeamWedges(m, feature, bvh, axis, 1e-9 * h);
                straightenSeamRows(m, feature, bvh, axis, 1e-9 * h);
            }
            seamDebug("premirror");
            geo::MirrorOptions mo;
            mo.plane = plane;
            mo.weld = true;
            mo.tolerance = std::max(1e-9, h * 1e-6);
            geo::mirrorMesh(m, mo);
            m.compact();
        }
        if (opts.sharpAngle > 0.0 && !lines.empty())
            geo::setHardEdgesByAngle(m, opts.sharpAngle);
        out.metrics = measureQuality(m, &bvh);
        out.mesh = std::move(m);
        return out;
    };

    const bool closed = isClosed(src);
    auto record = [&](QuadRemeshResult r) {
        if (r.ok())
            result.attempts.push_back({r.layout, r.mesh.numFaces(), int(defects(r.mesh, src, bvh, closed)), r.metrics.irregular});
        // LILA_DUMP_ATTEMPTS=<dir>: every attempt as attempt<n>_<layout>.obj, in the source's space.
        if (const char* dir = std::getenv("LILA_DUMP_ATTEMPTS"); dir && r.ok())
            dumpObj(r.mesh, std::string(dir) + "/attempt" + std::to_string(result.attempts.size()) + "_" + r.layout + ".obj");
        return r;
    };
    auto build = [&](const char* layout, const std::vector<glm::dvec3>& o, const std::vector<std::array<int, 2>>* steps = nullptr) {
        return record(buildFrom(extractMesh(s, q, o, scale, steps), layout));
    };
    const bool pinned = !opts.pinnedPoints.empty();
    auto flawsOf = [&](const QuadRemeshResult& r) { return r.ok() ? int(defects(r.mesh, src, bvh, closed) + largeFaces(r.mesh)) : 0; };
    // Pinned points only hold exactly in one global parametrization, whose lattice points round onto them. Where it
    // doesn't come out clean, the local field, which holds them only nearly, and the result says so.
    auto pinnedLayout = [&](const std::vector<glm::dvec3>& qPin) {
        std::optional<QuadRemeshResult> kept;
        int keptFlaws = 0;
        auto consider = [&](QuadRemeshResult r) {
            const int flaws = flawsOf(r);
            if (r.ok() && (!kept || flaws < keptFlaws || (flaws == keptFlaws && r.metrics.irregular < kept->metrics.irregular)))
            {
                kept = std::move(r);
                keptFlaws = flaws;
            }
            return flaws;
        };
        // Steering the rounding away from folds untangles some layouts, so a folded one tries again with it when the
        // budget leaves room. One folded through and through (busy creases, mostly) doesn't come right by steering.
        for (int foldRetries : {0, 48})
        {
            const auto started = std::chrono::steady_clock::now();
            std::vector<std::array<int, 2>> steps;
            Extracted grid;
            Index folds = 0;
            std::vector<glm::dvec3> o = solveSeamlessPositions(s, qPin, scale, nullptr, &steps, &grid, foldRetries, &folds);
            if (o.empty())
                break;
            // The grid traces folds left in the layout as holes; rounding to the nearest lattice point covers them.
            if (grid.mesh.numFaces() == 0 || consider(record(buildFrom(std::move(grid), "seamless"))) > 0)
                consider(record(buildFrom(extractMesh(s, qPin, o, scale, &steps), "seamless")));
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
            if (folds == 0 || folds > Index(s.tris.size() / 100) || Budget::remaining() <= 3.0 * took || (kept && keptFlaws == 0))
                break;
        }
        clock.lap("seamless");
        if (kept && keptFlaws == 0)
            return std::move(*kept);
        QuadRemeshResult field = build("field", solvePositionField(s, q, scale));
        if (kept && (!field.ok() || keptFlaws <= flawsOf(field)))
            return std::move(*kept);
        if (field.ok())
            result.notes.push_back("the pinned points don't fit a clean global layout here, so the local field holds them only nearly");
        return field;
    };
    // Close poles of opposite turn are only a kink in the flow, and poles packed together crowd the layout: the field is
    // solved again turning only at the poles left once those are cancelled and spread. The symmetric field did this once
    // already, on its half.
    std::vector<glm::dvec3> qLayout = q;
    if ((pinned || opts.method == RemeshMethod::Patches) && !opts.poles && !usesSymmetricField(w, opts))
    {
        // Weaker pull to the curvature lets the field keep to the poles it is given instead of growing new ones beside them.
        FieldOptions budgeted = fo;
        budgeted.curvatureWeight *= 0.3;
        for (int round = 0; round < 3; ++round)
        {
            const std::vector<FieldSingularity> poles = fieldSingularities(s, qLayout);
            const std::vector<FieldSingularity> kept = spreadPoles(s, cancelClosePoles(s, poles, scale, 1.5), scale, 1.0);
            bool same = kept.size() == poles.size();
            for (size_t i = 0; i < kept.size() && same; ++i)
                same = kept[i].tri == poles[i].tri && kept[i].index == poles[i].index;
            if (same)
                break;
            PrescribedTwist pt = prescribeSingularities(s, kept);
            if (!pt.error.empty())
                break;
            budgeted.twist = std::move(pt.twist);
            qLayout = solveCrossField(s, budgeted);
        }
        Budget::note(std::to_string(fieldSingularities(s, qLayout).size()) + " poles for the layout");
    }
    QuadRemeshResult best;
    if (pinned)
        best = pinnedLayout(qLayout);
    else if (opts.method == RemeshMethod::Patches)
    {
        std::string why;
        if (const char* dump = std::getenv("LILA_FIELD_DUMP"))
            if (FILE* f = std::fopen(dump, "w"))
            {
                for (Index v = 0; v < s.vertexCount(); ++v)
                    std::fprintf(f, "%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", s.pos[v].x, s.pos[v].y, s.pos[v].z, s.normal[v].x,
                                 s.normal[v].y, s.normal[v].z, qLayout[v].x, qLayout[v].y, qLayout[v].z);
                std::fclose(f);
            }
        // Handles that change the size a lot get a cross of lines, so their patches take the size in their side counts.
        std::vector<glm::dvec3> crosses;
        for (const DensityHandle& handle : opts.densityHandles)
            if (std::abs(std::log(std::max(handle.scale, 1e-3))) > std::log(1.3))
                crosses.push_back(handle.position);
        // An edge length asked for sets the count, as it does for the field.
        const double half = symmetric ? 0.5 : 1.0;
        const double patchFaces = opts.targetEdgeLength > 0.0
                                      ? std::max(8.0, s.totalArea() * half / (opts.targetEdgeLength * opts.targetEdgeLength))
                                      : double(opts.targetFaces) * half;
        Extracted ex = layoutPatches(s, qLayout, scale, patchFaces, &why, crosses);
        clock.lap("patches");
        if (const char* raw = std::getenv("LILA_PATCH_RAW"); raw && ex.mesh.numFaces() > 0)
            dumpObj(ex.mesh, raw);
        if (ex.mesh.numFaces() > 0)
            best = record(buildFrom(std::move(ex), "patches"));
        else
            best.error = why;
        // Where the field's lines drift (long limbs) the layout can fold or crowd its poles; the layout traced in the
        // continuous seamless charts doesn't drift, and is kept when it is clearly cleaner. Its poles are the field's
        // own, unmerged, so elsewhere the field layout stays. It carries a density handle's size more weakly, so not there.
        if (opts.densityHandles.empty())
        {
            const int fieldFlaws = flawsOf(best);
            const Index fieldCrowding = best.ok() ? sharpCorners(best.mesh) + clusteredPoles(best.mesh) : 0;
            if (!best.ok() || fieldFlaws > 0 || fieldCrowding > kRescueCrowding)
            {
                std::string whySeamless;
                Extracted seamlessEx = layoutPatches(s, qLayout, scale, patchFaces, &whySeamless, crosses, true);
                if (seamlessEx.mesh.numFaces() > 0)
                {
                    QuadRemeshResult seamless = record(buildFrom(std::move(seamlessEx), "patches"));
                    const int seamlessFlaws = flawsOf(seamless);
                    const Index seamlessCrowding = seamless.ok() ? sharpCorners(seamless.mesh) + clusteredPoles(seamless.mesh) : 0;
                    const bool take = seamless.ok() && (!best.ok() || seamlessFlaws < fieldFlaws ||
                                                        (seamlessFlaws == fieldFlaws && double(seamlessCrowding) < 0.6 * double(fieldCrowding)));
                    if (std::getenv("LILA_PATCH_DEBUG"))
                        std::fprintf(stderr, "seamless patches: field %d flaws %d crowding, seamless %d flaws %d crowding, %s\n", fieldFlaws,
                                     int(fieldCrowding), seamlessFlaws, int(seamlessCrowding), take ? "seamless kept" : "field kept");
                    if (take)
                    {
                        result.notes.push_back("the patch layout traced along the flow came out folded or crowded, so it was traced in a seamless parametrization instead");
                        best = std::move(seamless);
                    }
                }
            }
        }
        // Where the patches don't come out (a torus with no poles) or come out with flaws, the local field when it is
        // cleaner, said so. A few flaws stay when the field would pay for them in poles all over.
        const int patchFlaws = flawsOf(best);
        if (!best.ok() || patchFlaws > 0)
        {
            QuadRemeshResult field = build("field", solvePositionField(s, q, scale));
            const bool fewFlaws = patchFlaws <= std::max(8, int(best.mesh.numFaces() / 100));
            if (field.ok() && (!best.ok() || (flawsOf(field) < patchFlaws && (!fewFlaws || field.metrics.irregular <= 3 * best.metrics.irregular / 2))))
            {
                result.notes.push_back(best.ok() ? "the patch layout came out with " + std::to_string(patchFlaws) +
                                                       (patchFlaws == 1 ? " flaw" : " flaws") + ", so each point is placed from the flow near it instead, with more poles"
                                                 : "the patch layout didn't work out for this shape (" + why + "), so each point is placed from the flow near it instead");
                best = std::move(field);
            }
        }
    }
    else
        best = build("field", solvePositionField(s, q, scale));
    // Where lattice points land against a tight spot is down to where the lattice happens to sit; a lattice shifted by
    // half a cell often fits where the first didn't, at the same density.
    int bestDefects = best.ok() ? int(defects(best.mesh, src, bvh, closed)) : 0;
    if (best.layout == "field" && !(best.ok() && bestDefects == 0))
        for (glm::dvec2 shift : {glm::dvec2(0.5, 0.0), glm::dvec2(0.0, 0.5), glm::dvec2(0.5, 0.5)})
        {
            QuadRemeshResult again = build("shifted", solvePositionField(s, q, scale, shift));
            clock.lap("shifted");
            const int againDefects = again.ok() ? int(defects(again.mesh, src, bvh, closed)) : 0;
            if (better(again, againDefects, best, bestDefects))
            {
                best = std::move(again);
                bestDefects = againDefects;
            }
            if (best.ok() && bestDefects == 0)
                break;
        }
    result.defects = best.ok() ? int(bestDefects) : 0;
    if (!best.ok())
    {
        result.error = best.error;
        return result;
    }
    result.mesh = std::move(best.mesh);
    result.metrics = best.metrics;
    result.layout = best.layout;
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
        count += n > 0 && m.faceArea(f) > 6.0 * around / double(n);
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

/** Face corners under 30 degrees: sheared quads that relaxing couldn't open. */
Index sharpCorners(const geo::Mesh& m)
{
    const double limit = std::cos(glm::radians(30.0));
    Index count = 0;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        const std::vector<Index> pts = m.facePoints(f);
        const size_t n = pts.size();
        for (size_t k = 0; k < n; ++k)
        {
            const glm::dvec3 p = m.position(pts[k]), a = m.position(pts[(k + n - 1) % n]) - p, b = m.position(pts[(k + 1) % n]) - p;
            const double la = glm::length(a), lb = glm::length(b);
            count += la > 0.0 && lb > 0.0 && glm::dot(a, b) / (la * lb) > limit;
        }
    }
    return count;
}

/** Inner poles with another inner pole within two edges: crowded poles pinch the quads between them. */
Index clusteredPoles(const geo::Mesh& m)
{
    auto pole = [&](Index p) { return m.pointValid(p) && !m.isBoundaryPoint(p) && m.pointValence(p) != 4; };
    Index count = 0;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!pole(p))
            continue;
        bool near = false;
        m.forEachPointEdge(p, [&](Index e) {
            const Index a = m.edgeOtherPoint(e, p);
            near = near || pole(a);
            m.forEachPointEdge(a, [&](Index e2) {
                const Index b = m.edgeOtherPoint(e2, a);
                near = near || (b != p && pole(b));
            });
        });
        count += near;
    }
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
    std::vector<glm::dvec3> q = crossField(w, opts, out.error);
    if (!out.ok())
        return out;
    for (const FieldSingularity& f : fieldSingularities(w.s, q))
    {
        const auto& tri = w.s.tris[f.tri];
        glm::dvec3 c = (w.s.pos[tri[0]] + w.s.pos[tri[1]] + w.s.pos[tri[2]]) / 3.0;
        out.poles.push_back({c, 4 - f.index});
        if (w.symmetric && std::abs(w.plane.distance(c)) > 1e-9)
        {
            c[opts.symmetryAxis] = -c[opts.symmetryAxis];
            out.poles.push_back({c, 4 - f.index});
        }
    }
    for (int total : shellIndexTotals(w.s).indexTotal)
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

/** The first clean layout, retrying finer for thin strips and tight spots, up to about four times the faces asked for. */
QuadRemeshResult cleanest(const geo::Mesh& source, const QuadRemeshOptions& opts, int& asked)
{
    auto started = std::chrono::steady_clock::now();
    QuadRemeshResult result = quadRemeshOnce(source, opts);
    double lastTook = secondsSince(started);
    asked = opts.targetFaces;
    if (source.numFaces() == 0)
        return result;
    // Pinned points tie the lattice to the source's own spacing; a finer retry would no longer meet them. Patches meet
    // the count they are given rather than folding where it is coarse, so they keep it once they come out at all.
    if (!opts.pinnedPoints.empty() || (result.ok() && (result.defects == 0 || opts.method == RemeshMethod::Patches)))
        return result;
    QuadRemeshOptions finer = opts;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        // A finer lattice takes longer; one that wouldn't finish in time gives way to the best result so far.
        if (result.ok() && (Budget::remaining() < 2.0 * lastTook || std::getenv("LILA_NO_FINER")))
            break;
        if (opts.targetEdgeLength > 0.0)
            finer.targetEdgeLength *= 0.85;
        else
            finer.targetFaces = int(std::lround(finer.targetFaces / (0.85 * 0.85)));
        started = std::chrono::steady_clock::now();
        QuadRemeshResult again = quadRemeshOnce(source, finer);
        lastTook = secondsSince(started);
        const bool take = better(again, again.defects, result, result.defects) || (again.ok() && again.defects == 0);
        if (take)
            asked = finer.targetFaces;
        keepOrMerge(result, std::move(again), take);
        if (take)
            result.notes = {"at " + std::to_string(opts.targetFaces) + " faces the layout folded at thin parts or tight spots, so it was made finer (" +
                            std::to_string(finer.targetFaces) + " asked)"};
        if (result.ok() && result.defects == 0)
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
        const bool take = r.ok() && r.defects <= result.defects && nonQuads(r) <= nonQuads(result) && off(r) < off(result);
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
        gridNote = "Grid takes an open sheet seen from one side (" + result.error + "), so this is laid out by patches";
        result = {};
        laid.method = RemeshMethod::Patches;
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
