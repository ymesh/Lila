#include "TestUtil.h"
#include "Generators.h"

#include "curve/CurveNodes.h"
#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <tbb/global_control.h>

#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
Options gopts(const char* profile, json generators, uint32_t seed = 1)
{
    Options o;
    REQUIRE(builtinProfile(profile, &o.profile));
    o.seed = seed;
    o.generators = std::move(generators);
    return o;
}

Site site(double size)
{
    Site s;
    s.size = size;
    return s;
}

/** The site each built-in profile's targets were measured on: a size the style makes sense at. */
double targetSite(const std::string& profile)
{
    if (profile == "usTown" || profile == "europeanCity")
        return 120000.0;
    if (profile == "manhattan" || profile == "modernCity")
        return 150000.0;
    if (profile == "scifiMegacity")
        return 300000.0;
    return 40000.0;
}

/** A round hill of the given height in the middle of a square, as a grid mesh. */
lila::geo::Mesh hill(double size, double height, int n = 48)
{
    lila::geo::Mesh m;
    std::vector<lila::geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double px = -size / 2 + size * x / n, pz = -size / 2 + size * z / n;
            const double r = std::sqrt(px * px + pz * pz) / (0.3 * size);
            ids.push_back(m.addPoint({px, height * std::exp(-r * r), pz}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const lila::geo::Index a = ids[size_t(z * (n + 1) + x)], b = a + 1, c = a + lila::geo::Index(n + 1), d = c + 1;
            m.addFace({a, c, d, b});
        }
    return m;
}

/** Share of street length (edges whose middle passes keep) running within tol degrees of angle, either way. */
template <class F> double alignedShare(const Layout& l, double angle, double tol, F&& keep)
{
    double on = 0.0, all = 0.0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)))
            continue;
        const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
        if (!keep((a + b) * 0.5))
            continue;
        const double len = glm::length(b - a);
        double d = std::fmod(std::abs(std::atan2(b.y - a.y, b.x - a.x) * 180.0 / kPi - angle), 180.0);
        d = std::min(d, 180.0 - d);
        all += len;
        on += d <= tol ? len : 0.0;
    }
    return all > 0.0 ? on / all : 0.0;
}

bool anywhere(Vec2) { return true; }

double eraLength(const Layout& l, int era)
{
    double len = 0.0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)) && l.graph.edges[e].era == era)
            len += l.graph.length(int(e));
    return len;
}

