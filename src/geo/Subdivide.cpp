#include "geo/Ops.h"

#include "core/Profile.h"

#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <array>
#include <bit>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <utility>

namespace lila::geo
{
namespace
{
constexpr Index kRefineGrain = 2048;

/**
 * One uv layer refined like the positions, within each uv island: per source corner the uv at its point and at the
 * middle of its outgoing edge, and per face the uv at its centre. Seams, mesh borders and creases are island borders,
 * smoothed along themselves like a crease; a point whose island has only one face around it keeps its uv.
 */
struct UvRefine
{
    std::string name;
    std::vector<glm::vec2> point, edge, face;
};

/**
 * The layer whose islands a uv layer is refined within: its own, or for a layer named "<anything>@<set>" the Float2
 * layer <set>, so a layer derived from a uv set is smoothed exactly as that set is.
 */
const Attribute& islandLayer(const Mesh& src, const Attribute& layer)
{
    const std::string& name = layer.name();
    const size_t at = name.rfind('@');
    if (at == std::string::npos)
        return layer;
    const Attribute* set = src.cornerAttrs().find(std::string_view(name).substr(at + 1));
    return set && set->type() == AttrType::Float2 ? *set : layer;
}

template <class Sharpness>
UvRefine refineUvs(const Mesh& src, const Attribute& layer, const Attribute& islands, Sharpness&& sharpness, bool smoothCorners)
{
    LILA_ZONE();

    const std::span<const glm::vec2> uv = layer.values<glm::vec2>();
    UvRefine r{layer.name(), std::vector<glm::vec2>(src.cornerCapacity()), std::vector<glm::vec2>(src.cornerCapacity()),
               std::vector<glm::vec2>(src.faceCapacity())};
    const Index fc = src.faceCapacity(), pc = src.pointCapacity();
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        for (Index f = rg.begin(); f < rg.end(); ++f)
            if (src.faceValid(f))
            {
                glm::vec2 sum(0.0f);
                int n = 0;
                src.forEachFaceCorner(f, [&](Index c) {
                    sum += uv[c];
                    ++n;
                });
                r.face[f] = sum / float(n);
            }
    });
    const std::span<const glm::vec2> key = islands.values<glm::vec2>();
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(key[a] - key[b]), glm::vec2(1e-6f))); };
    // The corner at point p of the face holding corner c.
    auto cornerAt = [&](Index c, Index p) { return src.cornerPoint(c) == p ? c : src.cornerNext(c); };
    // Whether e joins its two faces' uvs, so it lies inside an island.
    auto joins = [&](Index e) {
        if (!src.isManifoldEdge(e) || sharpness(e) > 0.0)
            return false;
        const Index c = src.edgeCorner(e), rc = src.cornerRadial(c);
        const Index p0 = src.edgePoint(e, 0), p1 = src.edgePoint(e, 1);
        return same(cornerAt(c, p0), cornerAt(rc, p0)) && same(cornerAt(c, p1), cornerAt(rc, p1));
    };
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        for (Index f = rg.begin(); f < rg.end(); ++f)
            if (src.faceValid(f))
                src.forEachFaceCorner(f, [&](Index c) {
                    const Index e = src.cornerEdge(c);
                    const glm::vec2 mid = (uv[c] + uv[src.cornerNext(c)]) * 0.5f;
                    if (!joins(e))
                    {
                        r.edge[c] = mid;
                        return;
                    }
                    const Index ec = src.edgeCorner(e);
                    const Index f2 = src.cornerFace(ec) == f ? src.cornerFace(src.cornerRadial(ec)) : src.cornerFace(ec);
                    r.edge[c] = (mid + (r.face[f] + r.face[f2]) * 0.5f) * 0.5f;
                });
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRefineGrain), [&](const tbb::blocked_range<Index>& rg) {
        std::vector<Index> corners, island, stack;
        std::vector<uint8_t> done;
        for (Index p = rg.begin(); p < rg.end(); ++p)
        {
            if (!src.pointValid(p))
                continue;
            corners.clear();
            src.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
            done.assign(corners.size(), 0);
            for (size_t start = 0; start < corners.size(); ++start)
            {
                if (done[start])
                    continue;
                // The corners at p in one island, reached across the edges at p that join their faces.
                island.clear();
                stack.assign(1, corners[start]);
                done[start] = 1;
                while (!stack.empty())
                {
                    const Index c = stack.back();
                    stack.pop_back();
                    island.push_back(c);
                    for (Index e : {src.cornerEdge(c), src.cornerEdge(src.cornerPrev(c))})
                    {
                        if (!joins(e))
                            continue;
                        const Index ec = src.edgeCorner(e);
                        const Index nb = cornerAt(src.cornerFace(ec) == src.cornerFace(c) ? src.cornerRadial(ec) : ec, p);
                        const auto it = std::ranges::find(corners, nb);
                        if (it != corners.end() && !done[size_t(it - corners.begin())])
                        {
                            done[size_t(it - corners.begin())] = 1;
                            stack.push_back(nb);
                        }
                    }
                }
                const glm::vec2 P = uv[island.front()];
                glm::vec2 F(0.0f), R(0.0f), rim(0.0f);
                int inner = 0, borders = 0;
                for (Index c : island)
                {
                    F += r.face[src.cornerFace(c)];
                    const Index cn = src.cornerNext(c), cp = src.cornerPrev(c);
                    for (auto [e, q] : {std::pair{src.cornerEdge(c), cn}, std::pair{src.cornerEdge(cp), cp}})
                        if (joins(e))
                        {
                            // Met from both of its faces, so each sighting adds half.
                            R += (P + uv[q]) * 0.25f;
                            ++inner;
                        }
                        else
                        {
                            rim += uv[q];
                            ++borders;
                        }
                }
                const float n = float(island.size());
                glm::vec2 result = P;
                if (borders == 0 && island.size() >= 3)
                    result = (F / n + 2.0f * R / (float(inner) * 0.5f) + (n - 3.0f) * P) / n;
                else if (borders == 2 && (island.size() >= 2 || smoothCorners))
                    result = (6.0f * P + rim) / 8.0f;
                for (Index c : island)
                    r.point[c] = result;
            }
        }
    });
    return r;
}

/** Writes the refined uvs onto the quad of source corner c, whose first corner is oc. */
void writeUvs(Mesh& out, const Mesh& src, std::span<const UvRefine> uvs, Index oc, Index c)
{
    for (const UvRefine& u : uvs)
    {
        const std::span<glm::vec2> v = out.cornerAttrs().get<glm::vec2>(u.name);
        if (v.empty())
            continue;
        v[oc] = u.point[c];
        v[out.cornerNext(oc)] = u.edge[c];
        v[out.cornerNext(out.cornerNext(oc))] = u.face[src.cornerFace(c)];
        v[out.cornerPrev(oc)] = u.edge[src.cornerPrev(c)];
    }
}

/**
 * The subdivided mesh built in parallel: every face's quads laid out in source order and built at once, then their
 * attributes copied face by face. Null when the builder dropped a face, for the face-by-face build to handle.
 */
std::optional<Mesh> buildQuads(const Mesh& src, const std::vector<glm::dvec3>& pos, const std::vector<Index>& pId,
                               const std::vector<Index>& eId, const std::vector<Index>& fId, std::span<const float> crease,
                               std::span<const UvRefine> uvs, SubdivideMap* map)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    // The first quad of each source face: one per corner.
    std::vector<Index> firstQuad(size_t(fc) + 1, 0);
    for (Index f = 0; f < fc; ++f)
        firstQuad[f + 1] = firstQuad[f] + (fId[f] != kInvalid ? Index(src.faceSize(f)) : 0);
    const Index quads = firstQuad[fc];
    std::vector<Index> sizes(quads, 4), corners(size_t(quads) * 4);
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (fId[f] == kInvalid)
                continue;
            Index* out = corners.data() + size_t(firstQuad[f]) * 4;
            const Index first = src.faceCorner(f);
            Index c = first;
            do
            {
                *out++ = pId[src.cornerPoint(c)];
                *out++ = eId[src.cornerEdge(c)];
                *out++ = fId[f];
                *out++ = eId[src.cornerEdge(src.cornerPrev(c))];
                c = src.cornerNext(c);
            } while (c != first);
        }
    });
    Mesh out = Mesh::fromPolygons(pos, sizes, corners);
    if (out.faceCapacity() != quads)
        return std::nullopt;
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        out.attributes(Domain(d)).mergeLayout(src.attributes(Domain(d)));
    out.materialNames() = src.materialNames();
    out.colorSetInfo() = src.colorSetInfo();

    AttributeSet& pa = out.pointAttrs();
    if (pa.layerCount())
    {
        tbb::parallel_for(Index(0), pc, [&](Index p) {
            if (pId[p] != kInvalid)
                pa.copyFrom(src.pointAttrs(), pId[p], p);
        });
        tbb::parallel_for(Index(0), ec, [&](Index e) {
            if (eId[e] == kInvalid)
                return;
            const Index s[2] = {src.edgePoint(e, 0), src.edgePoint(e, 1)};
            const float w[2] = {0.5f, 0.5f};
            pa.mixFrom(src.pointAttrs(), eId[e], s, w);
        });
        tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
            std::vector<Index> s;
            std::vector<float> w;
            for (Index f = r.begin(); f < r.end(); ++f)
                if (fId[f] != kInvalid)
                {
                    s = src.facePoints(f);
                    w.assign(s.size(), 1.0f / float(s.size()));
                    pa.mixFrom(src.pointAttrs(), fId[f], s, w);
                }
        });
    }
    if (map)
    {
        map->points = pId;
        map->facePoints = fId;
        map->edges.assign(out.edgeCapacity(), kInvalid);
        map->faces.assign(out.faceCapacity(), kInvalid);
    }
    const bool hasCornerAttrs = out.cornerAttrs().layerCount() > 0, hasEdgeAttrs = out.edgeAttrs().layerCount() > 0;
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        std::vector<Index> faceCorners;
        std::vector<float> avg;
        for (Index f = r.begin(); f < r.end(); ++f)
        {
            if (fId[f] == kInvalid)
                continue;
            faceCorners = src.faceCorners(f);
            const size_t n = faceCorners.size();
            avg.assign(n, 1.0f / float(n));
            for (size_t i = 0; i < n; ++i)
            {
                const Index c = faceCorners[i];
                const Index cp = faceCorners[(i + n - 1) % n];
                const Index cn = faceCorners[(i + 1) % n];
                const Index nf = firstQuad[f] + Index(i);
                out.faceAttrs().copyFrom(src.faceAttrs(), nf, f);
                const Index oc = out.faceCorner(nf);
                // Each half of a source edge sits in a quad of every face on that edge; the edge's own first face writes it.
                auto outer = [&](Index outEdge, Index srcEdge) {
                    if (src.cornerFace(src.edgeCorner(srcEdge)) != f)
                        return;
                    if (hasEdgeAttrs)
                        out.edgeAttrs().copyFrom(src.edgeAttrs(), outEdge, srcEdge);
                    if (map)
                        map->edges[outEdge] = srcEdge;
                };
                outer(out.cornerEdge(oc), src.cornerEdge(c));
                outer(out.cornerEdge(out.cornerPrev(oc)), src.cornerEdge(cp));
                if (map)
                    map->faces[nf] = f;
                if (hasCornerAttrs)
                {
                    out.cornerAttrs().copyFrom(src.cornerAttrs(), oc, c);
                    const Index a[2] = {c, cn};
                    const Index b[2] = {cp, c};
                    const float half[2] = {0.5f, 0.5f};
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(oc), a, half);
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(out.cornerNext(oc)), faceCorners, avg);
                    out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerPrev(oc), b, half);
                    writeUvs(out, src, uvs, oc, c);
                }
            }
        }
    });
    if (!crease.empty())
    {
        std::span<float> cr = out.edgeAttrs().get<float>(attr::kCrease);
        for (float& v : cr)
            v = std::max(0.0f, v - 1.0f);
    }
    return out;
}

