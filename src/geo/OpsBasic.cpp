#include "geo/Ops.h"
#include "geo/OpsCommon.h"

#include "geo/Topology.h"
#include "geo/Triangulate.h"

#include "core/Profile.h"

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cmath>
#include <numeric>
#include <unordered_map>

namespace lila::geo
{
namespace detail
{
Index addFaceFrom(Mesh& mesh, std::span<const Index> points, Index srcFace, std::span<const Index> srcCorners,
                  std::span<const Index> srcEdges)
{
    const Index edgeCap = mesh.edgeCapacity();
    const Index f = mesh.addFace(points, srcFace, srcCorners);
    if (f == kInvalid || srcEdges.empty())
        return f;
    Index c = mesh.faceCorner(f);
    for (size_t i = 0; i < points.size(); ++i, c = mesh.cornerNext(c))
    {
        const Index e = mesh.cornerEdge(c);
        if (e >= edgeCap && i < srcEdges.size() && srcEdges[i] != kInvalid)
            mesh.edgeAttrs().copy(e, srcEdges[i]);
    }
    return f;
}

Index findFaceWithPoints(const Mesh& mesh, std::span<const Index> points)
{
    if (points.size() < 3)
        return kInvalid;
    const Index e = mesh.findEdge(points[0], points[1]);
    if (e == kInvalid)
        return kInvalid;
    Index found = kInvalid;
    mesh.forEachEdgeCorner(e, [&](Index c) {
        const Index f = mesh.cornerFace(c);
        if (found != kInvalid || mesh.faceSize(f) != points.size())
            return;
        bool all = true;
        for (Index p : points)
            all &= mesh.findCorner(f, p) != kInvalid;
        if (all)
            found = f;
    });
    return found;
}

void removeFacesClean(Mesh& mesh, std::span<const Index> faces)
{
    const std::vector<Index> edges = edgesOfFaces(mesh, faces);
    const std::vector<Index> points = pointsOfFaces(mesh, faces);
    for (Index f : faces)
        mesh.removeFace(f);
    mesh.removeLoose(edges, points);
}

bool pointFan(const Mesh& mesh, Index p, std::vector<Index>& corners, bool& closed)
{
    corners.clear();
    closed = false;
    std::vector<Index> all;
    mesh.forEachPointCorner(p, [&](Index c) { all.push_back(c); });
    if (all.empty())
        return false;
    Index start = all[0];
    for (Index c : all)
        if (!mesh.isManifoldEdge(mesh.cornerEdge(mesh.cornerPrev(c))))
        {
            start = c;
            break;
        }
    Index cur = start;
    for (;;)
    {
        corners.push_back(cur);
        const Index e = mesh.cornerEdge(cur);
        if (!mesh.isManifoldEdge(e))
            break;
        const Index r = mesh.cornerRadial(cur);
        if (mesh.cornerPoint(r) == p)
            return false;
        const Index next = mesh.cornerNext(r);
        if (next == start)
        {
            closed = true;
            break;
        }
        if (corners.size() > all.size())
            return false;
        cur = next;
    }
    return corners.size() == all.size();
}

Index acrossAtPoint(const Mesh& mesh, Index c, Index p)
{
    const Index e = mesh.cornerEdge(c);
    if (!mesh.isManifoldEdge(e))
        return kInvalid;
    const Index r = mesh.cornerRadial(c);
    return mesh.cornerPoint(r) == p ? r : mesh.cornerNext(r);
}

void mixCorners(Mesh& mesh, Index dst, std::span<const Index> srcs, std::span<const float> weights)
{
    mesh.cornerAttrs().mix(dst, srcs, weights);
}

FaceCopy copyFace(const Mesh& mesh, Index f)
{
    FaceCopy fc{f, {}, {}, {}};
    mesh.forEachFaceCorner(f, [&](Index c) {
        fc.points.push_back(mesh.cornerPoint(c));
        fc.corners.push_back(c);
        fc.edges.push_back(mesh.cornerEdge(c));
    });
    return fc;
}

bool polygonIsSimple(std::span<const glm::dvec2> p)
{
    const size_t n = p.size();
    auto cross = [](const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; };
    for (size_t i = 0; i < n; ++i)
    {
        const glm::dvec2 a = p[i], b = p[(i + 1) % n];
        if (glm::distance(a, b) <= 0.0)
            return false;
        for (size_t j = i + 2; j < n; ++j)
        {
            if (i == 0 && j == n - 1)
                continue;
            const glm::dvec2 c = p[j], d = p[(j + 1) % n];
            const double d1 = cross(b - a, c - a), d2 = cross(b - a, d - a);
            const double d3 = cross(d - c, a - c), d4 = cross(d - c, b - c);
            if (d1 == 0 && d2 == 0)
            {
                // Collinear: only overlap counts.
                const glm::dvec2 dir = b - a;
                const double t0 = glm::dot(c - a, dir), t1 = glm::dot(d - a, dir), len = glm::dot(dir, dir);
                if (std::max(t0, t1) > 0.0 && std::min(t0, t1) < len)
                    return false;
                continue;
            }
            if (((d1 > 0) != (d2 > 0) || d1 == 0 || d2 == 0) && ((d3 > 0) != (d4 > 0) || d3 == 0 || d4 == 0))
                return false;
        }
    }
    return true;
}
} // namespace detail

using namespace detail;

// Delete

void deleteFaces(Mesh& mesh, std::span<const Index> faces, const SymmetryMap* symmetry)
{
    LILA_ZONE();

    const std::vector<Index> sel = symmetry ? symmetry->expandFaces(mesh, faces) : uniqueSorted(faces);
    removeFacesClean(mesh, sel);
}

void deleteEdges(Mesh& mesh, std::span<const Index> edges, const SymmetryMap* symmetry)
{
    LILA_ZONE();

    const std::vector<Index> sel = symmetry ? symmetry->expandEdges(mesh, edges) : uniqueSorted(edges);
    const std::vector<Index> faces = facesOfEdges(mesh, sel);
    const std::vector<Index> nearEdges = edgesOfFaces(mesh, faces);
    std::vector<Index> nearPoints = pointsOfFaces(mesh, faces);
    const std::vector<Index> ends = pointsOfEdges(mesh, sel);
    nearPoints.insert(nearPoints.end(), ends.begin(), ends.end());
    for (Index f : faces)
        mesh.removeFace(f);
    for (Index e : sel)
        mesh.removeEdge(e);
    mesh.removeLoose(nearEdges, nearPoints);
}

void deletePoints(Mesh& mesh, std::span<const Index> points, const SymmetryMap* symmetry)
{
    LILA_ZONE();

    const std::vector<Index> sel = symmetry ? symmetry->expandPoints(points) : uniqueSorted(points);
    const std::vector<Index> faces = facesOfPoints(mesh, sel);
    const std::vector<Index> nearEdges = edgesOfFaces(mesh, faces);
    const std::vector<Index> nearPoints = pointsOfFaces(mesh, faces);
    for (Index p : sel)
        mesh.removePoint(p);
    mesh.removeLoose(nearEdges, nearPoints);
}

// Orientation

void flipNormals(Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    for (Index f : uniqueSorted(faces))
    {
        if (!mesh.faceValid(f))
            continue;
        mesh.flipFace(f);
        std::span<glm::vec3> n = mesh.cornerAttrs().get<glm::vec3>(attr::kNormal);
        if (!n.empty())
            mesh.forEachFaceCorner(f, [&](Index c) { n[c] = -n[c]; });
    }
}

void conformNormals(Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    std::vector<uint8_t> in(mesh.faceCapacity(), 0);
    if (faces.empty())
        for (Index f = 0; f < mesh.faceCapacity(); ++f)
            in[f] = mesh.faceValid(f);
    else
        for (Index f : faces)
            if (mesh.faceValid(f))
                in[f] = 1;

    std::vector<int8_t> flip(mesh.faceCapacity(), -1);
    std::vector<Index> comp, stack;
    for (Index seed = 0; seed < mesh.faceCapacity(); ++seed)
    {
        if (!in[seed] || flip[seed] >= 0)
            continue;
        comp.clear();
        flip[seed] = 0;
        stack.push_back(seed);
        bool closed = true;
        while (!stack.empty())
        {
            const Index f = stack.back();
            stack.pop_back();
            comp.push_back(f);
            mesh.forEachFaceCorner(f, [&](Index c) {
                const Index e = mesh.cornerEdge(c);
                if (!mesh.isManifoldEdge(e))
                {
                    closed = false;
                    return;
                }
                const Index r = mesh.cornerRadial(c);
                const Index g = mesh.cornerFace(r);
                if (!in[g])
                {
                    closed = false;
                    return;
                }
                if (flip[g] >= 0)
                    return;
                const bool sameDir = mesh.cornerPoint(r) == mesh.cornerPoint(c);
                flip[g] = int8_t(flip[f] ^ (sameDir ? 1 : 0));
                stack.push_back(g);
            });
        }

        bool invert = false;
        if (closed)
        {
            double vol = 0.0;
            for (Index f : comp)
            {
                const Index c0 = mesh.faceCorner(f);
                const glm::dvec3 a = mesh.position(mesh.cornerPoint(c0));
                double fv = 0.0;
                for (Index c = mesh.cornerNext(c0); mesh.cornerNext(c) != c0; c = mesh.cornerNext(c))
                    fv += glm::dot(a, glm::cross(mesh.position(mesh.cornerPoint(c)), mesh.position(mesh.cornerNextPoint(c))));
                vol += flip[f] ? -fv : fv;
            }
            invert = vol < 0.0;
        }
        else
        {
            size_t flipped = 0;
            for (Index f : comp)
                flipped += flip[f];
            invert = flipped * 2 > comp.size();
        }
        if (invert)
            for (Index f : comp)
                flip[f] ^= 1;
    }
    std::vector<Index> toFlip;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (in[f] && flip[f] == 1)
            toFlip.push_back(f);
    flipNormals(mesh, toFlip);
}

// Triangles and quads

std::vector<Index> triangulateFaces(Mesh& mesh, std::span<const Index> faces, const TriangulateOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> out;
    // Every face is split against the untouched mesh first, so the result matches what triangulate() displays.
    std::vector<Index> todo, tris, first;
    for (Index f : uniqueSorted(faces))
    {
        if (!mesh.faceValid(f))
            continue;
        if (mesh.faceSize(f) == 3)
        {
            out.push_back(f);
            continue;
        }
        todo.push_back(f);
        first.push_back(Index(tris.size()));
        triangulateFace(mesh, f, tris, opts);
    }
    first.push_back(Index(tris.size()));
    std::vector<Index> pts;
    for (size_t k = 0; k < todo.size(); ++k)
    {
        const std::span<const Index> corners = std::span(tris).subspan(first[k], first[k + 1] - first[k]);
        pts.resize(corners.size());
        for (size_t i = 0; i < corners.size(); ++i)
            pts[i] = mesh.cornerPoint(corners[i]);
        mesh.removeFace(todo[k]);
        for (size_t t = 0; t < corners.size(); t += 3)
        {
            const Index nf = mesh.addFace(std::span(pts).subspan(t, 3), todo[k], corners.subspan(t, 3));
            if (nf != kInvalid)
                out.push_back(nf);
        }
    }
    return out;
}

std::vector<Index> quadrangulateFaces(Mesh& mesh, std::span<const Index> faces, const QuadrangulateOptions& opts)
{
    LILA_ZONE();

    std::vector<uint8_t> in(mesh.faceCapacity(), 0);
    for (Index f : faces)
        if (mesh.faceValid(f) && mesh.faceSize(f) == 3)
            in[f] = 1;
    std::span<const bool> hard = std::as_const(mesh).edgeAttrs().get<bool>(attr::kHard);
    std::span<const bool> seam = std::as_const(mesh).edgeAttrs().get<bool>(attr::kSeam);
    std::span<const int32_t> mat = std::as_const(mesh).faceAttrs().get<int32_t>(attr::kMaterial);
    const double cosMax = std::cos(glm::radians(opts.maxFaceAngle));

    struct Candidate
    {
        double score;
        Index edge;
    };
    std::vector<Candidate> cands;
    for (Index e : edgesOfFaces(mesh, faces))
    {
        if (!mesh.isManifoldEdge(e))
            continue;
        const Index c1 = mesh.edgeCorner(e);
        const Index c2 = mesh.cornerRadial(c1);
        const Index f1 = mesh.cornerFace(c1), f2 = mesh.cornerFace(c2);
        if (!in[f1] || !in[f2] || mesh.cornerPoint(c1) == mesh.cornerPoint(c2))
            continue;
        if ((opts.keepHardEdges && !hard.empty() && hard[e]) || (opts.keepSeams && !seam.empty() && seam[e]))
            continue;
        if (opts.keepMaterials && !mat.empty() && mat[f1] != mat[f2])
            continue;
        const glm::dvec3 n1 = mesh.faceNormal(f1), n2 = mesh.faceNormal(f2);
        if (glm::dot(n1, n2) < cosMax)
            continue;
        // The quad joinFaces would make: q, x1, p, x2.
        const Index quad[4] = {mesh.cornerPoint(mesh.cornerNext(c1)), mesh.cornerPoint(mesh.cornerPrev(c1)),
                               mesh.cornerPoint(c1), mesh.cornerPoint(mesh.cornerPrev(c2))};
        const glm::dvec3 n = glm::normalize(n1 + n2);
        double score = 0.0;
        bool convex = true;
        for (int i = 0; i < 4; ++i)
        {
            const glm::dvec3 p = mesh.position(quad[i]);
            const glm::dvec3 a = mesh.position(quad[(i + 3) % 4]) - p;
            const glm::dvec3 b = mesh.position(quad[(i + 1) % 4]) - p;
            convex &= glm::dot(glm::cross(b, a), n) > 0.0;
            const double ang = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
            score += std::abs(glm::half_pi<double>() - ang);
        }
        if (convex)
            cands.push_back({score, e});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.score < b.score; });

