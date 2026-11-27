#include "TestUtil.h"
#include "SettlementCore.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <filesystem>
#include <random>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Polygon square(double lo, double hi) { return makePolygon({{lo, lo}, {hi, lo}, {hi, hi}, {lo, hi}}, 2); }

Site squareSite(double size)
{
    Site s;
    s.size = size;
    return s;
}

Options withProfile(const char* name, uint32_t seed = 1)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.seed = seed;
    return o;
}

/** A plane tilted along X: rises by grade per unit of X. */
geo::Mesh slope(double size, double grade, int n = 32)
{
    geo::Mesh m;
    std::vector<geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double px = -size / 2 + size * x / n, pz = -size / 2 + size * z / n;
            ids.push_back(m.addPoint({px, px * grade, pz}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const geo::Index a = ids[size_t(z * (n + 1) + x)], b = a + 1, c = a + geo::Index(n + 1), d = c + 1;
            m.addFace({a, c, d, b});
        }
    return m;
}

bool insideWithin(const Polygon& outer, Vec2 q, double tol)
{
    if (contains(outer.pts, q))
        return true;
    for (size_t k = 0; k < outer.size(); ++k)
    {
        const Vec2 a = outer.pts[k], b = outer.pts[(k + 1) % outer.size()];
        const Vec2 d = b - a;
        const double t = std::clamp(glm::dot(q - a, d) / std::max(glm::dot(d, d), 1e-12), 0.0, 1.0);
        if (glm::length(a + d * t - q) < tol)
            return true;
    }
    return false;
}

void loadSettlement()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
}
} // namespace

TEST_CASE("settlement: polygons inset, clip and keep their side tags")
{
    const Polygon sq = square(0, 100);
    CHECK(signedArea(sq.pts) == doctest::Approx(10000.0));
    const double d[] = {10, 10, 10, 10};
    const Polygon in = inset(sq, d);
    REQUIRE(in.size() == 4);
    CHECK(area(in) == doctest::Approx(6400.0));

    const Polygon half = clipHalfPlane(sq, {50, 0}, {1, 0}, 7);
    CHECK(area(half) == doctest::Approx(5000.0));
    int cut = 0;
    for (int t : half.tag)
        cut += t == 7;
    CHECK(cut == 1);

    // Clockwise input comes back counter-clockwise.
    const Polygon cw = makePolygon({{0, 0}, {0, 10}, {10, 10}, {10, 0}});
    CHECK(signedArea(cw.pts) > 0.0);

    // A concave L insets without folding over.
    const Polygon l = makePolygon({{0, 0}, {200, 0}, {200, 100}, {100, 100}, {100, 200}, {0, 200}});
    const std::vector<double> d6(6, 10.0);
    const Polygon li = inset(l, d6);
    REQUIRE(!li.pts.empty());
    CHECK_FALSE(selfIntersects(li.pts));
    CHECK(area(li) < area(l));
}

TEST_CASE("settlement: streets split each other where they cross and snap to what is near")
{
    StreetGraph g(10.0, 100.0);
    g.addStreet({0, 0}, {100, 0}, StreetClass::Local);
    g.addStreet({50, -50}, {50, 50}, StreetClass::Local);
    int junction = -1;
    for (size_t n = 0; n < g.nodes.size(); ++n)
        if (g.degree(int(n)) == 4)
            junction = int(n);
    REQUIRE(junction >= 0);
    CHECK(g.nodes[size_t(junction)].p.x == doctest::Approx(50.0));

    // An end a few units short of a street joins it (a T junction).
    g.addStreet({20, 40}, {20, 4}, StreetClass::Local);
    int tee = 0;
    for (size_t n = 0; n < g.nodes.size(); ++n)
        tee += g.degree(int(n)) == 3;
    CHECK(tee == 1);
    for (size_t e = 0; e < g.edges.size(); ++e)
        if (g.edges[e].alive)
            CHECK(g.firstCrossing(g.nodes[size_t(g.edges[e].a)].p, g.nodes[size_t(g.edges[e].b)].p, nullptr, nullptr,
                                  g.edges[e].a, g.edges[e].b) < 0);
}