void loadPackages(bool terrain = false)
{
    if (!ops::find("curve"))
    {
        lila::curve::registerTypes();
        lila::curve::registerCommands();
    }
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (terrain && !ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

/** A mesa: flat top 25 m high, a steep edge at edge cm from the centre. */
lila::geo::Mesh mesa(double size, double edge, int n = 80)
{
    lila::geo::Mesh m;
    std::vector<lila::geo::Index> ids;
    for (int z = 0; z <= n; ++z)
        for (int x = 0; x <= n; ++x)
        {
            const double px = -size / 2 + size * x / n, pz = -size / 2 + size * z / n;
            const double r = std::sqrt(px * px + pz * pz);
            ids.push_back(m.addPoint({px, 2500.0 / (1.0 + std::exp((r - edge) / 1500.0)), pz}));
        }
    for (int z = 0; z < n; ++z)
        for (int x = 0; x < n; ++x)
        {
            const lila::geo::Index a = ids[size_t(z * (n + 1) + x)], b = a + 1, c = a + lila::geo::Index(n + 1), d = c + 1;
            m.addFace({a, c, d, b});
        }
    return m;
}
} // namespace

TEST_CASE("settlement gen: tensor fields steer streets: grid angle, rings, a river's line, painted strokes")
{
    Options o = gopts("modernCity", {{"tensor", {{"fields", json::array({{{"kind", "grid"}, {"angle", 30.0}}})}}}});
    o.profile.pattern = Pattern::Tensor;
    const Layout grid = build(site(100000.0), Ground::plane(), o);
    CHECK(alignedShare(grid, 30.0, 5.0, anywhere) + alignedShare(grid, 120.0, 5.0, anywhere) > 0.85);
    CHECK(alignedShare(grid, 30.0, 5.0, anywhere) > 0.3);
    CHECK(metrics(grid)["orientationOrder"].get<double>() > 0.8);
    CHECK(grid.blocks.size() > 10);

    // Rings round the centre are far less ordered than a grid.
    Options r = gopts("modernCity", {{"tensor", {{"fields", json::array({{{"kind", "radial"}}})}}}});
    r.profile.pattern = Pattern::Tensor;
    const Layout rings = build(site(100000.0), Ground::plane(), r);
    CHECK(metrics(rings)["orientationEntropy"].get<double>() > metrics(grid)["orientationEntropy"].get<double>() + 1.0);

    // The default field on a site with a river: near the river, streets run along its line.
    Site s = site(120000.0);
    s.water.push_back({{{-70000, -20000}, {70000, 20000}}, 2500.0});
    Options d = gopts("modernCity", json::object());
    d.profile.pattern = Pattern::Tensor;
    const Layout river = build(s, Ground::plane(), d);
    const double riverAngle = std::atan2(40000.0, 140000.0) * 180.0 / kPi;
    CHECK(alignedShare(river, riverAngle, 10.0, [&](Vec2 q) { return std::abs(bankDistance(q, river.water)) < 2500.0; }) > 0.5);

    // A painted stroke turns the field inside its radius.
    Options pt = gopts("modernCity", {{"tensor", {{"fields", json::array({{{"kind", "grid"}}})},
                                                  {"paint", json::array({{{"points", {{-50000, 0}, {50000, 0}}}, {"radius", 30000.0}, {"angle", 45.0}, {"weight", 10.0}}})}}}});
    pt.profile.pattern = Pattern::Tensor;
    const Layout painted = build(site(100000.0), Ground::plane(), pt);
    auto band = [](Vec2 q) { return std::abs(q.y) < 10000.0; };
    CHECK(alignedShare(painted, 45.0, 8.0, band) + alignedShare(painted, 135.0, 8.0, band) > 0.6);
    const Vec2 at = tensorDirection(painted, Ground::plane(), *painted.gen->spec->tensor, Vec2(0.0, 0.0));
    CHECK(std::abs(at.x * at.y) == doctest::Approx(0.5).epsilon(0.05));

    // The same seed lays the same streets; growth thins them (streets only: block cutting would fill the rest).
    CHECK(layoutHash(build(site(100000.0), Ground::plane(), o)) == layoutHash(grid));
    Options full = o, half = o;
    full.blocks = half.blocks = false;
    half.growth = 0.4;
    CHECK(metrics(build(site(100000.0), Ground::plane(), half))["streetLengthKm"].get<double>() <
          0.8 * metrics(build(site(100000.0), Ground::plane(), full))["streetLengthKm"].get<double>());
    CHECK_THROWS_WITH_AS(parseGenerators({{"tensor", {{"fields", json::array({{{"kind", "spiral"}}})}}}}), doctest::Contains("kind is grid"), std::runtime_error);
}

TEST_CASE("settlement gen: eras build in order and every street, lot and envelope carries its era")
{
    Options o = gopts("medievalTown", {{"eras", "medievalToModern"}});
    const Layout l = build(site(200000.0), Ground::plane(), o);
    REQUIRE(l.gen);
    const GenLayout& gen = *l.gen;
    REQUIRE(gen.eras.size() == 4);
    CHECK(gen.eras[0].name == "medieval");
    CHECK(gen.eras[2].profile.name == "europeanCity");
    for (int k = 0; k < 4; ++k)
        CHECK(eraLength(l, k) > 1000.0);
    // The walls of the first era came down in the third, a ring boulevard on their line.
    REQUIRE(gen.walls.size() == 1);
    CHECK(gen.walls[0].era == 0);
    CHECK(gen.walls[0].demolished == 2);
    CHECK(gen.gates.empty());
    int ring = 0, standing = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        const StreetEdge& x = l.graph.edges[e];
        if (!x.alive)
            continue;
        const Vec2 m = (l.graph.nodes[size_t(x.a)].p + l.graph.nodes[size_t(x.b)].p) * 0.5;
        const bool onWall = gen::lineDistance(gen.walls[0].pts, m, nullptr, true) < 300.0;
        ring += onWall && x.cls == StreetClass::Arterial && x.era == 2;
        standing += onWall && x.cls == StreetClass::Boundary;
    }
    CHECK(ring > 10);
    CHECK(standing == 0);
    // The baroque axes cleared older streets in their corridors.
    CHECK(gen.cleared.size() >= 2);
    CHECK(gen.demolishedStreets > 0);
    // Lots take the era of their streets, envelopes the style of their era.
    int byEra[4] = {};
    for (const Lot& lot : l.lots)
        ++byEra[std::clamp(lot.era, 0, 3)];
    CHECK(byEra[0] > 20);
    CHECK(byEra[2] > 100);
    CHECK(byEra[3] > 20);
    int mansard = 0, era2 = 0, flat = 0, era3 = 0;
    for (const Envelope& e : l.envelopes)
    {
        CHECK(e.era == l.lots[size_t(e.lot)].era);
        // Towers, industry and landmarks take their own roofs whatever the style.
        if (e.tiers.size() > 1 || e.use == Use::Industrial || !e.landmark.empty())
            continue;
        era2 += e.era == 2;
        mansard += e.era == 2 && e.roof == "mansard";
        era3 += e.era == 3;
        flat += e.era == 3 && e.roof == "flat";
    }
    CHECK(era2 > 0);
    CHECK(mansard == era2);
    CHECK(flat == era3);
    bool highway = false;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        highway = highway || (l.graph.isStreet(int(e)) && l.graph.edges[e].era == 3 && l.graph.edges[e].profile >= 0 &&
                              l.roads[size_t(l.graph.edges[e].profile)].name == "highway");
    CHECK(highway);
    CHECK(layoutHash(build(site(200000.0), Ground::plane(), o)) == layoutHash(l));

    CHECK(eraPresetNames().size() == 3);
    for (const std::string& n : eraPresetNames())
        CHECK_NOTHROW(parseGenerators({{"eras", n}}));
    CHECK_THROWS_WITH_AS(parseGenerators({{"eras", "bronzeAge"}}), doctest::Contains("unknown preset"), std::runtime_error);
    CHECK_THROWS_WITH_AS(parseGenerators({{"eras", json::array({{{"profile", "atlantis"}}})}}), doctest::Contains("unknown profile"), std::runtime_error);
}

