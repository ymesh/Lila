// World spec: shape checks, worldBuild on a tiny world (terrain, settlement with buildings, sites with buildings, slot
// rules and pins, zones), worldSpec read back and rebuilt the same, worldMap, and worldCheck catching each issue kind.

#include "RemixSpec.h"
#include "World.h"

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Node.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;
using namespace openworld::world;

namespace
{
using owtest::failedSteps;
using owtest::run;
void loadWorldPackages()
{
    if (!lila::ops::find("terrainCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!lila::ops::find("settlementCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!lila::ops::find("buildingCreate"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_BUILDINGS_PACKAGE) == "Buildings");
    if (!lila::ops::find("worldBuild"))
        REQUIRE(lila::extensions::ExtensionManager::get().load(LILA_TEST_OPENWORLD_PACKAGE) == "OpenWorld");
}

json tinySpec()
{
    return json::parse(R"({
        "name": "Tiny",
        "culture": "mexican",
        "terrain": {"size": [260000, 260000], "resolution": 129, "seed": 3, "source": {"type": "noise", "amplitude": 1500, "featureSize": 120000}},
        "settlements": [{"name": "Pueblo", "create": {"profile": "usTown", "seed": 2,
                          "outline": [[-36000, -36000], [36000, -36000], [36000, 36000], [-36000, 36000]]},
                         "buildings": {"style": "colonial"}}],
        "sites": [{"kind": "village", "id": "v1", "at": [80000, 80000]},
                  {"kind": "cemetery", "id": "c1", "at": [-80000, 80000], "params": {"cemeteryWidth": 9000, "cemeteryDepth": 7000}}],
        "slots": {"rules": [{"id": "r1", "name": "Signs", "type": "sign.storefront", "payload": {"poolTag": "fictional_signs"}}],
                  "pins": ["owner:site:c1"]},
        "zones": {"fromDistricts": true,
                  "spec": {"layers": [{"id": "police", "type": "police", "zones": [{"id": "all", "name": "Comandancia Norte",
                            "polygons": [[[-130000, -130000], [130000, -130000], [130000, 130000], [-130000, 130000]]]}]}]}},
        "check": true
    })");
}

std::set<std::string> issueKinds(const json& c)
{
    std::set<std::string> k;
    for (const json& i : c["issues"])
        k.insert(i["kind"].get<std::string>());
    return k;
}

} // namespace

TEST_CASE("world spec: schema and shape checks name the bad field")
{
    const json& schema = worldSpecSchema();
    for (const char* k : {"remix", "terrain", "settlements", "buildings", "slots", "sites", "zones", "export"})
        CHECK(schema["properties"].contains(k));
    CHECK_NOTHROW(validateSpec(tinySpec()));
    auto fails = [](json s, const std::string& word) {
        try
        {
            validateSpec(s);
            FAIL("accepted: " << s.dump());
        }
        catch (const std::exception& e)
        {
            CHECK_MESSAGE(std::string(e.what()).find(word) != std::string::npos, e.what());
        }
    };
    json s = tinySpec();
    s["colour"] = 1;
    fails(s, "unknown key colour");
    s = tinySpec();
    s.erase("terrain");
    fails(s, "needs a terrain");
    s = tinySpec();
    s["settlements"][0]["osm"] = "remix";
    fails(s, "osm");
    s = tinySpec();
    s["settlements"][0].erase("create");
    s["settlements"][0]["osm"] = "remix";
    fails(s, "no remix");
    s = tinySpec();
    s["sites"][1]["id"] = "v1";
    fails(s, "used twice");
    s = tinySpec();
    s["settlements"][0]["area"] = {0, 0, -1, 5};
    fails(s, "x0 < x1");
    s = tinySpec();
    s["buildings"] = {{"enterable", {{"programs", {"police"}}, {"every", true}}}};
    fails(s, "no key 'every'");
    s["buildings"] = {{"enterable", {{{"programs", {"police"}}}, {{"landmarks", true}, {"limit", 2}}}}};
    CHECK_NOTHROW(validateSpec(s));

    // Relative paths resolve against the spec's folder; a missing file goes there too, unless found elsewhere (the workspace).
    const fs::path dir = fs::path(LILA_TEST_OPENWORLD_DATA);
    const json rel = json::parse(R"({"remix": {"sources": [{"id": "a", "dem": "coast.asc", "osm": "missing.json"}]},
                                     "terrain": {"coast": {"bathymetry": {"file": "real/gebco.tif"}}}})");
    json r = resolvePaths(rel, dir.generic_string());
    CHECK(fs::path(r["remix"]["sources"][0]["dem"].get<std::string>()).is_absolute());
    CHECK(fs::path(r["remix"]["sources"][0]["osm"].get<std::string>()) == (dir / "missing.json").lexically_normal());
    CHECK(fs::path(r["terrain"]["coast"]["bathymetry"]["file"].get<std::string>()) == (dir / "real/gebco.tif").lexically_normal());
    r = resolvePaths(rel, dir.generic_string(), [](const std::string& f) { return f == "missing.json"; });
    CHECK(r["remix"]["sources"][0]["osm"] == "missing.json");
}

