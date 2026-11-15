#include "retopo/Internal.h"

#include "core/Profile.h"
#include "geo/Triangulate.h"

#include <meshoptimizer.h>
#include <tbb/parallel_for.h>

#include <glm/gtc/constants.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <numeric>
#include <unordered_map>

namespace lila::retopo::detail
{
void Surface::buildTopology()
{
    LILA_ZONE();

    struct HalfEdge
    {
        uint64_t key;
        Index tri;
    };
    std::vector<HalfEdge> he;
    he.reserve(tris.size() * 3);
    for (Index t = 0; t < Index(tris.size()); ++t)
        for (int k = 0; k < 3; ++k)
            he.push_back({edgeKey(tris[t][k], tris[t][(k + 1) % 3]), t});
    std::sort(he.begin(), he.end(), [](const HalfEdge& a, const HalfEdge& b) { return a.key < b.key || (a.key == b.key && a.tri < b.tri); });

    edges.clear();
    edgeTris.clear();
    edgeTriCount.clear();
    for (size_t i = 0; i < he.size();)
    {
        size_t j = i;
        while (j < he.size() && he[j].key == he[i].key)
            ++j;
        edges.push_back({Index(he[i].key >> 32), Index(he[i].key & 0xffffffffu)});
        edgeTris.push_back({he[i].tri, j - i > 1 ? he[i + 1].tri : kInvalid});
        edgeTriCount.push_back(uint8_t(std::min<size_t>(j - i, 3)));
        i = j;
    }

    const Index n = vertexCount();
    adjStart.assign(n + 1, 0);
    for (const auto& e : edges)
    {
        ++adjStart[e[0] + 1];
        ++adjStart[e[1] + 1];
    }
    for (Index v = 0; v < n; ++v)
        adjStart[v + 1] += adjStart[v];
    adj.assign(adjStart[n], 0);
    adjEdge.assign(adjStart[n], 0);
    std::vector<Index> fill(adjStart.begin(), adjStart.end() - 1);
    for (Index e = 0; e < Index(edges.size()); ++e)
    {
        Index a = edges[e][0], b = edges[e][1];
        adj[fill[a]] = b;
        adjEdge[fill[a]++] = e;
        adj[fill[b]] = a;
        adjEdge[fill[b]++] = e;
    }
}

void Surface::buildGeometry()
{
    LILA_ZONE();

    triNormal.assign(tris.size(), glm::dvec3(0.0));
    triArea.assign(tris.size(), 0.0);
    tbb::parallel_for(size_t(0), tris.size(), [&](size_t t) {
        const auto& f = tris[t];
        glm::dvec3 c = glm::cross(pos[f[1]] - pos[f[0]], pos[f[2]] - pos[f[0]]);
        double len = glm::length(c);
        triArea[t] = 0.5 * len;
        triNormal[t] = len > 0.0 ? c / len : glm::dvec3(0.0);
    });
    normal.assign(pos.size(), glm::dvec3(0.0));
    area.assign(pos.size(), 0.0);
    for (size_t t = 0; t < tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
        {
            // Angle weights keep normals stable on long thin triangles.
            const glm::dvec3& p = pos[tris[t][k]];
            glm::dvec3 a = pos[tris[t][(k + 1) % 3]] - p, b = pos[tris[t][(k + 2) % 3]] - p;
            double la = glm::length(a), lb = glm::length(b);
            double ang = la > 0.0 && lb > 0.0 ? std::acos(std::clamp(glm::dot(a, b) / (la * lb), -1.0, 1.0)) : 0.0;
            normal[tris[t][k]] += triNormal[t] * ang;
            area[tris[t][k]] += triArea[t] / 3.0;
        }
    for (size_t v = 0; v < pos.size(); ++v)
    {
        double len = glm::length(normal[v]);
        normal[v] = len > 1e-300 ? normal[v] / len : glm::dvec3(0, 1, 0);
    }
}

void Surface::dropShortCreases(const std::vector<uint8_t>& byAngle, double minCreaseLength)
{
    constexpr double kRuns = 3.0;
    std::vector<Index> parent(vertexCount());
    std::iota(parent.begin(), parent.end(), Index(0));
    const auto find = [&](Index x) {
        while (parent[x] != x)
            x = parent[x] = parent[parent[x]];
        return x;
    };
    for (size_t e = 0; e < edges.size(); ++e)
        if (edgeFeature[e] && byAngle[e])
            parent[find(edges[e][0])] = find(edges[e][1]);
    std::vector<double> length(vertexCount(), 0.0);
    for (size_t e = 0; e < edges.size(); ++e)
        if (edgeFeature[e] && byAngle[e])
            length[find(edges[e][0])] += glm::length(pos[edges[e][1]] - pos[edges[e][0]]);
    for (size_t e = 0; e < edges.size(); ++e)
        if (edgeFeature[e] && byAngle[e] && length[find(edges[e][0])] < kRuns * minCreaseLength)
            edgeFeature[e] = 0;
    if (std::getenv("LILA_CREASE_DEBUG"))
        for (Index v = 0; v < vertexCount(); ++v)
            if (find(v) == v && length[v] > 0.0)
                std::fprintf(stderr, "crease run at %.3f %.3f %.3f: %.3f long (%.2fx min)%s\n", pos[v].x, pos[v].y, pos[v].z, length[v],
                             length[v] / minCreaseLength, length[v] < kRuns * minCreaseLength ? " dropped" : "");
}

void Surface::detectFeatures(double sharpAngleDeg, double cornerDeficitDeg, double minCreaseLength)
{
    LILA_ZONE();

    const double cosSharp = sharpAngleDeg >= 0.0 ? std::cos(glm::radians(sharpAngleDeg)) : -2.0;
    // Creases are judged on bilaterally filtered face normals: facet noise averages away, while across a real crease
    // the normals differ too much to mix.
    std::vector<glm::dvec3> filtered = triNormal;
    {
        std::vector<std::array<Index, 3>> across(tris.size(), {kInvalid, kInvalid, kInvalid});
        std::vector<uint8_t> fill(tris.size(), 0);
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeTriCount[e] == 2)
            {
                const Index a = edgeTris[e][0], b = edgeTris[e][1];
                if (fill[a] < 3)
                    across[a][fill[a]++] = b;
                if (fill[b] < 3)
                    across[b][fill[b]++] = a;
            }
        const double inv2s2 = 1.0 / (2.0 * 0.3 * 0.3);
        std::vector<glm::dvec3> next(tris.size());
        for (int it = 0; it < 6; ++it)
        {
            tbb::parallel_for(size_t(0), tris.size(), [&](size_t t) {
                glm::dvec3 sum = filtered[t] * triArea[t];
                for (Index o : across[t])
                    if (o != kInvalid)
                    {
                        const glm::dvec3 d = filtered[o] - filtered[t];
                        sum += filtered[o] * (triArea[o] * std::exp(-glm::dot(d, d) * inv2s2));
                    }
                const double len = glm::length(sum);
                next[t] = len > 0.0 ? sum / len : filtered[t];
            });
            filtered.swap(next);
        }
    }
    edgeFeature.assign(edges.size(), 0);
    edgeBoundary.assign(edges.size(), 0);
    for (size_t e = 0; e < edges.size(); ++e)
    {
        bool boundary = edgeTriCount[e] == 1;
        bool isFeature = boundary || edgeTriCount[e] > 2 || hard.contains(edgeKey(edges[e][0], edges[e][1]));
        if (!isFeature && edgeTriCount[e] == 2 && glm::dot(filtered[edgeTris[e][0]], filtered[edgeTris[e][1]]) < cosSharp)
            isFeature = true;
        edgeFeature[e] = isFeature;
        edgeBoundary[e] = boundary;
    }
    if (minCreaseLength > 0.0)
    {
        // Creases found only by angle must run some way: the short scraps a noisy surface is full of aren't features.
        std::vector<Index> parent(pos.size());
        std::iota(parent.begin(), parent.end(), Index(0));
        const auto find = [&](Index x) {
            while (parent[x] != x)
                x = parent[x] = parent[parent[x]];
            return x;
        };
        std::vector<uint8_t> byAngle(edges.size(), 0);
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeFeature[e] && edgeTriCount[e] == 2 && !hard.contains(edgeKey(edges[e][0], edges[e][1])))
            {
                byAngle[e] = 1;
                parent[find(edges[e][0])] = find(edges[e][1]);
            }
        // A web of short creases branching everywhere is noise too, however long in total: judge the runs between
        // branch points.
        std::vector<double> length(pos.size(), 0.0);
        std::vector<int> degree(pos.size(), 0), branches(pos.size(), 0);
        for (size_t e = 0; e < edges.size(); ++e)
            if (byAngle[e])
            {
                length[find(edges[e][0])] += glm::length(pos[edges[e][1]] - pos[edges[e][0]]);
                ++degree[edges[e][0]];
                ++degree[edges[e][1]];
            }
        for (Index v = 0; v < Index(pos.size()); ++v)
            branches[find(v)] += degree[v] >= 3;
        for (size_t e = 0; e < edges.size(); ++e)
            if (byAngle[e])
            {
                const Index r = find(edges[e][0]);
                if (length[r] / double(1 + branches[r]) < minCreaseLength)
                    edgeFeature[e] = 0;
            }
        // A crease has to bend more than the surface around it does over a cell, or it is just one more bump of noise.
        const double cell = 0.5 * minCreaseLength;
        double edgeSum = 0.0;
        for (size_t e = 0; e < edges.size(); ++e)
            edgeSum += glm::length(pos[edges[e][1]] - pos[edges[e][0]]);
        const int rings = int(std::clamp(std::round(cell * double(edges.size()) / std::max(edgeSum, 1e-300)), 1.0, 8.0));
        std::vector<double> bend(pos.size(), 0.0), ar = area;
        for (size_t e = 0; e < edges.size(); ++e)
            if (edgeTriCount[e] == 2 && !edgeFeature[e])
            {
                const double b = std::acos(std::clamp(glm::dot(triNormal[edgeTris[e][0]], triNormal[edgeTris[e][1]]), -1.0, 1.0)) *
                                 glm::length(pos[edges[e][1]] - pos[edges[e][0]]);
                bend[edges[e][0]] += 0.5 * b;
                bend[edges[e][1]] += 0.5 * b;
            }
        for (int it = 0; it < rings; ++it)
        {
            std::vector<double> nb(pos.size()), na(pos.size());
            for (Index v = 0; v < Index(pos.size()); ++v)
            {
                double sb = bend[v], sa = ar[v];
                for (Index k = adjStart[v]; k < adjStart[v + 1]; ++k)
                {
                    sb += bend[adj[k]];
                    sa += ar[adj[k]];
                }
                nb[v] = sb;
                na[v] = sa;
            }
            bend.swap(nb);
            ar.swap(na);
        }
        std::vector<double> runBend(pos.size(), 0.0), runRough(pos.size(), 0.0);
        for (size_t e = 0; e < edges.size(); ++e)
            if (byAngle[e] && edgeFeature[e])
            {
                const Index r = find(edges[e][0]);
                const double len = glm::length(pos[edges[e][1]] - pos[edges[e][0]]);
                runBend[r] += len * std::acos(std::clamp(glm::dot(filtered[edgeTris[e][0]], filtered[edgeTris[e][1]]), -1.0, 1.0));
                for (Index v : edges[e])
                    runRough[r] += 0.5 * len * cell * (ar[v] > 0.0 ? bend[v] / ar[v] : 0.0);
            }
        for (size_t e = 0; e < edges.size(); ++e)
            if (byAngle[e] && edgeFeature[e])
            {
                const Index r = find(edges[e][0]);
                if (runBend[r] < 1.5 * runRough[r])
                    edgeFeature[e] = 0;
            }
        dropShortCreases(byAngle, minCreaseLength);
    }