TEST_CASE("settlement gen: a district runs its own generator and style inside its polygon")
{
    const json poly = {{10000, -50000}, {50000, -50000}, {50000, 0}, {10000, 0}};
    Options o = gopts("europeanCity", {{"districts", json::array({{{"name", "newTown"}, {"pattern", "grid"}, {"profile", "usTown"}, {"era", 1}, {"polygon", poly}}})}});
    const Layout l = build(site(120000.0), Ground::plane(), o);
    REQUIRE(l.gen);
    CHECK(l.gen->districts == std::vector<std::string>{"newTown"});
    const Polygon d = makePolygon({{10000, -50000}, {50000, -50000}, {50000, 0}, {10000, 0}});
    auto deep = [&](Vec2 q) { return contains(d.pts, q) && boundaryDistance(d.pts, q) > 3000.0; };
    double inside = 0.0, era1 = 0.0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)) || l.graph.edges[e].cls == StreetClass::Arterial)
            continue;
        if (!deep((l.graph.nodes[size_t(l.graph.edges[e].a)].p + l.graph.nodes[size_t(l.graph.edges[e].b)].p) * 0.5))
            continue;
        inside += l.graph.length(int(e));
        era1 += l.graph.edges[e].era == 1 ? l.graph.length(int(e)) : 0.0;
    }
    CHECK(inside > 5000.0);
    CHECK(era1 > 0.9 * inside);
    CHECK(alignedShare(l, 0.0, 3.0, deep) + alignedShare(l, 90.0, 3.0, deep) > 0.8);
    int gable = 0, inDistrict = 0;
    for (const Envelope& e : l.envelopes)
        if (deep(centroid(l.lots[size_t(e.lot)].outline)) && e.tiers.size() <= 1 && e.use != Use::Industrial && e.landmark.empty())
        {
            ++inDistrict;
            gable += e.roof == "gable";
        }
    CHECK(inDistrict > 10);
    CHECK(gable == inDistrict);
}

TEST_CASE("settlement gen: walls follow the break of slope, gates where main roads cross, towers by spacing, a moat")
{
    const Ground g = Ground::fromMesh(mesa(140000.0, 30000.0), 400.0);
    Options o = gopts("medievalTown", {{"walls", {{"radius", 0.3}, {"moat", 1200.0}, {"towerSpacing", 3000.0}}}});
    o.profile.maxGrade = 1.0;
    const Layout l = build(site(120000.0), g, o);
    REQUIRE(l.gen);
    REQUIRE(l.gen->walls.size() == 1);
    const WallLine& w = l.gen->walls[0];
    double mean = 0.0;
    for (const Vec2& q : w.pts)
        mean += glm::length(q - l.center) / double(w.pts.size());
    CHECK(mean == doctest::Approx(30000.0).epsilon(0.12));

    // Every gate has a main road; no street crosses the wall anywhere else.
    CHECK(l.gen->gates.size() >= 3);
    for (const Gate& gt : l.gen->gates)
        CHECK(gt.cls <= StreetClass::Collector);
    int crossings = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
    {
        if (!l.graph.isStreet(int(e)))
            continue;
        const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
        if (contains(w.pts, a) == contains(w.pts, b) || gen::lineDistance(w.pts, a, nullptr, true) < 300.0 ||
            gen::lineDistance(w.pts, b, nullptr, true) < 300.0)
            continue;
        ++crossings;
    }
    CHECK(crossings == 0);
    int throughWall = 0;
    for (size_t n = 0; n < l.graph.nodes.size(); ++n)
    {
        if (gen::lineDistance(w.pts, l.graph.nodes[n].p, nullptr, true) > l.graph.snap() + 1.0)
            continue;
        bool in = false, out = false;
        for (int e : l.graph.incident(int(n)))
            if (l.graph.isStreet(e))
            {
                const StreetEdge& x = l.graph.edges[size_t(e)];
                (contains(w.pts, l.graph.nodes[size_t(x.a == int(n) ? x.b : x.a)].p) ? in : out) = true;
            }
        bool gate = false;
        for (const Gate& gt : l.gen->gates)
            gate = gate || glm::length(gt.p - l.graph.nodes[n].p) < 1.0;
        throughWall += in && out && !gate;
    }
    CHECK(throughWall == 0);
    double perimeter = 0.0;
    for (size_t k = 0; k < w.pts.size(); ++k)
        perimeter += glm::length(w.pts[(k + 1) % w.pts.size()] - w.pts[k]);
    CHECK(int(l.gen->towers.size()) >= int(perimeter / 3000.0 * 0.5));
    int gateTowers = 0;
    for (const Tower& t : l.gen->towers)
        gateTowers += t.gate;
    CHECK(gateTowers == 2 * int(l.gen->gates.size()));
    // The moat is water flagged as a moat, outside the wall.
    int moat = 0;
    for (const StreetEdge& e : l.graph.edges)
        moat += e.alive && e.moat && e.cls == StreetClass::Water;
    CHECK(moat > 10);
    REQUIRE_FALSE(l.water.empty());
    CHECK(l.water.back().kind == "moat");
    CHECK(gen::lineDistance(l.water.back().pts, w.pts.front()) > 1000.0);
    for (const Lot& lot : l.lots)
        CHECK(bankDistance(centroid(lot.outline), l.water) > 0.0);

    // A star fort: bastion points thrust out, a tower on each.
    const Layout star = build(site(100000.0), Ground::plane(), gopts("medievalTown", {{"walls", {{"starFort", true}, {"bastions", 6}}}}));
    REQUIRE(star.gen->walls.size() == 1);
    CHECK(star.gen->walls[0].pts.size() == 18);
    int bastions = 0;
    for (const Tower& t : star.gen->towers)
        bastions += t.bastion;
    CHECK(bastions == 6);
}

