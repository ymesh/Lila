#include "geo/CrossSection.h"

#include "core/Profile.h"
#include "geo/OpsCommon.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

namespace lila::geo
{
namespace
{
using detail::insidePolygon;

struct Basis
{
    glm::dvec3 n, u, v;
};

Basis planeBasis(const glm::dvec3& normal)
{
    Basis b;
    b.n = glm::normalize(normal);
    glm::dvec3 ref = std::abs(b.n.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0);
    b.u = glm::normalize(glm::cross(b.n, ref));
    b.v = glm::cross(b.n, b.u);
    return b;
}

glm::dvec2 project(const Basis& b, const glm::dvec3& origin, const glm::dvec3& p)
{
    glm::dvec3 d = p - origin;
    return {glm::dot(d, b.u), glm::dot(d, b.v)};
}

double cross2(const glm::dvec2& a, const glm::dvec2& b) { return a.x * b.y - a.y * b.x; }

double distanceTo(const std::vector<glm::dvec2>& poly, const glm::dvec2& q)
{
    double best = std::numeric_limits<double>::max();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
    {
        glm::dvec2 a = poly[j], ab = poly[i] - a;
        double len2 = glm::dot(ab, ab);
        double t = len2 > 0.0 ? std::clamp(glm::dot(q - a, ab) / len2, 0.0, 1.0) : 0.0;
        best = std::min(best, glm::length(a + ab * t - q));
    }
    return best;
}

/** The cut as a graph: one vertex per crossed mesh edge, one edge per face the plane passes through. */
struct CutGraph
{
    std::vector<glm::dvec3> pos;
    std::vector<std::pair<uint32_t, uint32_t>> edges;
    std::vector<std::vector<uint32_t>> adj; // edge ids per vertex
    std::unordered_map<uint64_t, uint32_t> keys;

    uint32_t vertex(uint64_t key, const glm::dvec3& p)
    {
        auto [it, added] = keys.try_emplace(key, uint32_t(pos.size()));
        if (added)
        {
            pos.push_back(p);
            adj.emplace_back();
        }
        return it->second;
    }

