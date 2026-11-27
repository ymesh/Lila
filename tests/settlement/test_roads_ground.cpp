#include "TestUtil.h"
#include "Generators.h"

#include "geo/Mesh.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>
#include <unordered_map>

using namespace lila;
using namespace settlement;

namespace
{
/** Up-facing faces of a mesh as XZ triangles with their heights, binned for point lookups. */
struct Cover
{
    struct Tri
    {
        glm::dvec3 a, b, c;
        int32_t material;
    };
    std::vector<Tri> tris;
    std::unordered_map<int64_t, std::vector<uint32_t>> bins;
    static constexpr double kCell = 1000.0;

    static int64_t key(int x, int z) { return (int64_t(x) << 32) ^ int64_t(uint32_t(z)); }

    explicit Cover(const geo::Mesh& m)
    {
        const auto mat = m.faceAttrs().get<int32_t>("material");
        for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
        {
            const std::vector<geo::Index> pts = m.facePoints(f);
            glm::dvec3 n(0.0);
            for (size_t k = 0; k < pts.size(); ++k)
            {
                const glm::dvec3 a = m.position(pts[k]), b = m.position(pts[(k + 1) % pts.size()]);
                n += glm::dvec3((a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y));
            }
            if (n.y < 0.5 * glm::length(n))
                continue;
            for (size_t k = 1; k + 1 < pts.size(); ++k)
            {
                const Tri t{m.position(pts[0]), m.position(pts[k]), m.position(pts[k + 1]), mat.empty() ? 0 : mat[f]};
                const uint32_t id = uint32_t(tris.size());
                tris.push_back(t);
                const double x0 = std::min({t.a.x, t.b.x, t.c.x}), x1 = std::max({t.a.x, t.b.x, t.c.x});
                const double z0 = std::min({t.a.z, t.b.z, t.c.z}), z1 = std::max({t.a.z, t.b.z, t.c.z});
                for (int x = int(std::floor(x0 / kCell)); x <= int(std::floor(x1 / kCell)); ++x)
                    for (int z = int(std::floor(z0 / kCell)); z <= int(std::floor(z1 / kCell)); ++z)
                        bins[key(x, z)].push_back(id);
            }
        }
    }
    /** The triangle over p, or nullptr. */
    const Tri* at(Vec2 p) const
    {
        const auto it = bins.find(key(int(std::floor(p.x / kCell)), int(std::floor(p.y / kCell))));
        if (it == bins.end())
            return nullptr;
        for (uint32_t i : it->second)
        {
            const Tri& t = tris[i];
            const double d1 = cross(Vec2(t.b.x - t.a.x, t.b.z - t.a.z), Vec2(p.x - t.a.x, p.y - t.a.z));
            const double d2 = cross(Vec2(t.c.x - t.b.x, t.c.z - t.b.z), Vec2(p.x - t.b.x, p.y - t.b.z));
            const double d3 = cross(Vec2(t.a.x - t.c.x, t.a.z - t.c.z), Vec2(p.x - t.c.x, p.y - t.c.z));
            if ((d1 >= 0.0 && d2 >= 0.0 && d3 >= 0.0) || (d1 <= 0.0 && d2 <= 0.0 && d3 <= 0.0))
                return &t;
        }
        return nullptr;
    }
};

Layout testLayout()
{
    Layout l{StreetGraph(150.0, 2000.0)};
    for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path})
    {
        RoadProfile r;
        builtinRoadProfile(defaultRoadProfile(c), &r);
        l.roads.push_back(r);
    }
    REQUIRE(builtinProfile("mexicanCity", &l.profile));
    l.radius = 100000.0;
    return l;
}

Layout city(double size, uint32_t seed)
{
    Options o;
    REQUIRE(builtinProfile("mexicanCity", &o.profile));
    o.seed = seed;
    Site s;
    s.size = size;
    return build(s, Ground::plane(), o);
}

