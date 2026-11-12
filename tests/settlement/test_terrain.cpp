// Settlement on a Terrain: siting terms, terrain routes, grading as road features, dirty re-planning, checks, explain.

#include "TestUtil.h"
#include "SettlementTerrain.h"

#include "extensions/ExtensionManager.h"
#include "geo/Mesh.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"

#include <doctest/doctest.h>

#include <cmath>
#include <set>

using namespace lila;
using namespace settlement;
using namespace settlementtest;

namespace
{
void terrainLoadPackages()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!ops::find("terrainStampPad"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

/** A grid mesh over [-half, half] in X and Z whose height is f(x, z). */
geo::Mesh heightMesh(double half, int n, const std::function<double(double, double)>& f)
{
    geo::Mesh m;
    std::vector<geo::Index> ids;
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
        {
            const double x = -half + 2.0 * half * i / n, z = -half + 2.0 * half * j / n;
            ids.push_back(m.addPoint({x, f(x, z), z}));
        }
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
        {
            const auto at = [&](int a, int b) { return ids[size_t(b * (n + 1) + a)]; };
            m.addFace({at(i, j), at(i, j + 1), at(i + 1, j + 1), at(i + 1, j)});
        }
    return m;
}

Options terrainProfileOptions(const char* name, uint32_t seed = 1)
{
    Options o;
    REQUIRE(builtinProfile(name, &o.profile));
    o.seed = seed;
    return o;
}

/** A layout with the class profiles in place and nothing else, for hand-built check cases. */
Layout emptyLayout()
{
    Layout l{StreetGraph(150.0, 2000.0)};
    for (StreetClass c : {StreetClass::Arterial, StreetClass::Collector, StreetClass::Local, StreetClass::Alley, StreetClass::Path})
    {
        RoadProfile r;
        builtinRoadProfile(defaultRoadProfile(c), &r);
        l.roads.push_back(r);
    }
    l.outline = makePolygon({{-50000, -50000}, {50000, -50000}, {50000, 50000}, {-50000, 50000}});
    l.radius = 70000.0;
    return l;
}

Lot squareLot(int id, Vec2 lo, double side, int frontages)
{
    Lot lot;
    lot.id = id;
    lot.outline = makePolygon({lo, lo + Vec2(side, 0.0), lo + Vec2(side, side), lo + Vec2(0.0, side)});
    for (int& t : lot.outline.tag)
        t = -1;
    if (frontages > 0)
        lot.outline.tag[0] = int(StreetClass::Local);
    lot.frontages = frontages;
    lot.area = side * side / kM2;
    return lot;
}

std::string terrainMesh(double size = 200000.0, int resolution = 129, double amplitude = 3000.0, int seed = 3)
{
    const json t = run("terrainCreate", {{"size", json::array({size, size})}, {"resolution", resolution}, {"amplitude", amplitude}, {"seed", seed}});
    return t["mesh"].get<std::string>();
}

json editsOf(const json& made)
{
    const std::string text = run("getAttr", {{"args", {made["modifier"].get<std::string>() + ".edits"}}}).get<std::string>();
    return text.empty() ? json::object() : json::parse(text);
}
} // namespace

TEST_CASE("settlement terrain: siting presets, weights as data and the score they give")
{
    SitingRules town, fort;
    REQUIRE(sitingPreset("town", &town));
    REQUIRE(sitingPreset("fort", &fort));
    CHECK(fort.defense > town.defense + 0.5);
    CHECK(sitingKinds().size() == 7);

    SitingRules r;
    applyJson(r, {{"kind", "mining"}, {"water", 0.9}});
    CHECK(r.kind == "mining");
    CHECK(r.water == doctest::Approx(0.9));
    CHECK(r.resource == doctest::Approx(0.6));
    CHECK_THROWS(applyJson(r, {{"kind", "atlantis"}}));
    CHECK_THROWS(applyJson(r, {{"gold", 1.0}}));

    Profile p;
    applyJson(p, {{"siting", {{"kind", "port"}}}});
    CHECK(p.siting.kind == "port");
    CHECK(toJson(p)["siting"]["kind"] == "port");

    // Only known terms count: flat 1 and water 0 under the town weights, then a flooded site keeps a fifth.
    SiteFacts f;
    f.set(SiteTerm::Flat, 1.0, "flat");
    f.set(SiteTerm::Water, 0.0, "dry");
    const SiteScore dry = scoreSite(f, town);
    CHECK(dry.score == doctest::Approx(0.45 / 0.85));
    CHECK(dry.reasons["terms"].size() == size_t(SiteTerm::Count));
    CHECK(dry.reasons["terms"]["defense"]["value"].is_null());
    f.set(SiteTerm::Flood, 1.0, "floodplain");
    CHECK(scoreSite(f, town).score == doctest::Approx(0.2 * 0.45 / 0.85));

    // On a hill mesh a fort goes to the top and a farming village to the flat land around it.
    const geo::Mesh hill = heightMesh(100000.0, 80, [](double x, double z) { return 6000.0 * std::exp(-(x * x + z * z) / (2.0 * 15000.0 * 15000.0)); });
    const Ground ground = Ground::fromMesh(hill, 500.0);
    const Polygon outline = makePolygon({{-90000, -90000}, {90000, -90000}, {90000, 90000}, {-90000, 90000}});
    SiteOptions so;
    so.count = 1;
    so.spacing = 50000.0;
    so.rules = fort;
    const std::vector<SiteScore> forts = suggestSites(outline, ground, {}, so);
    REQUIRE(forts.size() == 1);
    CHECK(glm::length(forts[0].p) < 15000.0);
    CHECK(forts[0].reasons["terms"]["defense"]["value"].get<double>() > 0.5);
    REQUIRE(sitingPreset("farming", &so.rules));
    const std::vector<SiteScore> farms = suggestSites(outline, ground, {}, so);
    REQUIRE(farms.size() == 1);
    CHECK(glm::length(farms[0].p) > 30000.0);
}

TEST_CASE("settlement terrain: sites on a Terrain come from its analysis with reasons per term")
{
    terrainLoadPackages();
    run("newScene");
    const std::string t = terrainMesh();
    run("terrainAddRiver", {{"args", {t}}, {"id", "main"}, {"path", json::array({json::array({-95000, -20000}), json::array({0, 0}), json::array({95000, 15000})})}});
    const json market = run("settlementSuggest", {{"ground", t}, {"kind", "market"}, {"count", 3}, {"spacing", 40000.0}});
    REQUIRE(market.size() == 3);
    for (const json& s : market)
    {
        CHECK(s["ground"] == "terrain");
        CHECK(s["kind"] == "market");
        const json& terms = s["reasons"]["terms"];
        for (const char* k : {"flat", "water", "trade", "defense", "resource", "climate", "flood"})
        {
            REQUIRE(terms.contains(k));
            CHECK_FALSE(terms[k]["value"].is_null());
            CHECK_FALSE(terms[k]["why"].get<std::string>().empty());
        }
    }
    CHECK(market[0]["score"].get<double>() >= market[1]["score"].get<double>());
    CHECK(market[0]["reasons"]["terms"]["flat"]["why"].get<std::string>().find("terrainRegionStats") != std::string::npos);

    const json forts = run("settlementSuggest", {{"ground", t}, {"kind", "fort"}, {"count", 1}});
    const json farms = run("settlementSuggest", {{"ground", t}, {"kind", "farming"}, {"count", 1}});
    CHECK(forts[0]["reasons"]["terms"]["defense"]["value"].get<double>() >= farms[0]["reasons"]["terms"]["defense"]["value"].get<double>());
    const json heavy = run("settlementSuggest", {{"ground", t}, {"kind", "fort"}, {"rules", {{"defense", 5.0}}}, {"count", 1}});
    CHECK(heavy[0]["reasons"]["terms"]["defense"]["weight"].get<double>() == doctest::Approx(5.0));
    CHECK_THROWS(run("settlementSuggest", {{"ground", t}, {"kind", "atlantis"}}));

    // Flat ground keeps its own scoring and says so.
    const json flat = run("settlementSuggest", {{"size", 200000.0}, {"count", 2}});
    CHECK(flat[0]["ground"] == "plane");
}

TEST_CASE("settlement terrain: main roads route over the Terrain and grading sends roads, rings, pads and water")
{
    terrainLoadPackages();
    run("newScene");
    const std::string t = terrainMesh(80000.0, 129, 1500.0, 3);
    const json made = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 40000.0}, {"seed", 2}, {"ground", t}});
    const std::string mesh = made["mesh"].get<std::string>();
    REQUIRE(made.contains("terrain"));
    const json routes = made["terrain"]["routes"];
    CAPTURE(routes.dump());
    CHECK(routes["routes"].get<int>() > 0);
    CHECK(routes["terrain"].get<int>() + routes["own"].get<int>() == routes["routes"].get<int>());
    CHECK(editsOf(made)["terrainRoutes"]["routes"].size() == size_t(routes["routes"].get<int>()));

    // An arterial on a terrain route says so.
    bool explained = false;
    for (const json& s : run("settlementStreets", {{"args", {mesh}}, {"streetClass", "arterial"}}))
    {
        const json e = run("settlementExplain", {{"args", {mesh}}, {"id", "street:" + std::to_string(s["id"].get<uint64_t>())}});
        CHECK(e["class"] == "arterial");
        CHECK(e["gradeLimit"].get<double>() > 0.0);
        for (const json& reason : e["reasons"])
            explained = explained || reason.get<std::string>().find("terrainRoute") != std::string::npos ||
                        reason.get<std::string>().find("own router") != std::string::npos;
    }
    CHECK(explained);

    // A canal through the town goes in as a carved channel.
    json edits = editsOf(made);
    edits["water"] = json::array({{{"points", json::array({json::array({-30000, 12000}), json::array({30000, 12000})})}, {"width", 1200.0}, {"kind", "canal"}}});
    run("setAttr", {{"args", {made["modifier"].get<std::string>() + ".edits", edits.dump()}}});

    const json graded = run("settlementGrade", {{"args", {mesh}}, {"padMode", "terraced"}});
    CAPTURE(graded.dump());
    CHECK(graded["errors"].empty());
    CHECK(graded["terrain"] == t);
    CHECK(graded["streets"].get<int>() > 0);
    CHECK(graded["streetsSent"] == graded["streets"]);
    CHECK(graded["pads"].get<int>() > 0);
    CHECK(graded["waterFeatures"].get<int>() == 1);
    CHECK(graded["plan"] == "pinned");
    int streetRoads = 0;
    const json roadLayers = run("terrainRoads", {{"args", {t}}})["layers"];
    std::string layerNames;
    for (auto& [name, list] : roadLayers.items())
        layerNames += name + "(" + std::to_string(list.size()) + ") ";
    CAPTURE(layerNames);
    for (const json& r : roadLayers.value("CityRoads", json::array()))
    {
        const std::string id = r["id"].get<std::string>();
        streetRoads += id.find("/street/") != std::string::npos && id[id.size() - 2] != '_';
    }
    CHECK(streetRoads == graded["streets"].get<int>());
    const json city = run("terrainStamps", {{"args", {t}}})["layers"]["City"];
    int canals = 0, terraced = 0;
    for (const json& s : city)
    {
        canals += s.value("kind", std::string()) == "canal";
        terraced += s.value("mode", std::string()) == "terraced";
    }
    CHECK(canals == 1);
    CHECK(terraced == graded["pads"].get<int>());

    // The plan is pinned at the graded terrain: grading again changes nothing.
    const json again = run("settlementGrade", {{"args", {mesh}}, {"padMode", "terraced"}});
    CAPTURE(again.dump());
    CHECK(again["errors"].empty());
    CHECK(again["streetsSent"].get<int>() == 0);
    CHECK(again["stampsSent"].get<int>() == 0);
    CHECK(again["removed"].get<int>() == 0);
    CHECK(again["replan"]["mode"] == "unchanged");

    // Without blocks every pad goes, and the streets stay as they were sent.
    run("setAttr", {{"args", {made["modifier"].get<std::string>() + ".blocks", false}}});
    const json bare = run("settlementGrade", {{"args", {mesh}}});
    CAPTURE(bare.dump());
    CHECK(bare["pads"].get<int>() == 0);
    CHECK(bare["removed"].get<int>() >= graded["pads"].get<int>());
    CHECK(bare["replan"]["mode"] == "full");
    CHECK(bare["plan"] == "pinned");
    CHECK_THROWS(run("settlementGrade", {{"args", {mesh}}, {"terrain", "NoSuchTerrain"}}));
}