TEST_CASE("world spec: clipOsm cuts lines at the area's edge, keeps closed ways mostly inside whole, and relations' kept members")
{
    openworld::OsmDoc d;
    std::vector<glm::dvec2> world;
    // World x is longitude, z latitude, so the edge node's lat/lon can be read back exactly.
    auto node = [&](int64_t id, double lat, double lon, openworld::OsmTags tags = {}) {
        d.nodes.push_back({id, {lat, lon}, std::move(tags)});
        world.push_back({lon, lat});
    };
    node(1, 0, 0), node(2, 0, 2), node(3, 0, 4), node(4, 5, 5), node(5, 6, 6), node(6, 0.5, 1, {{"amenity", "cafe"}}), node(7, 6, 7, {{"shop", "bakery"}});
    node(8, 0.2, 0.2), node(9, 0.2, 0.6), node(13, 0.6, 0.6), node(14, 0.6, 3.5);
    node(15, -0.5, -2), node(16, -0.5, 0.5), node(17, -0.5, 6), node(18, -0.5, 2), node(19, -0.5, 7);
    d.ways.push_back({10, {1, 2, 3}, {{"highway", "trunk"}}});
    d.ways.push_back({11, {4, 5}, {{"highway", "track"}}});
    d.ways.push_back({12, {8, 9, 13, 8}, {{"building", "yes"}}});
    d.ways.push_back({21, {8, 14, 13, 8}, {{"building", "yes"}}});
    d.ways.push_back({22, {15, 16, 17, 18, 19}, {{"highway", "primary"}}});
    d.relations.push_back({20, {{'w', 10, "outer"}, {'w', 11, "outer"}}, {{"type", "multipolygon"}}});
    d.index();
    const openworld::OsmDoc c = clipOsm(d, world, {-1, -1}, {3, 1});
    std::map<int64_t, const openworld::OsmDoc::Way*> ways;
    for (const auto& w : c.ways)
        ways[w.id] = &w;
    REQUIRE(ways.count(10));
    CHECK_FALSE(ways.count(11));
    CHECK(ways.count(12));
    CHECK(ways.count(21));
    // The trunk keeps 1 and 2, then a node on the edge at x = 3, halfway to 3.
    REQUIRE(ways[10]->nodes.size() == 3);
    const openworld::OsmDoc::Node* edge = c.node(ways[10]->nodes[2]);
    REQUIRE(edge);
    CHECK(edge->at.lon == doctest::Approx(3.0));
    CHECK(edge->at.lat == doctest::Approx(0.0));
    // The primary leaves and comes back: two pieces, each closed off on the edge.
    int primaries = 0;
    for (const auto& w : c.ways)
        primaries += w.tags.count("highway") && w.tags.at("highway") == "primary";
    CHECK(primaries == 2);
    std::set<int64_t> tagged;
    for (const auto& n : c.nodes)
        if (!n.tags.empty())
            tagged.insert(n.id);
    CHECK(tagged == std::set<int64_t>{6});
    REQUIRE(c.relations.size() == 1);
    CHECK(c.relations[0].members.size() == 1);

    // keep narrows the box: the trunk now ends where it leaves x < 1.5, found by halving down to that edge.
    const openworld::OsmDoc k = clipOsm(d, world, {-1, -1}, {3, 1}, [](glm::dvec2 p) { return p.x < 1.5; });
    const openworld::OsmDoc::Way* trunk = nullptr;
    for (const auto& w : k.ways)
        if (w.id == 10)
            trunk = &w;
    REQUIRE(trunk);
    REQUIRE(trunk->nodes.size() == 2);
    const openworld::OsmDoc::Node* cut = k.node(trunk->nodes[1]);
    REQUIRE(cut);
    CHECK(cut->at.lon == doctest::Approx(1.5).epsilon(1e-3));
}