/** A terrain like a heightfield's: one grid of quads, each split from its first corner, over rolling ground. */
geo::Mesh rollingTerrain(double half, double cell)
{
    const int n = int(std::ceil(2.0 * half / cell));
    std::vector<glm::dvec3> pts;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const double x = -half + i * cell, z = -half + j * cell;
            pts.push_back({x, 300.0 * std::sin(x / 1900.0) * std::cos(z / 1700.0) + 0.002 * x * x / 100.0, z});
        }
    std::vector<geo::Index> sizes, corners;
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const geo::Index p = geo::Index(j * (n + 1) + i);
            sizes.push_back(4);
            corners.insert(corners.end(), {p, p + geo::Index(n + 1), p + geo::Index(n + 2), p + 1});
        }
    return geo::Mesh::fromPolygons(pts, sizes, corners);
}

bool insidePolygon(const Polygon& poly, Vec2 p) { return contains(poly.pts, p); }
} // namespace

TEST_CASE("settlement blocks: a concave face whose short side collapses when moved in keeps its whole inset")
{
    // An L of 20 m arms with a 1 m chamfer at one outer corner: moved in 4.5 m the chamfer turns back on itself.
    Polygon l = makePolygon({{0.0, 0.0}, {3900.0, 0.0}, {4000.0, 100.0}, {4000.0, 2000.0}, {2000.0, 2000.0}, {2000.0, 4000.0}, {0.0, 4000.0}});
    const std::vector<double> d(l.size(), 450.0);
    const Polygon in = inset(l, d);
    REQUIRE(in.size() >= 6);
    // The exact inset of the unchamfered L: 31 x 11 m plus 11 x 20 m.
    CHECK(area(in) == doctest::Approx(3100.0 * 1100.0 + 1100.0 * 2000.0).epsilon(0.01));
    for (const Vec2& q : in.pts)
        CHECK(insidePolygon(l, q));
}

TEST_CASE("settlement ground: on a heightfield's grid the ground lies on the terrain drawn, never under it")
{
    const Layout l = city(30000.0, 3);
    const geo::Mesh terrain = rollingTerrain(40000.0, 780.0);
    const Ground g = Ground::fromMesh(terrain, 200.0);
    REQUIRE(g.lattice() != nullptr);
    CHECK(g.lattice()->cell.x == doctest::Approx(780.0));
    CHECK(g.lattice()->mainDiagonal);

    const MeshParts parts = toMeshes(l, g);
    const geo::Mesh& m = parts.ground;
    REQUIRE(m.numFaces() > 100);
    int samples = 0, under = 0;
    double worst = 0.0;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 1; k + 1 < pts.size(); ++k)
        {
            const glm::dvec3 a = m.position(pts[0]), b = m.position(pts[k]), c = m.position(pts[k + 1]);
            for (const glm::dvec3& w : {glm::dvec3(1.0, 1.0, 1.0) / 3.0, glm::dvec3(0.5, 0.5, 0.0), glm::dvec3(0.0, 0.5, 0.5), glm::dvec3(0.5, 0.0, 0.5)})
            {
                const glm::dvec3 p = a * w.x + b * w.y + c * w.z;
                const double gap = p.y - g.surfaceHeight({p.x, p.z});
                worst = std::min(worst, gap);
                under += gap < 0.5 ? 1 : 0;
                ++samples;
            }
        }
    }
    CAPTURE(samples);
    CAPTURE(worst);
    CHECK(under == 0);
}