TEST_CASE("settlement terrain: on rough ground every graded street keeps its grade or says why")
{
    terrainLoadPackages();
    run("newScene");
    const std::string t = terrainMesh(80000.0, 129, 4000.0, 5);
    const json made = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 40000.0}, {"seed", 3}, {"ground", t}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json graded = run("settlementGrade", {{"args", {mesh}}, {"lots", false}, {"plazas", false}});
    CAPTURE(graded.dump());
    CHECK(graded["errors"].empty());
    REQUIRE(graded["streets"].get<int>() > 20);
    // Only pieces whose junction heights leave no way within the grade remain: curve rounding never adds any.
    for (const json& o : graded["overGrade"])
        CHECK(o["why"].get<std::string>().find("the height keys need grades over maxGrade") != std::string::npos);
    MESSAGE("over grade: " << graded["overGrade"].size() << " of " << graded["streets"].get<int>());
    CHECK(graded["overGrade"].size() * 50 <= graded["streets"].get<size_t>());
    // Every terrain road steeper than its limit is in the report.
    std::set<std::string> reported;
    for (const json& o : graded["overGrade"])
        reported.insert(o["id"].get<std::string>());
    for (const json& r : run("terrainRoads", {{"args", {t}}})["layers"]["CityRoads"])
        if (r["maxGrade"].get<double>() > r["design"]["maxGrade"].get<double>() * 1.01 + 0.05)
            CHECK(reported.contains(r["id"].get<std::string>()));

    // The junction heights are kept, so grading again sends nothing.
    const json again = run("settlementGrade", {{"args", {mesh}}, {"lots", false}, {"plazas", false}});
    CHECK(again["streetsSent"].get<int>() == 0);
}