Mesh subdivideOnce(const Mesh& src, const SubdivideOptions& opts, SubdivideMap* map = nullptr)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    std::span<const float> crease = src.edgeAttrs().get<float>(attr::kCrease);
    auto sharpness = [&](Index e) -> double {
        if (!src.isManifoldEdge(e))
            return 1.0;
        return crease.empty() ? 0.0 : std::clamp(double(crease[e]), 0.0, 1.0);
    };

    // Dense ids: points, then edges, then faces.
    std::vector<Index> pId(pc, kInvalid), eId(ec, kInvalid), fId(fc, kInvalid);
    Index next = 0;
    for (Index p = 0; p < pc; ++p)
        if (src.pointValid(p))
            pId[p] = next++;
    for (Index e = 0; e < ec; ++e)
        if (src.edgeValid(e))
            eId[e] = next++;
    for (Index f = 0; f < fc; ++f)
        if (src.faceValid(f))
            fId[f] = next++;
    std::vector<glm::dvec3> pos(next);

    std::vector<glm::dvec3> facePt(fc);
    tbb::parallel_for(tbb::blocked_range<Index>(0, fc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index f = r.begin(); f < r.end(); ++f)
            if (src.faceValid(f))
                pos[fId[f]] = facePt[f] = src.faceCentroid(f);
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, ec, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index e = r.begin(); e < r.end(); ++e)
        {
            if (!src.edgeValid(e))
                continue;
            const glm::dvec3 mid = (src.position(src.edgePoint(e, 0)) + src.position(src.edgePoint(e, 1))) * 0.5;
            const double s = sharpness(e);
            glm::dvec3 smooth = mid;
            if (s < 1.0)
            {
                const Index c = src.edgeCorner(e);
                const glm::dvec3 faces = (facePt[src.cornerFace(c)] + facePt[src.cornerFace(src.cornerRadial(c))]) * 0.5;
                smooth = (mid + faces) * 0.5;
            }
            pos[eId[e]] = glm::mix(smooth, mid, s);
        }
    });
    tbb::parallel_for(tbb::blocked_range<Index>(0, pc, kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index p = r.begin(); p < r.end(); ++p)
        {
            if (!src.pointValid(p))
                continue;
            const glm::dvec3 P = src.position(p);
            glm::dvec3 R(0.0), F(0.0), creaseSum(0.0);
            int n = 0, nf = 0, nSharp = 0;
            double sharpSum = 0.0;
            src.forEachPointEdge(p, [&](Index e) {
                const glm::dvec3 q = src.position(src.edgeOtherPoint(e, p));
                R += (P + q) * 0.5;
                ++n;
                const double s = sharpness(e);
                if (s > 0.0)
                {
                    ++nSharp;
                    sharpSum += s;
                    creaseSum += q;
                }
            });
            src.forEachPointCorner(p, [&](Index c) {
                F += facePt[src.cornerFace(c)];
                ++nf;
            });
            if (n == 0 || nf == 0)
            {
                pos[pId[p]] = P;
                continue;
            }
            const glm::dvec3 smooth = n >= 3 && nf == n ? (F / double(nf) + 2.0 * R / double(n) + double(n - 3) * P) / double(n) : P;
            glm::dvec3 result = smooth;
            const bool boundaryCorner = nf < n && n == 2;
            if (nSharp == 2 && !(boundaryCorner && opts.sharpBoundary))
                result = glm::mix(smooth, (6.0 * P + creaseSum) / 8.0, sharpSum / 2.0);
            else if (nSharp > 2 || (boundaryCorner && opts.sharpBoundary))
                result = glm::mix(smooth, P, nSharp ? std::min(1.0, sharpSum / nSharp) : 1.0);
            pos[pId[p]] = result;
        }
    });

    std::vector<UvRefine> uvs;
    for (size_t l = 0; l < src.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = src.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            uvs.push_back(refineUvs(src, a, islandLayer(src, a), sharpness, opts.uvCorners == SubdivideUvCorners::Smooth));

    if (std::optional<Mesh> built = buildQuads(src, pos, pId, eId, fId, crease, uvs, map))
        return std::move(*built);

    Mesh out;
    for (size_t d = 0; d < size_t(Domain::Count); ++d)
        out.attributes(Domain(d)).mergeLayout(src.attributes(Domain(d)));
    out.materialNames() = src.materialNames();
    out.colorSetInfo() = src.colorSetInfo();
    out.reserve(next, size_t(src.numEdges()) * 2 + src.numCorners(), src.numCorners(), size_t(src.numCorners()) * 4);
    for (const glm::dvec3& p : pos)
        out.addPoint(p);

    AttributeSet& pa = out.pointAttrs();
    if (pa.layerCount())
    {
        for (Index p = 0; p < pc; ++p)
            if (pId[p] != kInvalid)
                pa.copyFrom(src.pointAttrs(), pId[p], p);
        for (Index e = 0; e < ec; ++e)
            if (eId[e] != kInvalid)
            {
                const Index s[2] = {src.edgePoint(e, 0), src.edgePoint(e, 1)};
                const float w[2] = {0.5f, 0.5f};
                pa.mixFrom(src.pointAttrs(), eId[e], s, w);
            }
        std::vector<Index> s;
        std::vector<float> w;
        for (Index f = 0; f < fc; ++f)
            if (fId[f] != kInvalid)
            {
                s = src.facePoints(f);
                w.assign(s.size(), 1.0f / float(s.size()));
                pa.mixFrom(src.pointAttrs(), fId[f], s, w);
            }
    }

    std::span<const float> srcCrease = crease;
    const bool hasCornerAttrs = out.cornerAttrs().layerCount() > 0;
    if (map)
    {
        map->points = pId;
        map->facePoints = fId;
        map->edges.clear();
        map->faces.clear();
    }
    std::vector<Index> corners;
    std::vector<float> avg;
    for (Index f = 0; f < fc; ++f)
    {
        if (fId[f] == kInvalid)
            continue;
        corners = src.faceCorners(f);
        const size_t n = corners.size();
        avg.assign(n, 1.0f / float(n));
        for (size_t i = 0; i < n; ++i)
        {
            const Index c = corners[i];
            const Index cp = corners[(i + n - 1) % n];
            const Index cn = corners[(i + 1) % n];
            const Index quad[4] = {pId[src.cornerPoint(c)], eId[src.cornerEdge(c)], fId[f], eId[src.cornerEdge(cp)]};
            const Index edgeCap = out.edgeCapacity();
            const Index nf = out.addFace(quad);
            if (nf == kInvalid)
                continue;
            out.faceAttrs().copyFrom(src.faceAttrs(), nf, f);
            Index oc = out.faceCorner(nf);
            // Outer sub-edges come from the original edges; the two inner ones are new and plain.
            const Index e0 = out.cornerEdge(oc);
            const Index e3 = out.cornerEdge(out.cornerPrev(oc));
            if (map)
            {
                map->edges.resize(out.edgeCapacity(), kInvalid);
                map->edges[e0] = src.cornerEdge(c);
                map->edges[e3] = src.cornerEdge(cp);
                map->faces.resize(out.faceCapacity(), kInvalid);
                map->faces[nf] = f;
            }
            if (e0 >= edgeCap)
                out.edgeAttrs().copyFrom(src.edgeAttrs(), e0, src.cornerEdge(c));
            if (e3 >= edgeCap)
                out.edgeAttrs().copyFrom(src.edgeAttrs(), e3, src.cornerEdge(cp));
            if (hasCornerAttrs)
            {
                out.cornerAttrs().copyFrom(src.cornerAttrs(), oc, c);
                const Index a[2] = {c, cn};
                const Index b[2] = {cp, c};
                const float half[2] = {0.5f, 0.5f};
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(oc), a, half);
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerNext(out.cornerNext(oc)), corners, avg);
                out.cornerAttrs().mixFrom(src.cornerAttrs(), out.cornerPrev(oc), b, half);
                writeUvs(out, src, uvs, oc, c);
            }
        }
    }
    if (!srcCrease.empty())
    {
        std::span<float> cr = out.edgeAttrs().get<float>(attr::kCrease);
        for (float& v : cr)
            v = std::max(0.0f, v - 1.0f);
    }
    return out;
}