    // A crease that runs along both sides of a thin triangle forks at a sliver no lattice corner fits: keep the sharper
    // side. Two creases that meet at a point with nothing else there are a sharp corner, which the layout turns at.
    // Only angle creases; hard edges and borders are the user's.
    {
        const auto bendOf = [&](size_t e) {
            return std::acos(std::clamp(glm::dot(filtered[edgeTris[e][0]], filtered[edgeTris[e][1]]), -1.0, 1.0));
        };
        const auto byAngleOnly = [&](size_t e) {
            return edgeFeature[e] && edgeTriCount[e] == 2 && !hard.contains(edgeKey(edges[e][0], edges[e][1]));
        };
        std::unordered_map<uint64_t, size_t> edgeIndex;
        for (size_t e = 0; e < edges.size(); ++e)
            edgeIndex[edgeKey(edges[e][0], edges[e][1])] = e;
        const double cosFork = std::cos(glm::radians(45.0));
        for (const auto& t : tris)
            for (int k = 0; k < 3; ++k)
            {
                const Index v = t[k], a = t[(k + 1) % 3], b = t[(k + 2) % 3];
                const size_t ea = edgeIndex[edgeKey(v, a)], eb = edgeIndex[edgeKey(v, b)];
                if (!edgeFeature[ea] || !edgeFeature[eb])
                    continue;
                if (glm::dot(glm::normalize(pos[a] - pos[v]), glm::normalize(pos[b] - pos[v])) < cosFork)
                    continue;
                const bool canA = byAngleOnly(ea), canB = byAngleOnly(eb);
                if (!canA && !canB)
                    continue;
                int meeting = 0;
                for (Index j = adjStart[v]; j < adjStart[v + 1]; ++j)
                    meeting += edgeFeature[adjEdge[j]] != 0;
                if (meeting == 2)
                    continue;
                const size_t drop = !canA ? eb : !canB ? ea : (bendOf(ea) < bendOf(eb) ? ea : eb);
                edgeFeature[drop] = 0;
            }
    }