    std::vector<uint8_t> used(mesh.faceCapacity(), 0);
    std::vector<Index> out;
    for (const Candidate& cand : cands)
    {
        if (!mesh.edgeValid(cand.edge) || !mesh.isManifoldEdge(cand.edge))
            continue;
        const Index c1 = mesh.edgeCorner(cand.edge);
        const Index f1 = mesh.cornerFace(c1), f2 = mesh.cornerFace(mesh.cornerRadial(c1));
        if (used[f1] || used[f2])
            continue;
        const Index f = mesh.joinFaces(cand.edge);
        if (f == kInvalid)
            continue;
        used[f1] = used[f2] = 1;
        out.push_back(f);
    }
    for (Index f : faces)
        if (in[f] && !used[f] && mesh.faceValid(f))
            out.push_back(f);
    return uniqueSorted(out);
}

std::vector<Index> pokeFaces(Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    std::vector<Index> out;
    for (Index f : uniqueSorted(faces))
    {
        if (!mesh.faceValid(f))
            continue;
        const FaceCopy fc = copyFace(mesh, f);
        const glm::dvec3 center = mesh.faceCentroid(f);
        const size_t n = fc.points.size();
        mesh.removeFace(f);
        const Index cp = mesh.addPoint(center);
        const std::vector<float> w(n, 1.0f / float(n));
        mesh.pointAttrs().mix(cp, fc.points, w);
        Index centerCorner = kInvalid;
        for (size_t i = 0; i < n; ++i)
        {
            const Index pts[3] = {fc.points[i], fc.points[(i + 1) % n], cp};
            const Index src[3] = {fc.corners[i], fc.corners[(i + 1) % n], centerCorner};
            const Index nf = mesh.addFace(pts, f, src);
            if (nf == kInvalid)
                continue;
            const Index cc = mesh.cornerPrev(mesh.faceCorner(nf));
            if (centerCorner == kInvalid)
            {
                mixCorners(mesh, cc, fc.corners, w);
                centerCorner = cc;
            }
            out.push_back(nf);
        }
    }
    return out;
}

