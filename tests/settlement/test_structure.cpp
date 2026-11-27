// Settlement around and on a large structure: exclusions, streets-only layouts, routes, walkable surfaces, scale, and
// the commands that edit them (undo, save and reopen).

#include "TestUtil.h"
#include "SettlementCore.h"

#include "extensions/ExtensionManager.h"
#include "geo/Bvh.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <random>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options profileOptions(const char* name, uint32_t seed = 1)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.seed = seed;
    return o;
}

/** A quad from x0 to x1 and z0 to z1, its height y0 along x0 rising to y1 along x1, facing up. */
void quad(geo::Mesh& m, double x0, double z0, double x1, double z1, double y0, double y1)
{
    const geo::Index a = m.addPoint({x0, y0, z0}), b = m.addPoint({x1, y1, z0}), c = m.addPoint({x0, y0, z1}), d = m.addPoint({x1, y1, z1});
    m.addFace({a, c, d, b});
}

/** A closed box, faces out. */
void box(geo::Mesh& m, glm::dvec3 lo, glm::dvec3 hi)
{
    geo::Index p[8];
    for (int k = 0; k < 8; ++k)
        p[k] = m.addPoint({k & 1 ? hi.x : lo.x, k & 2 ? hi.y : lo.y, k & 4 ? hi.z : lo.z});
    m.addFace({p[0], p[2], p[3], p[1]}); // -z
    m.addFace({p[4], p[5], p[7], p[6]}); // +z
    m.addFace({p[0], p[4], p[6], p[2]}); // -x
    m.addFace({p[1], p[3], p[7], p[5]}); // +x
    m.addFace({p[2], p[6], p[7], p[3]}); // top
    m.addFace({p[0], p[1], p[5], p[4]}); // bottom
}

/**
 * A small temple: a floor at ground level (x 0..4000), a ramp up to a terrace 10 m high (x 4000..8000 and 8000..12000),
 * and an upper floor right above the ground floor, reached from the terrace by a bridge.
 */
geo::Mesh temple()
{
    geo::Mesh m;
    quad(m, 0, 0, 4000, 4000, 0, 0);          // ground floor
    quad(m, 4000, 0, 8000, 1000, 0, 1000);    // ramp
    quad(m, 8000, 0, 12000, 4000, 1000, 1000); // terrace
    quad(m, 4000, 3000, 8000, 4000, 1000, 1000); // bridge
    quad(m, 0, 0, 4000, 4000, 1000, 1000);    // upper floor, over the ground floor
    return m;
}

bool anyInside(const Polygon& region, std::span<const Vec2> pts, double tol)
{
    for (const Vec2& q : pts)
        if (contains(region.pts, q) && boundaryDistance(region.pts, q) > tol)
            return true;
    return false;
}

/** Every street, lot and envelope stays out of the exclusions (sides may run along their edge). */
void checkClear(const Layout& l)
{
    REQUIRE(!l.exclusions.empty());
    const StreetGraph& g = l.graph;
    for (const Polygon& x : l.exclusions)
    {
        for (size_t e = 0; e < g.edges.size(); ++e)
        {
            if (!g.isStreet(int(e)))
                continue;
            const Vec2 a = g.nodes[size_t(g.edges[e].a)].p, b = g.nodes[size_t(g.edges[e].b)].p;
            const Vec2 pts[] = {a, (a + b) * 0.5, b};
            CHECK_FALSE(anyInside(x, pts, 5.0));
        }
        for (const Lot& lot : l.lots)
        {
            CHECK_FALSE(anyInside(x, lot.outline.pts, 5.0));
        }
        for (const Envelope& env : l.envelopes)
            CHECK_FALSE(anyInside(x, env.footprint.pts, 5.0));
    }
}

void loadSettlementPackage()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
}

json xzList(std::initializer_list<std::pair<double, double>> pts)
{
    json a = json::array();
    for (const auto& [x, z] : pts)
        a.push_back(json::array({x, z}));
    return a;
}
} // namespace