TEST_CASE("settlement terrain: re-planning keeps untouched blocks byte for byte")
{
    // Two grounds that differ only in a bump: blocks away from it keep their hash, blocks on it are laid again.
    Site site;
    site.size = 60000.0;
    const Options o = terrainProfileOptions("usTown", 4);
    const geo::Mesh flatMesh = heightMesh(40000.0, 40, [](double, double) { return 0.0; });
    const geo::Mesh bumped = heightMesh(40000.0, 40, [](double x, double z) {
        const double dx = x - 15000.0, dz = z - 15000.0;
        return 900.0 * std::exp(-(dx * dx + dz * dz) / (2.0 * 2500.0 * 2500.0));
    });
    const Layout before = build(site, Ground::fromMesh(flatMesh, 200.0), o);
    const Layout fresh = build(site, Ground::fromMesh(bumped, 200.0), o);
    const std::vector<DirtyRect> dirty = {{{8000.0, 8000.0}, {22000.0, 22000.0}}};
    json report;
    const Layout merged = mergeDirty(before, fresh, dirty, &report);
    CAPTURE(report.dump());
    int untouched = 0, maxOld = 0;
    for (const Block& b : before.blocks)
    {
        maxOld = std::max(maxOld, b.id);
        if (touches(b.outline, dirty[0]))
            continue;
        ++untouched;
        CHECK(blockHash(merged, b.id) == blockHash(before, b.id));
    }
    CHECK(untouched > 0);
    CHECK(report["keptBlocks"].get<int>() == untouched);
    CHECK(report["droppedBlocks"].get<int>() > 0);
    for (const Block& b : merged.blocks)
        if (touches(b.outline, dirty[0]))
            CHECK(b.id > maxOld);
    std::set<int> lotIds;
    for (const Lot& lot : merged.lots)
        CHECK(lotIds.insert(lot.id).second);

    // The plan round trips through its JSON exactly.
    const auto snap = planFromJson(planToJson(merged, 42, "r1", "Terrain"));
    Layout back = emptyLayout();
    applySnapshot(back, *snap);
    CHECK(snap->key == 42);
    CHECK(back.blocks.size() == merged.blocks.size());
    for (const Block& b : merged.blocks)
        CHECK(blockHash(back, b.id) == blockHash(merged, b.id));
}