TEST_CASE("settlement gen: imported streets go in as they are; their numbers steer a new layout")
{
    json streets = json::array();
    for (int i = -3; i <= 3; ++i)
    {
        const double c = i * 9000.0;
        streets.push_back({{"points", {{-30000, c}, {30000, c}}}, {"class", i == 0 ? "primary" : "residential"}});
        streets.push_back({{"points", {{c, -30000}, {c, 30000}}}, {"class", i == 0 ? "secondary" : "residential"}});
    }
    streets.push_back({{"points", {{-30000, -30000}, {30000, 30000}}}, {"class", "footway"}});
    streets.push_back({{"points", {{90000, 0}, {95000, 0}}}, {"class", "residential"}});
    Options o = gopts("medievalTown", {{"osm", streets}});
    o.profile.pattern = Pattern::Osm;
    const Layout l = build(site(80000.0), Ground::plane(), o);
    REQUIRE(l.gen);
    CHECK(l.gen->imported == 15);
    REQUIRE(l.gen->warnings.size() == 1);
    CHECK(l.gen->warnings[0].find("1 of 16") != std::string::npos);
    double arterial = 0.0, collector = 0.0, path = 0.0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)))
        {
            const double len = l.graph.length(int(e));
            arterial += l.graph.edges[e].cls == StreetClass::Arterial ? len : 0.0;
            collector += l.graph.edges[e].cls == StreetClass::Collector ? len : 0.0;
            path += l.graph.edges[e].cls == StreetClass::Path ? len : 0.0;
        }
    CHECK(arterial >= 60000.0 - 1.0);
    CHECK(collector >= 60000.0 - 1.0);
    CHECK(path == doctest::Approx(std::sqrt(2.0) * 60000.0).epsilon(0.01));
    CHECK(classFromOsm("motorway") == StreetClass::Arterial);
    CHECK(classFromOsm("tertiary") == StreetClass::Collector);
    CHECK(classFromOsm("service") == StreetClass::Alley);
    CHECK(classFromOsm("steps") == StreetClass::Path);
    CHECK(classFromOsm("living_street") == StreetClass::Local);

    // Numbers: a grid example makes an ordered network along its axes, an organic one does not.
    const StreetStats gridStats = statsFromStreets(parseGenerators({{"osm", streets}}).osm);
    CHECK(gridStats.valid);
    CHECK(gridStats.blockMedian == doctest::Approx(8100.0).epsilon(0.2));
    Options ex = gopts("medievalTown", {{"example", toJson(streetStats(build(site(120000.0), Ground::plane(), gopts("usTown", json::object())).graph))}});
    ex.profile.pattern = Pattern::Example;
    const Layout fromGrid = build(site(120000.0), Ground::plane(), ex);
    Options org = gopts("medievalTown", {{"example", toJson(streetStats(build(site(60000.0), Ground::plane(), gopts("medievalTown", json::object())).graph))}});
    org.profile.pattern = Pattern::Example;
    const Layout fromOrganic = build(site(120000.0), Ground::plane(), org);
    CHECK(metrics(fromGrid)["orientationOrder"].get<double>() > 0.7);
    CHECK(metrics(fromOrganic)["orientationOrder"].get<double>() < 0.4);
    CHECK(alignedShare(fromGrid, 0.0, 5.0, anywhere) + alignedShare(fromGrid, 90.0, 5.0, anywhere) > 0.8);
    const StreetStats again = statsFromJson(toJson(gridStats));
    CHECK(again.blockMedian == doctest::Approx(gridStats.blockMedian));
    CHECK(again.bins[0] == doctest::Approx(gridStats.bins[0]));
    Options none = gopts("medievalTown", json::object());
    none.profile.pattern = Pattern::Osm;
    CHECK_THROWS_WITH_AS(build(site(40000.0), Ground::plane(), none), doctest::Contains("needs imported streets"), std::runtime_error);
}

