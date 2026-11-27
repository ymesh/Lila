// Site kits: every kind lays out deterministically with typed slots and styled envelopes, settings are checked, and the
// commands build each kind on a synthetic coast terrain, keep slot ids across edits, publish slots, grade the terrain and
// undo, while siteCheck catches overlaps, water, steep ground and real names.

#include "Sites.h"

#include "extensions/ExtensionManager.h"
#include "names/Names.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <set>

using namespace lila;
using json = nlohmann::json;
namespace s = openworld::sites;

namespace
{
s::Spec specOf(const std::string& kind, json params = json::object())
{
    return s::specFromJson({{"kind", kind}, {"id", kind + "1"}, {"params", params}});
}

std::vector<std::string> slotIds(const s::Layout& L)
{
    std::vector<std::string> ids;
    for (const s::SlotOut& sl : L.slots)
        ids.push_back(sl.path);
    return ids;
}

void loadSitePackages()
{
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
    if (!ops::find("siteCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_OPENWORLD_PACKAGE) == "OpenWorld");
}

int issuesOf(const json& check, const std::string& kind, const std::string& site = {})
{
    int n = 0;
    for (const json& i : check["issues"])
        n += i["kind"] == kind && (site.empty() || i["site"] == site);
    return n;
}

/** A coast: land 3 m up for z > -60000, sea floor 15 m down past -61000, sea level 0; 10 m cells by default. */
std::string coastTerrain(int resolution = 501)
{
    const json t = ops::execute("terrainCreate", {{"name", "Coast"}, {"flat", true}, {"size", {500000, 500000}}, {"resolution", resolution}, {"center", {0, 300, 0}}});
    const std::string mesh = t["mesh"].get<std::string>();
    ops::execute("terrainStampRegion", {{"args", {mesh}},
                                        {"id", "sea"},
                                        {"footprint", {{-260000, -260000}, {260000, -260000}, {260000, -61500}, {-260000, -61500}}},
                                        {"mode", "carve"},
                                        {"height", -1500.0},
                                        {"falloff", 0.0},
                                        {"layer", "Base"}});
    ops::execute("terrainSeaLevel", {{"args", {mesh}}, {"level", 0.0}, {"preview", false}});
    return mesh;
}
} // namespace

TEST_CASE("site kits: every kind lays out the same way twice, typed slots, styled envelopes, fictional names")
{
    const std::vector<std::string>& styles = s::styles();
    for (const s::Kind& k : s::kinds())
    {
        CAPTURE(k.id);
        const s::Layout a = s::build(specOf(k.id)), b = s::build(specOf(k.id));
        CHECK(!a.slots.empty());
        CHECK(!a.envelopes.empty());
        CHECK(!a.ground.empty());
        CHECK(slotIds(a) == slotIds(b));
        const std::vector<std::string> ids = slotIds(a);
        CHECK(std::set<std::string>(ids.begin(), ids.end()).size() == ids.size());
        for (const s::SlotOut& sl : a.slots)
        {
            CHECK(!sl.type.empty());
            CHECK(sl.path.find("//") == std::string::npos);
        }
        std::set<std::string> envIds;
        for (const s::EnvelopeOut& e : a.envelopes)
        {
            CAPTURE(e.path);
            CHECK(std::ranges::find(styles, e.style) != styles.end());
            CHECK(!e.program.empty());
            CHECK(e.footprint.size() >= 3);
            CHECK(e.height > 0.0);
            CHECK(envIds.insert(e.path).second);
        }
        // No real brand among the names a kit hands out.
        std::vector<names::Text> texts;
        for (const auto& [role, n] : a.names.items())
            texts.push_back({role, n.get<std::string>()});
        CHECK(names::findRealNames(texts, names::RealNames(), true).empty());
        CHECK(!a.names.value("site", std::string()).empty());
    }

    // Kind-specific parts are there.
    const s::Layout port = s::build(specOf("port"));
    int containers = 0, sts = 0, rtg = 0, reefers = 0;
    std::set<std::string> liveryTypes;
    for (const s::SlotOut& sl : port.slots)
    {
        containers += sl.type.starts_with("container.");
        sts += sl.type == "crane.sts";
        rtg += sl.type == "crane.rtg";
        reefers += sl.type.starts_with("container.reefer.");
        if (sl.type.starts_with("container.40ft."))
            liveryTypes.insert(sl.type);
    }
    CHECK(containers == port.stats["containers"].get<int>());
    CHECK(containers > 200);
    CHECK(sts == 6);
    CHECK(rtg > 0);
    CHECK(reefers > 0);
    CHECK(port.liveries.size() == 6);
    CHECK(liveryTypes.size() > 1);
    // Every container carries its line's colour for the viewport and Unreal.
    for (const s::SlotOut& sl : port.slots)
        if (sl.type.starts_with("container."))
        {
            REQUIRE(sl.data.contains("liveryId"));
            CHECK(sl.data["color"] == port.liveries[size_t(sl.data["liveryId"].get<int>() - 1)]["colour"]);
        }
    const s::Layout cem = s::build(specOf("cemetery"));
    CHECK(cem.stats["mausoleums"].get<int>() > 0);
    CHECK(cem.stats["graves"].get<int>() > 0);
    const s::Layout farm = s::build(specOf("farm"));
    CHECK(farm.stats["cropRows"].get<int>() > 0);
    bool canal = false;
    for (const s::GroundOp& g : farm.ground)
        canal = canal || (g.command == "terrainAddFeature" && g.flags.value("kind", std::string()) == "canal");
    CHECK(canal);
    const s::Layout air = s::build(specOf("airstrip"));
    CHECK(std::ranges::any_of(air.ground, [](const s::GroundOp& g) { return g.command == "terrainAddAirstrip"; }));
    CHECK(std::ranges::any_of(air.slots, [](const s::SlotOut& sl) { return sl.type == "windsock"; }));
}

TEST_CASE("site kits: an unrelated edit keeps every other part's id; settings are checked")
{
    const std::vector<std::string> a = slotIds(s::build(specOf("port", {{"warehouses", 2}})));
    const std::vector<std::string> b = slotIds(s::build(specOf("port", {{"warehouses", 3}})));
    CHECK(a == b);
    const std::vector<std::string> c = slotIds(s::build(specOf("farm", {{"truckYard", false}})));
    std::vector<std::string> d = slotIds(s::build(specOf("farm")));
    std::erase_if(d, [](const std::string& id) { return id.starts_with("truckYard/"); });
    CHECK(c == d);
    // A different seed moves things; the same seed never does.
    CHECK(slotIds(s::build(s::specFromJson({{"kind", "cemetery"}, {"id", "c"}, {"seed", 1}}))) !=
          slotIds(s::build(s::specFromJson({{"kind", "cemetery"}, {"id", "c"}, {"seed", 2}}))));

    CHECK_THROWS(specOf("port", {{"runwayLength", 50000}}));
    CHECK_THROWS(specOf("port", {{"tiers", 40}}));
    CHECK_THROWS(specOf("airstrip", {{"aircraft", "jumbo"}}));
    CHECK_THROWS(s::specFromJson({{"kind", "castle"}}));
    CHECK_THROWS(s::specFromJson({{"kind", "port"}, {"nonsense", 1}}));
    const s::Spec back = s::specFromJson(specOf("village").toJson());
    CHECK(back.toJson() == specOf("village").toJson());
}

TEST_CASE("site commands: every kind on a coast terrain, slots published, envelopes, ids stable, check, undo")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain();

    // The port on the shore line, the water at -z.
    const json port = ops::execute("siteCreate", {{"kind", "port"}, {"terrain", terrain}, {"at", {0, -61000}}, {"id", "port1"}});
    CHECK(port["id"] == "port1");
    CHECK(port["waterLevelFrom"] == "terrain sea level");
    CHECK(port["ground"]["errors"].empty());
    CHECK(port["ground"]["sent"].get<int>() > 3);
    CHECK(port["envelopes"].get<int>() >= 3);
    const std::string portName = port["names"]["site"].get<std::string>();
    CHECK(!portName.empty());

    // Slots are published under the site's owner id with its region.
    const json found = ops::execute("slotFind", {{"type", "container"}, {"region", "site:port1"}, {"limit", 0}});
    const json portInfo = ops::execute("siteInfo", {{"args", {"port1"}}, {"slots", true}});
    REQUIRE(found.is_object());
    CHECK(found["count"].get<int>() == portInfo["stats"]["containers"].get<int>());
    std::vector<std::string> before;
    for (const json& sl : portInfo["slotList"])
        before.push_back(sl["id"].get<std::string>());
    CHECK(before.front().starts_with("site:port1/"));

    // An unrelated edit keeps the ids; the terrain is graded again.
    const json edited = ops::execute("siteEdit", {{"args", {"port1"}}, {"warehouses", 3}});
    CHECK(edited["ground"]["errors"].empty());
    std::vector<std::string> after;
    const json afterInfo = ops::execute("siteInfo", {{"args", {"port1"}}, {"slots", true}});
    for (const json& sl : afterInfo["slotList"])
        after.push_back(sl["id"].get<std::string>());
    CHECK(after == before);

    // Envelopes carry style tags and stable ids.
    const json envs = ops::execute("siteEnvelopes", {{"sites", {"port1"}}});
    CHECK(envs["count"].get<int>() >= 4);
    CHECK(envs["styles"].contains("portShed"));
    CHECK(envs["styles"].contains("civic"));
    for (const json& e : envs["envelopes"])
    {
        CHECK(e["id"].get<std::string>().starts_with("site:port1/"));
        CHECK(e["top"].get<double>() > e["base"].get<double>());
    }

    // Every other kind on the land, each well apart.
    const std::vector<std::pair<std::string, json>> others = {{"cruise", {120000, -61000}},  {"lighthouse", {-120000, -55000}}, {"cemetery", {-120000, 40000}},
                                                              {"airstrip", {0, 100000}},     {"farm", {130000, 100000}},       {"village", {-120000, 140000}},
                                                              {"compound", {60000, 30000}}, {"island", {0, -170000}}};
    for (const auto& [kind, at] : others)
    {
        CAPTURE(kind);
        json call = {{"kind", kind}, {"terrain", terrain}, {"at", at}, {"id", kind + "1"}};
        if (kind == "island")
            call["raiseIsland"] = true;
        const json r = ops::execute("siteCreate", call);
        CHECK(r["slots"].get<int>() > 0);
        CHECK(r["ground"]["errors"].empty());
    }
    const json list = ops::execute("siteList", json::object());
    CHECK(list.size() == 9);

    // The check: the layout above is clean but for nothing standing in water.
    const json clean = ops::execute("siteCheck", json::object());
    CAPTURE(clean.dump(1).substr(0, 4000));
    CHECK(issuesOf(clean, "realName") == 0);
    CHECK(issuesOf(clean, "overlap") == 0);
    CHECK(issuesOf(clean, "water", "port1") == 0);
    CHECK(issuesOf(clean, "shallow", "port1") == 0);
    CHECK(issuesOf(clean, "water", "cemetery1") == 0);
    CHECK(issuesOf(clean, "water", "island1") == 0);
    CHECK(issuesOf(clean, "groundStale") == 0);

    // A cemetery out at sea, a compound on top of the farm, a farm on steep hills.
    ops::execute("siteCreate", {{"kind", "cemetery"}, {"terrain", terrain}, {"at", {-150000, -150000}}, {"id", "wet"}});
    ops::execute("siteCreate", {{"kind", "compound"}, {"terrain", terrain}, {"at", {130000, 100000}}, {"id", "clash"}});
    const json hills = ops::execute("terrainCreate", {{"name", "Hills"}, {"size", {120000, 120000}}, {"resolution", 129}, {"center", {600000, 0, 0}},
                                                      {"amplitude", 30000.0}, {"featureSize", 15000.0}, {"seed", 3}});
    ops::execute("siteCreate", {{"kind", "farm"}, {"terrain", hills["mesh"]}, {"at", {600000, 0}}, {"id", "steep"}, {"ground", false}});
    const json bad = ops::execute("siteCheck", json::object());
    CHECK(issuesOf(bad, "water", "wet") > 0);
    CHECK(issuesOf(bad, "overlap", "clash") > 0);
    CHECK(issuesOf(bad, "steepGround", "steep") > 0);
    CHECK(issuesOf(bad, "groundStale", "steep") > 0);

    // A real brand as a name is refused; generated names never are.
    CHECK_THROWS(ops::execute("siteCreate", {{"kind", "farm"}, {"name", "Starbucks"}, {"terrain", ""}}));

    // Undo takes a site and its grading away again.
    auto siteStamps = [&] {
        int n = 0;
        const json stamps = ops::execute("terrainStamps", {{"args", {terrain}}});
        for (const auto& [layer, items] : stamps["layers"].items())
            for (const json& it : items)
                n += it.dump().find("site:lastone/") != std::string::npos;
        return n;
    };
    ops::execute("siteCreate", {{"kind", "compound"}, {"terrain", terrain}, {"at", {-40000, 200000}}, {"id", "lastone"}});
    CHECK(siteStamps() > 0);
    ops::execute("undo", json::object());
    CHECK(siteStamps() == 0);
    CHECK(ops::execute("siteList", json::object()).size() == 12);

    // Removing a site takes its stamps and roads off the terrain.
    ops::execute("siteRemove", {{"args", {"clash"}}});
    CHECK(ops::execute("siteList", json::object()).size() == 11);
}

TEST_CASE("site commands: a port on 32 m cells keeps its yard dry and its berths deep")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain(157);
    ops::execute("siteCreate", {{"kind", "port"}, {"terrain", terrain}, {"at", {0, -61000}}, {"id", "port1"}});
    const json c = ops::execute("siteCheck", json::object());
    CAPTURE(c["issues"].dump(1));
    CHECK(issuesOf(c, "water", "port1") == 0);
    CHECK(issuesOf(c, "shallow", "port1") == 0);
    // The quay sets the land next to its face though no sample lies within its width.
    const json stamps = ops::execute("terrainStamps", {{"args", {terrain}}});
    bool quay = false;
    for (const auto& [layer, items] : stamps["layers"].items())
        for (const json& it : items)
            if (it.value("id", std::string()) == "site:port1/quay")
                quay = it.value("applied", false);
    CHECK(quay);
    const json yard = ops::execute("terrainSample", {{"args", {terrain}}, {"points", {{0.0, -61000.0 + 2500.0}}}});
    CHECK(yard[0]["height"].get<double>() > 0.0);
}