TEST_CASE("world spec: the shipped real-data Sinaloa spec validates, and its cities keep to their built-up land")
{
    std::ifstream f(std::string(LILA_TEST_OPENWORLD_DATA) + "/sinaloa_real.json");
    const json spec = json::parse(f);
    CHECK_NOTHROW(validateSpec(spec));
    const openworld::RemixSpec remix = openworld::specFromJson(spec["remix"]);
    CHECK(remix.erosion.on());
    std::set<std::string> builtUp;
    for (const openworld::Source& src : remix.sources)
        if (src.extent == "builtUp")
            builtUp.insert("remixPresent_" + src.id);
    CHECK(builtUp.size() == 2);
    for (const json& s : spec["settlements"])
        CHECK(builtUp.count(s.value("within", std::string())));
}

TEST_CASE("world: worldBuild on a tiny world, read back, rebuilt the same, mapped and checked")
{
    loadWorldPackages();
    run("newScene");
    const json r = run("worldBuild", {{"spec", tinySpec()}});
    INFO(failedSteps(r));
    for (const json& s : r["steps"])
        CHECK_MESSAGE(s.value("ok", false), s.dump());
    REQUIRE(r["world"].is_string());
    CHECK(r["built"]["sites"] == json::array({"v1", "c1"}));
    CHECK(r.contains("check"));
    lila::scene::Graph& g = lila::scene::Graph::current();
    CHECK(g.find("Pueblo_Buildings"));
    CHECK(g.find("SiteBuildings"));
    // Zones are named from the naming cultures: Settlement's district names, fictional names for the rest, none numbered.
    for (const json& l : run("zoneSpec")["layers"])
        for (const json& z : l["zones"])
            CHECK_MESSAGE(z.value("nameSource", std::string()) != "numbered", z.dump().substr(0, 200));
    // Site buildings come from Buildings in their kits' styles; the site draws no massing.
    const json sb = run("siteBuildings", {{"args", {"v1", "c1"}}});
    CHECK(sb["buildings"].get<int>() > 3);
    CHECK(sb["styles"].contains("mausoleum"));
    CHECK((sb["styles"].contains("adobe") || sb["styles"].contains("ruralShack")));

    // Read back: the scene's sites, slots and zones; a Buildings edit survives a rebuild from the read-back spec.
    std::string someBuilding;
    for (lila::scene::NodeId c : g.children(g.find("Pueblo_Buildings")->id()))
    {
        const json info = run("buildingInfo", {{"args", {g.path(c)}}});
        if (info.contains("buildings") && info["buildings"].is_array() && !info["buildings"].empty())
        {
            const json& b = info["buildings"][0];
            someBuilding = b.is_string() ? b.get<std::string>() : b.value("id", std::string());
            break;
        }
    }
    CHECK_FALSE(someBuilding.empty());
    if (!someBuilding.empty())
        run("buildingEdit", {{"building", someBuilding}, {"floors", 5}});
    json back = run("worldSpec");
    const json spec = back["spec"];
    CHECK(back["from"]["sites"] == "scene");
    REQUIRE(spec["sites"].size() == 2);
    CHECK(spec["sites"][0]["id"] == "v1");
    CHECK(spec["sites"][0]["buildings"] == true);
    CHECK(spec["slots"]["rules"][0]["id"] == "r1");
    CHECK(spec["slots"]["pins"] == json::array({"owner:site:c1"}));
    CHECK(spec["zones"]["spec"]["layers"].size() >= 2);
    if (!someBuilding.empty())
        CHECK_MESSAGE(spec["buildings"]["edits"]["Pueblo_Buildings"].contains(someBuilding), someBuilding << " " << spec["buildings"].dump().substr(0, 600));
    CHECK(run("worldSpec", {{"spec", spec}})["ok"] == true);
    const size_t zonesBefore = spec["zones"]["spec"]["layers"].size();

    const json r2 = run("worldBuild", {{"spec", spec}, {"replace", true}});
    INFO(failedSteps(r2));
    for (const json& s : r2["steps"])
        CHECK_MESSAGE(s.value("ok", false), s.dump());
    const json again = run("worldSpec")["spec"];
    CHECK(again["sites"] == spec["sites"]);
    CHECK(again["slots"] == spec["slots"]);
    CHECK(again["zones"]["spec"]["layers"].size() == zonesBefore);
    if (!someBuilding.empty())
        CHECK(again["buildings"]["edits"]["Pueblo_Buildings"][someBuilding] == spec["buildings"]["edits"]["Pueblo_Buildings"][someBuilding]);
    CHECK_THROWS(run("worldBuild", {{"spec", spec}}));
}