std::vector<Index> spinEdges(Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    std::vector<Index> out;
    for (Index e : uniqueSorted(edges))
    {
        if (!mesh.edgeValid(e) || !mesh.isManifoldEdge(e))
            continue;
        const Index p = mesh.edgePoint(e, 0), q = mesh.edgePoint(e, 1);
        const Index f = mesh.joinFaces(e);
        if (f == kInvalid)
            continue;
        const Index cp = mesh.findCorner(f, p), cq = mesh.findCorner(f, q);
        const Index nf = mesh.splitFace(f, mesh.cornerNext(cp), mesh.cornerNext(cq));
        if (nf == kInvalid)
            continue;
        const Index ne = mesh.lastSplitEdge();
        mesh.edgeAttrs().copy(ne, e);
        out.push_back(ne);
    }
    return out;
}

std::vector<Index> fillHoles(Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    const std::vector<uint8_t> sel = makeMask(mesh.edgeCapacity(), edges);
    std::vector<Index> out;
    for (const EdgePath& loop : boundaryLoops(mesh))
    {
        if (!loop.closed || loop.points.size() < 3)
            continue;
        bool wanted = edges.empty();
        for (Index e : loop.edges)
            wanted |= sel[e] != 0;
        if (!wanted)
            continue;
        std::vector<Index> src(loop.points.size());
        for (size_t i = 0; i < loop.points.size(); ++i)
            src[i] = mesh.cornerNext(mesh.edgeCorner(loop.edges[i]));
        const Index f = mesh.addFace(loop.points, mesh.cornerFace(mesh.edgeCorner(loop.edges[0])), src);
        if (f != kInvalid)
            out.push_back(f);
    }
    return out;
}

// Merging

void weldPoints(Mesh& mesh, std::span<const Index> target)
{
    detail::weldPoints(mesh, target, nullptr);
}

void detail::weldPoints(Mesh& mesh, std::span<const Index> target, std::unordered_map<Index, Index>* rebuilt)
{
    LILA_ZONE();

    auto mapped = [&](Index p) { return p < target.size() && target[p] != kInvalid ? target[p] : p; };
    std::vector<Index> moving;
    for (Index p = 0; p < Index(target.size()); ++p)
        if (mesh.pointValid(p) && mapped(p) != p)
            moving.push_back(p);
    if (moving.empty())
        return;
    const std::vector<Index> faces = facesOfPoints(mesh, moving);
    const std::vector<Index> nearEdges = edgesOfFaces(mesh, faces);
    std::vector<FaceCopy> copies;
    copies.reserve(faces.size());
    for (Index f : faces)
        copies.push_back(copyFace(mesh, f));
    for (Index f : faces)
        mesh.removeFace(f);
    for (Index p : moving)
        mesh.removePoint(p);

    std::vector<Index> pts, corners, srcEdges;
    for (const FaceCopy& fc : copies)
    {
        pts.clear();
        corners.clear();
        srcEdges.clear();
        const size_t n = fc.points.size();
        // Collapse runs of corners that land on the same point; the run's last edge leads on.
        for (size_t i = 0; i < n; ++i)
        {
            const Index p = mapped(fc.points[i]);
            if (!pts.empty() && pts.back() == p)
            {
                srcEdges.back() = fc.edges[i];
                continue;
            }
            pts.push_back(p);
            corners.push_back(fc.corners[i]);
            srcEdges.push_back(fc.edges[i]);
        }
        while (pts.size() > 1 && pts.back() == pts.front())
        {
            pts.pop_back();
            corners.pop_back();
            srcEdges.pop_back();
        }
        const Index nf = pts.size() < 3 || findFaceWithPoints(mesh, pts) != kInvalid ? kInvalid
                                                                                    : addFaceFrom(mesh, pts, fc.face, corners, srcEdges);
        if (rebuilt)
            (*rebuilt)[fc.face] = nf;
    }
    std::vector<Index> survivors;
    for (Index p : moving)
        survivors.push_back(mapped(p));
    mesh.removeLoose(nearEdges, survivors);
}

namespace
{
struct UnionFind
{
    std::vector<Index> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), Index(0)); }
    Index find(Index i)
    {
        while (parent[i] != i)
            i = parent[i] = parent[parent[i]];
        return i;
    }
    void unite(Index a, Index b)
    {
        a = find(a);
        b = find(b);
        if (a != b)
            parent[std::max(a, b)] = std::min(a, b);
    }
};

struct WeldCellHash
{
    size_t operator()(const glm::i64vec3& c) const { return size_t(c.x * 73856093ll ^ c.y * 19349663ll ^ c.z * 83492791ll); }
};

