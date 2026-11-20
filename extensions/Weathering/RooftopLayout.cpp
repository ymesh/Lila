#include "RooftopLayout.h"

#include <lila/Profile.h>

#include <algorithm>
#include <cmath>
#include <map>

namespace weathering::rooftop
{
using gen::hash01;
using gen::unit;

bool Top::inside(const glm::dvec2& xz) const
{
    for (const auto& f : faces)
        if (gen::insidePolygon(f, xz))
            return true;
    return false;
}

namespace
{
double segDistance(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
{
    const glm::dvec2 d = b - a;
    const double l2 = glm::dot(d, d);
    const double t = l2 > 1e-12 ? std::clamp(glm::dot(p - a, d) / l2, 0.0, 1.0) : 0.0;
    return glm::length(p - (a + d * t));
}

struct Box2
{
    glm::dvec2 lo, hi;
};
} // namespace

Result layout(const Mesh& host, std::span<const Bounds> railings, std::span<const double> railingWeights, std::span<const Bounds> towers,
              std::span<const double> towerWeights, const Options& o)
{
    LILA_ZONE();

    Result r;
    const double minUp = std::cos(glm::radians(std::clamp(o.slopeMax, 0.0, 89.0)));
    std::vector<uint8_t> picked(host.faceCapacity(), 0);
    for (Index f = 0; f < host.faceCapacity(); ++f)
        if (host.faceValid(f) && host.faceArea(f) > 1e-6 && host.faceNormal(f).y >= minUp)
            picked[f] = 1;
    lila::geo::MeshBvh bvh(host);
    std::vector<int> regionOf(host.faceCapacity(), -1);
    std::vector<std::vector<Index>> groups;
    for (Index seed = 0; seed < host.faceCapacity(); ++seed)
    {
        if (!picked[seed] || regionOf[seed] >= 0)
            continue;
        const int id = int(groups.size());
        groups.emplace_back();
        const double y0 = host.faceCentroid(seed).y;
        std::vector<Index> stack{seed};
        regionOf[seed] = id;
        while (!stack.empty())
        {
            const Index f = stack.back();
            stack.pop_back();
            groups.back().push_back(f);
            for (Index c : host.faceCorners(f))
            {
                const Index nb = host.oppositeFace(host.cornerEdge(c), f);
                if (nb != lila::geo::kInvalid && picked[nb] && regionOf[nb] < 0 && std::abs(host.faceCentroid(nb).y - y0) < 2.0)
                {
                    regionOf[nb] = id;
                    stack.push_back(nb);
                }
            }
        }
    }

    std::vector<Box2> occupied;
    uint32_t counter = 0;
    for (size_t gi = 0; gi < groups.size(); ++gi)
    {
        const std::vector<Index>& grp = groups[gi];
        Top top;
        int open = 0;
        for (Index f : grp)
        {
            top.area += host.faceArea(f);
            top.y += host.faceCentroid(f).y * host.faceArea(f);
            lila::geo::BvhHit hit;
            if (!bvh.raycast(host.faceCentroid(f) + glm::dvec3(0, 0.5, 0), glm::dvec3(0, 1, 0), hit))
                ++open;
            std::vector<glm::dvec2> poly;
            for (Index p : host.facePoints(f))
                poly.push_back({host.position(p).x, host.position(p).z});
            top.faces.push_back(std::move(poly));
        }
        top.y /= std::max(top.area, 1e-12);
        if (top.area < o.minArea * 10000.0 || open * 2 < int(grp.size()))
            continue;

        // Edges of the top: face edges whose other side is not in it.
        std::vector<std::pair<glm::dvec3, glm::dvec3>> edges;
        for (Index f : grp)
            for (Index c : host.faceCorners(f))
            {
                const Index nb = host.oppositeFace(host.cornerEdge(c), f);
                if (nb != lila::geo::kInvalid && regionOf[nb] == int(gi))
                    continue;
                edges.push_back({host.position(host.cornerPoint(c)), host.position(host.cornerNextPoint(c))});
            }

        if (!railings.empty())
            for (const auto& [a, b] : edges)
            {
                const uint32_t id = counter++;
                if (hash01(o.seed, 1, id) >= o.railings)
                    continue;
                glm::dvec3 dir = b - a;
                dir.y = 0.0;
                const double len0 = glm::length(dir);
                if (len0 < 30.0)
                    continue;
                dir /= len0;
                glm::dvec3 inward(-dir.z, 0.0, dir.x);
                const glm::dvec3 mid = (a + b) * 0.5;
                if (!top.inside(glm::dvec2(mid.x + inward.x, mid.z + inward.z)))
                    inward = -inward;
                const glm::dvec3 start = a + dir * o.railInset + inward * o.railInset, end = b - dir * o.railInset + inward * o.railInset;
                const double len = glm::length(end - start);
                if (len < 20.0)
                    continue;
                const int pick = gen::pickWeighted(railingWeights, int(railings.size()), hash01(o.seed, 2, id));
                const gen::Fit f = gen::fitPiece(railings[size_t(pick)], false, true);
                const int n = std::max(1, int(std::round(len / f.size.x)));
                const double each = len / n;
                const glm::dvec3 z = glm::cross(dir, glm::dvec3(0, 1, 0));
                for (int k = 0; k < n; ++k)
                {
                    const glm::dvec3 c = start + dir * (each * (k + 0.5)) + glm::dvec3(0, top.y - start.y + f.size.y * 0.5, 0);
                    Placed p;
                    p.xf = gen::place(f, c, dir, glm::dvec3(0, 1, 0), z, glm::dvec3(each / f.size.x, 1.0, 1.0));
                    p.slot = 0;
                    p.piece = pick;
                    p.random = float(hash01(o.seed, 3, id, uint32_t(k)));
                    r.placed.push_back(p);
                    ++r.railings;
                }
            }

        if (!towers.empty() && hash01(o.seed, 4, uint32_t(gi)) < o.towers)
        {
            const int pick = gen::pickWeighted(towerWeights, int(towers.size()), hash01(o.seed, 5, uint32_t(gi)));
            const gen::Fit f = gen::fitPiece(towers[size_t(pick)]);
            glm::dvec2 lo(1e300), hi(-1e300);
            for (const auto& poly : top.faces)
                for (const glm::dvec2& p : poly)
                {
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
            const glm::dvec2 half(std::max(f.size.x, f.size.z) * 0.5 + o.towerMargin + o.edgeMargin);
            const int first = int(hash01(o.seed, 6, uint32_t(gi)) * 4.0);
            for (int t = 0; t < 4; ++t)
            {
                const int corner = (first + t) % 4;
                const glm::dvec2 at((corner & 1) ? hi.x - half.x : lo.x + half.x, (corner & 2) ? hi.y - half.y : lo.y + half.y);
                const glm::dvec2 rad(std::max(f.size.x, f.size.z) * 0.5);
                bool fits = top.inside(at);
                for (const glm::dvec2 q : {glm::dvec2(-1, -1), glm::dvec2(1, -1), glm::dvec2(1, 1), glm::dvec2(-1, 1)})
                    fits = fits && top.inside(at + q * rad);
                if (!fits)
                    continue;
                const double turn = 90.0 * int(hash01(o.seed, 7, uint32_t(gi)) * 4.0);
                const glm::dvec3 c(at.x, top.y + f.size.y * 0.5, at.y);
                Placed p;
                p.xf = gen::rotateAbout(c, glm::dvec3(0, 1, 0), turn) * gen::place(f, c, {1, 0, 0}, {0, 1, 0}, {0, 0, 1});
                p.slot = 1;
                p.piece = pick;
                p.random = float(hash01(o.seed, 8, uint32_t(gi)));
                r.placed.push_back(p);
                ++r.towers;
                occupied.push_back({at - rad - o.towerMargin, at + rad + o.towerMargin});
                break;
            }
        }

        // The surface: grid cells whose centres lie on the top, with the free mask at their corners.
        glm::dvec2 lo(1e300), hi(-1e300);
        for (const auto& poly : top.faces)
            for (const glm::dvec2& p : poly)
            {
                lo = glm::min(lo, p);
                hi = glm::max(hi, p);
            }
        const double cell = std::max(5.0, o.cell);
        const int nx = std::max(1, int(std::ceil((hi.x - lo.x) / cell))), nz = std::max(1, int(std::ceil((hi.y - lo.y) / cell)));
        const double cx = (hi.x - lo.x) / nx, cz = (hi.y - lo.y) / nz;
        std::map<std::pair<int, int>, Index> points;
        auto point = [&](int i, int j) {
            auto [it, added] = points.try_emplace({i, j}, lila::geo::kInvalid);
            if (added)
                it->second = r.surface.addPoint(glm::dvec3(lo.x + cx * i, top.y, lo.y + cz * j));
            return it->second;
        };
        for (int j = 0; j < nz; ++j)
            for (int i = 0; i < nx; ++i)
                if (top.inside({lo.x + cx * (i + 0.5), lo.y + cz * (j + 0.5)}))
                    r.surface.addFace({point(i, j), point(i, j + 1), point(i + 1, j + 1), point(i + 1, j)});
        r.tops.push_back(std::move(top));
        // Corners close to the edge are not free.
        auto free = r.surface.pointAttrs().getOrAdd<float>(kFreeAttr);
        for (const auto& [ij, p] : points)
        {
            const glm::dvec3 q = r.surface.position(p);
            const glm::dvec2 xz(q.x, q.z);
            double edge = 1e300;
            for (const auto& [a, b] : edges)
                edge = std::min(edge, segDistance(xz, {a.x, a.z}, {b.x, b.z}));
            float v = edge < o.edgeMargin ? 0.0f : 1.0f;
            for (const Box2& b : occupied)
                if (xz.x > b.lo.x && xz.x < b.hi.x && xz.y > b.lo.y && xz.y < b.hi.y)
                    v = 0.0f;
            free[p] = v;
        }
    }
    gen::boxUvs(r.surface);
    return r;
}
} // namespace weathering::rooftop