TEST_CASE("settlement: exclusions stay clear in every pattern")
{
    Site site;
    site.size = 120000.0;
    Exclusion x;
    x.id = 1;
    x.pts = {{-15000, -10000}, {15000, -10000}, {15000, 10000}, {-15000, 10000}};
    x.margin = 1000.0;
    site.exclusions.push_back(x);
    const std::pair<const char*, double> cases[] = {{"medievalTown", 120000}, {"usTown", 120000}, {"europeanCity", 160000},
                                                    {"scifiMegacity", 500000}, {"streetVillage", 120000}};
    for (const auto& [name, size] : cases)
    {
        const std::string label = name;
        CAPTURE(label);
        site.size = size;
        const Options o = profileOptions(name, 3);
        const Layout l = build(site, Ground::plane(), o);
        CHECK(!excluded(l, l.center));
        CHECK(!l.lots.empty());
        checkClear(l);
        const json c = checks(l, Ground::plane(), o);
        CAPTURE(c.dump());
        CHECK(c["inExclusions"].get<int>() == 0);
        CHECK(c["crossings"].get<int>() == 0);
    }
}

TEST_CASE("settlement: a mesh's footprint keeps its shape, a curve's corridor its width")
{
    // A U seen from above: the notch between its arms stays open.
    geo::Mesh u;
    box(u, {0, 0, 0}, {3000, 2000, 1000});
    box(u, {0, 0, 1000}, {1000, 2000, 4000});
    box(u, {2000, 0, 1000}, {3000, 2000, 4000});
    const std::vector<Polygon> fp = meshFootprint(u, 200.0);
    REQUIRE(fp.size() == 1);
    CHECK(contains(fp[0].pts, {500, 3500}));
    CHECK(contains(fp[0].pts, {2500, 3500}));
    CHECK(contains(fp[0].pts, {1500, 500}));
    CHECK_FALSE(contains(fp[0].pts, {1500, 3500}));
    CHECK(contains(fp[0].pts, {-100, 500})); // the margin
    CHECK_FALSE(contains(fp[0].pts, {-500, 500}));

    const std::vector<std::vector<Vec2>> lines = {{{0, 0}, {10000, 0}}};
    const std::vector<Polygon> corridor = lineFootprint(lines, 500.0);
    REQUIRE(corridor.size() == 1);
    CHECK(contains(corridor[0].pts, {5000, 400}));
    CHECK_FALSE(contains(corridor[0].pts, {5000, 700}));

    // A concave polygon grows without folding over.
    const Polygon l = makePolygon({{0, 0}, {4000, 0}, {4000, 1000}, {1000, 1000}, {1000, 4000}, {0, 4000}});
    const Polygon g = grow(l, 300.0);
    REQUIRE(!g.pts.empty());
    CHECK_FALSE(selfIntersects(g.pts));
    CHECK(contains(g.pts, {-200, 2000}));
    CHECK_FALSE(contains(g.pts, {2500, 2500}));
}

TEST_CASE("settlement: streets only, lots without buildings, and no generated streets")
{
    Options o = profileOptions("medievalTown", 2);
    o.blocks = false;
    const Layout roads = build(Site{.size = 60000.0}, Ground::plane(), o);
    CHECK(roads.blocks.empty());
    CHECK(roads.lots.empty());
    CHECK(roads.envelopes.empty());
    CHECK(stats(roads)["streetSegments"].get<int>() > 0);
    CHECK(checks(roads, Ground::plane(), o)["ok"].get<bool>());

    o.blocks = true;
    o.buildings = false;
    const Layout empty = build(Site{.size = 60000.0}, Ground::plane(), o);
    CHECK(!empty.lots.empty());
    CHECK(empty.envelopes.empty());

    // Pattern none: only what the user laid.
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    Site site{.size = 60000.0};
    SitePath p;
    p.id = 7;
    p.pts = {{-25000, std::nan(""), 0}, {25000, std::nan(""), 0}};
    p.profile = "avenue";
    site.paths.push_back(p);
    const Layout one = build(site, Ground::plane(), o);
    double metres = 0.0;
    for (size_t e = 0; e < one.graph.edges.size(); ++e)
        if (one.graph.isStreet(int(e)))
        {
            CHECK(one.graph.edges[e].path == 7);
            CHECK(edgeProfile(one, int(e)).name == "avenue");
            CHECK(edgeWidth(one, int(e)) == doctest::Approx(edgeProfile(one, int(e)).width()));
            metres += one.graph.length(int(e)) / 100.0;
        }
    CHECK(metres == doctest::Approx(500.0).epsilon(0.01));
}