TEST_CASE("settlement: a grid of streets in a square makes nine blocks, dead ends ignored")
{
    StreetGraph g(5.0, 100.0);
    const Vec2 box[] = {{0, 0}, {300, 0}, {300, 300}, {0, 300}};
    g.addPolyline(box, StreetClass::Boundary, 0, true);
    for (double c : {100.0, 200.0})
    {
        g.addStreet({c, 0}, {c, 300}, StreetClass::Local);
        g.addStreet({0, c}, {300, c}, StreetClass::Local);
    }
    g.addStreet({150, 150}, {150, 180}, StreetClass::Alley); // a dead end inside the middle block
    const std::vector<Face> faces = planarFaces(g);
    CHECK(faces.size() == 9);
    double total = 0.0;
    for (const Face& f : faces)
    {
        total += area(f.outline);
        CHECK(f.outline.size() == 4);
    }
    CHECK(total == doctest::Approx(90000.0));
}

TEST_CASE("settlement: a US grid town is ordered, connected and every building sits on its lot")
{
    const Layout l = build(squareSite(80000.0), Ground::plane(), withProfile("usTown"));
    const json m = metrics(l);
    const json c = checks(l, Ground::plane(), withProfile("usTown"));
    CHECK(m["orientationOrder"].get<double>() > 0.9);
    CHECK(c["crossings"].get<int>() == 0);
    CHECK(c["components"].get<int>() == 1);
    CHECK(c["ok"].get<bool>());
    REQUIRE(!l.envelopes.empty());
    for (const Envelope& env : l.envelopes)
    {
        const Lot& lot = l.lots[size_t(env.lot)];
        CHECK(lot.frontages > 0);
        for (const Vec2& q : env.footprint.pts)
            CHECK(insideWithin(lot.outline, q, 1.0));
        CHECK(env.floors >= 1);
        CHECK(env.base < env.ground);
    }
}

TEST_CASE("settlement: an organic town is less ordered than a grid, connected, and the same for the same seed")
{
    const Options o = withProfile("medievalTown", 7);
    const Layout a = build(squareSite(50000.0), Ground::plane(), o);
    const Layout b = build(squareSite(50000.0), Ground::plane(), o);
    CHECK(toJson(a).dump() == toJson(b).dump());

    const Layout grid = build(squareSite(50000.0), Ground::plane(), withProfile("usTown"));
    CHECK(metrics(a)["orientationEntropy"].get<double>() > metrics(grid)["orientationEntropy"].get<double>());
    const json c = checks(a, Ground::plane(), o);
    CHECK(c["crossings"].get<int>() == 0);
    CHECK(c["components"].get<int>() == 1);
    CHECK(!a.lots.empty());

    bool plaza = false;
    for (const Lot& lot : a.lots)
        plaza = plaza || lot.use == Use::Plaza;
    CHECK(plaza);

    const Layout other = build(squareSite(50000.0), Ground::plane(), withProfile("medievalTown", 8));
    CHECK(toJson(other).dump() != toJson(a).dump());
}

TEST_CASE("settlement: every built-in profile lays out streets, lots and buildings")
{
    const std::pair<const char*, double> sizes[] = {{"hamlet", 30000},     {"streetVillage", 40000}, {"medievalTown", 50000},
                                                    {"usTown", 80000},     {"manhattan", 100000},    {"europeanCity", 150000},
                                                    {"modernCity", 150000}, {"scifiMegacity", 500000}, {"mexicanCity", 100000}};
    for (const auto& [name, size] : sizes)
    {
        const std::string profileName = name;
        CAPTURE(profileName);
        const Options o = withProfile(name);
        const Layout l = build(squareSite(size), Ground::plane(), o);
        CHECK(stats(l)["streetSegments"].get<int>() > 0);
        CHECK(!l.lots.empty());
        CHECK(!l.envelopes.empty());
        CHECK(checks(l, Ground::plane(), o)["crossings"].get<int>() == 0);
    }
}