TEST_CASE("settlement ground: blocks are covered whole, open blocks only near their streets and buildings")
{
    const Layout l = city(60000.0, 5);
    const MeshParts parts = toMeshes(l, Ground::plane());
    const Cover ground(parts.ground);
    REQUIRE(!ground.tris.empty());

    auto open = [](const Block& b) { return std::ranges::any_of(b.outline.tag, [](int t) { return t < 0; }); };
    REQUIRE(std::ranges::any_of(l.blocks, open));
    REQUIRE(std::ranges::any_of(l.blocks, [&](const Block& b) { return !open(b); }));
    auto streetDistance = [&](Vec2 p) {
        double best = 1e300;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
        {
            if (!l.graph.isStreet(int(e)))
                continue;
            const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
            const double t = std::clamp(glm::dot(p - a, b - a) / std::max(glm::dot(b - a, b - a), 1e-9), 0.0, 1.0);
            best = std::min(best, glm::length(p - (a + (b - a) * t)) - 0.5 * edgeWidth(l, int(e)));
        }
        return best;
    };
    const double reach = std::max(l.profile.lotDepth, 500.0);

    // No holes: points inside a closed block are all covered, and in an open block those near a street.
    int closedSamples = 0, closedHoles = 0, openSamples = 0, openHoles = 0;
    for (const Block& b : l.blocks)
    {
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : b.outline.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        for (double x = lo.x + 53.0; x < hi.x; x += 271.0)
            for (double z = lo.y + 61.0; z < hi.y; z += 263.0)
            {
                const Vec2 p(x, z);
                if (!insidePolygon(b.outline, p) || boundaryDistance(b.outline.pts, p) < 30.0)
                    continue;
                if (!open(b))
                {
                    ++closedSamples;
                    closedHoles += ground.at(p) ? 0 : 1;
                }
                else if (streetDistance(p) < reach - 100.0)
                {
                    ++openSamples;
                    openHoles += ground.at(p) ? 0 : 1;
                }
            }
    }
    CAPTURE(closedSamples);
    CAPTURE(openSamples);
    CHECK(closedSamples > 1000);
    CHECK(openSamples > 100);
    CHECK(closedHoles == 0);
    CHECK(openHoles <= openSamples / 200);

    // No spill: every ground triangle lies in a block (to within the trace's grid), and in open land only near a street
    // or a building.
    std::vector<const Polygon*> buildings;
    for (const Envelope& env : l.envelopes)
        buildings.push_back(&env.footprint);
    int spilled = 0, far = 0;
    for (const Cover::Tri& t : ground.tris)
    {
        const Vec2 c((t.a.x + t.b.x + t.c.x) / 3.0, (t.a.z + t.b.z + t.c.z) / 3.0);
        const auto block = std::ranges::find_if(l.blocks, [&](const Block& b) { return insidePolygon(b.outline, c) || boundaryDistance(b.outline.pts, c) < 150.0; });
        if (block == l.blocks.end())
        {
            ++spilled;
            continue;
        }
        if (open(*block) && streetDistance(c) > reach + 200.0 &&
            std::ranges::none_of(buildings, [&](const Polygon* fp) { return insidePolygon(*fp, c) || boundaryDistance(fp->pts, c) < 400.0; }))
            ++far;
    }
    CHECK(spilled == 0);
    CHECK(far == 0);
}

TEST_CASE("settlement meshes: the carriageways and sidewalks cover every street and the islands between them")
{
    // A divided road 10 m apart, joined by cross streets: the face between its carriageways is too small for a block.
    Layout l = testLayout();
    l.graph.addStreet({-9000.0, 0.0}, {9000.0, 0.0}, StreetClass::Local);
    l.graph.addStreet({-9000.0, 1000.0}, {9000.0, 1300.0}, StreetClass::Local);
    for (double x : {-9000.0, -3000.0, 3000.0, 9000.0})
    {
        l.graph.addStreet({x, -6000.0}, {x, 0.0}, StreetClass::Local);
        l.graph.addStreet({x, 0.0}, {x, 1000.0 + 300.0 * (x + 9000.0) / 18000.0}, StreetClass::Local);
        l.graph.addStreet({x, 1000.0 + 300.0 * (x + 9000.0) / 18000.0}, {x, 7000.0}, StreetClass::Local);
    }
    // Faces wide enough for a block become blocks, as a layout's would; the rest are islands.
    for (const Face& f : planarFaces(l.graph))
    {
        std::vector<double> d;
        for (int e : f.outline.tag)
            d.push_back(0.5 * edgeWidth(l, e));
        if (Polygon in = inset(f.outline, d); in.size() >= 3 && area(in) >= l.profile.minLot * kM2)
        {
            Block b;
            b.id = int(l.blocks.size());
            b.outline = std::move(in);
            l.blocks.push_back(std::move(b));
        }
    }
    const MeshParts parts = toMeshes(l, Ground::plane());
    const Cover streets(parts.streets), ground(parts.ground);
    auto covered = [&](Vec2 p) { return streets.at(p) || ground.at(p); };

    int samples = 0, holes = 0;
    // Along and across every street's full width.
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)))
            continue;
        const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
        const double half = 0.5 * edgeWidth(l, int(e)) - 5.0, len = glm::length(b - a);
        const Vec2 d = (b - a) / len, n(-d.y, d.x);
        for (double s = 7.0; s < len; s += 97.0)
            for (double t = -half; t <= half; t += half / 6.0)
            {
                ++samples;
                holes += covered(a + d * s + n * t) ? 0 : 1;
            }
    }
    // Inside the faces between the carriageways: blocks or islands.
    int islandSamples = 0;
    for (const Face& f : planarFaces(l.graph))
    {
        Vec2 lo(1e300), hi(-1e300);
        for (const Vec2& q : f.outline.pts)
            lo = glm::min(lo, q), hi = glm::max(hi, q);
        if (hi.y - lo.y > 2000.0)
            continue;
        for (double x = lo.x + 31.0; x < hi.x; x += 113.0)
            for (double z = lo.y + 17.0; z < hi.y; z += 37.0)
                if (insidePolygon(f.outline, {x, z}))
                {
                    ++islandSamples;
                    holes += covered({x, z}) ? 0 : 1;
                }
    }
    CAPTURE(samples);
    CAPTURE(islandSamples);
    CHECK(islandSamples > 300);
    CHECK(holes == 0);
}