/** Welds clusters, each into its lowest id, optionally moving it to the cluster average. */
std::vector<Index> weldClusters(Mesh& mesh, UnionFind& uf, std::span<const Index> members, bool average)
{
    std::vector<Index> target(mesh.pointCapacity(), kInvalid);
    std::unordered_map<Index, std::pair<glm::dvec3, int>> sums;
    for (Index p : members)
    {
        const Index r = uf.find(p);
        target[p] = r;
        auto& s = sums[r];
        s.first += mesh.position(p);
        ++s.second;
    }
    std::vector<Index> survivors;
    for (const auto& [r, s] : sums)
    {
        if (average)
            mesh.setPosition(r, s.first / double(s.second));
        survivors.push_back(r);
    }
    weldPoints(mesh, target);
    return uniqueSorted(survivors);
}
} // namespace

std::vector<Index> mergeByDistance(Mesh& mesh, std::span<const Index> points, const MergeOptions& opts)
{
    LILA_ZONE();

    std::vector<Index> sel;
    if (points.empty())
    {
        for (Index p = 0; p < mesh.pointCapacity(); ++p)
            if (mesh.pointValid(p))
                sel.push_back(p);
    }
    else
        sel = opts.symmetry ? opts.symmetry->expandPoints(points) : uniqueSorted(points);

    const double cell = std::max(opts.distance, 1e-12);
    const double d2max = opts.distance * opts.distance;
    std::unordered_map<glm::i64vec3, std::vector<Index>, WeldCellHash> grid;
    for (Index p : sel)
        grid[glm::i64vec3(glm::floor(mesh.position(p) / cell))].push_back(p);
    UnionFind uf(mesh.pointCapacity());
    for (Index p : sel)
    {
        const glm::i64vec3 k(glm::floor(mesh.position(p) / cell));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    auto it = grid.find(k + glm::i64vec3(dx, dy, dz));
                    if (it == grid.end())
                        continue;
                    for (Index q : it->second)
                    {
                        const glm::dvec3 d = mesh.position(q) - mesh.position(p);
                        if (q > p && glm::dot(d, d) <= d2max)
                            uf.unite(p, q);
                    }
                }
    }
    const Index firstNew = mesh.pointCapacity();
    std::vector<Index> out = weldClusters(mesh, uf, sel, opts.average);
    applySymmetrySnap(mesh, opts.symmetry, firstNew);
    return out;
}

Index mergeToCenter(Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    const std::vector<Index> sel = uniqueSorted(points);
    if (sel.empty())
        return kInvalid;
    UnionFind uf(mesh.pointCapacity());
    for (Index p : sel)
        uf.unite(sel[0], p);
    const std::vector<Index> out = weldClusters(mesh, uf, sel, true);
    return out.empty() ? kInvalid : out[0];
}

Index mergeToTarget(Mesh& mesh, std::span<const Index> points, Index target)
{
    LILA_ZONE();

    if (!mesh.pointValid(target))
        return kInvalid;
    std::vector<Index> map(mesh.pointCapacity(), kInvalid);
    for (Index p : points)
        if (mesh.pointValid(p))
            map[p] = target;
    map[target] = kInvalid;
    weldPoints(mesh, map);
    return target;
}

std::vector<Index> targetWeld(Mesh& mesh, std::span<const WeldPair> pairs, bool center)
{
    LILA_ZONE();

    std::vector<Index> map(mesh.pointCapacity(), kInvalid);
    std::vector<bool> isTarget(mesh.pointCapacity(), false);
    std::vector<Index> out;
    for (const WeldPair& w : pairs)
    {
        if (!mesh.pointValid(w.source) || !mesh.pointValid(w.target) || w.source == w.target || isTarget[w.source] ||
            map[w.source] != kInvalid || map[w.target] != kInvalid)
            continue;
        if (center)
            mesh.setPosition(w.target, (mesh.position(w.source) + mesh.position(w.target)) * 0.5);
        map[w.source] = w.target;
        isTarget[w.target] = true;
        out.push_back(w.target);
    }
    weldPoints(mesh, map);
    return uniqueSorted(out);
}

std::array<WeldPair, 2> edgeWeldPairs(const Mesh& mesh, Index source, Index target)
{
    const Index s0 = mesh.edgePoint(source, 0), s1 = mesh.edgePoint(source, 1);
    const Index t0 = mesh.edgePoint(target, 0), t1 = mesh.edgePoint(target, 1);
    auto d = [&](Index a, Index b) { return glm::distance(mesh.position(a), mesh.position(b)); };
    bool crossed = d(s0, t1) + d(s1, t0) < d(s0, t0) + d(s1, t1);
    if (s0 == t0 || s1 == t1)
        crossed = false;
    else if (s0 == t1 || s1 == t0)
        crossed = true;
    return crossed ? std::array{WeldPair{s0, t1}, WeldPair{s1, t0}} : std::array{WeldPair{s0, t0}, WeldPair{s1, t1}};
}

std::vector<Index> collapseEdges(Mesh& mesh, std::span<const Index> edges)
{
    LILA_ZONE();

    UnionFind uf(mesh.pointCapacity());
    for (Index e : edges)
        if (mesh.edgeValid(e))
            uf.unite(mesh.edgePoint(e, 0), mesh.edgePoint(e, 1));
    return weldClusters(mesh, uf, pointsOfEdges(mesh, edges), true);
}

// Dissolve

namespace
{
/** Removes a point that has exactly two edges, joining them. Returns the rebuilt faces. */
std::vector<Index> dissolveTwoValent(Mesh& mesh, Index p)
{
    std::vector<Index> edges;
    mesh.forEachPointEdge(p, [&](Index e) { edges.push_back(e); });
    if (edges.size() != 2)
        return {};
    std::vector<FaceCopy> copies;
    for (Index f : mesh.pointFaces(p))
        copies.push_back(copyFace(mesh, f));
    for (const FaceCopy& fc : copies)
        mesh.removeFace(fc.face);
    mesh.removePoint(p);
    std::vector<Index> out;
    for (FaceCopy fc : copies)
    {
        const auto it = std::find(fc.points.begin(), fc.points.end(), p);
        const size_t i = size_t(it - fc.points.begin());
        const size_t before = (i + fc.points.size() - 1) % fc.points.size();
        fc.points.erase(fc.points.begin() + i);
        fc.corners.erase(fc.corners.begin() + i);
        // The joined edge takes the attributes of the edge leading into p.
        const Index into = fc.edges[before];
        fc.edges.erase(fc.edges.begin() + i);
        fc.edges[i == 0 ? fc.edges.size() - 1 : before] = into;
        const Index nf = addFaceFrom(mesh, fc.points, fc.face, fc.corners, fc.edges);
        if (nf != kInvalid)
            out.push_back(nf);
    }
    return out;
}

/** Two edges at p, each with the same faces. */
bool isWingedPoint(const Mesh& mesh, Index p)
{
    std::vector<Index> edges;
    mesh.forEachPointEdge(p, [&](Index e) { edges.push_back(e); });
    if (edges.size() != 2)
        return false;
    std::vector<Index> a, b;
    mesh.forEachEdgeCorner(edges[0], [&](Index c) { a.push_back(mesh.cornerFace(c)); });
    mesh.forEachEdgeCorner(edges[1], [&](Index c) { b.push_back(mesh.cornerFace(c)); });
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    return !a.empty() && a == b;
}
} // namespace