TEST_CASE("settlement gen: contour streets on a hill hold their level, with fallbacks that say so")
{
    const Ground g = Ground::fromMesh(hill(120000.0, 6000.0), 400.0);
    // Street length on the slopes that runs nearly level.
    auto levelShare = [&](const Layout& l) {
        double level = 0.0, all = 0.0;
        for (size_t e = 0; e < l.graph.edges.size(); ++e)
        {
            if (!l.graph.isStreet(int(e)))
                continue;
            const Vec2 a = l.graph.nodes[size_t(l.graph.edges[e].a)].p, b = l.graph.nodes[size_t(l.graph.edges[e].b)].p;
            if (g.grade((a + b) * 0.5) < 0.05)
                continue;
            all += glm::length(b - a);
            level += std::abs(g.height(a) - g.height(b)) / std::max(glm::length(b - a), 1.0) < 0.03 ? glm::length(b - a) : 0.0;
        }
        return all > 0.0 ? level / all : 0.0;
    };
    Options o = gopts("medievalTown", json::object());
    o.profile.pattern = Pattern::Contour;
    const Layout traced = build(site(100000.0), g, o);
    REQUIRE(traced.gen);
    REQUIRE(traced.gen->warnings.size() == 1);
    CHECK(traced.gen->warnings[0].find("ground grid") != std::string::npos);
    const Layout grown = build(site(100000.0), g, gopts("medievalTown", json::object()));
    CAPTURE(levelShare(traced));
    CAPTURE(levelShare(grown));
    CHECK(levelShare(traced) > levelShare(grown) + 0.15);

    // Given contour streets are laid by kind: contours as collectors, stairs as paths.
    const json given = json::array({{{"kind", "contour"}, {"points", {{-20000, 0, -20000}, {20000, 0, -20000}}}},
                                    {{"kind", "cross"}, {"points", {{0, 0, -20000}, {0, 0, -10000}}}},
                                    {{"kind", "stairs"}, {"points", {{5000, 0, -20000}, {5000, 0, -15000}}}}});
    Options c = gopts("hamlet", {{"contour", given}});
    c.profile.pattern = Pattern::Contour;
    const Layout laid = build(site(60000.0), Ground::plane(), c);
    int collectors = 0, paths = 0;
    for (size_t e = 0; e < laid.graph.edges.size(); ++e)
        if (laid.graph.isStreet(int(e)))
        {
            const Vec2 a = laid.graph.nodes[size_t(laid.graph.edges[e].a)].p, b = laid.graph.nodes[size_t(laid.graph.edges[e].b)].p;
            collectors += std::abs(a.y + 20000.0) < 1.0 && std::abs(b.y + 20000.0) < 1.0 && laid.graph.edges[e].cls == StreetClass::Collector;
            paths += std::abs(a.x - 5000.0) < 1.0 && std::abs(b.x - 5000.0) < 1.0 && laid.graph.edges[e].cls == StreetClass::Path;
        }
    CHECK(collectors > 0);
    CHECK(paths > 0);
    CHECK(laid.gen->warnings.empty());
    Options flat = gopts("hamlet", json::object());
    flat.profile.pattern = Pattern::Contour;
    const Layout plain = build(site(40000.0), Ground::plane(), flat);
    REQUIRE(plain.gen->warnings.size() == 1);
    CHECK(plain.gen->warnings[0].find("flat ground") != std::string::npos);
}

TEST_CASE("settlement gen: levels above the ground keep their own layer, joined by ramps; air lanes reach everywhere")
{
    const json deck = {{"name", "plaza"}, {"polygon", {{-30000, -30000}, {30000, -30000}, {30000, 30000}, {-30000, 30000}}}, {"height", 4000.0}, {"ramps", 3}};
    Options o = gopts("scifiMegacity", {{"levels", {{"decks", json::array({deck})},
                                                     {"elevated", json::array({{{"points", {{-90000, -60000}, {90000, 60000}}}, {"height", 1500.0}}})},
                                                     {"airHeights", {20000.0, 40000.0}}, {"airSpacing", 30000.0}, {"megablock", 20000.0}, {"skybridgeSpan", 8000.0}}}});
    const Layout l = build(site(200000.0), Ground::plane(), o);
    REQUIRE(l.gen);
    const GenLayout& gen = *l.gen;
    for (const StreetEdge& e : l.graph.edges)
        CHECK(e.layer == 0);
    REQUIRE(gen.levels.size() >= 2);
    CHECK(gen.levels[0].kind == "deck");
    CHECK(gen.levels[0].height == doctest::Approx(4000.0));
    int deckStreets = 0;
    for (size_t e = 0; e < gen.levels[0].graph.edges.size(); ++e)
        if (gen.levels[0].graph.isStreet(int(e)))
        {
            ++deckStreets;
            CHECK(gen.levels[0].graph.edges[e].layer == 1);
        }
    CHECK(deckStreets > 10);
    CHECK(gen.levels[1].kind == "elevated");
    for (const StreetEdge& e : gen.levels[1].graph.edges)
        CHECK(e.layer == 2);
    // The elevated road passes over the ground streets without meeting any of them.
    CHECK(gen.levels[1].graph.nodes.size() == 2);
    // Ramps: three off the deck, two off the elevated road's ends, each down to a ground street.
    REQUIRE(gen.ramps.size() == 5);
    for (const Ramp& r : gen.ramps)
    {
        CHECK(r.from == 0);
        CHECK(r.pts.front().y > r.pts.back().y + 1000.0);
        CHECK(l.graph.nearestNode({r.pts.back().x, r.pts.back().z}, l.graph.snap() + 1.0) >= 0);
    }
    CHECK(levelNetwork(l, 0).junctions.size() > 4);
    CHECK(gen.air.heights.size() == 2);
    std::vector<std::vector<int>> adj(gen.air.nodes.size());
    for (const auto& [a, b] : gen.air.links)
        adj[size_t(a)].push_back(b);
    std::vector<char> seen(adj.size(), 0);
    std::vector<int> stack = {0};
    seen[0] = 1;
    while (!stack.empty())
    {
        const int x = stack.back();
        stack.pop_back();
        for (int y : adj[size_t(x)])
            if (!seen[size_t(y)])
            {
                seen[size_t(y)] = 1;
                stack.push_back(y);
            }
    }
    CHECK(std::count(seen.begin(), seen.end(), char(1)) == int(seen.size()));
    CHECK(gen.air.nodes.size() > 20);
    CHECK_FALSE(gen.megablocks.empty());
    for (const Megablock& mb : gen.megablocks)
        CHECK(mb.top > mb.base + 10000.0);
    bool sky = false;
    for (const Level& v : gen.levels)
        sky = sky || (v.kind == "skybridge" && !v.lines3.empty());
    CHECK(sky);
    int layers[8] = {};
    for (const LevelLine& line : levelLines(l, Ground::plane()))
        ++layers[std::clamp(line.layer, 0, 7)];
    CHECK(layers[1] > 10);
    CHECK(layers[2] >= 1);
}