    const Index n = vertexCount();
    feature.assign(n, FeatureKind::None);
    featureDir.assign(n, glm::dvec3(0.0));
    onBoundary.assign(n, 0);
    const double cosStraight = std::cos(glm::radians(135.0));
    std::vector<double> angleSum(n, 0.0);
    for (const auto& t : tris)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 a = pos[t[(k + 1) % 3]] - pos[t[k]], b = pos[t[(k + 2) % 3]] - pos[t[k]];
            angleSum[t[k]] += std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
        }
    for (Index v = 0; v < n; ++v)
    {
        std::vector<glm::dvec3> dirs;
        for (Index k = adjStart[v]; k < adjStart[v + 1]; ++k)
            if (edgeFeature[adjEdge[k]])
            {
                glm::dvec3 d = pos[adj[k]] - pos[v];
                double len = glm::length(d);
                if (len > 0.0)
                    dirs.push_back(d / len);
                if (edgeBoundary[adjEdge[k]])
                    onBoundary[v] = 1;
            }
        if (dirs.empty())
        {
            if (cornerDeficitDeg >= 0.0 && !onBoundary[v])
            {
                // Over the ring as well, where the bumps of a noisy surface cancel out and a real tip doesn't. A needle
                // (one point pulled out of a ring that turns the other way) sums to the opposite sign: no tip.
                const double own = glm::two_pi<double>() - angleSum[v];
                double ring = own;
                for (Index k = adjStart[v]; k < adjStart[v + 1]; ++k)
                    ring += glm::two_pi<double>() - angleSum[adj[k]];
                const double limit = glm::radians(cornerDeficitDeg);
                if (std::abs(own) > limit && std::abs(ring) > limit && own * ring > 0.0)
                {
                    feature[v] = FeatureKind::Corner;
                    if (std::getenv("LILA_CREASE_DEBUG"))
                        std::fprintf(stderr, "tip corner at %.3f %.3f %.3f: %.1f deg, ring %.1f\n", pos[v].x, pos[v].y, pos[v].z, glm::degrees(own),
                                     glm::degrees(ring));
                }
            }
            continue;
        }
        glm::dvec3 t;
        if (dirs.size() == 2 && glm::dot(dirs[0], dirs[1]) < cosStraight)
            t = dirs[0] - dirs[1];
        else if (dirs.size() == 1)
            t = dirs[0];
        else
        {
            feature[v] = FeatureKind::Corner;
            continue;
        }
        t -= normal[v] * glm::dot(normal[v], t);
        double len = glm::length(t);
        if (len < 1e-12)
        {
            feature[v] = FeatureKind::Corner;
            continue;
        }
        feature[v] = FeatureKind::Line;
        featureDir[v] = t / len;
    }

    // Pinned positions are corners wherever the mesh has a vertex exactly there.
    pinned.assign(pos.size(), 0);
    if (!pins.empty())
    {
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
        std::unordered_set<uint64_t> at;
        for (const glm::dvec3& p : pins)
            at.insert(key(p));
        for (Index v = 0; v < vertexCount(); ++v)
            if (at.contains(key(pos[v])))
            {
                pinned[v] = 1;
                feature[v] = FeatureKind::Corner;
            }
    }
}