std::vector<Index> dissolveEdges(Mesh& mesh, std::span<const Index> edges, bool clean)
{
    LILA_ZONE();

    std::vector<Index> faces;
    const std::vector<Index> ends = pointsOfEdges(mesh, edges);
    for (Index e : uniqueSorted(edges))
    {
        if (!mesh.edgeValid(e))
            continue;
        const Index f = mesh.joinFaces(e);
        if (f != kInvalid)
            faces.push_back(f);
    }
    if (clean)
        for (Index p : ends)
            if (mesh.pointValid(p) && isWingedPoint(mesh, p))
            {
                const std::vector<Index> rebuilt = dissolveTwoValent(mesh, p);
                faces.insert(faces.end(), rebuilt.begin(), rebuilt.end());
            }
    std::vector<Index> out;
    for (Index f : uniqueSorted(faces))
        if (mesh.faceValid(f))
            out.push_back(f);
    return out;
}

std::vector<Index> dissolvePoints(Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    std::vector<Index> out;
    std::vector<Index> fan;
    for (Index p : uniqueSorted(points))
    {
        if (!mesh.pointValid(p))
            continue;
        if (mesh.pointValence(p) == 2)
        {
            const std::vector<Index> rebuilt = dissolveTwoValent(mesh, p);
            out.insert(out.end(), rebuilt.begin(), rebuilt.end());
            continue;
        }
        bool closed = false;
        if (!pointFan(mesh, p, fan, closed) || fan.size() < 2)
            continue;

        // Chains of each face without p, walked from the last fan face back to the first.
        std::vector<Index> pts, corners, edges;
        for (size_t k = fan.size(); k-- > 0;)
        {
            const Index c = fan[k];
            bool first = true;
            for (Index x = mesh.cornerNext(c); x != c; x = mesh.cornerNext(x))
            {
                if (first && k + 1 != fan.size())
                {
                    first = false;
                    continue;
                }
                first = false;
                pts.push_back(mesh.cornerPoint(x));
                corners.push_back(x);
                edges.push_back(mesh.cornerEdge(x));
            }
        }
        if (closed)
        {
            pts.pop_back();
            corners.pop_back();
            edges.pop_back();
        }
        std::vector<Index> sorted = pts;
        std::sort(sorted.begin(), sorted.end());
        if (pts.size() < 3 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
            continue;
        const Index srcFace = mesh.cornerFace(fan[0]);
        std::vector<Index> fanFaces;
        for (Index c : fan)
            fanFaces.push_back(mesh.cornerFace(c));
        for (Index f : fanFaces)
            mesh.removeFace(f);
        mesh.removePoint(p);
        const Index nf = addFaceFrom(mesh, pts, srcFace, corners, edges);
        if (nf != kInvalid)
            out.push_back(nf);
    }
    return out;
}

std::vector<Index> subdivideEdges(Mesh& mesh, std::span<const Index> edges, int count)
{
    LILA_ZONE();

    std::vector<Index> out;
    for (Index e : uniqueSorted(edges))
    {
        if (!mesh.edgeValid(e))
            continue;
        Index cur = e;
        for (int i = 0; i < count; ++i)
        {
            out.push_back(mesh.splitEdge(cur, 1.0 / double(count + 1 - i)));
            cur = mesh.lastSplitEdge();
        }
    }
    return out;
}

// Whole-mesh

std::vector<Index> connectPoints(Mesh& mesh, std::span<const Index> points)
{
    LILA_ZONE();

    std::vector<uint8_t> chosen = makeMask(mesh.pointCapacity(), points);
    std::vector<Index> work = facesOfPoints(mesh, points), out;
    while (!work.empty())
    {
        Index f = work.back();
        work.pop_back();
        if (!mesh.faceValid(f))
            continue;
        std::vector<Index> corners;
        for (Index c : mesh.faceCorners(f))
            if (chosen[mesh.cornerPoint(c)])
                corners.push_back(c);
        if (corners.size() < 2)
            continue;
        for (size_t i = 0; i < corners.size(); ++i)
        {
            Index a = corners[i], b = corners[(i + 1) % corners.size()];
            if (a == b || mesh.cornerNext(a) == b || mesh.cornerPrev(a) == b ||
                mesh.findEdge(mesh.cornerPoint(a), mesh.cornerPoint(b)) != kInvalid)
                continue;
            Index g = mesh.splitFace(f, a, b);
            if (g == kInvalid)
                continue;
            out.push_back(mesh.lastSplitEdge());
            // Both halves may still hold another unjoined pair.
            work.push_back(f);
            work.push_back(g);
            break;
        }
    }
    return out;
}

Mesh extractFaces(const Mesh& mesh, std::span<const Index> faces)
{
    LILA_ZONE();

    Mesh out;
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        out.attributes(Domain(d)).mergeLayout(mesh.attributes(Domain(d)));
    out.materialNames() = mesh.materialNames();
    out.colorSetInfo() = mesh.colorSetInfo();
    std::vector<Index> pmap(mesh.pointCapacity(), kInvalid);
    for (Index p : pointsOfFaces(mesh, faces))
    {
        pmap[p] = out.addPoint(mesh.position(p));
        out.pointAttrs().copyFrom(mesh.pointAttrs(), pmap[p], p);
    }
    std::vector<Index> pts;
    for (Index f : uniqueSorted(faces))
    {
        if (!mesh.faceValid(f))
            continue;
        pts.clear();
        mesh.forEachFaceCorner(f, [&](Index c) { pts.push_back(pmap[mesh.cornerPoint(c)]); });
        const Index edgeCap = out.edgeCapacity();
        const Index nf = out.addFace(pts);
        if (nf == kInvalid)
            continue;
        out.faceAttrs().copyFrom(mesh.faceAttrs(), nf, f);
        Index c = out.faceCorner(nf);
        mesh.forEachFaceCorner(f, [&](Index oc) {
            out.cornerAttrs().copyFrom(mesh.cornerAttrs(), c, oc);
            const Index e = out.cornerEdge(c);
            if (e >= edgeCap)
                out.edgeAttrs().copyFrom(mesh.edgeAttrs(), e, mesh.cornerEdge(oc));
            c = out.cornerNext(c);
        });
    }
    return out;
}

Mesh combine(std::span<const Mesh* const> meshes)
{
    LILA_ZONE();

    Mesh out;
    for (const Mesh* m : meshes)
        if (m)
            out.append(*m);
    return out;
}

std::vector<Mesh> separate(const Mesh& mesh)
{
    LILA_ZONE();

    std::vector<Index> comp;
    const Index count = faceComponents(mesh, comp);
    std::vector<std::vector<Index>> groups(count);
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (comp[f] != kInvalid)
            groups[comp[f]].push_back(f);
    std::vector<Mesh> out;
    out.reserve(count);
    for (const std::vector<Index>& g : groups)
        out.push_back(extractFaces(mesh, g));
    return out;
}

namespace
{
/** The face's on-plane corners where its outline passes from one side of the plane to the other. */
template <class Side> std::vector<Index> planeCrossings(const Mesh& m, Index face, const Side& side)
{
    std::vector<Index> out;
    if (!m.faceValid(face))
        return out;
    m.forEachFaceCorner(face, [&](Index c) {
        if (side(m.cornerPoint(c)) == 0 && side(m.cornerPoint(m.cornerPrev(c))) * side(m.cornerNextPoint(c)) < 0)
            out.push_back(c);
    });
    return out;
}
} // namespace

PlaneCut cutAlongPlane(Mesh& m, const glm::dvec3& origin, const glm::dvec3& normal, double tol, const std::function<bool(const glm::dvec3&)>& within,
                       std::span<const Index> faces)
{
    LILA_ZONE();

    PlaneCut out;
    const double len = glm::length(normal);
    if (len <= 0.0)
        return out;
    const glm::dvec3 n = normal / len;
    auto dist = [&](const glm::dvec3& x) { return glm::dot(n, x - origin); };
    auto inside = [&](const glm::dvec3& x) { return !within || within(x - dist(x) * n); };
    auto onPlane = [&](Index p) {
        const glm::dvec3& x = m.position(p);
        return std::abs(dist(x)) <= tol && inside(x);
    };
    auto side = [&](Index p) { return onPlane(p) ? 0 : dist(m.position(p)) >= 0.0 ? 1 : -1; };

    std::vector<Index> todo(faces.begin(), faces.end());
    if (faces.empty())
        for (Index f = 0; f < m.faceCapacity(); ++f)
            if (m.faceValid(f))
                todo.push_back(f);
    std::vector<uint8_t> edgeIn(m.edgeCapacity(), 0);
    for (Index f : todo)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                edgeIn[m.cornerEdge(c)] = 1;
                if (onPlane(m.cornerPoint(c)))
                    m.setPosition(m.cornerPoint(c), m.position(m.cornerPoint(c)) - dist(m.position(m.cornerPoint(c))) * n);
            });

    for (Index e = 0; e < Index(edgeIn.size()); ++e)
    {
        if (!edgeIn[e] || !m.edgeValid(e))
            continue;
        const glm::dvec3 a = m.position(m.edgePoint(e, 0)), b = m.position(m.edgePoint(e, 1));
        const double da = dist(a), db = dist(b);
        if (!((da > tol && db < -tol) || (da < -tol && db > tol)))
            continue;
        const double t = da / (da - db);
        const glm::dvec3 x = a + (b - a) * t;
        if (!inside(x))
            continue;
        const Index mid = m.splitEdge(e, t);
        m.setPosition(mid, x - dist(x) * n);
    }

    // Each sign change round a face now passes through an on-plane point: split between consecutive ones. A concave face
    // can cross more than twice (an arch's frame); its crossings pair up in order along the cut line instead, as walking
    // round it would pair two that the cut only joins through the hole.
    std::vector<Index> concave;
    for (Index f : todo)
        if (planeCrossings(m, f, side).size() > 2)
            concave.push_back(f);
    while (!concave.empty())
    {
        const Index face = concave.back();
        concave.pop_back();
        std::vector<Index> cs = planeCrossings(m, face, side);
        if (cs.size() < 2)
            continue;
        const glm::dvec3 along = glm::cross(n, m.faceNormal(face));
        std::ranges::sort(cs, [&](Index a, Index b) { return glm::dot(m.position(m.cornerPoint(a)), along) < glm::dot(m.position(m.cornerPoint(b)), along); });
        if (m.cornerNext(cs[0]) == cs[1] || m.cornerPrev(cs[0]) == cs[1])
            continue;
        const Index rest = m.splitFace(face, cs[0], cs[1]);
        if (rest == kInvalid)
            continue;
        out.newFaces.push_back(rest);
        concave.push_back(face);
        concave.push_back(rest);
    }
    for (Index f : todo)
    {
        Index face = f;
        while (m.faceValid(face))
        {
            bool pos = false, neg = false;
            Index start = kInvalid;
            m.forEachFaceCorner(face, [&](Index c) {
                const int sd = side(m.cornerPoint(c));
                pos |= sd > 0;
                neg |= sd < 0;
                if (start == kInvalid && sd == 0 && side(m.cornerNextPoint(c)) > 0)
                    start = c;
            });
            if (!pos || !neg || start == kInvalid)
                break;
            Index end = m.cornerNext(start);
            while (side(m.cornerPoint(end)) != 0)
                end = m.cornerNext(end);
            if (end == start || m.cornerNext(start) == end || m.cornerPrev(start) == end)
                break;
            const Index rest = m.splitFace(face, start, end);
            if (rest == kInvalid)
                break;
            out.newFaces.push_back(rest);
            face = rest;
        }
    }

    for (Index f : todo)
        out.newFaces.push_back(f);
    for (Index f : out.newFaces)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                const Index e = m.cornerEdge(c);
                if (onPlane(m.edgePoint(e, 0)) && onPlane(m.edgePoint(e, 1)) && std::ranges::find(out.edges, e) == out.edges.end())
                    out.edges.push_back(e);
            });
    out.newFaces.resize(out.newFaces.size() - todo.size());
    return out;
}