TEST_CASE("settlement gen: every built-in profile lands inside its metric targets over 20 seeds")
{
    for (const std::string& name : profileNames())
        for (uint32_t seed = 1; seed <= 20; ++seed)
        {
            const json t = targetReport(build(site(targetSite(name)), Ground::plane(), gopts(name.c_str(), json::object(), seed)));
            CAPTURE(name);
            CAPTURE(seed);
            CAPTURE(t.dump());
            CHECK(t["ok"].get<bool>());
        }
}

TEST_CASE("settlement gen: the same layout at 1, 4 and all threads")
{
    const lila::geo::Mesh mesh = hill(120000.0, 4000.0);
    auto layout = [&] {
        const Ground g = Ground::fromMesh(mesh, 400.0);
        const json district = {{"pattern", "tensor"}, {"polygon", {{20000, 20000}, {50000, 20000}, {50000, 50000}, {20000, 50000}}}};
        return layoutHash(build(site(100000.0), g, gopts("medievalTown", {{"eras", "walledTown"}, {"districts", json::array({district})}})));
    };
    uint64_t hashes[3] = {};
    const size_t counts[3] = {1, 4, std::max<size_t>(1, std::thread::hardware_concurrency())};
    for (int k = 0; k < 3; ++k)
    {
        tbb::global_control limit(tbb::global_control::max_allowed_parallelism, counts[k]);
        hashes[k] = layout();
    }
    CHECK(hashes[0] == hashes[1]);
    CHECK(hashes[0] == hashes[2]);
}

TEST_CASE("settlement gen: a 5 km city grows in seconds and a one-block edit re-cuts only its blocks, several times faster")
{
    using clock = std::chrono::steady_clock;
    const Ground g = Ground::plane();
    const Options o = gopts("europeanCity", json::object());
    const auto t0 = clock::now();
    const Site city = site(500000.0);
    Layout l = build(city, g, o);
    const double grow = std::chrono::duration<double>(clock::now() - t0).count();
    MESSAGE("5 km city: " << grow << " s, " << l.lots.size() << " lots");
    CHECK(l.lots.size() > 20000);
    CHECK(grow < 20.0);

    // A lane through one block: only the blocks round it are remade.
    size_t target = l.blocks.size();
    for (size_t b = 0; b < l.blocks.size(); ++b)
        if (glm::length(centroid(l.blocks[b].outline) - Vec2(150000.0, 150000.0)) < 50000.0 &&
            (target == l.blocks.size() || l.blocks[b].area > l.blocks[target].area))
            target = b;
    REQUIRE(target < l.blocks.size());
    const Polygon outline = l.blocks[target].outline;
    const Vec2 c = centroid(outline);
    Vec2 lo(1e300), hi(-1e300);
    for (const Vec2& q : outline.pts)
        lo = glm::min(lo, q), hi = glm::max(hi, q);
    const size_t before = l.blocks.size();
    const auto t1 = clock::now();
    l.graph.addStreet(Vec2(lo.x - 2000.0, c.y), Vec2(hi.x + 2000.0, c.y), StreetClass::Alley);
    const int remade = updateBlocks(l, city, g, o, lo - Vec2(2000.0), hi + Vec2(2000.0));
    const double edit = std::chrono::duration<double>(clock::now() - t1).count();
    MESSAGE("one-block edit: " << edit * 1000.0 << " ms, " << remade << " blocks remade");
    CHECK(remade >= 2);
    CHECK(remade < 20);
    CHECK(l.blocks.size() >= before + 1);
    CHECK(edit < 0.04 * grow); // relative, so a loaded machine slows both alike; 100 ms is the target on an idle one
    for (size_t k = 0; k < l.lots.size(); ++k)
    {
        CHECK(l.lots[k].id == int(k));
        CHECK(l.lots[k].block < int(l.blocks.size()));
    }
    for (const Envelope& e : l.envelopes)
        CHECK(e.lot < int(l.lots.size()));
}