namespace fit
{
using SpMat = Eigen::SparseMatrix<double>;

/**
 * A mesh's uv islands for one island layer: its corners grouped per point within an island (the points of the island's
 * own surface), each group on the island's border or inside it.
 */
struct Islands
{
    std::vector<Index> groupOfCorner;
    std::vector<uint8_t> border;
    std::vector<Index> islandOfGroup;
    std::vector<std::vector<Index>> groupCorners;
    std::vector<std::vector<Index>> faces; // per island
};

Islands islandsOf(const Mesh& m, const Attribute& layer)
{
    LILA_ZONE();

    const std::span<const glm::vec2> key = layer.values<glm::vec2>();
    auto same = [&](Index a, Index b) { return glm::all(glm::lessThanEqual(glm::abs(key[a] - key[b]), glm::vec2(1e-6f))); };
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    auto joins = [&](Index e) {
        if (!m.isManifoldEdge(e))
            return false;
        const Index c = m.edgeCorner(e), rc = m.cornerRadial(c);
        const Index p0 = m.edgePoint(e, 0), p1 = m.edgePoint(e, 1);
        return same(cornerAt(c, p0), cornerAt(rc, p0)) && same(cornerAt(c, p1), cornerAt(rc, p1));
    };
    Islands is;
    is.groupOfCorner.assign(m.cornerCapacity(), kInvalid);
    std::vector<Index> corners, stack;
    for (Index p = 0; p < m.pointCapacity(); ++p)
    {
        if (!m.pointValid(p))
            continue;
        corners.clear();
        m.forEachPointCorner(p, [&](Index c) { corners.push_back(c); });
        for (Index start : corners)
        {
            if (is.groupOfCorner[start] != kInvalid)
                continue;
            const Index g = Index(is.border.size());
            is.border.push_back(0);
            is.groupCorners.emplace_back();
            is.groupOfCorner[start] = g;
            stack.assign(1, start);
            while (!stack.empty())
            {
                const Index c = stack.back();
                stack.pop_back();
                is.groupCorners[g].push_back(c);
                for (Index e : {m.cornerEdge(c), m.cornerEdge(m.cornerPrev(c))})
                {
                    if (!joins(e))
                    {
                        is.border[g] = 1;
                        continue;
                    }
                    const Index ec = m.edgeCorner(e);
                    const Index nb = cornerAt(m.cornerFace(ec) == m.cornerFace(c) ? m.cornerRadial(ec) : ec, p);
                    if (is.groupOfCorner[nb] == kInvalid)
                    {
                        is.groupOfCorner[nb] = g;
                        stack.push_back(nb);
                    }
                }
            }
        }
    }
    // Islands: groups joined through the faces they share.
    std::vector<Index> parent(is.border.size());
    for (Index g = 0; g < parent.size(); ++g)
        parent[g] = g;
    auto root = [&](Index g) {
        while (parent[g] != g)
            g = parent[g] = parent[parent[g]];
        return g;
    };
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
        {
            const Index a = root(is.groupOfCorner[m.faceCorner(f)]);
            m.forEachFaceCorner(f, [&](Index c) {
                const Index b = root(is.groupOfCorner[c]);
                if (b != a)
                    parent[b] = a;
            });
        }
    std::vector<Index> id(parent.size(), kInvalid);
    is.islandOfGroup.resize(parent.size());
    for (Index g = 0; g < parent.size(); ++g)
    {
        Index& i = id[root(g)];
        if (i == kInvalid)
        {
            i = Index(is.faces.size());
            is.faces.emplace_back();
        }
        is.islandOfGroup[g] = i;
    }
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            is.faces[is.islandOfGroup[is.groupOfCorner[m.faceCorner(f)]]].push_back(f);
    return is;
}

/** One island's interior system: the cotan Laplacian over its interior groups and their couplings to its border. */
struct System
{
    std::vector<Index> interior; // groups, in unknown order
    std::vector<Index> border;   // groups
    SpMat a;                     // interior x interior
    SpMat b;                     // interior x border: rhs = b * border values
};

struct Factor
{
    Eigen::SimplicialLDLT<SpMat> ldlt;
    bool ok = false;
};

/** Factorisations by matrix content, so re-evaluating an unchanged surface (new uvs, more probe layers) skips them. */
class FactorCache
{
public:
    std::shared_ptr<const Factor> get(const SpMat& a)
    {
        const uint64_t key = hashOf(a);
        {
            std::lock_guard lock(mMutex);
            for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
                if (it->key == key && it->n == a.rows() && it->nnz == a.nonZeros())
                {
                    mEntries.splice(mEntries.begin(), mEntries, it);
                    return it->factor;
                }
        }
        auto f = std::make_shared<Factor>();
        f->ldlt.compute(a);
        f->ok = f->ldlt.info() == Eigen::Success;
        std::lock_guard lock(mMutex);
        mEntries.push_front({key, a.rows(), a.nonZeros(), f});
        mNonZeros += size_t(a.nonZeros());
        while (mEntries.size() > 1 && (mNonZeros > kBudget || mEntries.size() > kMaxEntries))
        {
            mNonZeros -= size_t(mEntries.back().nnz);
            mEntries.pop_back();
        }
        return f;
    }

private:
    static constexpr size_t kBudget = size_t(8) << 20;
    static constexpr size_t kMaxEntries = 4096;

    static uint64_t hashOf(const SpMat& a)
    {
        uint64_t h = 0x9e3779b97f4a7c15ull ^ uint64_t(a.rows());
        auto mix = [&](uint64_t v) {
            h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
            h *= 0xff51afd7ed558ccdull;
        };
        for (Eigen::Index k = 0; k <= a.outerSize(); ++k)
            mix(uint64_t(a.outerIndexPtr()[k]));
        for (Eigen::Index k = 0; k < a.nonZeros(); ++k)
        {
            mix(uint64_t(a.innerIndexPtr()[k]));
            mix(std::bit_cast<uint64_t>(a.valuePtr()[k]));
        }
        return h;
    }