TEST_CASE("settlement terrain: settlementReplan re-plans only where the Terrain changed")
{
    terrainLoadPackages();
    run("newScene");
    const std::string t = terrainMesh(80000.0, 129, 600.0, 5);
    const json made = run("settlementCreate", {{"profile", "usTown"}, {"size", 50000.0}, {"seed", 4}, {"ground", t}, {"terrainWater", false}});
    const std::string mesh = made["mesh"].get<std::string>();

    const json first = run("settlementReplan", {{"args", {mesh}}});
    CAPTURE(first.dump());
    CHECK(first["mode"] == "full");
    CHECK(run("settlementInfo", {{"args", {mesh}}})["plan"] == "pinned");
    CHECK(run("settlementReplan", {{"args", {mesh}}})["mode"] == "unchanged");

    const json planBefore = editsOf(made)["plan"];
    run("terrainSculpt", {{"args", {t}}, {"op", "raise"}, {"points", json::array({json::array({18000, 18000})})}, {"radius", 3000.0}, {"strength", 600.0}, {"stage", "final"}});
    const json dirty = run("settlementReplan", {{"args", {mesh}}});
    CAPTURE(dirty.dump());
    REQUIRE(dirty["mode"] == "dirty");
    CHECK(dirty["regions"].size() > 0);
    CHECK(dirty["keptBlocks"].get<int>() > 0);

    std::vector<DirtyRect> rects;
    for (const json& r : dirty["regions"])
        rects.push_back({{r["min"][0].get<double>(), r["min"][1].get<double>()}, {r["max"][0].get<double>(), r["max"][1].get<double>()}});
    Layout a = emptyLayout(), b = emptyLayout();
    applySnapshot(a, *planFromJson(planBefore));
    applySnapshot(b, *planFromJson(editsOf(made)["plan"]));
    int same = 0;
    for (const Block& blk : a.blocks)
    {
        bool hit = false;
        for (const DirtyRect& r : rects)
            hit = hit || touches(blk.outline, r);
        if (hit)
            continue;
        CHECK(blockHash(b, blk.id) == blockHash(a, blk.id));
        ++same;
    }
    CHECK(same == dirty["keptBlocks"].get<int>());
    CHECK(run("settlementReplan", {{"args", {mesh}}})["mode"] == "unchanged");

    // Changing the settlement's own inputs leaves the plan stale until it is planned again.
    run("setAttr", {{"args", {made["modifier"].get<std::string>() + ".seed", 9}}});
    CHECK(run("settlementInfo", {{"args", {mesh}}})["plan"] == "stale");
    const json full = run("settlementReplan", {{"args", {mesh}}});
    CHECK(full["mode"] == "full");
    CHECK(full["why"].get<std::string>().find("inputs changed") != std::string::npos);
}