TEST_CASE("settlement gen: commands create, read and change generators; walls go out as curves")
{
    loadPackages();
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 160000.0}, {"eras", "walledTown"}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json eras = run("settlementEras", {{"args", {mesh}}});
    REQUIRE(eras["eras"].size() == 2);
    CHECK(eras["eras"][0]["name"] == "medieval");
    CHECK(eras["eras"][1]["streetKm"].get<double>() > 0.5);
    CHECK(eras["eras"][0]["walls"] == 1);

    const json walls = run("settlementWalls", {{"args", {mesh}}, {"curves", true}});
    REQUIRE(walls["walls"].size() == 1);
    CHECK(walls["walls"][0]["gates"].size() >= 2);
    CHECK(walls["walls"][0]["points"].size() > 16);
    REQUIRE(walls["made"].size() == 1);
    CHECK(run("objExists", {{"args", {walls["made"][0]}}}) == true);
    if (!ops::find("masonryWall"))
        CHECK_THROWS_WITH(run("settlementWalls", {{"args", {mesh}}, {"masonry", true}}), doctest::Contains("masonryWall is not available"));

    // A tensor pattern instead of the eras, then undo.
    const json changed = run("settlementGenerators", {{"args", {mesh}}, {"clear", true}, {"tensor", {{"fields", json::array({{{"kind", "grid"}, {"angle", 20.0}}})}}}});
    CHECK(changed["generators"].contains("tensor"));
    CHECK_FALSE(changed["generators"].contains("eras"));
    CHECK(run("getAttr", {{"args", {made["modifier"].get<std::string>() + ".pattern"}}}) == "tensor");
    run("undo");
    CHECK(run("settlementGenerators", {{"args", {mesh}}})["generators"].contains("eras"));
    CHECK_THROWS_WITH(run("settlementGenerators", {{"args", {mesh}}, {"walls", {{"bastions", 2}}}}), doctest::Contains("bastions takes 3"));

    // Imported streets from a curve carrying OSM classes.
    const json curve = run("curve", {{"splines", json::array({{{"type", "poly"}, {"closed", false}, {"points", {{-30000, 0, 0}, {30000, 0, 0}}}},
                                                             {{"type", "poly"}, {"closed", false}, {"points", {{0, 0, -30000}, {0, 0, 30000}}}}})},
                                     {"name", "OsmRoads"}});
    const std::string curvePath = curve.is_array() ? curve[0].get<std::string>() : curve.get<std::string>();
    run("addAttr", {{"args", {curvePath}}, {"longName", "osmFeatures"}, {"dataType", "string"}});
    run("setAttr", {{"args", {curvePath + ".osmFeatures", json::array({{{"class", "primary"}}, {{"class", "footway"}}}).dump()}}});
    const json osm = run("settlementCreate", {{"profile", "hamlet"}, {"size", 80000.0}, {"osm", {curvePath}}, {"name", "Imported"}});
    const json info = run("settlementGenerators", {{"args", {osm["mesh"]}}});
    CHECK(info["imported"] == 2);
    CHECK(run("getAttr", {{"args", {osm["modifier"].get<std::string>() + ".pattern"}}}) == "osm");
    CHECK(run("settlementStreets", {{"args", {osm["mesh"]}}, {"streetClass", "arterial"}}).size() >= 1);
    CHECK(run("settlementStreets", {{"args", {osm["mesh"]}}, {"streetClass", "path"}}).size() >= 1);

    const json ex = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 120000.0}, {"example", mesh}, {"name", "Like"}});
    CHECK(run("getAttr", {{"args", {ex["modifier"].get<std::string>() + ".pattern"}}}) == "example");

    const json lv = run("settlementCreate", {{"profile", "scifiMegacity"}, {"size", 200000.0}, {"name", "Arcology"},
                                             {"levels", {{"decks", json::array({{{"polygon", {{-30000, -30000}, {30000, -30000}, {30000, 30000}, {-30000, 30000}}}, {"height", 4000.0}}})},
                                                         {"airHeights", {20000.0}}}}});
    const json levels = run("settlementLevels", {{"args", {lv["mesh"]}}, {"curves", true}});
    REQUIRE(levels["levels"].size() == 1);
    CHECK(levels["levels"][0]["streets"].get<int>() > 10);
    CHECK(levels["air"]["connected"] == true);
    CHECK(levels["curves"].contains("deck"));
    CHECK(levels["curves"].contains("air"));
}

TEST_CASE("settlement gen: walls are built by Masonry with the gates as openings")
{
    loadPackages();
    if (!ops::find("masonryWall"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_MASONRY_PACKAGE) == "Masonry");
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "hamlet"}, {"size", 30000.0}, {"walls", {{"radius", 0.4}}}, {"name", "Walled"}});
    const json walls = run("settlementWalls", {{"args", {made["mesh"]}}, {"masonry", true}, {"height", 300.0}, {"thickness", 80.0},
                                               {"masonryFlags", {{"stoneLength", 80.0}, {"stoneHeight", 40.0}, {"stoneDepth", 40.0}}}});
    REQUIRE(walls["walls"].size() == 1);
    const json& w = walls["walls"][0];
    CAPTURE(w["gates"].dump());
    CHECK(w["gates"].size() >= 1);
    REQUIRE(w.contains("masonry"));
    REQUIRE(w["masonry"].is_string());
    const json info = run("masonryInfo", {{"args", {w["masonry"]}}});
    CAPTURE(info.dump().substr(0, 400));
    CHECK(info.value("stones", 0) > 100);
    // The gates are arched openings: voussoirs round them.
    CHECK(info["roles"]["voussoir"].get<int>() > 0);
}