    struct Entry
    {
        uint64_t key;
        Eigen::Index n, nnz;
        std::shared_ptr<const Factor> factor;
    };
    std::mutex mMutex;
    std::list<Entry> mEntries;
    size_t mNonZeros = 0;
};

FactorCache& factorCache()
{
    // Never destroyed: meshes may still be subdivided while statics shut down.
    static FactorCache& c = *new FactorCache();
    return c;
}

/** Cotan weights of the island's faces on the mesh's surface; a quad averages both of its diagonals. */
System systemOf(const Mesh& m, const Islands& is, Index island)
{
    System s;
    // Per group, its row among the unknowns or column among the knowns; reused across islands, cleared after.
    thread_local std::vector<int> slot;
    if (slot.size() < is.groupCorners.size())
        slot.resize(is.groupCorners.size(), -1);
    struct Clear
    {
        System& s;
        ~Clear()
        {
            for (const std::vector<Index>* v : {&s.border, &s.interior})
                for (Index g : *v)
                    slot[g] = -1;
        }
    } clear{s};
    for (Index f : is.faces[island])
        m.forEachFaceCorner(f, [&](Index c) {
            const Index g = is.groupOfCorner[c];
            if (slot[g] >= 0)
                return;
            std::vector<Index>& to = is.border[g] ? s.border : s.interior;
            slot[g] = int(to.size());
            to.push_back(g);
        });
    if (s.interior.empty() || s.border.empty())
        return s;
    std::vector<Eigen::Triplet<double>> ta, tb;
    auto half = [&](Index g, Index o, double w) {
        if (is.border[g])
            return;
        const int r = slot[g];
        ta.emplace_back(r, r, w);
        if (is.border[o])
            tb.emplace_back(r, slot[o], w);
        else
            ta.emplace_back(r, slot[o], -w);
    };
    auto addTri = [&](Index c0, Index c1, Index c2, double tw) {
        const Index cs[3] = {c0, c1, c2};
        for (int k = 0; k < 3; ++k)
        {
            const Index ci = cs[(k + 1) % 3], cj = cs[(k + 2) % 3];
            const glm::dvec3 pk = m.position(m.cornerPoint(cs[k]));
            const glm::dvec3 u = m.position(m.cornerPoint(ci)) - pk, v = m.position(m.cornerPoint(cj)) - pk;
            const double sn = glm::length(glm::cross(u, v));
            if (sn <= 1e-300)
                continue;
            const double w = 0.5 * tw * glm::dot(u, v) / sn;
            const Index gi = is.groupOfCorner[ci], gj = is.groupOfCorner[cj];
            half(gi, gj, w);
            half(gj, gi, w);
        }
    };
    std::vector<Index> fc;
    for (Index f : is.faces[island])
    {
        fc = m.faceCorners(f);
        if (fc.size() == 4)
        {
            addTri(fc[0], fc[1], fc[2], 0.5);
            addTri(fc[0], fc[2], fc[3], 0.5);
            addTri(fc[1], fc[2], fc[3], 0.5);
            addTri(fc[1], fc[3], fc[0], 0.5);
        }
        else
            for (size_t i = 1; i + 1 < fc.size(); ++i)
                addTri(fc[0], fc[i], fc[i + 1], 1.0);
    }
    const auto n = Eigen::Index(s.interior.size());
    s.a.resize(n, n);
    s.a.setFromTriplets(ta.begin(), ta.end());
    s.b.resize(n, Eigen::Index(s.border.size()));
    s.b.setFromTriplets(tb.begin(), tb.end());
    return s;
}

/** A layer's value at a group, whose corners agree up to float noise. */
glm::dvec2 groupValue(std::span<const glm::vec2> uv, const std::vector<Index>& corners)
{
    glm::dvec2 sum(0.0);
    for (Index c : corners)
        sum += glm::dvec2(uv[c]);
    return sum / double(corners.size());
}

/** A border group's new value: the blend of two border groups' values at t. */
struct BorderBlend
{
    Index group, a, b;
    double t;
};

/** Border edges whose spacing proxy is shorter than this share of their surface length are bunched. */
constexpr double kBunched = 0.2;
/** A bunched run takes in its neighbours until the run is at most 1 - this share of the window's surface length. */
constexpr double kRespaced = 0.6;
/** Margin edges per window edge over which a respaced window eases into the spacing beside it. */
constexpr double kRespaceTaper = 1.0;
/** A border point where the proxy folds back sharper than this (radians) is a closed slit's tip, which respacing keeps. */
constexpr double kClosedTip = 0.5;
/** Where the proxy turns more than this (radians) inside a window, the point taking that turn on the surface lands on it. */
constexpr double kWindowTurn = 1.0;

double angleSum(const Mesh& m, const Islands& is, Index g);

/**
 * Where an island's border bunches up in uv (a bevel's corner points all carry the one cage uv of the corner they
 * replaced, and subdivision keeps uv corners while it rounds the surface), the run and enough of its neighbours spread
 * along the same uv polyline by their length on the surface. Runs, windows and where each point lands on the polyline
 * are measured in the spacing proxy (see addFitSpacing), so the blends are the same for any layout of the cage and a uv
 * tool's map of the export stays exact after it writes. Every layer using the island layer is blended alike.
 */
std::vector<BorderBlend> respaceBorder(const Mesh& m, const Islands& is, Index island, std::span<const glm::vec2> proxy,
                                       std::span<const glm::vec2> set)
{
    std::vector<BorderBlend> out;
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    std::unordered_map<Index, Index> next;
    std::unordered_map<Index, double> spacingFrom;
    std::vector<Index> fc;
    double a3 = 0.0, auv = 0.0, apx = 0.0;
    for (Index f : is.faces[island])
    {
        fc = m.faceCorners(f);
        const glm::dvec3 p0 = m.position(m.cornerPoint(fc[0]));
        const glm::dvec2 u0 = set[fc[0]], q0 = proxy[fc[0]];
        for (size_t k = 1; k + 1 < fc.size(); ++k)
        {
            a3 += 0.5 * glm::length(glm::cross(m.position(m.cornerPoint(fc[k])) - p0, m.position(m.cornerPoint(fc[k + 1])) - p0));
            const glm::dvec2 u1 = glm::dvec2(set[fc[k]]) - u0, u2 = glm::dvec2(set[fc[k + 1]]) - u0;
            auv += 0.5 * (u1.x * u2.y - u1.y * u2.x);
            const glm::dvec2 q1 = glm::dvec2(proxy[fc[k]]) - q0, q2 = glm::dvec2(proxy[fc[k + 1]]) - q0;
            apx += q1.x * q2.y - q1.y * q2.x;
        }
        for (Index c : fc)
        {
            const Index e = m.cornerEdge(c), cn = m.cornerNext(c);
            bool joined = false;
            if (m.isManifoldEdge(e))
            {
                const Index ec = m.edgeCorner(e), rc = m.cornerFace(ec) == f ? m.cornerRadial(ec) : ec;
                joined = is.groupOfCorner[cornerAt(rc, m.cornerPoint(c))] == is.groupOfCorner[c] &&
                         is.groupOfCorner[cornerAt(rc, m.cornerPoint(cn))] == is.groupOfCorner[cn];
            }
            if (joined)
                continue;
            // A group with two borders leaving it pinches the island; its loops are left alone.
            if (!next.emplace(is.groupOfCorner[c], is.groupOfCorner[cn]).second)
                return out;
            spacingFrom[is.groupOfCorner[c]] = glm::length(glm::dvec2(proxy[cn]) - glm::dvec2(proxy[c]));
        }
    }
    const double density = std::sqrt(std::abs(auv) / std::max(a3, 1e-300));
    auto pos = [&](Index g) { return m.position(m.cornerPoint(is.groupCorners[g].front())); };
    std::unordered_map<Index, uint8_t> seen;
    std::vector<Index> loop;
    std::vector<double> l, u;
    std::vector<size_t> dist;
    for (const auto& [first, unused] : next)
    {
        if (seen.count(first))
            continue;
        loop.clear();
        for (Index g = first; !seen.count(g);)
        {
            seen[g] = 1;
            loop.push_back(g);
            const auto it = next.find(g);
            if (it == next.end())
                break;
            g = it->second;
        }
        const size_t n = loop.size();
        if (n < 4 || next.find(loop.back()) == next.end() || next.at(loop.back()) != first)
            continue;
        l.resize(n);
        u.resize(n);
        bool known = true;
        for (size_t i = 0; i < n; ++i)
        {
            const Index a = loop[i], b = loop[(i + 1) % n];
            l[i] = glm::length(pos(b) - pos(a));
            u[i] = spacingFrom[a];
            known = known && std::isfinite(u[i]);
        }
        if (!known)
            continue;
        auto bad = [&](size_t i) { return u[i] < kBunched * l[i]; };
        // Start on the edge farthest from any run, so no window wraps past the scan's start.
        const size_t far = n + 1;
        dist.assign(n, far);
        for (int pass = 0; pass < 2; ++pass)
            for (size_t k = 0; k < 2 * n; ++k)
            {
                const size_t i = pass == 0 ? k % n : (2 * n - 1 - k) % n, j = pass == 0 ? (i + n - 1) % n : (i + 1) % n;
                dist[i] = bad(i) ? 0 : std::min(dist[i], dist[j] == far ? far : dist[j] + 1);
            }
        size_t start = 0;
        for (size_t i = 1; i < n; ++i)
            if (dist[i] > dist[start])
                start = i;
        if (dist[start] == 0 || dist[start] == far)
            continue;
        // Edges are counted from start from here on.
        auto at = [&](size_t k) { return (start + k) % n; };
        // A closed slit's tip (the border folding straight back in the proxy) stays put: a point moved past it would land
        // on the other side.
        auto proxyAt = [&](size_t i) { return glm::dvec2(proxy[is.groupCorners[loop[i % n]].front()]); };
        auto tip = [&](size_t k) {
            const size_t i = at(k);
            const glm::dvec2 a = proxyAt(i + n - 1) - proxyAt(i), b = proxyAt(i + 1) - proxyAt(i);
            const double la = glm::length(a), lb = glm::length(b);
            // A bunched edge has no direction to fold back along.
            return !bad((i + n - 1) % n) && !bad(i) && glm::dot(a, b) > std::cos(kClosedTip) * la * lb;
        };
        std::vector<uint8_t> covered(n, 0);
        covered[0] = 1;
        std::vector<std::pair<size_t, size_t>> windows;
        for (size_t k = 1; k < n; ++k)
        {
            if (covered[k] || !bad(at(k)))
                continue;
            // Edges lo..hi, grown both ways over uncovered edges until spaced enough.
            size_t lo = k, hi = k;
            double L = l[at(k)], U = u[at(k)];
            while (U < kRespaced * L)
            {
                const bool back = lo > 1 && !covered[lo - 1] && !tip(lo), fwd = hi + 1 < n && !covered[hi + 1] && !tip(hi + 1);
                if (!back && !fwd)
                    break;
                if (back)
                {
                    --lo;
                    L += l[at(lo)];
                    U += u[at(lo)];
                }
                if (fwd)
                {
                    ++hi;
                    L += l[at(hi)];
                    U += u[at(hi)];
                }
            }
            for (size_t j = lo; j <= hi; ++j)
                covered[j] = 1;
            // A window touching the last one would pin the point between them where Blend left it.
            if (!windows.empty() && windows.back().second + 1 == lo && !tip(lo))
                windows.back().second = hi;
            else
                windows.push_back({lo, hi});
        }
        std::sort(windows.begin(), windows.end());
        std::vector<double> share, targets, param;
        const double orient = apx < 0.0 ? -1.0 : 1.0;
        // The uv polyline turns where the cage's border turns, and so does the proxy. Where the window's own border takes
        // that turn at one point on the surface (a bevel's corner patch can move it along the border), that point lands on
        // the polyline's turn and the others spread on either side of it.
        auto alignWindowTurns = [&](size_t a, size_t b) {
            std::vector<double> arc(b - a + 2, 0.0);
            double reach = 0.0;
            for (size_t j = a; j <= b; ++j)
            {
                arc[j + 1 - a] = arc[j - a] + param[j - a];
                reach += u[at(j)];
            }
            reach *= 0.5 / double(b - a + 1);
            std::vector<std::pair<size_t, double>> anchors{{a, 0.0}};
            for (size_t j = a + 1; j <= b; ++j)
            {
                const glm::dvec2 pj = proxyAt(at(j));
                size_t lo = j, hi = j;
                while (lo > a && glm::length(proxyAt(at(lo)) - pj) <= reach)
                    --lo;
                while (hi <= b && glm::length(proxyAt(at(hi)) - pj) <= reach)
                    ++hi;
                const glm::dvec2 din = pj - proxyAt(at(lo)), dout = proxyAt(at(hi)) - pj;
                if (glm::length(din) <= reach || glm::length(dout) <= reach)
                    continue;
                const double turn = orient * std::atan2(din.x * dout.y - din.y * dout.x, glm::dot(din, dout));
                const size_t bunchEnd = hi - 1;
                if (std::abs(turn) < kWindowTurn)
                {
                    j = std::max(j, bunchEnd);
                    continue;
                }
                // The bunch's point where its turn on the surface, summed along the border, reaches half the proxy's.
                size_t best = 0;
                double sum = 0.0;
                for (size_t v = std::max(anchors.back().first, lo) + 1; v < hi && v <= b && !best; ++v)
                {
                    const double t = std::numbers::pi - angleSum(m, is, loop[at(v)]);
                    sum += t;
                    if (t * turn > 0.0 && sum * turn >= 0.5 * turn * turn)
                        best = v;
                }
                if (best && arc[j - a] > anchors.back().second)
                    anchors.push_back({best, arc[j - a]});
                j = std::max(j, bunchEnd);
            }
            if (anchors.size() == 1)
                return;
            anchors.push_back({b + 1, targets[b + 1 - a]});
            std::vector<double> moved = targets;
            for (size_t k = 0; k + 1 < anchors.size(); ++k)
            {
                const auto [i0, s0] = anchors[k];
                const auto [i1, s1] = anchors[k + 1];
                const double t0 = targets[i0 - a], t1 = targets[i1 - a];
                for (size_t v = i0; v <= i1; ++v)
                    moved[v - a] = t1 > t0 ? s0 + (targets[v - a] - t0) * (s1 - s0) / (t1 - t0) : s0;
            }
            targets = std::move(moved);
        };
        for (size_t w = 0; w < windows.size(); ++w)
        {
            // Offsets shifted by n, so margins can run back past the start edge (the window before is the last one).
            const size_t lo = windows[w].first + n, hi = windows[w].second + n;
            const size_t prevHi = w > 0 ? windows[w - 1].second + n : windows.back().second;
            const size_t nextLo = w + 1 < windows.size() ? windows[w + 1].first + n : windows.front().first + 2 * n;
            // Each window eases into the spacing beside it over margins of the free edges it shares with its neighbours.
            const size_t width = size_t(double(hi - lo + 1) * kRespaceTaper);
            size_t mL = std::min(width, (lo - prevHi - 1) / 2), mR = std::min(width, (nextLo - hi) / 2);
            for (size_t v = lo + 1 - mL; v <= lo && mL > 0; ++v)
                if (tip(v))
                    mL = lo - v;
            for (size_t v = hi + 1; v <= hi + mR; ++v)
                if (tip(v))
                    mR = v - hi - 1;
            const size_t a = lo - mL, b = hi + mR;
            auto ease = [](double x) { return x * x * (3.0 - 2.0 * x); };
            share.assign(b - a + 1, 1.0);
            for (size_t q = 0; q < mL; ++q)
                share[q] = ease(double(q + 1) / double(mL + 1));
            for (size_t q = 0; q < mR; ++q)
                share[b - a - q] = ease(double(q + 1) / double(mR + 1));
            // The window's edges and each margin edge's share take one spacing p; the rest keep their own spacing.
            param.resize(b - a + 1);
            for (size_t j = a; j <= b; ++j)
                param[j - a] = density * u[at(j)];
            double U = 0.0, kept = 0.0, eased = 0.0;
            for (size_t j = a; j <= b; ++j)
            {
                U += param[j - a];
                kept += (1.0 - share[j - a]) * param[j - a];
                eased += share[j - a] * l[at(j)];
            }
            if (U <= 0.0 || eased <= 0.0)
                continue;
            const double p = (U - kept) / eased;
            // Vertices a+1..b move to their new distance along the region's uv polyline.
            targets.assign(b - a + 2, 0.0);
            for (size_t v = a + 1; v <= b; ++v)
            {
                const double s = share[v - 1 - a];
                targets[v - a] = targets[v - 1 - a] + (1.0 - s) * param[v - 1 - a] + s * p * l[at(v - 1)];
            }
            targets[b + 1 - a] = U;
            alignWindowTurns(a, b);
            double segStart = 0.0;
            size_t seg = a;
            for (size_t v = a + 1; v <= b; ++v)
            {
                const double target = targets[v - a];
                while (seg < b && segStart + param[seg - a] < target)
                {
                    segStart += param[seg - a];
                    ++seg;
                }
                const double len = param[seg - a];
                const double t = len > 0.0 ? std::clamp((target - segStart) / len, 0.0, 1.0) : 0.0;
                out.push_back({loop[at(v)], loop[at(seg)], loop[at(seg + 1)], t});
            }
        }
    }
    return out;
}

/** An island corner lifted off its border: its value plus (b - a) * lift, values read as u + iv. */
struct CornerApex
{
    Index group, a, b;
    std::complex<double> lift;
};

/** Island corners turning less than this on the surface (radians off straight) are left to the border's spacing. */
constexpr double kCornerTurn = 1.0;
/** A rebuilt corner rises this share of its height over its neighbours' edge on the surface: enough to open its quad. */
constexpr double kApexHeight = 0.5;

/** A uv read as u + iv, so a complex weight scales and rotates it. */
std::complex<double> cx(glm::dvec2 v)
{
    return {v.x, v.y};
}

/**
 * Island corners (one quad of the island at the point) that respacing moved onto its straight uv polyline although they
 * turn on the surface: pinned there, no fill could open their quad. Each rises off the border, square to the edge its two
 * border neighbours make, by part of its height over that edge on the surface, keeping its respaced place along the border.
 * Which corners, and their lift, come from the surface and the spacing proxy; only the island's orientation is read from
 * the uvs, so the weights hold for any layout of the cage.
 */
std::vector<CornerApex> openCorners(const Mesh& m, const Islands& is, Index island, std::span<const glm::vec2> set,
                                    std::span<const BorderBlend> blends)
{
    std::unordered_set<Index> respaced;
    for (const BorderBlend& b : blends)
        respaced.insert(b.group);
    std::vector<CornerApex> out;
    double area = 0.0;
    for (Index f : is.faces[island])
    {
        const std::vector<Index> fc = m.faceCorners(f);
        for (size_t i = 1; i + 1 < fc.size(); ++i)
        {
            const glm::dvec2 u0 = set[fc[0]], u1 = glm::dvec2(set[fc[i]]) - u0, u2 = glm::dvec2(set[fc[i + 1]]) - u0;
            area += u1.x * u2.y - u1.y * u2.x;
        }
    }
    const double side = area < 0.0 ? -1.0 : 1.0;
    for (Index f : is.faces[island])
        m.forEachFaceCorner(f, [&](Index c) {
            const Index g = is.groupOfCorner[c];
            if (!is.border[g] || is.groupCorners[g].size() != 1 || m.faceSize(m.cornerFace(c)) != 4 || !respaced.count(g))
                return;
            const Index ca = m.cornerPrev(c), cb = m.cornerNext(c);
            const glm::dvec3 pa = m.position(m.cornerPoint(ca)), pc = m.position(m.cornerPoint(c)), pb = m.position(m.cornerPoint(cb));
            const glm::dvec3 ab = pb - pa;
            const double ab2 = glm::dot(ab, ab);
            const double ea = glm::length(pa - pc), eb = glm::length(pb - pc);
            if (ab2 <= 0.0 || ea <= 0.0 || eb <= 0.0)
                return;
            if (std::acos(std::clamp(glm::dot(pa - pc, pb - pc) / (ea * eb), -1.0, 1.0)) > std::numbers::pi - kCornerTurn)
                return;
            // -i turns the edge into the island.
            out.push_back({g, is.groupOfCorner[ca], is.groupOfCorner[cb], {0.0, -side * glm::length(glm::cross(pc - pa, ab)) / ab2 * kApexHeight}});
        });
    return out;
}

} // namespace fit
} // namespace

