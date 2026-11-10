#include "retopo/Construct.h"

#include "core/Profile.h"
#include "geo/Ops.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <map>
#include <limits>

namespace lila::retopo
{
namespace
{
constexpr std::array<const char*, 7> kKindNames = {"patch", "strip", "stroke", "bridge", "sweep", "cylinder", "region"};

/** The existing point within weld of p, else a new one. */
Index pointNear(geo::Mesh& m, const glm::dvec3& p, double weld)
{
    Index best = geo::kInvalid;
    double bestD = weld * weld;
    for (Index q = 0; q < m.pointCapacity(); ++q)
        if (m.pointValid(q))
        {
            glm::dvec3 d = m.position(q) - p;
            if (double d2 = glm::dot(d, d); d2 <= bestD)
            {
                bestD = d2;
                best = q;
            }
        }
    return best != geo::kInvalid ? best : m.addPoint(p);
}

/** Adds a face wound against the faces it shares edges with; kInvalid when it exists or would make a non-manifold edge. */
Index addOriented(geo::Mesh& m, std::vector<Index> pts)
{
    std::vector<Index> sorted = pts;
    std::sort(sorted.begin(), sorted.end());
    if (pts.size() < 3 || std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end())
        return geo::kInvalid;
    bool sameWay = false;
    for (size_t k = 0; k < pts.size(); ++k)
    {
        Index a = pts[k], b = pts[(k + 1) % pts.size()];
        Index e = m.findEdge(a, b);
        if (e == geo::kInvalid)
            continue;
        if (m.edgeFaceCount(e) >= 2)
            return geo::kInvalid;
        m.forEachEdgeCorner(e, [&](Index c) { sameWay |= m.cornerPoint(c) == a; });
    }
    if (sameWay)
        std::reverse(pts.begin(), pts.end());
    if (Index e0 = m.findEdge(pts[0], pts[1]); e0 != geo::kInvalid)
    {
        bool exists = false;
        m.forEachEdgeCorner(e0, [&](Index c) {
            std::vector<Index> fp = m.facePoints(m.cornerFace(c));
            std::sort(fp.begin(), fp.end());
            exists |= fp == sorted;
        });
        if (exists)
            return geo::kInvalid;
    }
    return m.addFace(pts);
}
/** Moves the generated border points (of the faces in use) onto open border points of the mesh within snap, one each. */
void snapToBorder(const geo::Mesh& mesh, const GenResult& gen, const std::vector<uint8_t>& used, std::vector<glm::dvec3>& pos, double snap)
{
    std::map<std::pair<Index, Index>, int> edgeUse;
    for (const std::vector<Index>& face : gen.faces)
        for (size_t k = 0; k < face.size(); ++k)
        {
            Index a = face[k], b = face[(k + 1) % face.size()];
            ++edgeUse[{std::min(a, b), std::max(a, b)}];
        }
    std::vector<uint8_t> genBorder(pos.size(), 0);
    for (const auto& [e, n] : edgeUse)
        if (n == 1)
            genBorder[e.first] = genBorder[e.second] = 1;
    std::vector<Index> meshBorder;
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p) && mesh.isBoundaryPoint(p))
            meshBorder.push_back(p);
    struct Pair
    {
        double d;
        size_t gen;
        Index mesh;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < pos.size(); ++i)
        if (used[i] && genBorder[i] && !(i < gen.fixed.size() && gen.fixed[i]))
            for (Index p : meshBorder)
                if (double d = glm::length(mesh.position(p) - pos[i]); d < snap)
                    pairs.push_back({d, i, p});
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) { return a.d < b.d; });
    std::vector<uint8_t> genTaken(pos.size(), 0);
    std::set<Index> meshTaken;
    for (const Pair& pr : pairs)
        if (!genTaken[pr.gen] && !meshTaken.contains(pr.mesh))
        {
            genTaken[pr.gen] = 1;
            meshTaken.insert(pr.mesh);
            pos[pr.gen] = mesh.position(pr.mesh);
        }
}
} // namespace

const char* constructKindName(ConstructKind k) { return kKindNames[size_t(k)]; }

bool constructKindFromName(std::string_view name, ConstructKind& out)
{
    for (size_t i = 0; i < kKindNames.size(); ++i)
        if (name == kKindNames[i])
        {
            out = ConstructKind(i);
            return true;
        }
    return false;
}

ConstructKind constructKind(const ConstructSpec& spec) { return ConstructKind(spec.index()); }