TEST_CASE("settlement terrain: checks find each kind of problem by id")
{
    Layout l = emptyLayout();
    const Ground ground = Ground::plane(0.0);
    l.lots.push_back(squareLot(0, {0.0, 0.0}, 1000.0, 1));
    l.lots.push_back(squareLot(1, {500.0, 500.0}, 1000.0, 1)); // overlaps lot 0
    l.lots.push_back(squareLot(2, {5000.0, 0.0}, 1000.0, 0));  // built, no frontage
    l.lots.push_back(squareLot(3, {9000.0, 0.0}, 1000.0, 1));
    Envelope env;
    env.lot = 3;
    env.footprint = makePolygon({{9100, 100}, {9900, 100}, {9900, 900}, {9100, 900}});
    env.base = 200.0; // over flat ground at 0
    l.envelopes.push_back(env);
    Envelope outside = env;
    outside.lot = 0;
    outside.footprint = makePolygon({{-500, 100}, {900, 100}, {900, 900}, {-500, 900}});
    outside.base = -50.0;
    l.envelopes.push_back(outside);

    // A river across x = 20000; a local street and an arterial cross it, and a one-way lane closes a loop one way only.
    l.water.push_back({{{20000.0, -40000.0}, {20000.0, 40000.0}}, 2000.0, ""});
    l.graph.addStreet({15000.0, -10000.0}, {25000.0, -10000.0}, StreetClass::Local);
    l.graph.addStreet({15000.0, 10000.0}, {25000.0, 10000.0}, StreetClass::Arterial);
    l.graph.addStreet({15000.0, -10000.0}, {15000.0, 10000.0}, StreetClass::Collector);
    RoadProfile oneWay = l.roads[size_t(StreetClass::Local)];
    oneWay.oneWay = true;
    oneWay.name = "oneWay";
    l.roads.push_back(oneWay);
    StreetEdge like;
    like.cls = StreetClass::Local;
    like.profile = int(l.roads.size()) - 1;
    l.graph.addStreet({-20000.0, -20000.0}, {-10000.0, -20000.0}, like);

    const json c = checkLayout(l, ground, Options());
    CAPTURE(c.dump());
    CHECK_FALSE(c["ok"].get<bool>());
    CHECK(c["counts"]["overlap"] == 1);
    CHECK(c["counts"]["frontage"] == 1);
    CHECK(c["counts"]["foundation"] == 1);
    CHECK(c["counts"]["envelope"].get<int>() >= 1);
    CHECK(c["counts"]["water"] == 1);
    CHECK(c["counts"]["lanes"].get<int>() >= 1);
    CHECK(c["counts"]["phases"] == 0);
    bool frontage = false;
    for (const json& i : c["issues"])
        frontage = frontage || (i["check"] == "frontage" && i["id"] == "lot:2");
    CHECK(frontage);

    // The arterial over the river is a bridge; the local street is not.
    int bridges = 0;
    for (size_t e = 0; e < l.graph.edges.size(); ++e)
        if (l.graph.isStreet(int(e)) && edgeInWater(l, int(e)))
            bridges += edgeIsBridge(l, int(e));
    CHECK(bridges >= 1);
}