struct FitFill::Impl
{
    fit::SpMat b; // interior x border
    std::shared_ptr<const fit::Factor> factor;
};

namespace
{
namespace fit
{
thread_local FitRecorder* tRecorder = nullptr;

/** The island's record for uv tools: its groups, its border groups as blends of the values before the fit, its fill. */
FitIslandRecord recordIsland(const Islands& is, std::span<const BorderBlend> blends, std::span<const CornerApex> apexes, const System& s,
                             std::shared_ptr<const Factor> factor)
{
    FitIslandRecord r;
    std::unordered_map<Index, Index> local;
    auto id = [&](Index g) {
        auto [it, added] = local.emplace(g, Index(r.groups.size()));
        if (added)
            r.groups.push_back(is.groupCorners[g]);
        return it->second;
    };
    for (Index g : s.border)
        id(g);
    for (Index g : s.interior)
        id(g);
    // Respacing reads the values before it, the corners read respaced ones.
    using Blend = std::vector<std::pair<Index, std::complex<double>>>;
    std::unordered_map<Index, Blend> from;
    auto value = [&](Index g) {
        auto it = from.find(g);
        return it != from.end() ? it->second : Blend{{id(g), 1.0}};
    };
    std::vector<std::pair<Index, Blend>> moved;
    for (const BorderBlend& b : blends)
        moved.push_back({b.group, {{id(b.a), 1.0 - b.t}, {id(b.b), b.t}}});
    for (auto& [g, v] : moved)
        from[g] = std::move(v);
    moved.clear();
    for (const CornerApex& x : apexes)
    {
        Blend v;
        for (auto [g, w] : {std::pair{x.group, std::complex<double>(1.0)}, std::pair{x.a, -x.lift}, std::pair{x.b, x.lift}})
            for (auto [q, wq] : value(g))
                v.push_back({q, w * wq});
        moved.push_back({x.group, std::move(v)});
    }
    for (auto& [g, v] : moved)
        from[g] = std::move(v);
    for (Index g : s.border)
    {
        r.border.push_back(id(g));
        r.borderFrom.push_back(value(g));
    }
    // An island with no interior lists no border groups in its system, but its moved groups still move.
    for (const auto& [g, v] : from)
        if (std::find(s.border.begin(), s.border.end(), g) == s.border.end())
        {
            r.border.push_back(id(g));
            r.borderFrom.push_back(v);
        }
    if (factor && !s.interior.empty())
    {
        for (Index g : s.interior)
            r.interior.push_back(id(g));
        auto impl = std::make_shared<FitFill::Impl>();
        impl->b = s.b;
        impl->factor = std::move(factor);
        r.fill = std::make_shared<const FitFill>(std::move(impl));
    }
    return r;
}

/** Prefix of the layers that carry each uv island's border development through the subdivision. */
constexpr std::string_view kSpacingProxy = "__fitSpacing@";

/** An island layer's border loops, each a cycle of border groups with the island on its left. Pinched loops are left out. */
std::vector<std::vector<Index>> borderLoops(const Mesh& m, const Islands& is)
{
    auto cornerAt = [&](Index c, Index p) { return m.cornerPoint(c) == p ? c : m.cornerNext(c); };
    std::unordered_map<Index, Index> next;
    std::unordered_set<Index> pinched;
    for (Index f = 0; f < m.faceCapacity(); ++f)
        if (m.faceValid(f))
            m.forEachFaceCorner(f, [&](Index c) {
                const Index e = m.cornerEdge(c), cn = m.cornerNext(c);
                if (m.isManifoldEdge(e))
                {
                    const Index ec = m.edgeCorner(e), rc = m.cornerFace(ec) == f ? m.cornerRadial(ec) : ec;
                    if (is.groupOfCorner[cornerAt(rc, m.cornerPoint(c))] == is.groupOfCorner[c] &&
                        is.groupOfCorner[cornerAt(rc, m.cornerPoint(cn))] == is.groupOfCorner[cn])
                        return;
                }
                if (!next.emplace(is.groupOfCorner[c], is.groupOfCorner[cn]).second)
                    pinched.insert(is.groupOfCorner[c]);
            });
    std::vector<std::vector<Index>> out;
    std::unordered_set<Index> seen;
    for (const auto& [first, unused] : next)
    {
        if (seen.count(first))
            continue;
        std::vector<Index> loop;
        bool closed = false, clean = true;
        for (Index g = first;;)
        {
            seen.insert(g);
            loop.push_back(g);
            clean = clean && !pinched.count(g);
            const auto it = next.find(g);
            if (it == next.end())
                break;
            g = it->second;
            closed = g == first;
            if (closed || seen.count(g))
                break;
        }
        if (closed && clean && loop.size() >= 3)
            out.push_back(std::move(loop));
    }
    return out;
}

/** The island's surface angle at a group: the sum of its faces' corner angles there. */
double angleSum(const Mesh& m, const Islands& is, Index g)
{
    double sum = 0.0;
    for (Index c : is.groupCorners[g])
    {
        const glm::dvec3 p = m.position(m.cornerPoint(c));
        const glm::dvec3 a = m.position(m.cornerPoint(m.cornerNext(c))) - p, b = m.position(m.cornerPoint(m.cornerPrev(c))) - p;
        const double la = glm::length(a), lb = glm::length(b);
        if (la > 0.0 && lb > 0.0)
            sum += std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0));
    }
    return sum;
}