TEST_CASE("settlement gen: OSM files come in through terrainImportOsm, contour streets through terrainContourStreets")
{
    loadPackages(true);
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 129}, {"amplitude", 4000.0}, {"seed", 5}});
    const std::string ground = terrain["mesh"].get<std::string>();
    // A small OSM file round (47, 8): five streets 100 m apart, the middle one primary.
    const auto dir = std::filesystem::temp_directory_path() / "lila_settlement_gen_osm";
    std::filesystem::create_directories(dir);
    const auto file = dir / "town.osm";
    {
        std::ofstream f(file);
        f << "<?xml version='1.0' encoding='UTF-8'?>\n<osm version='0.6'>\n";
        const double dlat = 0.0009, dlon = 0.00132;
        int id = 1;
        for (int i = -2; i <= 2; ++i)
        {
            f << "<node id='" << id++ << "' lat='" << 47.0 + i * dlat << "' lon='" << 8.0 - 2.5 * dlon << "'/>\n";
            f << "<node id='" << id++ << "' lat='" << 47.0 + i * dlat << "' lon='" << 8.0 + 2.5 * dlon << "'/>\n";
        }
        for (int i = 0; i < 5; ++i)
            f << "<way id='" << 100 + i << "'><nd ref='" << 2 * i + 1 << "'/><nd ref='" << 2 * i + 2 << "'/><tag k='highway' v='"
              << (i == 2 ? "primary" : "residential") << "'/></way>\n";
        f << "</osm>\n";
    }
    const json osm = run("settlementCreate", {{"profile", "hamlet"}, {"size", 60000.0}, {"ground", ground}, {"osmFile", file.string()},
                                              {"osmOrigin", {47.0, 8.0}}, {"name", "FromOsm"}});
    const json info = run("settlementGenerators", {{"args", {osm["mesh"]}}});
    CAPTURE(info.dump());
    CHECK(info["imported"].get<int>() == 5);
    CHECK(run("settlementStreets", {{"args", {osm["mesh"]}}, {"streetClass", "arterial"}}).size() >= 1);

    const json hill = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 60000.0}, {"ground", ground}, {"contourStreets", true}, {"name", "Hillside"}});
    const json hinfo = run("settlementGenerators", {{"args", {hill["mesh"]}}});
    CAPTURE(hinfo["warnings"].dump());
    CHECK(hinfo["generators"]["contour"].size() > 3);
    CHECK(run("getAttr", {{"args", {hill["modifier"].get<std::string>() + ".pattern"}}}) == "contour");
    CHECK(hinfo["warnings"].empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

TEST_CASE("settlement gen: calibrate" * doctest::skip())
{
    for (const std::string& name : profileNames())
    {
        double lo[4] = {1e9, 1e9, 1e9, 1e9}, hi[4] = {-1e9, -1e9, -1e9, -1e9};
        for (uint32_t seed = 1; seed <= 20; ++seed)
        {
            Options o = gopts(name.c_str(), json::object(), seed);
            const json t = targetReport(build(site(targetSite(name)), Ground::plane(), o));
            const char* keys[] = {"orientationEntropy", "deadEndRatio", "fourWayShare", "blockAreaMedianM2"};
            for (int k = 0; k < 4; ++k)
            {
                lo[k] = std::min(lo[k], t[keys[k]]["value"].get<double>());
                hi[k] = std::max(hi[k], t[keys[k]]["value"].get<double>());
            }
        }
        MESSAGE(name << " entropy " << lo[0] << ".." << hi[0] << " dead " << lo[1] << ".." << hi[1] << " four " << lo[2] << ".." << hi[2]
                     << " block " << lo[3] << ".." << hi[3]);
    }
    for (const char* name : {"modernCity", "europeanCity"})
    {
        const auto t0 = std::chrono::steady_clock::now();
        Options o = gopts(name, json::object());
        const Layout l = build(site(500000.0), Ground::plane(), o);
        MESSAGE(std::string(name) << " 5km " << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s, edges "
                     << l.graph.edges.size() << " lots " << l.lots.size());
    }
}


TEST_CASE("settlement gen: a main road to an attractor stays inside the outline when an exclusion blocks the way")
{
    // A long floor almost cut across by an exclusion from below: the way round outside is shorter than the gap inside.
    Site s;
    s.outline = {{0, 0}, {40000, 0}, {40000, 8000}, {0, 8000}};
    s.attractors = {{32000, 4000}, {4000, 7600}};
    s.hasCenter = true;
    s.exclusions.push_back({1, "", "", {{15000, -3000}, {18000, -3000}, {18000, 5000}, {15000, 5000}}, 200.0});
    Options o = gopts("europeanCity", json::object());
    o.profile.pattern = Pattern::None;
    o.blocks = false;
    const Layout l = build(s, Ground::plane(), o);
    int arterials = 0;
    for (const StreetEdge& e : l.graph.edges)
    {
        if (!e.alive || e.cls != StreetClass::Arterial)
            continue;
        ++arterials;
        const Vec2 mid = 0.5 * (l.graph.nodes[size_t(e.a)].p + l.graph.nodes[size_t(e.b)].p);
        CAPTURE(mid.x);
        CAPTURE(mid.y);
        CHECK(contains(s.outline, mid));
    }
    CHECK(arterials > 0);
}