TEST_CASE("settlement: routes go around exclusions and carry their class and profile")
{
    Site site{.size = 100000.0};
    site.exclusions.push_back({1, "monolith", "", {{-10000, -20000}, {10000, -20000}, {10000, 20000}, {-10000, 20000}}, 500.0});
    SitePath r;
    r.id = 2;
    r.routed = true;
    r.cls = StreetClass::Collector;
    r.profile = "serviceRoad";
    r.pts = {{-40000, std::nan(""), 0}, {40000, std::nan(""), 0}};
    site.paths.push_back(r);
    Options o = profileOptions("medievalTown");
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    const Layout l = build(site, Ground::plane(), o);
    REQUIRE(l.pathResults.size() == 1);
    CHECK(l.pathResults[0].ok);
    CHECK(l.pathResults[0].length > 80000.0); // longer than straight through
    checkClear(l);
    int edges = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
        {
            ++edges;
            CHECK(l.graph.edges[e].cls == StreetClass::Collector);
            CHECK(edgeProfile(l, int(e)).name == "serviceRoad");
        }
    CHECK(edges > 0);

    // An unknown profile is reported, not laid.
    site.paths[0].profile = "autobahn";
    const Layout bad = build(site, Ground::plane(), o);
    REQUIRE(bad.pathResults.size() == 1);
    CHECK_FALSE(bad.pathResults[0].ok);
    CHECK(checks(bad, Ground::plane(), o)["failedPaths"].size() == 1);
}

TEST_CASE("settlement: paths walk the structure's floors, ramps and terraces")
{
    const geo::Mesh mesh = temple();
    const WalkSurface walk = WalkSurface::fromMesh(mesh);
    REQUIRE(!walk.empty());
    geo::MeshBvh bvh(mesh);

    // Ground floor to the floor right above it: only by the ramp, the terrace and the bridge.
    WalkSurface::Spot a, b;
    REQUIRE(walk.locate({2000, 0, 2000}, 300, &a));
    REQUIRE(walk.locate({2000, 1000, 2000}, 300, &b));
    CHECK(a.p.y == doctest::Approx(0.0));
    CHECK(b.p.y == doctest::Approx(1000.0));
    const std::vector<glm::dvec3> up = walk.route(a, b);
    REQUIRE(up.size() >= 2);
    double len = 0.0, east = 0.0;
    for (size_t k = 0; k + 1 < up.size(); ++k)
        len += glm::length(up[k + 1] - up[k]);
    for (const glm::dvec3& p : up)
    {
        east = std::max(east, p.x);
        geo::BvhClosest c;
        REQUIRE(bvh.closestPoint(p, c, 100.0));
        CHECK(c.distance < 1.0);
    }
    CHECK(len > 10000.0);
    CHECK(east > 7000.0);

    // From the ground outside to the terrace: over the ground to the temple, then up the ramp.
    Site site{.size = 60000.0};
    site.attractors = {{6000, 2000}};
    site.hasCenter = true;
    site.exclusions.push_back({1, "", "", {{0, 0}, {12000, 0}, {12000, 4000}, {0, 4000}}, 300.0});
    SitePath r;
    r.id = 3;
    r.routed = true;
    r.cls = StreetClass::Path;
    r.pts = {{-20000, std::nan(""), 2000}, {10000, 1000, 2000}};
    site.paths.push_back(r);
    Options o = profileOptions("medievalTown");
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    const Layout l = build(site, Ground::plane(), o, &walk);
    REQUIRE(l.pathResults.size() == 1);
    CAPTURE(l.pathResults[0].error);
    CHECK(l.pathResults[0].ok);
    CHECK(l.pathResults[0].surfaceLength > 4000.0);
    REQUIRE(!l.surfacePaths.empty());
    // Laid in the route's direction: from the ground up to the terrace.
    CHECK(l.surfacePaths.front().pts.back().y == doctest::Approx(1000.0));
    CHECK(l.surfacePaths.front().pts.back().x == doctest::Approx(10000.0));
    CHECK(l.surfacePaths.front().pts.front().y == doctest::Approx(0.0));
    checkClear(l);
    // Surface points within the temple lie on it.
    for (const glm::dvec3& p : l.surfacePaths.front().pts)
        if (p.x > 0.0 && p.x < 12000.0 && p.z > 0.0 && p.z < 4000.0)
        {
            geo::BvhClosest c;
            REQUIRE(bvh.closestPoint(p, c, 100.0));
            CHECK(c.distance < 1.0);
        }
    // And the street lines carry both parts, the surface one in 3D.
    bool surface = false, ground = false;
    for (const StreetLine& s : streetLines(l, Ground::plane()))
    {
        surface = surface || (s.surface && s.path == 3);
        ground = ground || (!s.surface && s.path == 3);
    }
    CHECK(surface);
    CHECK(ground);
}