TEST_CASE("settlement meshes: a street's open end runs out as a dirt track or ends at a turning head, never alone")
{
    Layout l = testLayout();
    l.outline = makePolygon({{-20000.0, -20000.0}, {20000.0, -20000.0}, {20000.0, 20000.0}, {-20000.0, 20000.0}});
    // A cross street through the middle; one leg stops 20 m short of the outline, one ends in town.
    l.graph.addStreet({-18000.0, 0.0}, {0.0, 0.0}, StreetClass::Local);
    l.graph.addStreet({0.0, 0.0}, {6000.0, 0.0}, StreetClass::Local);
    l.graph.addStreet({0.0, -18000.0}, {0.0, 0.0}, StreetClass::Local);
    l.graph.addStreet({0.0, 0.0}, {0.0, 18000.0}, StreetClass::Local);
    const MeshParts parts = toMeshes(l, Ground::plane());
    const Cover streets(parts.streets);
    const auto& names = parts.streets.materialNames();
    auto slot = [&](const char* name) { return int32_t(std::find(names.begin(), names.end(), name) - names.begin()); };

    // At the edge: dirt past the street's end, narrower than the carriageway by its far end.
    const Cover::Tri* track = streets.at({-18000.0 - 1000.0, 0.0});
    REQUIRE(track != nullptr);
    CHECK(track->material == slot("Settlement_Dirt"));
    CHECK(streets.at({-18000.0 - 1900.0, 0.0}) != nullptr);
    CHECK(streets.at({-18000.0 - 1900.0, 300.0}) == nullptr);
    // Inside the town: a turning head wider than the street round its end, with its sidewalk round it.
    const double inner = 0.5 * edgeWidth(l, 1) - 0.5 * edgeWidth(l, 1) * 0.3;
    const Cover::Tri* head = streets.at({6000.0 + 200.0, inner + 50.0});
    REQUIRE(head != nullptr);
    CHECK(head->material == slot("Settlement_Asphalt"));
    CHECK(streets.at({6000.0 + inner + 300.0, 0.0}) != nullptr);

    // One piece: every up-facing face reaches every other through shared corners.
    const geo::Mesh& m = parts.streets;
    std::vector<int> parent(m.numPoints());
    std::iota(parent.begin(), parent.end(), 0);
    auto root = [&](int x) {
        while (parent[size_t(x)] != x)
            x = parent[size_t(x)] = parent[size_t(parent[size_t(x)])];
        return x;
    };
    std::map<std::tuple<long long, long long, long long>, int> at;
    for (geo::Index p = 0; p < geo::Index(m.numPoints()); ++p)
    {
        const glm::dvec3 q = m.position(p);
        const auto k = std::make_tuple(std::llround(q.x), std::llround(q.y), std::llround(q.z));
        if (auto it = at.find(k); it != at.end())
            parent[size_t(root(int(p)))] = root(it->second);
        else
            at[k] = int(p);
    }
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
    {
        const std::vector<geo::Index> pts = m.facePoints(f);
        for (size_t k = 1; k < pts.size(); ++k)
            parent[size_t(root(int(pts[k])))] = root(int(pts[0]));
    }
    std::set<int> pieces;
    for (geo::Index f = 0; f < geo::Index(m.numFaces()); ++f)
        pieces.insert(root(int(m.facePoints(f)[0])));
    CHECK(pieces.size() == 1);
}