TEST_CASE("settlement terrain: settlementCheck and settlementExplain on a settlement")
{
    terrainLoadPackages();
    run("newScene");
    const json made = run("settlementCreate", {{"profile", "medievalTown"}, {"size", 40000.0}, {"seed", 3}});
    const std::string mesh = made["mesh"].get<std::string>();
    CHECK_FALSE(made.contains("terrain"));
    const json c = run("settlementCheck", {{"args", {mesh}}});
    for (const char* k : {"frontage", "overlap", "grade", "water", "envelope", "foundation", "lanes", "phases"})
        CHECK(c["counts"].contains(k));
    CHECK(c["counts"]["overlap"] == 0);
    CHECK(c["counts"]["envelope"] == 0);
    CHECK(c["counts"]["foundation"] == 0);
    CHECK(c["plan"] == "none");
    CHECK(c["warnings"].empty());
    CHECK(c["counts"]["laneGraph"] == 0);
    const json reported = run("settlementCheck", {{"args", {mesh}}, {"report", true}});
    CHECK(reported["reported"].get<size_t>() == std::min<size_t>(reported["issues"].size(), 200));

    const json centre = run("settlementExplain", {{"args", {mesh}}, {"id", "centre"}});
    CHECK(centre["reasons"].size() >= 2);
    CHECK_THROWS(run("settlementExplain", {{"args", {mesh}}, {"id", "site"}}));
    CHECK_THROWS(run("settlementExplain", {{"args", {mesh}}, {"id", "lot:999999"}}));
    const json lot = run("settlementExplain", {{"args", {mesh}}, {"id", "lot:0"}});
    CHECK(lot["reasons"].size() >= 1);
    CHECK_FALSE(lot["use"].get<std::string>().empty());

    // A region on a Terrain keeps each settlement's site, which explain reads back term by term.
    const std::string t = terrainMesh(400000.0, 129, 4000.0, 7);
    const json region = run("settlementRegion", {{"ground", t}, {"cities", 0}, {"towns", 1}, {"villages", 1}, {"kind", "market"}, {"townRadius", 15000.0}, {"villageRadius", 8000.0}});
    CAPTURE(region.dump());
    CHECK(region["ground"] == "terrain");
    REQUIRE(region["settlements"].size() >= 1);
    const json site = run("settlementExplain", {{"args", {region["settlements"][0]["name"].get<std::string>()}}, {"id", "site"}});
    CHECK(site["site"]["kind"] == "market");
    CHECK(site["reasons"].size() >= 5);
}