std::vector<CreaseJunction> creaseJunctions(const Surface& s)
{
    std::vector<CreaseJunction> out;
    const Index nv = s.vertexCount();
    std::vector<int> creases(nv, 0), border(nv, 0);
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeFeature[e])
            for (Index v : s.edges[e])
            {
                ++creases[v];
                border[v] += s.edgeBoundary[e];
            }
    std::vector<std::vector<Index>> vertexTris(nv);
    for (size_t t = 0; t < s.tris.size(); ++t)
        for (Index v : s.tris[t])
            vertexTris[v].push_back(Index(t));
    const auto isCrease = [&](Index a, Index b) {
        for (Index k = s.adjStart[a]; k < s.adjStart[a + 1]; ++k)
            if (s.adj[k] == b)
                return s.edgeFeature[s.adjEdge[k]] != 0;
        return false;
    };
    const double quarter = glm::half_pi<double>();
    for (Index v = 0; v < nv; ++v)
    {
        if (border[v] || creases[v] < 2 || s.feature[v] != FeatureKind::Corner)
            continue;
        using Wedge = CreaseJunction::Wedge;
        const auto wedgeOf = [&](Index t) {
            const auto& tri = s.tris[t];
            int k = 0;
            while (tri[k] != v)
                ++k;
            Wedge w;
            w.tri = t;
            w.next = tri[(k + 1) % 3];
            w.prev = tri[(k + 2) % 3];
            const glm::dvec3 a = s.pos[w.next] - s.pos[v], b = s.pos[w.prev] - s.pos[v];
            w.angle = std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
            return w;
        };
        // Triangle (v, n, p) is followed counter-clockwise by the one whose n is this one's p.
        std::vector<Wedge> fan = {wedgeOf(vertexTris[v].front())};
        while (fan.size() < vertexTris[v].size())
        {
            Index found = kInvalid;
            for (Index t : vertexTris[v])
                if (wedgeOf(t).next == fan.back().prev)
                    found = t;
            if (found == kInvalid || found == fan.front().tri)
                break;
            fan.push_back(wedgeOf(found));
        }
        if (fan.size() != vertexTris[v].size() || fan.back().prev != fan.front().next)
            continue;
        size_t start = 0;
        while (start < fan.size() && !isCrease(v, fan[start].next))
            ++start;
        if (start == fan.size())
            continue;
        std::rotate(fan.begin(), fan.begin() + long(start), fan.end());
        for (size_t i = 0; i < fan.size();)
        {
            size_t j = i;
            double theta = 0.0;
            do
                theta += fan[j].angle;
            while (!isCrease(v, fan[j++].prev) && j < fan.size());
            const double alpha = std::max(1, int(std::lround(theta / quarter))) * quarter / std::max(theta, 1e-9);
            double before = 0.0;
            for (size_t w = i; w < j; ++w)
            {
                fan[w].before = before;
                fan[w].alpha = alpha;
                before += fan[w].angle;
            }
            i = j;
        }
        out.push_back({v, std::move(fan)});
    }
    return out;
}

double roundThickness(const Surface& s, const geo::Mesh& src, const geo::MeshBvh& bvh, const std::vector<double>& curvature, Index v,
                      double maxThickness)
{
    const glm::dvec3 nrm = s.normal[v];
    if (glm::dot(nrm, nrm) < 0.5)
        return 0.0;
    // A crease bends sharply without being thin, so points on or next to one don't count; and a round part bulges out,
    // so its neighbours sit below the tangent plane, unlike a pipe's bore.
    double below = 0.0;
    bool nearCrease = s.feature[v] != FeatureKind::None;
    for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
    {
        below += glm::dot(s.pos[s.adj[k]] - s.pos[v], nrm);
        nearCrease |= s.feature[s.adj[k]] != FeatureKind::None;
    }
    if (nearCrease || below >= 0.0)
        return 0.0;
    const double eps = 1e-6 * maxThickness;
    geo::BvhHit hit;
    // Across a round part the far wall faces straight back and the surface curves at about 2 / thickness; a cone's tip,
    // a spike, an ellipsoid off its axes, a slab or a pipe's wall doesn't.
    if (bvh.raycast(s.pos[v] - eps * nrm, -nrm, hit, maxThickness) && !hit.frontFacing && hit.face != geo::kInvalid &&
        glm::dot(src.faceNormal(hit.face), nrm) < -0.9 && curvature[v] * hit.t > 1.5)
        return hit.t + eps;
    return 0.0;
}