TEST_CASE("world: the spec's own material layers are laid last, over the finished ground")
{
    loadWorldPackages();
    run("newScene");
    json spec = tinySpec();
    spec.erase("zones");
    spec["check"] = false;
    spec["terrain"]["materials"] = json::parse(R"({"layers": [{"name": "ground", "color": [0.1, 0.1, 0.05]},
        {"name": "steep", "color": [0.2, 0.16, 0.12], "when": {"slope": {"range": [30, null], "fade": 5}}}]})");
    const json r = run("worldBuild", {{"spec", spec}});
    INFO(failedSteps(r));
    std::vector<std::string> steps;
    for (const json& s : r["steps"])
    {
        CHECK_MESSAGE(s.value("ok", false), s.dump().substr(0, 300));
        steps.push_back(s["step"].get<std::string>());
    }
    const auto at = [&](const std::string& name) { return std::find(steps.begin(), steps.end(), name) - steps.begin(); };
    REQUIRE(at("terrain.materials") < std::ptrdiff_t(steps.size()));
    CHECK(at("terrain.materials") > at("sites.grade"));
    CHECK(at("terrain.materials") > at("buildings.sites"));
    for (const json& n : r["notes"])
        CHECK(n.get<std::string>().find("default layers") == std::string::npos);
    const json layers = run("terrainMaterialRules", {{"args", {r["built"]["terrain"]}}, {"query", true}})["layers"];
    REQUIRE(layers.size() == 2);
    CHECK(layers[0]["name"] == "ground");
    CHECK(layers[1]["name"] == "steep");
}

TEST_CASE("world: worldCheck catches each issue kind, reports to Validation, and worldMap writes the overview")
{
    loadWorldPackages();
    run("newScene");
    json spec = tinySpec();
    spec["slots"].erase("pins");
    const json r = run("worldBuild", {{"spec", spec}});
    INFO(failedSteps(r));
    REQUIRE(r["world"].is_string());
    lila::scene::Graph& g = lila::scene::Graph::current();
    const std::string terrain = r["terrain"].get<std::string>();

    const json c0 = run("worldCheck");
    CHECK(c0["placeholders"]["onPlaceholder"].get<int>() > 0);
    CHECK(c0["placeholders"]["byType"].size() > 3);
    CHECK(issueKinds(c0).count("placeholders"));
    for (const char* k : {"realName", "propFloating", "propBuried", "slotOrphan", "buildingsStale", "zoneNumberedName", "buildingOverRoad", "buildingOverWater"})
        CHECK_MESSAGE(issueKinds(c0).count(k) == 0, std::string(k) << " " << c0["counts"].dump());

    // Plant one of each.
    const double x = 20000, z = -100000;
    const json h = run("terrainSample", {{"args", {terrain}}, {"points", json::array({json::array({x, z})})}});
    const double ground = h[0]["height"].get<double>();
    run("slotPublish", {{"args", {"Planted"}},
                        {"slots", json::array({{{"owner", "test"}, {"path", "float"}, {"type", "lamp.street"}, {"position", {x, ground + 500, z}}},
                                               {{"owner", "test"}, {"path", "bury"}, {"type", "lamp.street"}, {"position", {x + 600, ground - 500, z}}},
                                               {{"owner", "test"}, {"path", "sign"}, {"type", "sign.storefront"}, {"position", {x, ground + 300, z + 600}},
                                                {"payload", {{"data", {{"text", "Walmart"}}}}}}})}});
    run("slotSet", {{"id", "ghost/slot"}, {"data", {{"k", 1}}}});
    for (lila::scene::NodeId m : lila::scene::modifiers::stack(g, g.find("Pueblo")->id()))
        if (lila::scene::Node* mod = g.node(m); mod && mod->type().attrIndex("walkable") >= 0)
            run("setAttr", {{"args", {mod->path() + ".seed", 9}}});
    json zs = run("zoneSpec");
    std::string districtLayer, firstZone;
    for (json& l : zs["layers"])
        if (l["type"] == "district" && !l["zones"].empty())
        {
            districtLayer = l["id"];
            firstZone = l["zones"][0]["id"];
            l["zones"].back()["nameSource"] = "numbered";
        }
    REQUIRE_FALSE(districtLayer.empty());
    run("zoneSpec", {{"spec", zs}});
    run("zoneDelete", {{"layer", districtLayer}, {"zone", firstZone}});
    run("terrainAddLane", {{"args", {terrain}}, {"id", "dryLane"}, {"path", json::array({{-60000, -60000}, {-20000, -60000}})}, {"craft", "skiff"}});
    const json envs = run("siteEnvelopes", {{"sites", {"v1"}}});
    REQUIRE(envs["envelopes"].size() > 0);
    double cx = 0, cz = 0;
    for (const json& p : envs["envelopes"][0]["footprint"])
        cx += p[0].get<double>() / envs["envelopes"][0]["footprint"].size(), cz += p[1].get<double>() / envs["envelopes"][0]["footprint"].size();
    run("terrainAddRoad", {{"args", {terrain}}, {"id", "throughVillage"}, {"path", json::array({{cx - 3000, cz}, {cx + 3000, cz}})}});

    const json c1 = run("worldCheck", {{"report", true}});
    const std::set<std::string> k = issueKinds(c1);
    CHECK_FALSE(c1["ok"].get<bool>());
    for (const char* want : {"realName", "propFloating", "propBuried", "slotOrphan", "buildingsStale", "zoneNumberedName", "zone.gap", "unreachableWater", "buildingOverRoad"})
        CHECK_MESSAGE(k.count(want), want << " missing; kinds: " << c1["counts"].dump());
    const json listed = run("listIssues", {{"source", "worldCheck"}});
    CHECK(listed.size() > 5);
    bool staleFix = false;
    for (const json& i : listed)
        staleFix = staleFix || i.value("fix", std::string()).find("buildingRefresh") != std::string::npos;
    CHECK(staleFix);

    // The stale fix clears its kind; the sea over everything puts buildings on water.
    run("buildingRefresh");
    CHECK(issueKinds(run("worldCheck")).count("buildingsStale") == 0);
    run("terrainSeaLevel", {{"args", {terrain}}, {"level", 100000.0}});
    CHECK(issueKinds(run("worldCheck")).count("buildingOverWater"));

    // Overview map: image and JSON with districts, zones, settlements and sites.
    const fs::path out = fs::temp_directory_path() / "lila_world_map_test";
    const json m = run("worldMap", {{"resolution", 256}, {"folder", out.generic_string()}});
    CHECK(fs::exists(fs::path(m["image"].get<std::string>())));
    CHECK(m["counts"]["districts"].get<int>() > 0);
    CHECK(m["counts"]["sites"] == 2);
    std::ifstream f(fs::path(m["json"].get<std::string>()));
    const json doc = json::parse(f);
    CHECK(doc["format"] == "lila.worldMap");
    CHECK(doc["size"][0] == 256);
    CHECK(doc["zones"].size() >= 2);
}