/**
 * The island's groups flattened from the surface alone: the least squares conformal map of its faces (fanned into
 * triangles), two far apart border groups pinned at their surface distance. Places loops of an island with holes against
 * each other. Empty if the solve fails.
 */
std::unordered_map<Index, glm::dvec2> conformalPlaces(const Mesh& m, const Islands& is, Index island)
{
    std::vector<std::array<Index, 3>> tris;
    std::vector<Index> fc;
    std::unordered_map<Index, int> col;
    std::vector<Index> groups;
    for (Index f : is.faces[island])
    {
        fc = m.faceCorners(f);
        for (Index c : fc)
            if (col.emplace(is.groupOfCorner[c], -1).second)
                groups.push_back(is.groupOfCorner[c]);
        for (size_t k = 1; k + 1 < fc.size(); ++k)
            tris.push_back({fc[0], fc[k], fc[k + 1]});
    }
    auto pos = [&](Index g) { return m.position(m.cornerPoint(is.groupCorners[g].front())); };
    Index p0 = kInvalid, p1 = kInvalid;
    for (Index g : groups)
        if (is.border[g])
        {
            p0 = g;
            break;
        }
    if (p0 == kInvalid)
        return {};
    double far = 0.0;
    for (Index g : groups)
        if (const double d = glm::length(pos(g) - pos(p0)); is.border[g] && d > far)
        {
            far = d;
            p1 = g;
        }
    if (p1 == kInvalid)
        return {};
    int n = 0;
    for (Index g : groups)
        if (g != p0 && g != p1)
            col[g] = n++;
    const auto pinOf = [&](Index g) { return g == p0 ? glm::dvec2(0.0) : glm::dvec2(far, 0.0); };
    std::vector<Eigen::Triplet<double>> trips;
    std::vector<double> rhs;
    for (const auto& t : tris)
    {
        const glm::dvec3 a = m.position(m.cornerPoint(t[0])), e1 = m.position(m.cornerPoint(t[1])) - a, e2 = m.position(m.cornerPoint(t[2])) - a;
        const double l1 = glm::length(e1), twice = glm::length(glm::cross(e1, e2));
        if (l1 <= 0.0 || twice <= 1e-300)
            continue;
        const glm::dvec3 x = e1 / l1, y = glm::normalize(glm::cross(glm::cross(e1, e2), e1));
        const glm::dvec2 q[3] = {{0.0, 0.0}, {l1, 0.0}, {glm::dot(e2, x), glm::dot(e2, y)}};
        const double w = 1.0 / std::sqrt(twice);
        const int row = int(rhs.size());
        rhs.push_back(0.0);
        rhs.push_back(0.0);
        for (int k = 0; k < 3; ++k)
        {
            // The rows are Re and Im of sum (q_{k+2} - q_{k+1}) (u_k + i v_k).
            const glm::dvec2 d = (q[(k + 2) % 3] - q[(k + 1) % 3]) * w;
            const Index g = is.groupOfCorner[t[k]];
            if (const int c = col[g]; c >= 0)
            {
                trips.emplace_back(row, c, d.x);
                trips.emplace_back(row, n + c, -d.y);
                trips.emplace_back(row + 1, c, d.y);
                trips.emplace_back(row + 1, n + c, d.x);
            }
            else
            {
                const glm::dvec2 v = pinOf(g);
                rhs[size_t(row)] -= d.x * v.x - d.y * v.y;
                rhs[size_t(row) + 1] -= d.y * v.x + d.x * v.y;
            }
        }
    }
    if (n == 0 || rhs.empty())
        return {};
    SpMat a(Eigen::Index(rhs.size()), 2 * n);
    a.setFromTriplets(trips.begin(), trips.end());
    const SpMat at = a.transpose();
    Eigen::SimplicialLDLT<SpMat> ldlt(at * a);
    if (ldlt.info() != Eigen::Success)
        return {};
    const Eigen::VectorXd sol = ldlt.solve(at * Eigen::Map<const Eigen::VectorXd>(rhs.data(), Eigen::Index(rhs.size())));
    if (!sol.allFinite())
        return {};
    std::unordered_map<Index, glm::dvec2> out;
    for (Index g : groups)
        out[g] = col[g] >= 0 ? glm::dvec2(sol[col[g]], sol[n + col[g]]) : pinOf(g);
    return out;
}

/** Turns and moves a loop's places onto where the conformal places put its groups, by least squares. */
void alignLoop(std::vector<glm::dvec2>& places, std::span<const Index> loop, const std::unordered_map<Index, glm::dvec2>& target)
{
    glm::dvec2 ca(0.0), cb(0.0);
    for (size_t i = 0; i < loop.size(); ++i)
    {
        ca += places[i];
        cb += target.at(loop[i]);
    }
    ca /= double(loop.size());
    cb /= double(loop.size());
    double sc = 0.0, ss = 0.0;
    for (size_t i = 0; i < loop.size(); ++i)
    {
        const glm::dvec2 a = places[i] - ca, b = target.at(loop[i]) - cb;
        sc += glm::dot(a, b);
        ss += a.x * b.y - a.y * b.x;
    }
    const double r = std::hypot(sc, ss);
    const double c = r > 0.0 ? sc / r : 1.0, s = r > 0.0 ? ss / r : 0.0;
    for (glm::dvec2& p : places)
    {
        const glm::dvec2 a = p - ca;
        p = cb + glm::dvec2(c * a.x - s * a.y, s * a.x + c * a.y);
    }
}

/**
 * One border loop laid into the plane from its edges' surface lengths and the turn at each of its points (turn[i] at
 * loop[i]). The loop turns a full turn in all (the other way round a hole); what is left over spreads by length over the
 * edges walked once, and the lengths of those edges give a little, by least squares, until the loop closes. Edges walked
 * both ways (a slit's two sides) keep their length and turns, so the slit stays as the surface has it.
 */
std::vector<glm::dvec2> layLoop(const Mesh& m, const Islands& is, std::span<const Index> loop, std::span<const double> turn, bool hole)
{
    const size_t n = loop.size();
    std::vector<double> l(n), once(n);
    std::set<std::pair<Index, Index>> walked;
    auto point = [&](size_t i) { return m.cornerPoint(is.groupCorners[loop[i % n]].front()); };
    for (size_t i = 0; i < n; ++i)
        walked.insert({point(i), point(i + 1)});
    double total = 0.0, turned = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        l[i] = glm::length(m.position(point(i + 1)) - m.position(point(i)));
        once[i] = walked.count({point(i + 1), point(i)}) ? 0.0 : l[i];
        total += once[i];
        turned += turn[i];
    }
    if (!(total > 0.0))
        return {};
    const double rest = (hole ? -2.0 : 2.0) * std::numbers::pi - turned;
    std::vector<glm::dvec2> dir(n);
    double heading = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        heading += turn[i] + rest * 0.5 * (once[(i + n - 1) % n] + once[i]) / total;
        dir[i] = {std::cos(heading), std::sin(heading)};
    }
    // Closest lengths (weighted by 1 / length) whose edges sum to nothing: l - W D^T (D W D^T)^-1 D l.
    glm::dvec2 gap(0.0);
    double sxx = 0.0, sxy = 0.0, syy = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        gap += l[i] * dir[i];
        sxx += once[i] * dir[i].x * dir[i].x;
        sxy += once[i] * dir[i].x * dir[i].y;
        syy += once[i] * dir[i].y * dir[i].y;
    }
    const double det = sxx * syy - sxy * sxy;
    glm::dvec2 lambda(0.0);
    if (std::abs(det) > 1e-12 * total * total)
        lambda = glm::dvec2(syy * gap.x - sxy * gap.y, sxx * gap.y - sxy * gap.x) / det;
    std::vector<glm::dvec2> out(n, glm::dvec2(0.0));
    for (size_t i = 0; i + 1 < n; ++i)
        out[i + 1] = out[i] + std::max(0.0, l[i] - once[i] * glm::dot(dir[i], lambda)) * dir[i];
    return out;
}

/**
 * Per island border group, the turn its border makes when the island is flattened keeping every border edge's surface
 * length and spreading the inside's curvature as a conformal scale (boundary first flattening): the surface's own turn
 * there, pi less the island's angle sum, plus what the scale that flattens the inside bends it by.
 */