void Surface::thinCorners(const std::vector<double>& spacing)
{
    std::vector<std::pair<int, Index>> corners;
    for (Index v = 0; v < vertexCount(); ++v)
        if (feature[v] == FeatureKind::Corner)
        {
            int count = 0;
            for (Index k = adjStart[v]; k < adjStart[v + 1]; ++k)
                count += edgeFeature[adjEdge[k]];
            corners.push_back({-count, v});
        }
    std::sort(corners.begin(), corners.end());
    std::vector<Index> kept;
    for (auto [neg, v] : corners)
    {
        bool clear = true;
        if (!(v < pinned.size() && pinned[v]))
            for (Index k : kept)
                clear &= glm::length(pos[k] - pos[v]) >= 0.6 * std::max(spacing[k], spacing[v]);
        if (clear)
            kept.push_back(v);
        else
            feature[v] = FeatureKind::None;
    }
}

double Surface::totalArea() const
{
    double a = 0.0;
    for (const auto& f : tris)
        a += 0.5 * glm::length(glm::cross(pos[f[1]] - pos[f[0]], pos[f[2]] - pos[f[0]]));
    return a;
}

double Surface::meanEdgeLength() const
{
    double sum = 0.0;
    size_t count = 0;
    for (const auto& f : tris)
        for (int k = 0; k < 3; ++k)
        {
            sum += glm::length(pos[f[k]] - pos[f[(k + 1) % 3]]);
            ++count;
        }
    return count ? sum / double(count) : 0.0;
}

Surface surfaceFromMesh(const geo::Mesh& mesh, bool useHardEdges)
{
    LILA_ZONE();

    Surface s;
    std::vector<Index> remap(mesh.pointCapacity(), kInvalid);
    auto painted = mesh.pointAttrs().get<float>("density");
    for (Index p = 0; p < mesh.pointCapacity(); ++p)
        if (mesh.pointValid(p))
        {
            remap[p] = Index(s.pos.size());
            s.pos.push_back(mesh.position(p));
            s.density.push_back(painted.empty() ? 1.0f : std::max(0.05f, painted[p]));
        }
    geo::Triangulation tr = geo::triangulate(mesh);
    s.tris.reserve(tr.triangleCount());
    for (Index t = 0; t < tr.triangleCount(); ++t)
    {
        std::array<Index, 3> f;
        for (int k = 0; k < 3; ++k)
            f[k] = remap[mesh.cornerPoint(tr.corners[size_t(t) * 3 + k])];
        if (f[0] != f[1] && f[1] != f[2] && f[2] != f[0])
            s.tris.push_back(f);
    }
    if (useHardEdges)
        if (auto hard = mesh.edgeAttrs().get<bool>(geo::attr::kHard); !hard.empty())
            for (Index e = 0; e < mesh.edgeCapacity(); ++e)
                if (mesh.edgeValid(e) && hard[e])
                    s.hard.insert(edgeKey(remap[mesh.edgePoint(e, 0)], remap[mesh.edgePoint(e, 1)]));
    return s;
}

void refineSurface(Surface& s, double maxLength)
{
    LILA_ZONE();

    const double max2 = maxLength * maxLength;
    for (int pass = 0; pass < 12; ++pass)
    {
        std::unordered_map<uint64_t, Index> mid;
        auto split = [&](Index a, Index b) -> Index {
            if (glm::dot(s.pos[a] - s.pos[b], s.pos[a] - s.pos[b]) <= max2)
                return kInvalid;
            uint64_t key = edgeKey(a, b);
            auto [it, fresh] = mid.try_emplace(key, Index(s.pos.size()));
            if (fresh)
            {
                s.pos.push_back(0.5 * (s.pos[a] + s.pos[b]));
                s.density.push_back(0.5f * (s.density[a] + s.density[b]));
                if (s.hard.contains(key))
                {
                    s.hard.insert(edgeKey(a, it->second));
                    s.hard.insert(edgeKey(it->second, b));
                }
            }
            return it->second;
        };
        std::vector<std::array<Index, 3>> out;
        out.reserve(s.tris.size() * 2);
        bool any = false;
        for (const auto& f : s.tris)
        {
            Index m[3] = {split(f[0], f[1]), split(f[1], f[2]), split(f[2], f[0])};
            int count = (m[0] != kInvalid) + (m[1] != kInvalid) + (m[2] != kInvalid);
            if (count == 0)
            {
                out.push_back(f);
                continue;
            }
            any = true;
            if (count == 3)
            {
                out.push_back({f[0], m[0], m[2]});
                out.push_back({m[0], f[1], m[1]});
                out.push_back({m[2], m[1], f[2]});
                out.push_back({m[0], m[1], m[2]});
                continue;
            }
            // Rotate so the pattern starts at a fixed edge.
            int r = 0;
            if (count == 1)
                r = m[0] != kInvalid ? 0 : m[1] != kInvalid ? 1 : 2;
            else
                r = m[0] == kInvalid ? 1 : m[1] == kInvalid ? 2 : 0;
            Index a = f[r], b = f[(r + 1) % 3], c = f[(r + 2) % 3];
            Index mab = m[r], mbc = m[(r + 1) % 3];
            if (count == 1)
            {
                out.push_back({a, mab, c});
                out.push_back({mab, b, c});
                continue;
            }
            // Edges ab and bc split: corner triangle at b plus the quad a, mab, mbc, c on its shorter diagonal.
            out.push_back({mab, b, mbc});
            if (glm::length(s.pos[a] - s.pos[mbc]) < glm::length(s.pos[mab] - s.pos[c]))
            {
                out.push_back({a, mab, mbc});
                out.push_back({a, mbc, c});
            }
            else
            {
                out.push_back({a, mab, c});
                out.push_back({mab, mbc, c});
            }
        }
        s.tris = std::move(out);
        if (!any)
            break;
    }
}