TEST_CASE("settlement: organic and mixed layouts never leave streets crossing without a junction")
{
    const std::pair<const char*, double> cases[] = {{"medievalTown", 50000}, {"europeanCity", 150000}, {"hamlet", 30000}};
    for (const auto& [name, size] : cases)
        for (uint32_t seed = 1; seed <= 6; ++seed)
        {
            const std::string label = std::string(name) + " seed " + std::to_string(seed);
            CAPTURE(label);
            const Options o = withProfile(name, seed);
            const Layout l = build(squareSite(size), Ground::plane(), o);
            const json c = checks(l, Ground::plane(), o);
            CAPTURE(c.dump());
            CHECK(c["crossings"].get<int>() == 0);
            CHECK(c["components"].get<int>() == 1);
        }
}

TEST_CASE("settlement: on a slope streets keep to the grade and buildings get foundations")
{
    const geo::Mesh hill = slope(60000.0, 0.15);
    const Ground ground = Ground::fromMesh(hill, 200.0);
    CHECK(ground.height({1000, 0}) == doctest::Approx(150.0).epsilon(0.02));
    CHECK(ground.grade({0, 0}) == doctest::Approx(0.15).epsilon(0.05));

    Site site;
    site.outline = {{-25000, -25000}, {25000, -25000}, {25000, 25000}, {-25000, 25000}};
    const Options o = withProfile("medievalTown", 3);
    const Layout l = build(site, ground, o);
    REQUIRE(!l.envelopes.empty());
    for (const Envelope& env : l.envelopes)
    {
        CHECK(env.base < env.ground);
        CHECK(env.top > env.ground);
    }
    const json c = checks(l, ground, o);
    CHECK(c["crossings"].get<int>() == 0);
    // Grown streets turn across a slope steeper than the profile allows; few, if any, stay steep.
    CHECK(c["steepStreets"].get<int>() <= int(l.graph.edges.size() / 10));
}

TEST_CASE("settlement: a river bounds blocks, only main roads bridge it, and no lot lies in it")
{
    Site site;
    site.size = 50000.0;
    site.water.push_back({{{-30000, 4000}, {-5000, 6000}, {30000, 3000}}, 1500.0});
    const Options o = withProfile("medievalTown", 2);
    const Layout l = build(site, Ground::plane(), o);
    const json c = checks(l, Ground::plane(), o);
    CHECK(c["crossings"].get<int>() == 0);
    CHECK(!l.bridges.empty());
    for (const Lot& lot : l.lots)
        CHECK(bankDistance(centroid(lot.outline), l.water) > 0.0);
    // Every street touching the river at a node is an arterial: the bridges.
    const StreetGraph& g = l.graph;
    for (size_t n = 0; n < g.nodes.size(); ++n)
    {
        bool wet = false;
        for (int e : g.incident(int(n)))
            wet = wet || g.edges[size_t(e)].cls == StreetClass::Water;
        if (!wet)
            continue;
        for (int e : g.incident(int(n)))
            if (g.isStreet(e))
                CHECK(g.edges[size_t(e)].cls == StreetClass::Arterial);
    }
    const json j = toJson(l);
    CHECK(j["water"].size() == 1);
    CHECK(j["bridges"].size() == l.bridges.size());
}

TEST_CASE("settlement: sites sit by the river, out of its flood, on flat ground")
{
    const Polygon outline = makePolygon({{-200000, -200000}, {200000, -200000}, {200000, 200000}, {-200000, 200000}});
    const std::vector<WaterLine> water = {{{{-200000, 0}, {200000, 0}}, 3000.0}};
    SiteOptions so;
    so.count = 3;
    so.spacing = 100000.0;
    const std::vector<SiteScore> sites = suggestSites(outline, Ground::plane(), water, so);
    REQUIRE(sites.size() == 3);
    for (const SiteScore& s : sites)
    {
        const double bank = bankDistance(s.p, water);
        CHECK(bank >= so.floodMargin);
        CHECK(bank < so.waterReach);
        CHECK_FALSE(s.reasons["floodplain"].get<bool>());
    }
    CHECK(sites[0].score >= sites[1].score);
}