    void connect(uint32_t a, uint32_t b)
    {
        if (a == b)
            return;
        adj[a].push_back(uint32_t(edges.size()));
        adj[b].push_back(uint32_t(edges.size()));
        edges.emplace_back(a, b);
    }
};

void cutMesh(CutGraph& g, uint32_t meshIndex, const SectionMesh& sm, const glm::dvec3& origin, const glm::dvec3& n)
{
    const Mesh& m = *sm.mesh;
    std::vector<glm::dvec3> wp(m.pointCapacity());
    std::vector<double> dist(m.pointCapacity(), 0.0);
    for (Index p = 0; p < m.pointCapacity(); ++p)
        if (m.pointValid(p))
        {
            wp[p] = glm::dvec3(sm.world * glm::dvec4(m.position(p), 1.0));
            dist[p] = glm::dot(wp[p] - origin, n);
        }

    struct Crossing
    {
        uint32_t vertex;
        glm::dvec3 p;
    };
    std::vector<Crossing> hits;
    for (Index f = 0; f < m.faceCapacity(); ++f)
    {
        if (!m.faceValid(f))
            continue;
        hits.clear();
        m.forEachFaceCorner(f, [&](Index c) {
            Index a = m.cornerPoint(c), b = m.cornerNextPoint(c);
            // On-plane points count as in front, so every crossing lies on an edge with a strict sign change.
            if ((dist[a] >= 0.0) == (dist[b] >= 0.0))
                return;
            double t = dist[a] / (dist[a] - dist[b]);
            glm::dvec3 p = glm::mix(wp[a], wp[b], t);
            uint64_t key = (uint64_t(meshIndex) << 32) | m.cornerEdge(c);
            hits.push_back({g.vertex(key, p), p});
        });
        if (hits.size() < 2)
            continue;
        if (hits.size() > 2)
        {
            // A concave n-gon crosses several times: its pieces are consecutive pairs along the cut line.
            glm::dvec3 newell(0.0);
            m.forEachFaceCorner(f, [&](Index c) { newell += glm::cross(wp[m.cornerPoint(c)], wp[m.cornerNextPoint(c)]); });
            glm::dvec3 line = glm::cross(n, newell);
            if (glm::dot(line, line) > 1e-30)
                std::sort(hits.begin(), hits.end(),
                          [&](const Crossing& x, const Crossing& y) { return glm::dot(x.p, line) < glm::dot(y.p, line); });
        }
        for (size_t i = 0; i + 1 < hits.size(); i += 2)
            g.connect(hits[i].vertex, hits[i + 1].vertex);
    }
}

/** Splits the cut graph into chains; closed ones are loops, the rest end at a boundary or junction. */
void chains(const CutGraph& g, std::vector<std::vector<glm::dvec3>>& closed, std::vector<std::vector<glm::dvec3>>& open)
{
    std::vector<char> used(g.edges.size(), 0);
    auto nextEdge = [&](uint32_t v) -> int64_t {
        for (uint32_t e : g.adj[v])
            if (!used[e])
                return e;
        return -1;
    };
    auto walk = [&](uint32_t start) {
        std::vector<glm::dvec3> chain{g.pos[start]};
        uint32_t cur = start;
        bool loop = false;
        for (int64_t e; (e = nextEdge(cur)) >= 0;)
        {
            used[size_t(e)] = 1;
            auto [a, b] = g.edges[size_t(e)];
            cur = a == cur ? b : a;
            if (cur == start)
            {
                loop = true;
                break;
            }
            chain.push_back(g.pos[cur]);
        }
        (loop ? closed : open).push_back(std::move(chain));
    };
    // Open ends first, so a chain is walked whole from one end rather than split in two.
    for (uint32_t v = 0; v < g.pos.size(); ++v)
        if (g.adj[v].size() % 2 == 1)
            while (nextEdge(v) >= 0)
                walk(v);
    for (uint32_t v = 0; v < g.pos.size(); ++v)
        while (nextEdge(v) >= 0)
            walk(v);
}

SectionContour finishContour(std::vector<glm::dvec3> pts, bool bridged, const Basis& b, const glm::dvec3& origin)
{
    SectionContour c;
    c.bridged = bridged;
    double a2 = 0.0;
    glm::dvec2 sum(0.0), avg(0.0);
    for (size_t i = 0; i < pts.size(); ++i)
    {
        glm::dvec2 p = project(b, origin, pts[i]), q = project(b, origin, pts[(i + 1) % pts.size()]);
        double w = cross2(p, q);
        a2 += w;
        sum += (p + q) * w;
        avg += p;
    }
    glm::dvec2 c2 = std::abs(a2) > 1e-18 ? sum / (3.0 * a2) : avg / double(pts.size());
    c.area = std::abs(a2) * 0.5;
    c.centroid = origin + b.u * c2.x + b.v * c2.y;
    c.points = std::move(pts);
    return c;
}
} // namespace

std::vector<SectionContour> crossSections(std::span<const SectionMesh> meshes, const glm::dvec3& origin, const glm::dvec3& normal)
{
    LILA_ZONE();

    std::vector<SectionContour> out;
    if (glm::length(normal) < 1e-12)
        return out;
    Basis b = planeBasis(normal);
    CutGraph g;
    for (size_t i = 0; i < meshes.size(); ++i)
        if (meshes[i].mesh)
            cutMesh(g, uint32_t(i), meshes[i], origin, b.n);

    std::vector<std::vector<glm::dvec3>> closed, open;
    chains(g, closed, open);
    for (auto& c : closed)
        if (c.size() >= 3)
            out.push_back(finishContour(std::move(c), false, b, origin));

    // Open chains join whichever other open end is nearer than their own start, else close on themselves.
    while (!open.empty())
    {
        std::vector<glm::dvec3> cur = std::move(open.back());
        open.pop_back();
        for (;;)
        {
            double self = glm::length(cur.back() - cur.front());
            size_t best = SIZE_MAX;
            bool reversed = false;
            double bestDist = self;
            for (size_t i = 0; i < open.size(); ++i)
            {
                double df = glm::length(open[i].front() - cur.back()), db = glm::length(open[i].back() - cur.back());
                if (df < bestDist)
                    bestDist = df, best = i, reversed = false;
                if (db < bestDist)
                    bestDist = db, best = i, reversed = true;
            }
            if (best == SIZE_MAX)
                break;
            std::vector<glm::dvec3> next = std::move(open[best]);
            open.erase(open.begin() + std::ptrdiff_t(best));
            if (reversed)
                std::reverse(next.begin(), next.end());
            cur.insert(cur.end(), next.begin(), next.end());
        }
        if (cur.size() >= 3)
            out.push_back(finishContour(std::move(cur), true, b, origin));
    }
    return out;
}

std::vector<SectionChain> sectionChains(std::span<const SectionMesh> meshes, const glm::dvec3& origin, const glm::dvec3& normal)
{
    LILA_ZONE();

    std::vector<SectionChain> out;
    if (glm::length(normal) < 1e-12)
        return out;
    Basis b = planeBasis(normal);
    CutGraph g;
    for (size_t i = 0; i < meshes.size(); ++i)
        if (meshes[i].mesh)
            cutMesh(g, uint32_t(i), meshes[i], origin, b.n);
    std::vector<std::vector<glm::dvec3>> closed, open;
    chains(g, closed, open);
    for (auto& c : closed)
        if (c.size() >= 3)
            out.push_back({std::move(c), true});
    for (auto& c : open)
        if (c.size() >= 2)
            out.push_back({std::move(c), false});
    return out;
}

std::optional<SectionPick> sectionCenter(std::span<const SectionContour> contours, const glm::dvec3& point, const glm::dvec3& direction)
{
    if (contours.empty() || glm::length(direction) < 1e-12)
        return std::nullopt;
    Basis b = planeBasis(direction);
    const glm::dvec2 q(0.0);
    const SectionContour* inside = nullptr;
    const SectionContour* nearest = nullptr;
    double nearestDist = std::numeric_limits<double>::max();
    std::vector<glm::dvec2> poly;
    for (const SectionContour& c : contours)
    {
        poly.clear();
        for (const glm::dvec3& p : c.points)
            poly.push_back(project(b, point, p));
        if (insidePolygon(poly, q))
        {
            // Nested loops (a sleeve over an arm, a hollow pipe) all contain it; the tightest is the one it sits in.
            if (!inside || c.area < inside->area)
                inside = &c;
        }
        else if (double d = distanceTo(poly, q); d < nearestDist)
        {
            nearestDist = d;
            nearest = &c;
        }
    }
    const SectionContour* pick = inside ? inside : nearest;
    if (!pick)
        return std::nullopt;
    return SectionPick{.center = pick->centroid, .area = pick->area, .inside = inside != nullptr};
}

std::optional<SectionPick> sectionCenter(std::span<const SectionMesh> meshes, const glm::dvec3& point, const glm::dvec3& direction)
{
    std::vector<SectionContour> contours = crossSections(meshes, point, direction);
    return sectionCenter(contours, point, direction);
}
} // namespace lila::geo