void smoothSurface(Surface& s, const geo::MeshBvh& source, int iterations)
{
    LILA_ZONE();

    const Index n = s.vertexCount();
    std::vector<glm::dvec3> next(n);
    auto facing = [&](const std::array<Index, 3>& t, const std::vector<glm::dvec3>& p) {
        return glm::cross(p[t[1]] - p[t[0]], p[t[2]] - p[t[0]]);
    };
    // A move that turns a triangle over (a projection onto the far side of a thin part, or neighbours crossing) is undone.
    auto keepUnflipped = [&]() {
        for (int pass = 0; pass < 8; ++pass)
        {
            bool undone = false;
            for (const auto& t : s.tris)
            {
                const glm::dvec3 before = facing(t, s.pos), after = facing(t, next);
                if (glm::dot(before, after) > 0.1 * glm::length(before) * glm::length(after))
                    continue;
                for (Index v : t)
                    if (next[v] != s.pos[v])
                    {
                        next[v] = s.pos[v];
                        undone = true;
                    }
            }
            if (!undone)
                break;
        }
    };
    for (int it = 0; it < iterations; ++it)
    {
        tbb::parallel_for(Index(0), n, [&](Index v) {
            next[v] = s.pos[v];
            if (s.feature[v] != FeatureKind::None || s.adjStart[v] == s.adjStart[v + 1])
                return;
            glm::dvec3 avg(0.0);
            for (Index k = s.adjStart[v]; k < s.adjStart[v + 1]; ++k)
                avg += s.pos[s.adj[k]];
            glm::dvec3 d = avg / double(s.adjStart[v + 1] - s.adjStart[v]) - s.pos[v];
            d -= s.normal[v] * glm::dot(s.normal[v], d);
            geo::BvhClosest c;
            if (source.closestPoint(s.pos[v] + d, c))
                next[v] = c.point;
        });
        keepUnflipped();
        s.pos.swap(next);
        s.buildGeometry();
    }
}

void decimateSurface(Surface& s, Index targetTris)
{
    LILA_ZONE();

    if (targetTris >= s.tris.size())
        return;
    std::vector<float> p(s.pos.size() * 3);
    for (size_t v = 0; v < s.pos.size(); ++v)
        for (int k = 0; k < 3; ++k)
            p[v * 3 + k] = float(s.pos[v][k]);
    std::vector<unsigned char> lock(s.pos.size(), 0);
    for (uint64_t key : s.hard)
    {
        lock[key >> 32] = 1;
        lock[key & 0xffffffffu] = 1;
    }
    std::vector<unsigned int> idx(s.tris.size() * 3), dst(s.tris.size() * 3);
    for (size_t t = 0; t < s.tris.size(); ++t)
        for (int k = 0; k < 3; ++k)
            idx[t * 3 + k] = s.tris[t][k];
    size_t count = meshopt_simplifyWithAttributes(dst.data(), idx.data(), idx.size(), p.data(), s.pos.size(), sizeof(float) * 3, nullptr, 0,
                                                  nullptr, 0, lock.data(), size_t(targetTris) * 3, 1.0f, meshopt_SimplifyLockBorder, nullptr);
    s.tris.resize(count / 3);
    for (size_t t = 0; t < count / 3; ++t)
        s.tris[t] = {dst[t * 3], dst[t * 3 + 1], dst[t * 3 + 2]};
    dropFins(s);
    compactSurface(s);
}

int dropFins(Surface& s)
{
    auto sorted = [](std::array<Index, 3> t) {
        std::sort(t.begin(), t.end());
        return t;
    };
    std::map<std::array<Index, 3>, size_t> first;
    std::vector<uint8_t> dead(s.tris.size(), 0);
    for (size_t t = 0; t < s.tris.size(); ++t)
    {
        const auto& f = s.tris[t];
        if (f[0] == f[1] || f[1] == f[2] || f[2] == f[0])
        {
            dead[t] = 1;
            continue;
        }
        const auto [it, fresh] = first.try_emplace(sorted(f), t);
        if (fresh)
            continue;
        dead[it->second] = dead[t] = 1;
        first.erase(it);
    }
    size_t kept = 0;
    for (size_t t = 0; t < s.tris.size(); ++t)
        if (!dead[t])
            s.tris[kept++] = s.tris[t];
    const int dropped = int(s.tris.size() - kept);
    s.tris.resize(kept);
    return dropped;
}