std::unordered_map<Index, double> flattenedTurns(const Mesh& m, const Islands& is, const System& sys)
{
    std::unordered_map<Index, double> out;
    for (Index g : sys.border)
        out[g] = std::numbers::pi - angleSum(m, is, g);
    if (sys.interior.empty())
        return out;
    Eigen::VectorXd k(Eigen::Index(sys.interior.size()));
    for (size_t i = 0; i < sys.interior.size(); ++i)
        k[Eigen::Index(i)] = 2.0 * std::numbers::pi - angleSum(m, is, sys.interior[i]);
    const std::shared_ptr<const Factor> f = factorCache().get(sys.a);
    if (!f->ok)
        return out;
    // u = -A^-1 K flattens the inside; the border turns by L_BI u = -b^T u more.
    const Eigen::VectorXd bend = sys.b.transpose() * f->ldlt.solve(k);
    if (!bend.allFinite())
        return out;
    for (size_t i = 0; i < sys.border.size(); ++i)
        out[sys.border[i]] += bend[Eigen::Index(i)];
    return out;
}

/**
 * Adds, per island layer, a layer holding each island laid flat from its surface alone: border loops as long as on the
 * surface, turning as a flattening that spreads the inside's curvature turns them (see flattenedTurns, layLoop), holes
 * placed as the island's conformal map has them, and the inside as the harmonic fill of the border. Blended and subdivided
 * within the same islands by the same rules, its spacing along a border is the island layer's for an even layout, so
 * respacing finds where the border bunches without reading how the cage is laid out.
 */
void addSpacingProxies(Mesh& m)
{
    std::set<std::string> islandLayers;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            islandLayers.insert(islandLayer(m, a).name());
    for (const std::string& name : islandLayers)
    {
        const Islands is = islandsOf(m, *m.cornerAttrs().find(name));
        std::span<glm::vec2> proxy = m.cornerAttrs().getOrAdd<glm::vec2>(std::string(kSpacingProxy) + name);
        const float nan = std::numeric_limits<float>::quiet_NaN();
        for (Index g = 0; g < is.groupCorners.size(); ++g)
            for (Index c : is.groupCorners[g])
                proxy[c] = is.border[g] ? glm::vec2(nan) : glm::vec2(0.0f);
        const std::vector<std::vector<Index>> loops = borderLoops(m, is);
        std::vector<std::vector<size_t>> loopsOf(is.faces.size());
        for (size_t k = 0; k < loops.size(); ++k)
            loopsOf[is.islandOfGroup[loops[k].front()]].push_back(k);
        tbb::parallel_for(Index(0), Index(is.faces.size()), [&](Index island) {
            const std::vector<size_t>& mine = loopsOf[island];
            if (mine.empty())
                return;
            const System sys = systemOf(m, is, island);
            const std::unordered_map<Index, double> turns = flattenedTurns(m, is, sys);
            // Loops of an island with holes are placed against each other as its conformal map has them; the outer one
            // encloses the most area there.
            std::unordered_map<Index, glm::dvec2> flat;
            size_t outer = mine.front();
            if (mine.size() > 1)
            {
                flat = conformalPlaces(m, is, island);
                if (flat.empty())
                    return;
                double most = -1.0;
                for (size_t k : mine)
                {
                    double a = 0.0;
                    const std::vector<Index>& lp = loops[k];
                    for (size_t i = 0; i < lp.size(); ++i)
                    {
                        const glm::dvec2 p = flat.at(lp[i]), q = flat.at(lp[(i + 1) % lp.size()]);
                        a += p.x * q.y - p.y * q.x;
                    }
                    if (std::abs(a) > most)
                    {
                        most = std::abs(a);
                        outer = k;
                    }
                }
            }
            std::vector<double> turn;
            for (size_t k : mine)
            {
                turn.clear();
                for (Index g : loops[k])
                {
                    const auto it = turns.find(g);
                    turn.push_back(it != turns.end() ? it->second : std::numbers::pi - angleSum(m, is, g));
                }
                std::vector<glm::dvec2> places = layLoop(m, is, loops[k], turn, k != outer);
                if (places.empty())
                    return;
                if (!flat.empty())
                    alignLoop(places, loops[k], flat);
                for (size_t i = 0; i < loops[k].size(); ++i)
                    for (Index c : is.groupCorners[loops[k][i]])
                        proxy[c] = glm::vec2(places[i]);
            }
            // Inside as the harmonic fill of the border, so bevel points blended off the border land near it.
            if (sys.interior.empty() || sys.border.empty())
                return;
            Eigen::MatrixXd xb(Eigen::Index(sys.border.size()), 2);
            for (size_t k = 0; k < sys.border.size(); ++k)
            {
                const glm::vec2 v = proxy[is.groupCorners[sys.border[k]].front()];
                if (!std::isfinite(v.x) || !std::isfinite(v.y))
                    return;
                xb(Eigen::Index(k), 0) = v.x;
                xb(Eigen::Index(k), 1) = v.y;
            }
            const std::shared_ptr<const Factor> f = factorCache().get(sys.a);
            if (!f->ok)
                return;
            const Eigen::MatrixXd xi = f->ldlt.solve(sys.b * xb);
            if (!xi.allFinite())
                return;
            for (size_t k = 0; k < sys.interior.size(); ++k)
                for (Index c : is.groupCorners[sys.interior[k]])
                    proxy[c] = glm::vec2(float(xi(Eigen::Index(k), 0)), float(xi(Eigen::Index(k), 1)));
        });
    }
}

/** Replaces each uv island's interior with the harmonic fill of its border, in every layer using those islands. */
void fitSurface(Mesh& m)
{
    LILA_ZONE();

    std::map<std::string, std::vector<glm::vec2>> proxies;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.name().starts_with(kSpacingProxy))
        {
            const std::span<const glm::vec2> v = a.values<glm::vec2>();
            proxies[a.name().substr(kSpacingProxy.size())].assign(v.begin(), v.end());
        }
    for (const auto& [name, unused] : proxies)
        m.cornerAttrs().remove(std::string(kSpacingProxy) + name);
    std::map<std::string, std::vector<std::string>> byIslands;
    for (size_t l = 0; l < m.cornerAttrs().layerCount(); ++l)
        if (const Attribute& a = m.cornerAttrs().layer(l); a.type() == AttrType::Float2)
            byIslands[islandLayer(m, a).name()].push_back(a.name());
    for (const auto& [islandName, layers] : byIslands)
    {
        const Islands is = islandsOf(m, *m.cornerAttrs().find(islandName));
        std::vector<std::span<glm::vec2>> spans;
        for (const std::string& l : layers)
            spans.push_back(m.cornerAttrs().get<glm::vec2>(l));
        const std::span<const glm::vec2> set = m.cornerAttrs().find(islandName)->values<glm::vec2>();
        const auto proxyIt = proxies.find(islandName);
        if (proxyIt == proxies.end())
            continue;
        const std::vector<glm::vec2>& proxy = proxyIt->second;
        FitRecord* record = nullptr;
        if (tRecorder)
        {
            record = &tRecorder->records.emplace_back();
            record->islandLayer = islandName;
            record->cornerCapacity = m.cornerCapacity();
            record->islands.resize(is.faces.size());
        }
        // Islands share no corners, so each writes only its own groups while the others read theirs.
        tbb::parallel_for(Index(0), Index(is.faces.size()), [&](Index island) {
            const std::vector<BorderBlend> blends = respaceBorder(m, is, island, proxy, set);
            std::vector<glm::vec2> moved(blends.size());
            // The island's orientation, all openCorners reads of the layout, is the same before and after respacing.
            const std::vector<CornerApex> apexes = openCorners(m, is, island, set, blends);
            if (!record)
                for (std::span<glm::vec2> uv : spans)
                {
                    for (size_t k = 0; k < blends.size(); ++k)
                        moved[k] = glm::vec2(glm::mix(groupValue(uv, is.groupCorners[blends[k].a]), groupValue(uv, is.groupCorners[blends[k].b]), blends[k].t));
                    for (size_t k = 0; k < blends.size(); ++k)
                        for (Index c : is.groupCorners[blends[k].group])
                            uv[c] = moved[k];
                }
            const System s = systemOf(m, is, island);
            std::shared_ptr<const Factor> f;
            if (!s.interior.empty() && !s.border.empty())
                if (f = factorCache().get(s.a); !f->ok)
                    f.reset();
            if (record)
            {
                record->islands[island] = recordIsland(is, blends, apexes, s, f);
                return;
            }
            moved.resize(apexes.size());
            for (std::span<glm::vec2> uv : spans)
            {
                for (size_t k = 0; k < apexes.size(); ++k)
                {
                    const CornerApex& x = apexes[k];
                    const std::complex<double> v =
                        cx(groupValue(uv, is.groupCorners[x.group])) + (cx(groupValue(uv, is.groupCorners[x.b])) - cx(groupValue(uv, is.groupCorners[x.a]))) * x.lift;
                    moved[k] = glm::vec2(float(v.real()), float(v.imag()));
                }
                for (size_t k = 0; k < apexes.size(); ++k)
                    for (Index c : is.groupCorners[apexes[k].group])
                        uv[c] = moved[k];
            }
            if (!f)
                return;
            const auto nb = Eigen::Index(s.border.size()), cols = Eigen::Index(spans.size()) * 2;
            Eigen::MatrixXd xb(nb, cols);
            for (size_t l = 0; l < spans.size(); ++l)
                for (Eigen::Index k = 0; k < nb; ++k)
                {
                    const glm::dvec2 v = groupValue(spans[l], is.groupCorners[s.border[size_t(k)]]);
                    xb(k, Eigen::Index(l) * 2) = v.x;
                    xb(k, Eigen::Index(l) * 2 + 1) = v.y;
                }
            const Eigen::MatrixXd rhs = s.b * xb;
            const Eigen::MatrixXd xi = f->ldlt.solve(rhs);
            if (!xi.allFinite())
                return;
            for (size_t l = 0; l < spans.size(); ++l)
                for (size_t k = 0; k < s.interior.size(); ++k)
                {
                    const auto r = Eigen::Index(k), col = Eigen::Index(l) * 2;
                    const glm::vec2 v(float(xi(r, col)), float(xi(r, col + 1)));
                    for (Index c : is.groupCorners[s.interior[k]])
                        spans[l][c] = v;
                }
        });
    }
}
} // namespace fit
} // namespace