TEST_CASE("settlement: the ground grid grows with the terrain and ribbons follow it at 10 km")
{
    // 10 km of rolling hills, 100 m quads.
    geo::Mesh hills;
    const int n = 100;
    const double size = 1.0e6;
    auto h = [](double x, double z) { return 4000.0 * std::sin(x / 90000.0) * std::cos(z / 70000.0); };
    std::vector<geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double px = -size / 2 + size * x / n, pz = -size / 2 + size * z / n;
            ids.push_back(hills.addPoint({px, h(px, pz), pz}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const geo::Index a = ids[size_t(z * (n + 1) + x)], b = a + 1, c = a + geo::Index(n + 1), d = c + 1;
            hills.addFace({a, c, d, b});
        }
    const Ground ground = Ground::fromMesh(hills, 200.0);
    CHECK(ground.gridWidth() > 1024);
    CHECK(ground.cellSize() < 300.0);
    // Split the way the ground and the heightfield renderer split the quads.
    geo::TriangulateOptions split;
    split.quads = geo::QuadSplit::FirstCorner;
    geo::MeshBvh bvh;
    bvh.build(hills, geo::triangulate(hills, split));
    auto meshY = [&](double x, double z) {
        geo::BvhHit hit;
        REQUIRE(bvh.raycast({x, 1e5, z}, {0, -1, 0}, hit, 2e5));
        return hit.point.y;
    };
    CHECK(ground.height({123456, -234567}) == doctest::Approx(meshY(123456, -234567)).epsilon(0.001));

    Site site;
    site.outline = {{-480000, -480000}, {480000, -480000}, {480000, 480000}, {-480000, 480000}};
    // Routed over the hills and around a 400 m structure in the middle.
    site.exclusions.push_back({1, "", "", {{-20000, -20000}, {20000, -20000}, {20000, 20000}, {-20000, 20000}}, 2000.0});
    SitePath p;
    p.id = 1;
    p.routed = true;
    p.pts = {{-450000, std::nan(""), -400000}, {450000, std::nan(""), 400000}};
    site.paths.push_back(p);
    Options o = profileOptions("medievalTown");
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    const auto t0 = std::chrono::steady_clock::now();
    const Layout l = build(site, ground, o);
    MESSAGE("10 km route: " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s");
    REQUIRE(l.pathResults.size() == 1);
    CHECK(l.pathResults[0].ok);
    checkClear(l);
    const MeshParts parts = toMeshes(l, ground);
    REQUIRE(parts.streets.numFaces() > 100);
    // Between draped points the ribbon stays within a few cm of its lift over the hills: no road dives into a hillside.
    double worst = 0.0;
    for (geo::Index f = 0; f < parts.streets.faceCapacity(); ++f)
    {
        if (!parts.streets.faceValid(f))
            continue;
        const std::vector<geo::Index> pts = parts.streets.facePoints(f);
        const glm::dvec3 m = (parts.streets.position(pts[0]) + parts.streets.position(pts[1])) * 0.5;
        const double above = m.y - meshY(m.x, m.z);
        worst = std::max({worst, -above, above - 20.0});
    }
    CHECK(worst < 5.0);
}

TEST_CASE("settlement: km-scale routes around and up a 300 m structure")
{
    // A tower 300 m tall, a terrace 100 m up on its north side and a 400 m ramp down to the ground.
    geo::Mesh s;
    box(s, {-15000, 0, -15000}, {15000, 30000, 15000});
    quad(s, -20000, 15000, 20000, 25000, 10000, 10000);
    quad(s, 20000, 15000, 60000, 25000, 10000, 0);
    const auto t0 = std::chrono::steady_clock::now();
    const WalkSurface walk = WalkSurface::fromMesh(s);
    Site site;
    const double half = 500000.0;
    site.outline = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
    for (const Polygon& p : meshFootprint(s, 2000.0))
        site.exclusions.push_back({int(site.exclusions.size()) + 1, "structure", "", p.pts, 0.0});
    REQUIRE(!site.exclusions.empty());
    int id = 10;
    for (const Vec2 corner : {Vec2(-1, -1), Vec2(1, -1), Vec2(1, 1), Vec2(-1, 1)})
    {
        SitePath r;
        r.id = id++;
        r.routed = true;
        r.cls = StreetClass::Collector;
        r.profile = "serviceRoad";
        r.pts = {{corner.x * 450000, std::nan(""), corner.y * 450000}, {0, 10000, 20000}};
        site.paths.push_back(r);
    }
    SitePath ring;
    ring.id = id++;
    ring.routed = true;
    ring.cls = StreetClass::Arterial;
    ring.pts = {{120000, std::nan(""), 0}, {0, std::nan(""), 120000}, {-120000, std::nan(""), 0}, {0, std::nan(""), -120000}, {120000, std::nan(""), 0}};
    site.paths.push_back(ring);
    Options o = profileOptions("modernCity");
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    const Layout l = build(site, Ground::plane(), o, &walk);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("km-scale structure layout: " << seconds << " s");
    CHECK(seconds < 60.0);
    for (const PathResult& r : l.pathResults)
    {
        CAPTURE(r.id);
        CAPTURE(r.error);
        CHECK(r.ok);
        if (r.id < 14)
            CHECK(r.surfaceLength > 3000.0);
    }
    checkClear(l);
    const json c = checks(l, Ground::plane(), o);
    CHECK(c["crossings"].get<int>() == 0);
    CHECK(c["inExclusions"].get<int>() == 0);

    // A grid town 4 km across fills in around the structure.
    Site town;
    town.size = 400000.0;
    town.exclusions = site.exclusions;
    const Options grid = profileOptions("usTown");
    const Layout g = build(town, Ground::plane(), grid);
    CHECK(g.lots.size() > 100);
    checkClear(g);
    const RoadNetwork net = roadNetwork(g, Ground::plane());
    CHECK(!net.junctions.empty());
    for (const Junction& j : net.junctions)
    {
        CHECK(j.ends.size() >= 3);
        for (const RoadEnd& e : j.ends)
            CHECK(glm::length(glm::dvec2(e.p.x - j.p.x, e.p.z - j.p.z)) > 0.0);
    }
}

TEST_CASE("settlement: edits read from JSON and refuse what they cannot use")
{
    Site site;
    applyEdits(site, json::parse(R"({"next": 4,
        "streets": [{"id": 2, "points": [[0, 0], [100, 50, 0]], "class": "path", "profile": "stairs"}],
        "routes": [{"id": 1, "stops": [[0, 0], [10, 10]], "mode": "ground"}],
        "exclusions": [{"id": 3, "polygon": [[0, 0], [10, 0], [10, 10]], "margin": 20}],
        "roadProfiles": {"causeway": {"base": "street", "sidewalk": 400}}})"));
    REQUIRE(site.paths.size() == 2);
    CHECK(site.paths[0].id == 1); // ordered by id
    CHECK(site.paths[0].mode == PathMode::Ground);
    CHECK(site.paths[1].cls == StreetClass::Path);
    CHECK(std::isnan(site.paths[1].pts[0].y));
    CHECK(site.paths[1].pts[1].y == doctest::Approx(50.0));
    REQUIRE(site.exclusions.size() == 1);
    CHECK(site.exclusions[0].margin == doctest::Approx(20.0));
    REQUIRE(site.roadProfiles.size() == 1);
    CHECK(site.roadProfiles[0].width() == doctest::Approx(600.0 + 800.0));
    Site bad;
    CHECK_THROWS(applyEdits(bad, json::parse(R"({"streets": [{"id": 1, "points": [[0, 0]]}]})")));
    CHECK_THROWS(applyEdits(bad, json::parse(R"({"routes": [{"id": 1, "stops": [[0, 0], [1, 1]], "class": "boundary"}]})")));
    CHECK_THROWS(applyEdits(bad, json::parse(R"({"roadProfiles": {"x": {"lanez": 2}}})")));
}

TEST_CASE("settlement: edit commands route, change and delete, undo, and survive save and reopen")
{
    loadSettlementPackage();
    run("newScene");
    const geo::Mesh t = temple();
    json points = json::array(), faces = json::array();
    for (geo::Index p = 0; p < t.pointCapacity(); ++p)
        points.push_back(json::array({t.position(p).x, t.position(p).y, t.position(p).z}));
    for (geo::Index f = 0; f < t.faceCapacity(); ++f)
        faces.push_back(t.facePoints(f));
    const std::string templeName = run("createMesh", {{"points", points}, {"faces", faces}, {"name", "Temple"}}).get<std::string>();

    const json made = run("settlementCreate", {{"size", 60000.0}, {"pattern", "none"}, {"blocks", false}, {"exclude", json::array({templeName})},
                                               {"exclusionMargin", 300.0}, {"walkable", templeName}, {"attractors", json::array({json::array({6000, 2000})})}});
    const std::string mesh = made["mesh"].get<std::string>();
    CHECK(made["exclusions"].get<int>() == 1);
    CHECK(made["lots"].get<int>() == 0);

    const json routed = run("settlementRoute", {{"args", {mesh}}, {"start", json::array({-20000, 2000})}, {"end", json::array({10000, 1000, 2000})},
                                                 {"streetClass", "path"}, {"profile", "footpath"}});
    REQUIRE(routed["paths"].size() == 1);
    CAPTURE(routed.dump());
    CHECK(routed["paths"][0]["ok"].get<bool>());
    CHECK(routed["paths"][0]["surfaceLengthM"].get<double>() > 40.0);
    const int routeId = routed["ids"][0].get<int>();

    const json street = run("settlementAddStreet", {{"args", {mesh}}, {"points", xzList({{-20000, -25000}, {20000, -25000}})}, {"profile", "avenue"}});
    const int streetId = street["ids"][0].get<int>();
    CHECK(street["paths"][0]["lengthM"].get<double>() == doctest::Approx(400.0).epsilon(0.01));

    const json edited = run("settlementEditStreet", {{"args", {mesh}}, {"id", streetId}, {"points", xzList({{-20000, -25000}, {0, -25000}})}});
    CHECK(edited["paths"][0]["lengthM"].get<double>() == doctest::Approx(200.0).epsilon(0.01));
    run("undo");
    json info = run("settlementInfo", {{"args", {mesh}}});
    double streetLength = 0.0;
    for (const json& p : info["paths"])
        if (p["id"].get<int>() == streetId)
            streetLength = p["lengthM"].get<double>();
    CHECK(streetLength == doctest::Approx(400.0).epsilon(0.01));

    const json excl = run("settlementAddExclusion", {{"args", {mesh}}, {"polygon", xzList({{-15000, -27000}, {-10000, -27000}, {-10000, -23000}, {-15000, -23000}})}});
    CHECK(excl["checks"]["inExclusions"].get<int>() == 0);
    run("settlementDelete", {{"args", {mesh}}, {"ids", json::array({excl["ids"][0]})}});
    CHECK_THROWS(run("settlementDelete", {{"args", {mesh}}, {"ids", json::array({999})}}));
    CHECK_THROWS(run("settlementRoute", {{"args", {mesh}}, {"start", json::array({0, 0})}, {"end", json::array({1, 1})}, {"streetClass", "boundary"}}));

    const json before = run("settlementInfo", {{"args", {mesh}}});
    std::random_device rd;
    const auto path = std::filesystem::temp_directory_path() / ("lila_settlement_" + std::to_string(rd()) + ".lila");
    run("saveScene", {{"args", {path.string()}}});
    run("newScene");
    run("openScene", {{"args", {path.string()}}});
    const json after = run("settlementInfo", {{"args", {mesh}}});
    CHECK(after["paths"] == before["paths"]);
    CHECK(after["exclusions"] == before["exclusions"]);
    CHECK(after["surfacePaths"] == before["surfacePaths"]);
    CHECK(after["settings"]["edits"] == before["settings"]["edits"]);
    bool hasRoute = false;
    for (const json& p : after["paths"])
        hasRoute = hasRoute || (p["id"].get<int>() == routeId && p["ok"].get<bool>());
    CHECK(hasRoute);
    std::error_code ec;
    std::filesystem::remove(path, ec);

    // Surfaces switch off and back on.
    const json off = run("settlementSurfaces", {{"args", {mesh}}, {"walkable", ""}});
    CHECK(off["walkableNodes"].get<int>() == 0);
    CHECK_FALSE(off["checks"]["ok"].get<bool>()); // the route to the terrace cannot be laid now
    const json on = run("settlementSurfaces", {{"args", {mesh}}, {"walkable", templeName}});
    CHECK(on["walkableNodes"].get<int>() > 0);
}

TEST_CASE("settlement: grading stamps lots and squares into the terrain's City layer, and again after edits")
{
    loadSettlementPackage();
    if (!ops::find("terrainStampPad"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({60000.0, 60000.0})}, {"resolution", 129}, {"amplitude", 1500.0}, {"seed", 3}});
    const std::string ground = terrain["mesh"].get<std::string>();
    const json made = run("settlementCreate", {{"profile", "hamlet"}, {"size", 30000.0}, {"seed", 2}, {"ground", ground}});
    const std::string mesh = made["mesh"].get<std::string>();
    CHECK_THROWS(run("settlementGrade", {{"args", {mesh}}, {"terrain", "NoSuchTerrain"}, {"lots", false}, {"plazas", false}}));

    const json graded = run("settlementGrade", {{"args", {mesh}}, {"padMode", "terraced"}});
    CAPTURE(graded.dump());
    CHECK(graded["errors"].empty());
    CHECK(graded["terrain"] == ground);
    CHECK(graded["streets"].get<int>() > 0);
    CHECK(graded["pads"].get<int>() > 0);
    CHECK(graded.contains("revision"));
    const json stamps = run("terrainStamps", {{"args", {ground}}});
    const json city = stamps["layers"]["City"];
    CHECK(int(city.size()) == graded["pads"].get<int>() + graded.value("regions", 0));
    int terraced = 0;
    for (const json& s : city)
        terraced += s.value("mode", std::string()) == "terraced";
    CHECK(terraced == graded["pads"].get<int>());

    // Without the buildings, grading again removes every pad.
    run("setAttr", {{"args", {made["modifier"].get<std::string>() + ".blocks", false}}});
    const json again = run("settlementGrade", {{"args", {mesh}}});
    CAPTURE(again.dump());
    CHECK(again["errors"].empty());
    CHECK(again["pads"].get<int>() == 0);
    CHECK(again["removed"].get<int>() >= graded["pads"].get<int>());
    CHECK(run("terrainStamps", {{"args", {ground}}})["layers"]["City"].size() == size_t(again["regions"].get<int>()));
}