TEST_CASE("site commands: moving or turning the site mesh moves the site and its grading in one undo step")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain();
    const json made = ops::execute("siteCreate", {{"kind", "compound"}, {"terrain", terrain}, {"at", {-40000, 100000}}, {"id", "yard"}});
    const std::string mesh = made["mesh"].get<std::string>();
    auto spec = [&] { return ops::execute("siteInfo", {{"args", {"yard"}}})["spec"]; };
    auto translate = [&] { return ops::execute("getAttr", {{"args", {mesh + ".translate"}}}); };
    auto stale = [&] { return issuesOf(ops::execute("siteCheck", {{"args", {"yard"}}}), "groundStale"); };

    ops::execute("move", {{"args", {mesh}}, {"by", {10000, 0, 5000}}});
    json s = spec();
    CHECK(s["at"][0].get<double>() == doctest::Approx(-30000));
    CHECK(s["at"][1].get<double>() == doctest::Approx(105000));
    CHECK(translate()[0].get<double>() == doctest::Approx(0));
    CHECK(stale() == 0);
    CHECK(ops::execute("undoInfo", json::object())["undo"] == "Move Site");

    ops::execute("rotate", {{"args", {mesh}}, {"by", {0, 30, 0}}});
    s = spec();
    CHECK(s["heading"].get<double>() == doctest::Approx(30));
    CHECK(s["at"][0].get<double>() == doctest::Approx(-30000));

    // One undo per drag: the turn, then the move.
    ops::execute("undo", json::object());
    CHECK(spec()["heading"].get<double>() == doctest::Approx(0));
    ops::execute("undo", json::object());
    s = spec();
    CHECK(s["at"][0].get<double>() == doctest::Approx(-40000));
    CHECK(translate()[0].get<double>() == doctest::Approx(0));
    CHECK(stale() == 0);

    // A transform set some other way is folded in on the next edit.
    ops::execute("setAttr", {{"args", {mesh + ".translateX", 2000.0}}});
    const json e = ops::execute("siteEdit", {{"args", {"yard"}}});
    CHECK(e["fromTransform"] == true);
    CHECK(e["spec"]["at"][0].get<double>() == doctest::Approx(-38000));
}