TEST_CASE("settlement: a region places cities, towns and villages apart and joins them by road")
{
    const Polygon outline = makePolygon({{-200000, -200000}, {200000, -200000}, {200000, 200000}, {-200000, 200000}});
    const std::vector<WaterLine> water = {{{{-200000, -30000}, {0, 10000}, {200000, 40000}}, 3000.0}};
    RegionOptions ro;
    ro.cities = 1;
    ro.towns = 2;
    ro.villages = 4;
    const RegionPlan plan = planRegion(outline, Ground::plane(), water, ro);
    int tiers[3] = {};
    for (const RegionSettlement& s : plan.settlements)
        ++tiers[int(s.tier)];
    CHECK(tiers[0] == 1);
    CHECK(tiers[1] == 2);
    CHECK(tiers[2] >= 2);
    CHECK(plan.roads.size() >= plan.settlements.size() - 1);
    for (size_t i = 0; i < plan.settlements.size(); ++i)
        for (size_t j = i + 1; j < plan.settlements.size(); ++j)
            CHECK(glm::length(plan.settlements[i].score.p - plan.settlements[j].score.p) >
                  plan.settlements[i].radius + plan.settlements[j].radius);
    // Every settlement lays out on its site, its main roads continuing the region's.
    for (const RegionSettlement& s : plan.settlements)
    {
        Options o;
        REQUIRE(builtinProfile(s.profile, &o.profile));
        const Layout l = build(s.site, Ground::plane(), o);
        CHECK(!l.lots.empty());
        CHECK(checks(l, Ground::plane(), o)["crossings"].get<int>() == 0);
    }
}

TEST_CASE("settlement: profiles round trip as JSON and refuse unknown fields")
{
    Profile p;
    REQUIRE(builtinProfile("manhattan", &p));
    Profile q;
    applyJson(q, toJson(p));
    CHECK(toJson(q) == toJson(p));
    CHECK_THROWS(applyJson(q, json{{"maxFlors", 3}}));
    CHECK_THROWS(applyJson(q, json{{"pattern", "spiral"}}));
    applyJson(q, json{{"maxFloors", 9}, {"pattern", "radial"}});
    CHECK(q.maxFloors == 9);
    CHECK(q.pattern == Pattern::Radial);
}

TEST_CASE("settlement: commands build, measure and export a settlement")
{
    loadSettlement();
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "hamlet"}, {"size", 30000.0}, {"seed", 4}});
    CHECK(made["lots"].get<int>() > 0);
    CHECK(made["buildings"].get<int>() > 0);
    CHECK(made["checks"]["crossings"].get<int>() == 0);

    const json info = run("settlementInfo", {{"args", {made["mesh"]}}});
    CHECK(info["lots"] == made["lots"]);
    CHECK(info["resolvedProfile"]["name"] == "hamlet");

    run("setAttr", {{"args", {made["modifier"].get<std::string>() + ".overrides", "{\"maxFloors\": 5, \"minFloors\": 5}"}}});
    const json taller = run("settlementInfo", {{"args", {made["mesh"]}}});
    CHECK(taller["floors"]["max"].get<int>() >= 5);

    std::random_device rd;
    const auto path = std::filesystem::temp_directory_path() / ("lila_settlement_" + std::to_string(rd()) + ".json");
    const json out = run("settlementExport", {{"args", {made["mesh"]}}, {"path", path.string()}});
    CHECK(out["envelopes"].get<int>() == taller["buildings"].get<int>());
    CHECK(std::filesystem::exists(path));
    std::error_code ec;
    std::filesystem::remove(path, ec);

    CHECK(run("settlementProfiles").contains("scifiMegacity"));

    const json sites = run("settlementSuggest", {{"size", 200000.0}, {"water", json::array({json::array({json::array({-100000, 0}), json::array({100000, 0})})})}, {"count", 2}});
    CHECK(sites.size() == 2);

    const json region = run("settlementRegion", {{"size", 200000.0}, {"cities", 0}, {"towns", 1}, {"villages", 2},
                                                 {"water", json::array({json::array({json::array({-100000, -20000}), json::array({100000, 30000})})})}});
    CHECK(region["settlements"].size() == 3);
    for (const json& s : region["settlements"])
        CHECK(s["checks"]["crossings"].get<int>() == 0);
    CHECK_THROWS(run("settlementCreate", {{"profile", "atlantis"}}));
}