void compactSurface(Surface& s)
{
    std::vector<Index> remap(s.pos.size(), kInvalid);
    Surface out;
    for (auto& f : s.tris)
        for (Index& v : f)
        {
            if (remap[v] == kInvalid)
            {
                remap[v] = Index(out.pos.size());
                out.pos.push_back(s.pos[v]);
                out.density.push_back(s.density[v]);
            }
            v = remap[v];
        }
    for (uint64_t key : s.hard)
    {
        Index a = remap[key >> 32], b = remap[key & 0xffffffffu];
        if (a != kInvalid && b != kInvalid)
            out.hard.insert(edgeKey(a, b));
    }
    s.pos = std::move(out.pos);
    s.density = std::move(out.density);
    s.hard = std::move(out.hard);
}

void clipSurface(Surface& s, const geo::SymmetryPlane& plane, int axis)
{
    LILA_ZONE();

    const double eps = std::max(1e-9, s.meanEdgeLength() * 1e-6);
    std::vector<double> d(s.pos.size());
    for (size_t v = 0; v < s.pos.size(); ++v)
    {
        d[v] = plane.distance(s.pos[v]);
        if (std::abs(d[v]) < eps)
        {
            d[v] = 0.0;
            s.pos[v][axis] = plane.offset;
        }
    }
    std::unordered_map<uint64_t, Index> cut;
    auto cutPoint = [&](Index a, Index b) {
        uint64_t key = edgeKey(a, b);
        auto [it, fresh] = cut.try_emplace(key, Index(s.pos.size()));
        if (fresh)
        {
            // Interpolate from the lower id so both triangles sharing the edge get the same point.
            Index lo = std::min(a, b), hi = std::max(a, b);
            double t = d[lo] / (d[lo] - d[hi]);
            glm::dvec3 p = glm::mix(s.pos[lo], s.pos[hi], t);
            p[axis] = plane.offset;
            s.pos.push_back(p);
            s.density.push_back(float(glm::mix(double(s.density[lo]), double(s.density[hi]), t)));
            d.push_back(0.0);
            if (s.hard.contains(key))
            {
                s.hard.insert(edgeKey(a, it->second));
                s.hard.insert(edgeKey(it->second, b));
            }
        }
        return it->second;
    };
    std::vector<std::array<Index, 3>> out;
    for (const auto& f : s.tris)
    {
        bool anyPos = d[f[0]] > 0.0 || d[f[1]] > 0.0 || d[f[2]] > 0.0;
        bool anyNeg = d[f[0]] < 0.0 || d[f[1]] < 0.0 || d[f[2]] < 0.0;
        if (!anyPos)
            continue;
        if (!anyNeg)
        {
            out.push_back(f);
            continue;
        }
        std::vector<Index> poly;
        for (int k = 0; k < 3; ++k)
        {
            Index a = f[k], b = f[(k + 1) % 3];
            if (d[a] >= 0.0)
                poly.push_back(a);
            if ((d[a] > 0.0 && d[b] < 0.0) || (d[a] < 0.0 && d[b] > 0.0))
                poly.push_back(cutPoint(a, b));
        }
        for (size_t k = 1; k + 1 < poly.size(); ++k)
            if (poly[0] != poly[k] && poly[k] != poly[k + 1] && poly[0] != poly[k + 1])
                out.push_back({poly[0], poly[k], poly[k + 1]});
    }
    s.tris = std::move(out);
    compactSurface(s);
}

Surface mirrorSurface(const Surface& half, const geo::SymmetryPlane& plane, std::vector<Index>& mirrorOf)
{
    LILA_ZONE();

    Surface w;
    const Index n = half.vertexCount();
    const double eps = std::max(1e-9, half.meanEdgeLength() * 1e-6);
    w.pos = half.pos;
    w.density = half.density;
    mirrorOf.assign(n, kInvalid);
    for (Index v = 0; v < n; ++v)
    {
        if (std::abs(plane.distance(half.pos[v])) < eps)
        {
            mirrorOf[v] = v;
            continue;
        }
        mirrorOf[v] = Index(w.pos.size());
        w.pos.push_back(plane.reflect(half.pos[v]));
        w.density.push_back(half.density[v]);
    }
    for (Index v = 0; v < n; ++v)
        if (mirrorOf[v] >= n)
            mirrorOf.push_back(v);
    w.tris = half.tris;
    for (const auto& t : half.tris)
        w.tris.push_back({mirrorOf[t[0]], mirrorOf[t[2]], mirrorOf[t[1]]});
    w.hard = half.hard;
    for (uint64_t key : half.hard)
        w.hard.insert(edgeKey(mirrorOf[key >> 32], mirrorOf[key & 0xffffffffu]));
    w.pins = half.pins;
    for (const glm::dvec3& p : half.pins)
        if (std::abs(plane.distance(p)) >= eps)
            w.pins.push_back(plane.reflect(p));
    return w;
}

// FeatureLines

std::array<int64_t, 3> FeatureLines::cell(const glm::dvec3& p) const
{
    return {int64_t(std::floor(p.x / mCell)), int64_t(std::floor(p.y / mCell)), int64_t(std::floor(p.z / mCell))};
}