void cutAtPlane(Mesh& mesh, const SymmetryPlane& plane, int keepSign, double tolerance, const glm::dmat4& space)
{
    LILA_ZONE();

    const bool identity = space == glm::dmat4(1.0);
    const glm::dmat4 inv = identity ? space : glm::inverse(space);
    auto toPlane = [&](const glm::dvec3& p) { return identity ? p : glm::dvec3(space * glm::dvec4(p, 1.0)); };
    auto fromPlane = [&](const glm::dvec3& p) { return identity ? p : glm::dvec3(inv * glm::dvec4(p, 1.0)); };
    auto distance = [&](Index p) { return plane.distance(toPlane(mesh.position(p))); };
    auto side = [&](Index p) {
        const double d = distance(p);
        return d > tolerance ? 1 : d < -tolerance ? -1 : 0;
    };

    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && side(p) == 0)
            mesh.setPosition(p, fromPlane(plane.project(toPlane(mesh.position(p)))));

    const Index edgeCap = mesh.edgeCapacity();
    for (Index e = 0; e < edgeCap; ++e)
    {
        if (!mesh.edgeValid(e))
            continue;
        const Index a = mesh.edgePoint(e, 0);
        const Index b = mesh.edgePoint(e, 1);
        if (side(a) * side(b) >= 0)
            continue;
        const double da = distance(a);
        const double db = distance(b);
        const Index m = mesh.splitEdge(e, da / (da - db));
        mesh.setPosition(m, fromPlane(plane.project(toPlane(mesh.position(m)))));
    }

    // Every sign change round a face now passes through an on-plane point, so each positive run is bounded by two.
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        Index face = f;
        while (mesh.faceValid(face))
        {
            bool pos = false;
            bool neg = false;
            Index start = kInvalid;
            mesh.forEachFaceCorner(face, [&](Index c) {
                const int s = side(mesh.cornerPoint(c));
                pos |= s > 0;
                neg |= s < 0;
                if (start == kInvalid && s == 0 && side(mesh.cornerNextPoint(c)) > 0)
                    start = c;
            });
            if (!pos || !neg || start == kInvalid)
                break;
            Index end = mesh.cornerNext(start);
            while (side(mesh.cornerPoint(end)) != 0)
                end = mesh.cornerNext(end);
            const Index rest = mesh.splitFace(face, start, end);
            if (rest == kInvalid)
                break;
            face = rest;
        }
    }

    std::vector<Index> drop;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
    {
        if (!mesh.faceValid(f))
            continue;
        bool kept = false;
        bool discarded = false;
        mesh.forEachFaceCorner(f, [&](Index c) {
            const int s = side(mesh.cornerPoint(c));
            kept |= s == keepSign;
            discarded |= s == -keepSign;
        });
        if (discarded || !kept)
            drop.push_back(f);
    }
    for (Index f : drop)
        mesh.removeFace(f);
    mesh.removeAllLoose();
}