StitchResult stitchConstruct(geo::Mesh& mesh, const GenResult& gen, int id, double weld, const std::optional<geo::SymmetryPlane>& symmetry,
                             double snap)
{
    LILA_ZONE();

    StitchResult out;
    std::vector<glm::dvec3> pos = gen.positions;
    // With symmetry the construct keeps the half most of it lies in, clamped to the plane, and the mirror builds the other.
    std::vector<uint8_t> beyond(pos.size(), 0);
    if (symmetry)
    {
        double ahead = 0.0, behind = 0.0;
        for (const glm::dvec3& p : pos)
            (symmetry->distance(p) > 0.0 ? ahead : behind) += std::abs(symmetry->distance(p));
        const double side = ahead >= behind ? 1.0 : -1.0;
        const double tol = std::max(weld, 1e-9);
        const double planeSnap = std::max(tol, snap);
        for (size_t i = 0; i < pos.size(); ++i)
        {
            const double d = symmetry->distance(pos[i]) * side;
            beyond[i] = d <= -tol;
            if (d < planeSnap)
                pos[i] = symmetry->project(pos[i]);
        }
    }

    auto dropped = [&](const std::vector<Index>& face) {
        return std::all_of(face.begin(), face.end(), [&](Index i) { return beyond[i] != 0; });
    };
    std::vector<uint8_t> used(pos.size(), 0);
    for (const std::vector<Index>& face : gen.faces)
        if (!dropped(face))
            for (Index i : face)
                used[i] = 1;

    if (snap > 0.0)
        snapToBorder(mesh, gen, used, pos, snap);

    const Index before = mesh.pointCapacity();
    auto place = [&](const std::vector<glm::dvec3>& ps) {
        std::vector<Index> ids(ps.size(), geo::kInvalid);
        for (size_t i = 0; i < ps.size(); ++i)
        {
            if (!used[i])
                continue;
            bool fixed = i < gen.fixed.size() && gen.fixed[i];
            ids[i] = pointNear(mesh, ps[i], fixed ? std::max(weld, 1e-6) : weld);
        }
        return ids;
    };
    std::vector<Index> ids = place(pos);
    std::vector<Index> mirrorIds;
    if (symmetry)
    {
        std::vector<glm::dvec3> mirrored(pos.size());
        for (size_t i = 0; i < pos.size(); ++i)
            mirrored[i] = symmetry->reflect(pos[i]);
        mirrorIds = place(mirrored);
    }

    mesh.faceAttrs().getOrAdd<int32_t>(kConstructAttr);
    auto add = [&](const std::vector<Index>& local, const std::vector<Index>& map) {
        if (dropped(local))
            return;
        std::vector<Index> pts;
        for (Index i : local)
            pts.push_back(map[i]);
        Index f = addOriented(mesh, pts);
        if (f == geo::kInvalid)
            return;
        mesh.faceAttrs().get<int32_t>(kConstructAttr)[f] = id;
        out.faces.push_back(f);
    };
    for (const std::vector<Index>& face : gen.faces)
        add(face, ids);
    if (symmetry)
        for (const std::vector<Index>& face : gen.faces)
            add(face, mirrorIds);

    for (Index p = before; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
            out.movable.push_back(p);
    std::sort(out.faces.begin(), out.faces.end());
    return out;
}

std::vector<Index> constructFaces(const geo::Mesh& mesh, int id)
{
    std::vector<Index> out;
    std::span<const int32_t> tags = mesh.faceAttrs().get<int32_t>(kConstructAttr);
    if (tags.empty() || id == 0)
        return out;
    for (Index f = 0; f < mesh.faceCapacity() && f < tags.size(); ++f)
        if (mesh.faceValid(f) && tags[f] == id)
            out.push_back(f);
    return out;
}

void removeConstructFaces(geo::Mesh& mesh, int id)
{
    std::vector<Index> faces = constructFaces(mesh, id);
    if (!faces.empty())
        geo::deleteFaces(mesh, faces);
}

void untagConstruct(geo::Mesh& mesh, int id)
{
    for (int32_t& t : mesh.faceAttrs().get<int32_t>(kConstructAttr))
        if (t == id)
            t = 0;
}

uint64_t constructHash(const geo::Mesh& mesh, int id)
{
    std::vector<Index> faces = constructFaces(mesh, id);
    // Scaled by the construct's own extent, so geometry added elsewhere doesn't change it.
    glm::dvec3 lo(std::numeric_limits<double>::max()), hi(std::numeric_limits<double>::lowest());
    for (Index f : faces)
        for (Index p : mesh.facePoints(f))
        {
            lo = glm::min(lo, mesh.position(p));
            hi = glm::max(hi, mesh.position(p));
        }
    double size = faces.empty() ? 1.0 : glm::length(hi - lo);
    double quantum = std::max(1e-9, (std::isfinite(size) ? size : 1.0) * 1e-6);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](int64_t v) {
        h ^= uint64_t(v);
        h *= 1099511628211ull;
    };
    mix(int64_t(faces.size()));
    for (Index f : faces)
    {
        mix(int64_t(mesh.faceSize(f)));
        for (Index p : mesh.facePoints(f))
            for (int k = 0; k < 3; ++k)
                mix(std::llround(mesh.position(p)[k] / quantum));
    }
    return h;
}

GenResult gridBetween(const std::vector<glm::dvec3>& a, const std::vector<glm::dvec3>& b, int rows, const geo::MeshBvh* surface)
{
    GenResult r;
    if (a.size() != b.size() || a.size() < 2 || rows < 1)
    {
        r.error = "the two sides need the same number of points, at least two";
        return r;
    }
    const size_t n = a.size();
    for (int j = 0; j <= rows; ++j)
        for (size_t i = 0; i < n; ++i)
        {
            glm::dvec3 p = j == 0 ? a[i] : j == rows ? b[i] : glm::mix(a[i], b[i], double(j) / rows);
            if (surface && j > 0 && j < rows)
                if (geo::BvhClosest c; surface->closestPoint(p, c))
                    p = c.point;
            r.positions.push_back(p);
        }
    r.fixed.assign(r.positions.size(), 0);
    for (int j = 0; j < rows; ++j)
        for (size_t i = 0; i + 1 < n; ++i)
        {
            Index p0 = Index(j * n + i), p1 = Index(j * n + i + 1), p2 = Index((j + 1) * n + i + 1), p3 = Index((j + 1) * n + i);
            r.faces.push_back({p0, p1, p2, p3});
        }
    return r;
}
} // namespace lila::retopo