uint64_t FeatureLines::hash(const std::array<int64_t, 3>& c) const
{
    uint64_t h = uint64_t(c[0]) * 73856093ull ^ uint64_t(c[1]) * 19349663ull ^ uint64_t(c[2]) * 83492791ull;
    return h % mBuckets.size();
}

std::vector<uint8_t> Surface::saddleTips() const
{
    const Index n = vertexCount();
    std::vector<uint8_t> out(n, 0);
    std::vector<double> deficit(n, glm::two_pi<double>());
    for (const auto& t : tris)
        for (int k = 0; k < 3; ++k)
        {
            const glm::dvec3 a = pos[t[(k + 1) % 3]] - pos[t[k]], b = pos[t[(k + 2) % 3]] - pos[t[k]];
            deficit[t[k]] -= std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
        }
    std::vector<uint8_t> onCrease(n, 0);
    for (size_t e = 0; e < edges.size(); ++e)
        if (edgeFeature[e] || edgeTriCount[e] != 2)
            onCrease[edges[e][0]] = onCrease[edges[e][1]] = 1;
    for (Index v = 0; v < n; ++v)
        out[v] = feature[v] == FeatureKind::Corner && deficit[v] < 0.0 && !onCrease[v] && (pinned.empty() || !pinned[v]);
    return out;
}

void FeatureLines::build(const Surface& s, double cellSize, bool boundaryOnly, bool innerOnly)
{
    LILA_ZONE();

    mSegs.clear();
    mCorners.clear();
    mCell = std::max(cellSize, 1e-9);
    for (size_t e = 0; e < s.edges.size(); ++e)
        if (s.edgeFeature[e] && (!boundaryOnly || s.edgeBoundary[e]) && (!innerOnly || !s.edgeBoundary[e]))
            mSegs.push_back({s.pos[s.edges[e][0]], s.pos[s.edges[e][1]]});
    const std::vector<uint8_t> tip = s.saddleTips();
    for (Index v = 0; v < s.vertexCount(); ++v)
        if (s.feature[v] == FeatureKind::Corner && !tip[v] && (!boundaryOnly || s.onBoundary[v]) && (!innerOnly || !s.onBoundary[v]))
            mCorners.push_back(s.pos[v]);
    mBuckets.assign(std::max<size_t>(64, mSegs.size() * 2), {});
    mCornerBuckets.assign(mBuckets.size(), {});
    for (Index i = 0; i < Index(mSegs.size()); ++i)
    {
        auto lo = cell(glm::min(mSegs[i][0], mSegs[i][1])), hi = cell(glm::max(mSegs[i][0], mSegs[i][1]));
        for (int64_t x = lo[0]; x <= hi[0]; ++x)
            for (int64_t y = lo[1]; y <= hi[1]; ++y)
                for (int64_t z = lo[2]; z <= hi[2]; ++z)
                    mBuckets[hash({x, y, z})].push_back(i);
    }
    for (Index i = 0; i < Index(mCorners.size()); ++i)
        mCornerBuckets[hash(cell(mCorners[i]))].push_back(i);
    for (auto& b : mBuckets)
        b.erase(std::unique(b.begin(), b.end()), b.end());
}

bool FeatureLines::closest(const glm::dvec3& p, double maxDist, glm::dvec3& out, glm::dvec3* tangent) const
{
    if (mSegs.empty())
        return false;
    double best = maxDist * maxDist;
    bool found = false;
    int r = int(std::ceil(maxDist / mCell));
    auto c = cell(p);
    for (int64_t x = c[0] - r; x <= c[0] + r; ++x)
        for (int64_t y = c[1] - r; y <= c[1] + r; ++y)
            for (int64_t z = c[2] - r; z <= c[2] + r; ++z)
                for (Index i : mBuckets[hash({x, y, z})])
                {
                    const glm::dvec3 &a = mSegs[i][0], &b = mSegs[i][1];
                    glm::dvec3 ab = b - a;
                    double len2 = glm::dot(ab, ab);
                    double t = len2 > 0.0 ? std::clamp(glm::dot(p - a, ab) / len2, 0.0, 1.0) : 0.0;
                    glm::dvec3 q = a + ab * t;
                    double d2 = glm::dot(q - p, q - p);
                    if (d2 < best)
                    {
                        best = d2;
                        out = q;
                        found = true;
                        if (tangent)
                            *tangent = len2 > 0.0 ? ab / std::sqrt(len2) : glm::dvec3(0.0);
                    }
                }
    return found;
}

bool FeatureLines::closestCorner(const glm::dvec3& p, double maxDist, glm::dvec3& out) const
{
    if (mCorners.empty())
        return false;
    double best = maxDist * maxDist;
    bool found = false;
    int r = int(std::ceil(maxDist / mCell));
    auto c = cell(p);
    for (int64_t x = c[0] - r; x <= c[0] + r; ++x)
        for (int64_t y = c[1] - r; y <= c[1] + r; ++y)
            for (int64_t z = c[2] - r; z <= c[2] + r; ++z)
                for (Index i : mCornerBuckets[hash({x, y, z})])
                {
                    double d2 = glm::dot(mCorners[i] - p, mCorners[i] - p);
                    if (d2 < best)
                    {
                        best = d2;
                        out = mCorners[i];
                        found = true;
                    }
                }
    return found;
}
} // namespace lila::retopo::detail