std::vector<Index> mirrorMesh(Mesh& mesh, const MirrorOptions& opts)
{
    LILA_ZONE();

    SymmetryPlane plane = opts.plane;
    const double len = glm::length(plane.normal);
    plane.normal /= len;
    plane.offset /= len;
    if (opts.direction != MirrorDirection::Both)
        cutAtPlane(mesh, plane, opts.direction == MirrorDirection::Positive ? 1 : -1, opts.tolerance, opts.space);

    // Off identity the reflection is the plane-space one carried back into mesh space.
    const bool identity = opts.space == glm::dmat4(1.0);
    const glm::dmat4 inv = identity ? opts.space : glm::inverse(opts.space);
    glm::dmat4 reflect(1.0);
    if (!identity)
    {
        glm::dmat4 r(1.0);
        for (int col = 0; col < 3; ++col)
            for (int row = 0; row < 3; ++row)
                r[col][row] -= 2.0 * plane.normal[row] * plane.normal[col];
        r[3] = glm::dvec4(2.0 * plane.offset * plane.normal, 1.0);
        reflect = inv * r * opts.space;
    }
    const glm::mat3 linear = glm::mat3(glm::dmat3(reflect));
    const glm::mat3 normalXform = glm::transpose(glm::inverse(linear));
    auto inPlane = [&](const glm::dvec3& p) { return identity ? p : glm::dvec3(opts.space * glm::dvec4(p, 1.0)); };

    std::vector<Index> live;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            live.push_back(f);
    Mesh copy = extractFaces(mesh, live);
    for (glm::dvec3& p : copy.positions())
        p = identity ? plane.reflect(p) : glm::dvec3(reflect * glm::dvec4(p, 1.0));
    for (Index f = 0; f < copy.faceCapacity(); ++f)
        copy.flipFace(f);
    const glm::vec3 n(plane.normal);
    if (std::span<glm::vec3> cn = copy.cornerAttrs().get<glm::vec3>(attr::kNormal); !cn.empty())
        for (glm::vec3& v : cn)
            v = identity ? v - 2.0f * glm::dot(v, n) * n : glm::normalize(normalXform * v);
    if (std::span<glm::vec4> ct = copy.cornerAttrs().get<glm::vec4>(attr::kTangent); !ct.empty())
        for (glm::vec4& v : ct)
        {
            glm::vec3 t(v);
            t = identity ? t - 2.0f * glm::dot(t, n) * n : glm::normalize(linear * t);
            v = glm::vec4(t, -v.w);
        }

    // extractFaces keeps the original point order, so copy point i matches the i-th used point.
    const std::vector<Index> used = pointsOfFaces(mesh, live);
    const Index firstFace = mesh.faceCapacity();
    const Index offset = mesh.append(copy);
    std::vector<Index> out;
    for (Index f = firstFace; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            out.push_back(f);

    if (opts.weld)
    {
        std::vector<Index> target(mesh.pointCapacity(), kInvalid);
        for (size_t i = 0; i < used.size(); ++i)
        {
            const Index p = used[i];
            const glm::dvec3 at = inPlane(mesh.position(p));
            if (std::abs(plane.distance(at)) <= opts.tolerance)
            {
                mesh.setPosition(p, identity ? plane.project(at) : glm::dvec3(inv * glm::dvec4(plane.project(at), 1.0)));
                target[offset + Index(i)] = p;
            }
        }
        weldPoints(mesh, target);
        std::vector<Index> alive;
        for (Index f : out)
            if (mesh.faceValid(f))
                alive.push_back(f);
        // Welding rebuilds faces on the seam with new ids.
        for (Index f = firstFace; f < mesh.faceCapacity(); ++f)
            if (mesh.faceValid(f) && std::find(alive.begin(), alive.end(), f) == alive.end())
                alive.push_back(f);
        out = uniqueSorted(alive);
    }
    return out;
}

SymmetryMap symmetrizeTopology(Mesh& mesh, int axis, int keepSign, double tolerance, double offset)
{
    LILA_ZONE();

    axis = std::clamp(axis, 0, 2);
    MirrorOptions o;
    o.plane = axisPlane(axis, offset);
    o.direction = keepSign < 0 ? MirrorDirection::Negative : MirrorDirection::Positive;
    o.tolerance = tolerance;
    mirrorMesh(mesh, o);
    // The mirrored half carries copies of the kept half's keys, which pair nothing.
    removeSymmetryLayers(mesh);
    mesh.compact();
    SymmetryMap map = SymmetryMap::build(mesh, o.plane, tolerance);
    map.store(mesh, axis);
    return map;
}

namespace
{
struct KeepSets
{
    std::vector<Index> parent;

    explicit KeepSets(Index n) : parent(n)
    {
        std::iota(parent.begin(), parent.end(), Index(0));
    }
    Index find(Index p)
    {
        while (parent[p] != p)
            p = parent[p] = parent[parent[p]];
        return p;
    }
    /** keep's root survives. */
    void unite(Index keep, Index other)
    {
        const Index a = find(keep), b = find(other);
        if (a != b)
            parent[b] = a;
    }
};

/** wedge carried by element g of radial: positions, corner normals and tangents, and winding when g reflects. */
Mesh carried(const Mesh& wedge, const RadialSymmetry& radial, int g)
{
    Mesh copy = wedge;
    if (g == 0)
        return copy;
    for (glm::dvec3& p : copy.positions())
        p = radial.apply(g, p);
    const glm::mat3 linear(radial.linear(g));
    if (std::span<glm::vec3> cn = copy.cornerAttrs().get<glm::vec3>(attr::kNormal); !cn.empty())
        for (glm::vec3& c : cn)
            c = linear * c;
    if (std::span<glm::vec4> ct = copy.cornerAttrs().get<glm::vec4>(attr::kTangent); !ct.empty())
        for (glm::vec4& c : ct)
            c = glm::vec4(linear * glm::vec3(c), radial.reflects(g) ? -c.w : c.w);
    if (radial.reflects(g))
        for (Index f = 0; f < copy.faceCapacity(); ++f)
            copy.flipFace(f);
    return copy;
}

/**
 * Pairs the fundamental wedge's points on its far boundary with those on its near one, turned back by a wedge, nearest
 * first. Empty when the two seams hold different numbers of points.
 */
std::vector<std::pair<Index, Index>> wedgeSeamPairs(const Mesh& wedge, const RadialSymmetry& radial, double tolerance)
{
    const glm::dvec3 a = radial.axis(), u = radial.reference(), v = glm::cross(a, u), o = radial.origin();
    const double t = radial.wedgeAngle();
    const glm::dvec3 far = std::cos(t) * u + std::sin(t) * v;
    std::vector<Index> nearSeam, farSeam;
    for (Index p = 0; p < wedge.pointCapacity(); ++p)
    {
        if (!wedge.pointValid(p))
            continue;
        const glm::dvec3 d = wedge.position(p) - o;
        if (std::abs(glm::dot(d, glm::cross(a, u))) <= tolerance && glm::dot(d, u) >= -tolerance)
            nearSeam.push_back(p);
        if (std::abs(glm::dot(d, glm::cross(a, far))) <= tolerance && glm::dot(d, far) >= -tolerance)
            farSeam.push_back(p);
    }
    std::vector<std::pair<Index, Index>> out;
    if (nearSeam.size() != farSeam.size() || nearSeam.size() > 4096)
        return out;
    struct Candidate
    {
        double d;
        size_t i, j;
    };
    std::vector<Candidate> all;
    const int back = radial.inverse(1);
    for (size_t i = 0; i < farSeam.size(); ++i)
    {
        const glm::dvec3 at = radial.apply(back, wedge.position(farSeam[i]));
        for (size_t j = 0; j < nearSeam.size(); ++j)
            all.push_back({glm::length(at - wedge.position(nearSeam[j])), i, j});
    }
    std::sort(all.begin(), all.end(), [](const Candidate& x, const Candidate& y) { return x.d < y.d; });
    std::vector<uint8_t> usedFar(farSeam.size(), 0), usedNear(nearSeam.size(), 0);
    for (const Candidate& c : all)
        if (!usedFar[c.i] && !usedNear[c.j])
        {
            usedFar[c.i] = usedNear[c.j] = 1;
            out.push_back({farSeam[c.i], nearSeam[c.j]});
        }
    return out;
}
} // namespace

void cutToWedge(Mesh& mesh, const RadialSymmetry& radial, int keep, double tolerance)
{
    LILA_ZONE();

    if (radial.order() < 2)
        return;
    // The fundamental wedge runs from the reference direction to wedgeAngle past it: in front of the first plane,
    // behind the second. keep's wedge is bounded by those planes carried by keep.
    const glm::dvec3 a = radial.axis(), u = radial.reference(), v = glm::cross(a, u);
    const double t = radial.wedgeAngle();
    const glm::dvec3 bounds[2] = {glm::cross(a, u), glm::cross(a, std::cos(t) * u + std::sin(t) * v)};
    for (int k = 0; k < 2; ++k)
    {
        SymmetryPlane plane;
        plane.normal = glm::normalize(radial.applyVector(std::clamp(keep, 0, radial.order() - 1), bounds[k]));
        plane.offset = glm::dot(plane.normal, radial.origin());
        cutAtPlane(mesh, plane, k == 0 ? 1 : -1, tolerance);
    }
}

SymmetryMap symmetrizeRadialTopology(Mesh& mesh, const RadialSymmetry& radial, int keep, double tolerance)
{
    LILA_ZONE();

    const int n = radial.order();
    keep = std::clamp(keep, 0, n - 1);
    removeSymmetryLayers(mesh);
    cutToWedge(mesh, radial, keep, tolerance);

    std::vector<Index> live;
    for (Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            live.push_back(f);
    // The kept wedge carried back onto the fundamental one, then into every slot.
    const Mesh wedge = carried(extractFaces(mesh, live), radial, radial.inverse(keep));
    mesh = Mesh();
    std::vector<Index> offsets;
    for (int g = 0; g < n; ++g)
        offsets.push_back(mesh.append(carried(wedge, radial, g)));

    KeepSets sets(mesh.pointCapacity());
    // Turns alone meet a copy's far seam against the next copy's near one, which need not lie at the same spots: pair
    // them up, the near seam's points surviving. Reflections meet each seam against its own mirror image.
    if (!radial.mirror() && radial.count() > 1)
        for (auto [far, nearPoint] : wedgeSeamPairs(wedge, radial, tolerance))
            for (int g = 0; g < n; ++g)
                sets.unite(offsets[size_t(radial.compose(1, g))] + nearPoint, offsets[size_t(g)] + far);

    // Whatever else coincides (the axis, a mirror seam) merges into the first point there.
    const double cell = std::max(tolerance * 2.0, 1e-9);
    auto key = [&](const glm::dvec3& p) { return glm::i64vec3(glm::floor(p / cell)); };
    struct Hash
    {
        size_t operator()(const glm::i64vec3& c) const { return size_t(c.x * 73856093ll ^ c.y * 19349663ll ^ c.z * 83492791ll); }
    };
    std::unordered_map<glm::i64vec3, std::vector<Index>, Hash> grid;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
    {
        if (!mesh.pointValid(p))
            continue;
        const glm::dvec3 at = mesh.position(p);
        const glm::i64vec3 k = key(at);
        Index found = kInvalid;
        for (int dz = -1; dz <= 1 && found == kInvalid; ++dz)
            for (int dy = -1; dy <= 1 && found == kInvalid; ++dy)
                for (int dx = -1; dx <= 1 && found == kInvalid; ++dx)
                    if (auto it = grid.find(k + glm::i64vec3(dx, dy, dz)); it != grid.end())
                        for (Index q : it->second)
                            if (glm::length(mesh.position(q) - at) <= tolerance)
                            {
                                found = q;
                                break;
                            }
        if (found != kInvalid)
            sets.unite(found, p);
        else
            grid[k].push_back(p);
    }
    std::vector<Index> target(mesh.pointCapacity(), kInvalid);
    bool any = false;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && sets.find(p) != p)
        {
            target[p] = sets.find(p);
            any = true;
        }
    if (any)
        weldPoints(mesh, target);
    mesh.removeAllLoose();
    mesh.compact();

    SymmetryMap map = SymmetryMap::buildRadial(mesh, radial, tolerance);
    std::vector<Index> points;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        points.push_back(p);
    map.snapToPlane(mesh, points);
    map.symmetrizeWedge(mesh, 0);
    map.store(mesh);
    return map;
}
} // namespace lila::geo