TEST_CASE("settlement terrain: every built-in style and era preset passes settlementCheck")
{
    terrainLoadPackages();
    std::vector<std::pair<std::string, json>> cases;
    const json profiles = run("settlementProfiles");
    for (const auto& [name, profile] : profiles.items())
        for (int seed : {1, 2, 7})
            cases.push_back({name, {{"profile", name}, {"seed", seed}}});
    for (const char* eras : {"walledTown", "medievalToModern", "gridToSprawl"})
        cases.push_back({eras, {{"profile", "medievalTown"}, {"seed", 11}, {"eras", eras}}});
    for (auto& [label, flags] : cases)
    {
        run("newScene");
        flags["size"] = 60000.0;
        const json made = run("settlementCreate", flags);
        const json c = run("settlementCheck", {{"args", {made["mesh"]}}});
        INFO(label << " " << flags.dump() << " " << c["issues"].dump().substr(0, 400));
        CHECK(c["ok"] == true);
    }
}

TEST_CASE("settlement terrain: a walled town at a river keeps streets out of the water, stays connected and sits on its graded ground")
{
    terrainLoadPackages();
    run("newScene");
    const std::string t = terrainMesh(200000.0, 129, 4000.0, 3);
    run("terrainAddRiver", {{"args", {t}}, {"id", "main"}, {"path", json::array({json::array({-95000, -20000}), json::array({0, 0}), json::array({95000, 15000})})}});
    const double cx = 31478.9, cz = -12861.6, half = 30000.0;
    const json outline = json::array({json::array({cx - half, cz - half}), json::array({cx + half, cz - half}), json::array({cx + half, cz + half}),
                                      json::array({cx - half, cz + half})});
    const json made = run("settlementCreate", {{"outline", outline}, {"attractors", json::array({json::array({cx, cz})})}, {"ground", t},
                                               {"profile", "medievalTown"}, {"seed", 2}, {"eras", "walledTown"}});
    const std::string mesh = made["mesh"].get<std::string>();
    const json before = run("settlementCheck", {{"args", {mesh}}});
    INFO(before["issues"].dump().substr(0, 600));
    for (const char* k : {"water", "lanes", "laneGraph", "overlap", "frontage", "envelope", "foundation"})
        CHECK(before["counts"][k] == 0);

    run("settlementGrade", {{"args", {mesh}}, {"streets", false}, {"water", false}});
    const json after = run("settlementCheck", {{"args", {mesh}}});
    CHECK(after["plan"] == "pinned");
    CHECK(after["counts"]["foundation"] == 0);
}