TEST_CASE("site commands: airstrips on a low flat coast clear their corridors")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain();
    // Inland, at the shore with a corridor out over the sea, near the shore, and a twin on a long turned strip.
    ops::execute("siteCreate", {{"kind", "airstrip"}, {"terrain", terrain}, {"at", {0, 100000}}, {"id", "inland"}});
    ops::execute("siteCreate", {{"kind", "airstrip"}, {"terrain", terrain}, {"at", {-150000, -57000}}, {"id", "shore"}});
    ops::execute("siteCreate", {{"kind", "airstrip"}, {"terrain", terrain}, {"at", {150000, -30000}}, {"id", "along"}});
    ops::execute("siteCreate", {{"kind", "airstrip"}, {"terrain", terrain}, {"at", {150000, 150000}}, {"heading", 30}, {"id", "twin"},
                                {"aircraft", "twin"}, {"runwayLength", 110000}});
    const json c = ops::execute("siteCheck", json::object());
    CAPTURE(c["issues"].dump(1));
    CHECK(issuesOf(c, "airClearance") == 0);
    const json w = ops::execute("waterCheck", {{"args", {terrain}}});
    for (const json& i : w["issues"])
        CHECK(i.value("kind", std::string()) != "airClearance");
}

TEST_CASE("site commands: an opened scene's imported real names reach its sites' layouts before any site command runs")
{
    loadSitePackages();
    ops::execute("newScene", json::object());
    const std::string terrain = coastTerrain();
    const json port = ops::execute("siteCreate", {{"kind", "port"}, {"terrain", terrain}, {"at", {0, -61000}}, {"id", "port1"}});
    const std::string mesh = port["mesh"].get<std::string>();
    auto firstLine = [&] {
        ops::execute("meshInfo", {{"args", {mesh}}});
        const json ships = ops::execute("slotFind", {{"type", "ship.cargo"}, {"region", "site:port1"}, {"limit", 0}, {"full", true}});
        REQUIRE(ships["count"].get<int>() > 0);
        std::string line;
        for (const json& sl : ships["slots"])
            if (sl["payload"]["data"].value("berth", 0) == 1)
                line = sl["payload"]["data"].value("line", std::string());
        return line;
    };
    const std::string line = firstLine();
    REQUIRE_FALSE(line.empty());
    // An import that recorded that line's name as a real one.
    const std::string imported = ops::execute("polyCube", {{"name", "Imported"}}).at(0).get<std::string>();
    ops::execute("addAttr", {{"args", {imported}}, {"longName", "sourceNames"}, {"dataType", "string"}});
    ops::execute("setAttr", {{"args", {imported + ".sourceNames", json{{"operator", line}}.dump()}}});
    const auto file = std::filesystem::temp_directory_path() / "lila_sites_avoid.lila";
    ops::execute("saveScene", {{"args", {file.generic_string()}}});

    // Another scene's site command leaves the process with no real names to avoid.
    ops::execute("newScene", json::object());
    ops::execute("siteCreate", {{"kind", "farm"}, {"terrain", ""}, {"id", "elsewhere"}});
    ops::execute("openScene", {{"args", {file.generic_string()}}});
    CHECK(firstLine() != line);
    std::filesystem::remove(file);
}
TEST_CASE("site kits: villages find every house a spot along their streets")
{
    for (const auto& [id, houses, radius] : {std::tuple{"habal", 10, 12000.0}, std::tuple{"noria", 12, 14000.0}, std::tuple{"quelite", 12, 14000.0},
                                             std::tuple{"v4", 16, 9000.0}, std::tuple{"v5", 20, 14000.0}})
        for (int seed : {0, 1, 2})
        {
            CAPTURE(id);
            CAPTURE(seed);
            const s::Layout L = s::build(s::specFromJson({{"kind", "village"}, {"id", id}, {"seed", seed}, {"params", {{"houses", houses}, {"villageRadius", radius}}}}));
            CHECK_MESSAGE(L.notes.empty(), (L.notes.empty() ? std::string() : L.notes[0]));
            CHECK(L.stats["houses"].get<int>() == houses);
        }
}