TEST_CASE("world: the tiny Sinaloa world builds headless from one spec file, remix to zones, with no real name out")
{
    loadWorldPackages();
    run("newScene");
    const fs::path file = fs::path(LILA_TEST_OPENWORLD_DATA) / "world_tiny" / "world.json";
    const fs::path out = fs::temp_directory_path() / "lila_world_tiny_test";
    std::error_code ec;
    fs::remove_all(out, ec);
    const json r = run("worldBuild", {{"file", file.generic_string()}, {"folder", out.generic_string()}});
    INFO(failedSteps(r));
    for (const json& s : r["steps"])
    {
        CHECK_MESSAGE(s.value("ok", false), s.dump().substr(0, 600));
        MESSAGE(s["step"].get<std::string>() << " " << s["seconds"].get<double>() << " s " << s["memoryMB"].get<double>() << " MB");
    }
    REQUIRE(r["world"].is_string());
    lila::scene::Graph& g = lila::scene::Graph::current();
    CHECK(fs::exists(fs::path(r["built"]["remixOsm"].get<std::string>())));
    // The remixed OpenStreetMap lines are hidden in the built world, and the result says so.
    REQUIRE(r["built"].contains("remixFeatures"));
    CHECK(run("getAttr", {{"args", {r["built"]["remixFeatures"].get<std::string>() + ".visibility"}}}) == false);
    bool noted = false;
    for (const json& n : r["notes"])
        noted |= n.get<std::string>().find("are hidden") != std::string::npos;
    CHECK(noted);
    CHECK(fs::exists(out / "settlements" / "Puerto.osm.json"));
    CHECK(g.find("Puerto_Buildings"));
    CHECK(g.find("Capital_Buildings"));
    CHECK(g.find("SiteBuildings"));
    CHECK(r["built"]["sites"].size() == 9);
    const json c = r["check"];
    MESSAGE(c["counts"].dump());
    for (const char* k : {"realName", "remix.riverNoDrain", "remix.danglingRoad", "remix.coastGap", "zone.gap", "zone.overlap", "buildingOverWater",
                          "buildingOverRoad", "propFloating", "unreachableWater", "buildingsStale", "settlement.overlap", "building.overlappingNeighbours"})
        CHECK_MESSAGE(c["counts"].value(k, 0) == 0, k << ": " << c["counts"].dump());
    // The reservoir forms behind its dam, and the countryside between the towns is one district.
    const json water = run("terrainWater", {{"args", {r["terrain"]}}});
    bool reservoir = false;
    for (const json& l : water.value("lakes", json::array()))
        reservoir = reservoir || l.value("id", std::string()) == "picachos_reservoir";
    CHECK(reservoir);
    CHECK(r["exports"].contains("map"));
    CHECK(fs::exists(fs::path(r["exports"]["map"]["image"].get<std::string>())));
    // Every imported brand and street became fictional: nothing named as in the source OSM.
    const json leaks = run("realNameCheck");
    CHECK_MESSAGE(leaks.value("ok", false), leaks.dump().substr(0, 800));

    // Site pads standing across streams were laid with culverts: no site stamp blocks a stream.
    int sitePadsInStreams = 0;
    for (const json& i : run("terrainCheck", {{"args", {r["terrain"]}}})["issues"])
        sitePadsInStreams += i["type"] == "blocksFlow" && i["about"].get<std::string>().starts_with("Sites/");
    CHECK(sitePadsInStreams == 0);

    // The spec's walk-in buildings: the capital's police and government palaces, a bar, a mansion, a church, each with an entry.
    std::vector<std::string> enterable;
    for (const json& s : r["steps"])
        if (s["step"] == "buildings.enterable")
            for (const json& b : s["result"].value("buildings", json::array()))
            {
                enterable.push_back(b["building"].get<std::string>());
                CHECK_MESSAGE(b["entries"].get<int>() > 0, b.dump());
                CHECK(b["rooms"].get<int>() > 0);
            }
    CHECK(enterable.size() >= 5);
    CHECK(c["counts"].value("interiorNoEntry", 0) == 0);
    // An interior taken away by hand: worldCheck lists the building as an interior without entry.
    REQUIRE(!enterable.empty());
    const std::string gone = "Interior_" + enterable[0].substr(enterable[0].find(':') + 1);
    REQUIRE(g.find(gone));
    run("delete", {{"args", {gone}}});
    CHECK(run("worldCheck")["counts"].value("interiorNoEntry", 0) >= 1);
    run("undo");

    // The terrain draws in its material layers and the sea in its own material, as built and after a reopen.
    const std::string terrainName = g.find(r["terrain"].get<std::string>())->name();
    auto looks = [&](const char* when) {
        const json masks = run("terrainInfo", {{"args", {terrainName}}})["masks"];
        for (const char* layer : {"layer_grass", "layer_sand", "layer_road", "layer_forest", "layer_rock"})
            CHECK_MESSAGE(masks.dump().find(layer) != std::string::npos, when << " " << layer);
        // A low tropical coast: no snow anywhere.
        CHECK_MESSAGE(masks.value("layer_snow", json::object()).value("max", 1.0) < 0.01, when << " " << masks.value("layer_snow", json()).dump());
        const json wm = run("meshInfo", {{"args", {terrainName + "_water"}}});
        CHECK_MESSAGE(wm["materialSlots"].dump().find("TerrainWater") != std::string::npos, when << " " << wm["materialSlots"].dump());
    };
    looks("built");
    const fs::path scene = out / "tiny.lila";
    run("saveScene", {{"args", {scene.generic_string()}}});
    run("openScene", {{"args", {scene.generic_string()}}});
    looks("reopened");
}

TEST_CASE("world spec: the remix's bathymetry files, which make worldBuild skip the terrain spec's own")
{
    CHECK(remixBathymetryFiles(json::object()).empty());
    CHECK(remixBathymetryFiles({{"sources", {{{"id", "a"}, {"dem", "a.tif"}}}}}).empty());
    const json remix = {{"sources", {{{"id", "coast"}, {"bathymetry", "real/gebco.tif"}}, {{"id", "capital"}}, {{"id", "bay"}, {"bathymetry", "b.tif"}}}}};
    CHECK(remixBathymetryFiles(remix) == "real/gebco.tif, b.tif");
}