FitFill::FitFill(std::shared_ptr<const Impl> impl) : mImpl(std::move(impl)) {}
FitFill::~FitFill() = default;

size_t FitFill::borderCount() const
{
    return size_t(mImpl->b.cols());
}

size_t FitFill::interiorCount() const
{
    return size_t(mImpl->b.rows());
}

void FitFill::solve(std::span<const double> border, std::span<double> interior, size_t columns) const
{
    const auto nb = mImpl->b.cols(), ni = mImpl->b.rows(), k = Eigen::Index(columns);
    const Eigen::Map<const Eigen::MatrixXd> x(border.data(), nb, k);
    Eigen::Map<Eigen::MatrixXd> y(interior.data(), ni, k);
    y = mImpl->factor->ldlt.solve(mImpl->b * x);
}

void FitFill::solveTransposed(std::span<const double> g, std::span<double> border, size_t columns) const
{
    const auto nb = mImpl->b.cols(), ni = mImpl->b.rows(), k = Eigen::Index(columns);
    const Eigen::Map<const Eigen::MatrixXd> x(g.data(), ni, k);
    Eigen::Map<Eigen::MatrixXd> y(border.data(), nb, k);
    // The system is symmetric, so the transpose of A^-1 B is B^T A^-1.
    y = mImpl->b.transpose() * mImpl->factor->ldlt.solve(x);
}

FitRecorder::FitRecorder() : mOuter(fit::tRecorder)
{
    fit::tRecorder = this;
}

FitRecorder::~FitRecorder()
{
    fit::tRecorder = mOuter;
}

void addFitSpacing(Mesh& mesh)
{
    LILA_ZONE();

    removeFitSpacing(mesh);
    fit::addSpacingProxies(mesh);
}

bool hasFitSpacing(const Mesh& mesh)
{
    for (size_t l = 0; l < mesh.cornerAttrs().layerCount(); ++l)
        if (mesh.cornerAttrs().layer(l).name().starts_with(fit::kSpacingProxy))
            return true;
    return false;
}

void removeFitSpacing(Mesh& mesh)
{
    std::vector<std::string> drop;
    for (size_t l = 0; l < mesh.cornerAttrs().layerCount(); ++l)
        if (const std::string& n = mesh.cornerAttrs().layer(l).name(); n.starts_with(fit::kSpacingProxy))
            drop.push_back(n);
    for (const std::string& n : drop)
        mesh.cornerAttrs().remove(n);
}

Mesh subdivide(const Mesh& mesh, const SubdivideOptions& opts, SubdivideMap* map)
{
    LILA_ZONE();

    const bool fitting = opts.uvs == SubdivideUvs::FitSurface && opts.levels > 0;
    Mesh cur = mesh;
    if (!fitting)
        removeFitSpacing(cur);
    else if (!hasFitSpacing(cur))
        fit::addSpacingProxies(cur);
    for (int i = 0; i < opts.levels; ++i)
    {
        if (!map)
        {
            cur = subdivideOnce(cur, opts);
            continue;
        }
        SubdivideMap once;
        cur = subdivideOnce(cur, opts, &once);
        if (i == 0)
        {
            *map = std::move(once);
            continue;
        }
        // Compose: the cage's points and face centres stay put, and each new piece keeps its parent's cage edge or face.
        for (Index& p : map->points)
            if (p != kInvalid)
                p = once.points[p];
        for (Index& p : map->facePoints)
            if (p != kInvalid)
                p = once.points[p];
        for (Index& e : once.edges)
            if (e != kInvalid)
                e = map->edges[e];
        for (Index& f : once.faces)
            if (f != kInvalid)
                f = map->faces[f];
        map->edges = std::move(once.edges);
        map->faces = std::move(once.faces);
    }
    if (fitting)
        fit::fitSurface(cur);
    if (!opts.keepHardEdges && opts.levels > 0)
    {
        cur.edgeAttrs().remove(attr::kHard);
        cur.cornerAttrs().remove(attr::kNormal);
    }
    if (map)
    {
        map->edges.resize(cur.edgeCapacity(), kInvalid);
        map->faces.resize(cur.faceCapacity(), kInvalid);
    }
    return cur;
}

std::vector<glm::dvec3> PointStencil::apply(std::span<const glm::dvec3> in) const
{
    LILA_ZONE();

    std::vector<glm::dvec3> out(rows());
    tbb::parallel_for(tbb::blocked_range<Index>(0, rows(), kRefineGrain), [&](const tbb::blocked_range<Index>& r) {
        for (Index i = r.begin(); i < r.end(); ++i)
        {
            glm::dvec3 sum(0.0);
            for (Index k = offsets[i]; k < offsets[i + 1]; ++k)
                sum += in[cols[k]] * weights[k];
            out[i] = sum;
        }
    });
    return out;
}

namespace
{
/** subdivideOnce's position rules as weights instead of positions. Must stay in step with it. */
PointStencil stencilOnce(const Mesh& src, const SubdivideOptions& opts)
{
    LILA_ZONE();

    const Index pc = src.pointCapacity(), ec = src.edgeCapacity(), fc = src.faceCapacity();
    std::span<const float> crease = src.edgeAttrs().get<float>(attr::kCrease);
    auto sharpness = [&](Index e) -> double {
        if (!src.isManifoldEdge(e))
            return 1.0;
        return crease.empty() ? 0.0 : std::clamp(double(crease[e]), 0.0, 1.0);
    };

    using Terms = std::vector<std::pair<Index, double>>;
    auto addFace = [&](Terms& t, Index f, double w) {
        const double each = w / double(src.faceSize(f));
        src.forEachFaceCorner(f, [&](Index c) { t.push_back({src.cornerPoint(c), each}); });
    };

    std::vector<Terms> rows;
    rows.reserve(size_t(src.numPoints()) + src.numEdges() + src.numFaces());
    for (Index p = 0; p < pc; ++p)
    {
        if (!src.pointValid(p))
            continue;
        Terms t;
        int n = 0, nf = 0, nSharp = 0;
        double sharpSum = 0.0;
        Terms creaseNbrs;
        src.forEachPointEdge(p, [&](Index e) {
            ++n;
            const double s = sharpness(e);
            if (s > 0.0)
            {
                ++nSharp;
                sharpSum += s;
                creaseNbrs.push_back({src.edgeOtherPoint(e, p), 1.0});
            }
        });
        src.forEachPointCorner(p, [&](Index) { ++nf; });
        if (n == 0 || nf == 0)
        {
            rows.push_back({{p, 1.0}});
            continue;
        }
        // smooth = (F/nf + 2R/n + (n-3)P) / n, R the edge midpoints' sum and F the face points' sum.
        Terms smooth;
        if (n >= 3 && nf == n)
        {
            const double inv = 1.0 / double(n);
            src.forEachPointCorner(p, [&](Index c) { addFace(smooth, src.cornerFace(c), inv / double(nf)); });
            src.forEachPointEdge(p, [&](Index e) {
                smooth.push_back({p, inv * inv});
                smooth.push_back({src.edgeOtherPoint(e, p), inv * inv});
            });
            smooth.push_back({p, double(n - 3) * inv});
        }
        else
            smooth.push_back({p, 1.0});
        const bool boundaryCorner = nf < n && n == 2;
        double mix = 0.0;
        Terms sharp;
        if (nSharp == 2 && !(boundaryCorner && opts.sharpBoundary))
        {
            mix = sharpSum / 2.0;
            sharp.push_back({p, 6.0 / 8.0});
            for (auto& [q, w] : creaseNbrs)
                sharp.push_back({q, 1.0 / 8.0});
        }
        else if (nSharp > 2 || (boundaryCorner && opts.sharpBoundary))
        {
            mix = nSharp ? std::min(1.0, sharpSum / nSharp) : 1.0;
            sharp.push_back({p, 1.0});
        }
        for (auto& [q, w] : smooth)
            t.push_back({q, w * (1.0 - mix)});
        for (auto& [q, w] : sharp)
            t.push_back({q, w * mix});
        rows.push_back(std::move(t));
    }
    for (Index e = 0; e < ec; ++e)
    {
        if (!src.edgeValid(e))
            continue;
        const double s = sharpness(e);
        Terms t{{src.edgePoint(e, 0), 0.5 * s}, {src.edgePoint(e, 1), 0.5 * s}};
        if (s < 1.0)
        {
            const double k = 1.0 - s;
            t.push_back({src.edgePoint(e, 0), 0.25 * k});
            t.push_back({src.edgePoint(e, 1), 0.25 * k});
            const Index c = src.edgeCorner(e);
            addFace(t, src.cornerFace(c), 0.25 * k);
            addFace(t, src.cornerFace(src.cornerRadial(c)), 0.25 * k);
        }
        rows.push_back(std::move(t));
    }
    for (Index f = 0; f < fc; ++f)
        if (src.faceValid(f))
        {
            Terms t;
            addFace(t, f, 1.0);
            rows.push_back(std::move(t));
        }

    PointStencil out;
    out.inputs = pc;
    for (Terms& t : rows)
    {
        std::sort(t.begin(), t.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < t.size();)
        {
            size_t j = i;
            double w = 0.0;
            for (; j < t.size() && t[j].first == t[i].first; ++j)
                w += t[j].second;
            if (w != 0.0)
            {
                out.cols.push_back(t[i].first);
                out.weights.push_back(w);
            }
            i = j;
        }
        out.offsets.push_back(Index(out.cols.size()));
    }
    return out;
}
} // namespace

std::vector<PointStencil> subdivideStencils(const Mesh& mesh, const SubdivideOptions& opts, Mesh* result)
{
    LILA_ZONE();

    std::vector<PointStencil> out;
    Mesh cur = mesh;
    for (int i = 0; i < opts.levels; ++i)
    {
        out.push_back(stencilOnce(cur, opts));
        cur = subdivideOnce(cur, opts);
    }
    if (result)
        *result = std::move(cur);
    return out;
}
} // namespace lila::geo
