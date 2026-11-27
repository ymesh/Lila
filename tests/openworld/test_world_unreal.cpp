// worldBuild's Unreal export against a stand-in LilaLink that keeps a level's placement groups: the first build sends
// everything, a rebuild with a changed slot rule sends only what changed and imports nothing, and a rebuild without one
// of the sites removes that site's groups and their slot table rows, leaving no group the build did not send.

#include "World.h"

#include "../link/FakeServer.h"

#include "core/PrefStore.h"
#include "extensions/ExtensionManager.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "OpenWorldTest.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
using owtest::failedSteps;
using owtest::run;
/** A level as LilaLink keeps it: placement groups with their signature and actor count, the landscape's hash, imports. */
struct FakeLevel
{
    std::mutex m;
    std::map<std::string, json> groups;
    std::map<std::string, std::string> slotRows; // slot id -> group
    std::map<std::string, std::string> slotTypes; // slot id -> type
    std::string landscapeHash;
    int imports = 0;
    std::set<std::string> cells;      // "<settlement key>/<cell>"
    std::set<std::string> zoneGraphs; // settlement keys holding lanes
    std::set<std::string> splines;    // spline ids
    std::map<std::string, json> splineParams; // spline id -> the last setSpline params
    std::vector<json> massingImports;         // importAsset params of massing cells
    std::map<std::string, std::string> zoneSets; // id -> label
    std::set<std::string> zoneDataDeleted;      // labels whose ZD_ asset went
    std::string guid = "11111111-1111-1111-1111-111111111111"; // the level package's persistent GUID

    /** The level deleted and made again under the same path: empty, with a new GUID. */
    void recreate()
    {
        std::scoped_lock lock(m);
        groups.clear();
        slotRows.clear();
        slotTypes.clear();
        landscapeHash.clear();
        cells.clear();
        zoneGraphs.clear();
        splines.clear();
        splineParams.clear();
        zoneSets.clear();
        guid = "22222222-2222-2222-2222-222222222222";
    }

    std::optional<json> handle(const json& req)
    {
        std::scoped_lock lock(m);
        const std::string method = req["method"];
        const json p = req.value("params", json::object());
        if (method == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"},
                                           {"features", {"progress", "placements", "placementHashes", "levelInfo", "landscape", "zones", "buildings",
                                                         "buildingInteriors", "slots", "splines", "settlement", "settlementCells"}}});
        if (method == "levelInfo")
            return FakeServer::reply(req, {{"level", "/Game/Test/World"}, {"guid", guid}, {"partitioned", false}});
        if (method == "setLandscape")
        {
            const bool same = !landscapeHash.empty() && p.value("hash", std::string()) == landscapeHash;
            landscapeHash = p.value("hash", std::string());
            return FakeServer::reply(req, {{"actor", p.value("label", std::string())}, {"created", false}, {"unchanged", same}, {"layers", json::array()}});
        }
        if (method == "importAsset")
        {
            ++imports;
            if (p.value("contentPath", std::string()).find("_Massing_") != std::string::npos)
                massingImports.push_back(p);
            return FakeServer::reply(req, {{"assets", {p["contentPath"].get<std::string>() + ".Imported"}}, {"warnings", json::array()}, {"errors", json::array()}});
        }
        if (method == "placeInstances")
        {
            const bool holder = (p["mode"] == "ism" || p["mode"] == "hism") && !p.contains("actorClass");
            groups[p["group"].get<std::string>()] = {{"hash", p.value("hash", std::string())}, {"actors", holder ? size_t(1) : p["placements"].size()}};
            return FakeServer::reply(req, {{"spawned", p["placements"].size()}, {"updated", 0}, {"removed", 0}});
        }
        if (method == "placementHashes")
            return FakeServer::reply(req, {{"groups", json(groups)}});
        if (method == "removePlacements")
        {
            std::set<std::string> keep;
            for (const json& k : p.value("keep", json::array()))
                keep.insert(k.get<std::string>());
            int removed = 0;
            for (auto it = groups.begin(); it != groups.end();)
            {
                bool gone = false;
                for (const json& k : p.value("groups", json::array()))
                    gone = gone || k == it->first;
                for (const json& pre : p.value("prefixes", json::array()))
                    gone = gone || (it->first.starts_with(pre.get<std::string>()) && !keep.contains(it->first));
                if (gone)
                {
                    ++removed;
                    it = groups.erase(it);
                }
                else
                    ++it;
            }
            return FakeServer::reply(req, {{"removed", removed}});
        }
        if (method == "writeSlotTable")
        {
            for (const json& src : p.value("sources", json::array()))
                std::erase_if(slotRows, [&](const auto& r) { return r.second.starts_with(src.get<std::string>()); });
            for (const json& row : p.value("rows", json::array()))
            {
                slotRows[row.value("id", std::string())] = row.value("group", std::string());
                slotTypes[row.value("id", std::string())] = row.value("type", std::string());
            }
            return FakeServer::reply(req, {{"rows", slotRows.size()}});
        }
        if (method == "setZones")
        {
            zoneSets[p["id"].get<std::string>()] = p.value("label", std::string());
            return FakeServer::reply(req, {{"volumes", 1}, {"created", 0}, {"updated", 0}, {"unchanged", 1}, {"removed", 0}});
        }
        if (method == "removeZones")
        {
            const bool had = zoneSets.erase(p["id"].get<std::string>()) > 0;
            if (had && !p.value("label", std::string()).empty())
                zoneDataDeleted.insert(p["label"].get<std::string>());
            return FakeServer::reply(req, {{"removed", had ? 1 : 0}, {"dataLayers", 0}, {"zoneData", had}});
        }
        if (method == "setSettlementCell")
        {
            cells.insert(p["id"].get<std::string>() + "/" + p["cell"].get<std::string>());
            return FakeServer::reply(req, {{"asset", p["asset"]}, {"spawned", true}});
        }
        if (method == "removeSettlementCells")
        {
            const std::string pre = p["id"].get<std::string>() + "/";
            int removed = 0;
            for (auto it = cells.begin(); it != cells.end();)
            {
                bool gone = it->starts_with(pre);
                if (gone && p.contains("cells"))
                    gone = std::find(p["cells"].begin(), p["cells"].end(), json(it->substr(pre.size()))) != p["cells"].end();
                if (gone)
                    ++removed, it = cells.erase(it);
                else
                    ++it;
            }
            return FakeServer::reply(req, {{"removed", removed}});
        }
        if (method == "setZoneGraph")
        {
            zoneGraphs.insert(p["id"].get<std::string>());
            return FakeServer::reply(req, {{"actor", "Lanes"}, {"shapes", 1}, {"profiles", 1}, {"warnings", json::array()}});
        }
        if (method == "removeSettlement")
        {
            // The last settlement in the project's levels on a lane profile takes it out of DefaultPlugins.ini.
            const bool had = zoneGraphs.erase(p["id"].get<std::string>()) > 0;
            return FakeServer::reply(req, {{"removed", had},
                                           {"laneProfilesRemoved", had && zoneGraphs.empty() ? json::array({"Lila_Street"}) : json::array()},
                                           {"configBackup", "Saved/LilaLink/ConfigBackups/DefaultPlugins-1.ini"}});
        }
        if (method == "setSpline")
        {
            splines.insert(p["id"].get<std::string>());
            splineParams[p["id"].get<std::string>()] = p;
            return FakeServer::reply(req, {{"actor", p.value("label", std::string())}});
        }
        if (method == "removeSpline")
        {
            // A road paint layer Lila added that someone painted by hand stays, with a warning.
            const std::string id = p["id"].get<std::string>();
            const bool had = splines.erase(id) > 0;
            const bool roads = had && id.ends_with("/roads");
            return FakeServer::reply(req, {{"removed", had},
                                           {"paintLayersRemoved", roads ? json::array({"roadShoulder"}) : json::array()},
                                           {"warnings", roads ? json::array({"paint layer roadAsphalt, added for Lila's roads, has paint of its own, so it stays on the landscape"})
                                                              : json::array()}});
        }
        if (method == "deleteAssets")
            return FakeServer::reply(req, {{"deleted", 0}, {"missing", p["paths"]}});
        return FakeServer::reply(req, json::object());
    }
};

/** Buildings modifiers in the scene: a rebuild must not leave the replaced build's behind (they keep publishing slots). */
size_t buildingModifiers()
{
    lila::scene::Graph& g = lila::scene::Graph::current();
    size_t n = 0;
    for (lila::scene::NodeId id : g.nodes())
        if (const lila::scene::Node* node = g.node(id); node && node->name().starts_with("LilaBuildingsModifier"))
            ++n;
    return n;
}

json stepResult(const json& build, const std::string& name)
{
    for (const json& s : build["steps"])
        if (s["step"] == name)
            return s;
    return json();
}

json spec(bool withCemetery, const std::string& ruleType)
{
    json s = json::parse(R"({
        "name": "LinkTiny",
        "culture": "mexican",
        "terrain": {"size": [200000, 200000], "resolution": 65, "seed": 5, "source": {"type": "noise", "amplitude": 1200, "featureSize": 90000}},
        "sites": [{"kind": "village", "id": "v1", "at": [40000, 40000], "params": {"houses": 4, "villageRadius": 9000}},
                  {"kind": "cemetery", "id": "c1", "at": [-50000, -40000], "params": {"cemeteryWidth": 9000, "cemeteryDepth": 7000}}],
        "slots": {"rules": []},
        "zones": {"spec": {"layers": [{"id": "police", "type": "police", "zones": [{"id": "all", "name": "Comandancia Norte",
                    "polygons": [[[-100000, -100000], [100000, -100000], [100000, 100000], [-100000, 100000]]]}]}]}},
        "check": false,
        "export": {"target": "unreal", "map": false}
    })");
    if (!withCemetery)
        s["sites"].erase(1);
    if (!ruleType.empty())
        s["slots"]["rules"].push_back({{"id", "swap"}, {"name", "Swap"}, {"type", ruleType}, {"payload", {{"unrealMesh", "/Engine/BasicShapes/Cube.Cube"}}}});
    return s;
}
} // namespace

TEST_CASE("world: Unreal export sends a rebuild's changes only and removes what the world no longer makes")
{
    owtest::loadPackages(true, true);
    if (!lila::ops::find("unrealPlaceInstances"))
        lila::link::registerCommands();
    lila::link::registerExporter("fbx:instanceSource", [](const std::vector<lila::scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    run("newScene");
    const fs::path cache = fs::temp_directory_path() / "lila_world_unreal";
    fs::remove_all(cache);
    lila::PrefStore::get().set("unreal", "/cacheDir", cache.generic_string());
    FakeLevel level;
    FakeServer server([&](const json& req) { return level.handle(req); });
    lila::link::Link::ConnectOptions o;
    o.port = server.port();
    lila::link::Link::get().connect(o);

    // First build: every group goes, all keyed by the world's link id prefix.
    const json r1 = run("worldBuild", {{"spec", spec(true, "")}, {"folder", (cache / "w1").generic_string()}});
    INFO(failedSteps(r1));
    REQUIRE(r1.value("failed", json()).is_null());
    const std::string prefix = r1["exports"]["unreal"]["prefix"];
    CHECK(prefix.size() == 9);
    for (const char* step : {"export.unreal.link", "export.unreal.terrain", "export.unreal.sites", "export.unreal.buildings", "export.unreal.zones", "export.unreal.stale"})
        CHECK_MESSAGE(stepResult(r1, step).value("ok", false), step);
    const int imports1 = level.imports;
    CHECK(imports1 > 0);
    size_t first;
    {
        std::scoped_lock lock(level.m);
        first = level.groups.size();
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }
    CHECK(first > 5);
    const size_t modifiers = buildingModifiers();
    CHECK(modifiers > 0);
    CHECK(stepResult(r1, "export.unreal.stale")["result"]["stale"] == 0);
    // Not a World Partition level: the step says why the proxies stayed home.
    CHECK(stepResult(r1, "export.unreal.buildings")["result"].contains("proxiesSkipped"));

    // Rebuilt with a slot rule swapping the commonest slot type for a project mesh: nothing imported, the landscape left
    // alone, that type's groups sent, every other group held unchanged.
    std::string common;
    {
        std::scoped_lock lock(level.m);
        std::map<std::string, int> byType;
        for (const auto& [id, type] : level.slotTypes)
            ++byType[type];
        int most = 0;
        for (const auto& [type, n] : byType)
            if (n > most)
                most = n, common = type;
    }
    REQUIRE_FALSE(common.empty());
    const json r2 = run("worldBuild", {{"spec", spec(true, common)}, {"replace", true}, {"folder", (cache / "w2").generic_string()}});
    INFO(failedSteps(r2));
    REQUIRE(r2.value("failed", json()).is_null());
    CHECK(level.imports == imports1);
    CHECK(buildingModifiers() == modifiers);
    CHECK(stepResult(r2, "export.unreal.terrain")["result"]["unchanged"] == true);
    const json sites2 = stepResult(r2, "export.unreal.sites")["result"];
    const json shells2 = stepResult(r2, "export.unreal.buildings")["result"]["shellsAndModules"];
    INFO(sites2.dump() << " " << shells2.dump());
    CHECK(sites2["unchanged"].get<int>() + shells2["unchanged"].get<int>() > 0);
    CHECK(sites2["sent"].get<int>() + shells2["sent"].get<int>() > 0);
    CHECK(sites2["imported"] == 0);
    CHECK(shells2["imported"] == 0);
    {
        std::scoped_lock lock(level.m);
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }

    // Rebuilt without the cemetery: its groups and slot rows leave the level; nothing else is imported again.
    size_t rowsBefore, groupsBefore;
    {
        std::scoped_lock lock(level.m);
        rowsBefore = level.slotRows.size();
        groupsBefore = level.groups.size();
    }
    const json r3 = run("worldBuild", {{"spec", spec(false, common)}, {"replace", true}, {"folder", (cache / "w3").generic_string()}});
    INFO(failedSteps(r3));
    REQUIRE(r3.value("failed", json()).is_null());
    const json stale = stepResult(r3, "export.unreal.stale")["result"];
    CHECK(stale["stale"].get<int>() > 0);
    CHECK(stale.contains("slotTable"));
    CHECK(level.imports == imports1);
    {
        std::scoped_lock lock(level.m);
        CHECK(level.slotRows.size() < rowsBefore);
        CHECK(level.groups.size() < groupsBefore);
        CHECK(level.groups.size() <= stale["sentThisBuild"].get<size_t>());
        for (const auto& [k, v] : level.groups)
            CHECK_MESSAGE(k.starts_with(prefix), k);
    }

    lila::link::unregisterExporter("fbx:instanceSource");
    lila::link::Link::get().disconnect();
    fs::remove_all(cache);
}

TEST_CASE("world: a settlement or the zone set taken out of the spec leaves the Unreal level whole")
{
    owtest::loadPackages(true, true);
    if (!lila::ops::find("unrealPlaceInstances"))
        lila::link::registerCommands();
    lila::link::registerExporter("fbx:instanceSource", [](const std::vector<lila::scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    run("newScene");
    const fs::path cache = fs::temp_directory_path() / "lila_world_unreal_drop";
    fs::remove_all(cache);
    lila::PrefStore::get().set("unreal", "/cacheDir", cache.generic_string());
    FakeLevel level;
    FakeServer server([&](const json& req) { return level.handle(req); });
    lila::link::Link::ConnectOptions o;
    o.port = server.port();
    lila::link::Link::get().connect(o);

    json s = spec(false, "");
    s["settlements"] = json::parse(R"([{"name": "Pueblo", "create": {"profile": "usTown", "seed": 2,
        "outline": [[-30000, -30000], [10000, -30000], [10000, 10000], [-30000, 10000]]}}])");
    const json r1 = run("worldBuild", {{"spec", s}, {"folder", (cache / "w1").generic_string()}});
    INFO(failedSteps(r1));
    REQUIRE(r1.value("failed", json()).is_null());
    std::string key;
    {
        std::scoped_lock lock(level.m);
        REQUIRE(level.zoneGraphs.size() == 1);
        key = *level.zoneGraphs.begin();
        CHECK_FALSE(level.cells.empty());
        CHECK(level.zoneSets.size() == 1);
        CHECK(std::any_of(level.splines.begin(), level.splines.end(), [&](const std::string& id) { return id.starts_with(key + "/"); }));
        // Massing cells import with managed materials, so no Default material asset is made beside them.
        REQUIRE_FALSE(level.massingImports.empty());
        for (const json& imp : level.massingImports)
            CHECK((imp.contains("materials") && imp["materials"].is_array()));
        // The world's landscape draws in terrain material layers, so landscape roads paint its Road layer over the carriageway
        // and shape the landscape; no shoulder paint (the material has no verge layer).
        REQUIRE(level.splineParams.count(key + "/roads") == 1);
        const json& roads = level.splineParams[key + "/roads"];
        CHECK(roads["mode"] == "landscape");
        int surface = 0, verge = 0;
        for (const json& sp : roads["splines"])
        {
            if (sp.value("layer", std::string()) == "Road")
            {
                ++surface;
                CHECK(sp["deform"] == true);
                CHECK(sp["layerWidth"].get<double>() > 0.0);
                CHECK(sp["layerWidth"].get<double>() <= 1.0);
            }
            else if (sp.value("layer", std::string()) == "roadVerge")
                ++verge;
        }
        CHECK(surface > 0);
        CHECK(verge == 0);
        CHECK(stepResult(r1, "export.unreal.settlement.Pueblo")["result"].value("roadLayer", std::string()).starts_with("Road: "));
    }
    // Rebuilt unchanged: nothing dropped.
    const json r2 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w2").generic_string()}});
    INFO(failedSteps(r2));
    REQUIRE(r2.value("failed", json()).is_null());
    const json kept = stepResult(r2, "export.unreal.dropped")["result"];
    CHECK(kept["settlements"].empty());
    CHECK(kept["zoneSets"].empty());

    // Rebuilt without the settlement and without zones: every cell, lane, spline and placement of the settlement goes,
    // and the zone set with its zone data.
    s.erase("settlements");
    s.erase("zones");
    const json r3 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w3").generic_string()}});
    INFO(failedSteps(r3));
    REQUIRE(r3.value("failed", json()).is_null());
    const json dropped = stepResult(r3, "export.unreal.dropped")["result"];
    INFO(dropped.dump());
    REQUIRE(dropped["settlements"].size() == 1);
    CHECK(dropped["settlements"][0]["key"] == key);
    CHECK_FALSE(dropped["settlements"][0].contains("error"));
    // The lane profiles it took out of the project ini, and where the ini as it was went, reach the build report.
    CHECK(dropped["laneProfilesRemoved"] == json::array({"Lila_Street"}));
    CHECK(dropped["configBackups"].size() == 1);
    // So do the road paint layers that went with the roads, and the warning for one kept because it was painted by hand.
    CHECK(dropped["paintLayersRemoved"] == json::array({"roadShoulder"}));
    REQUIRE(dropped["warnings"].size() == 1);
    CHECK(dropped["warnings"][0].get<std::string>().starts_with(key + ": paint layer roadAsphalt"));
    REQUIRE(dropped["zoneSets"].size() == 1);
    {
        std::scoped_lock lock(level.m);
        CHECK(level.zoneGraphs.empty());
        CHECK(level.cells.empty());
        CHECK(std::none_of(level.splines.begin(), level.splines.end(), [&](const std::string& id) { return id.starts_with(key + "/"); }));
        CHECK(std::none_of(level.groups.begin(), level.groups.end(), [&](const auto& g) { return g.first.starts_with(key + "/"); }));
        CHECK(level.zoneSets.empty());
        CHECK(level.zoneDataDeleted.size() == 1);
    }
    // The record no longer lists them, so the next rebuild has nothing to drop.
    const json r4 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w4").generic_string()}});
    REQUIRE(r4.value("failed", json()).is_null());
    CHECK(stepResult(r4, "export.unreal.dropped")["result"]["settlements"].empty());
    CHECK(stepResult(r4, "export.unreal.dropped")["result"]["zoneSets"].empty());

    lila::link::unregisterExporter("fbx:instanceSource");
    lila::link::Link::get().disconnect();
    fs::remove_all(cache);
}

TEST_CASE("world: a level deleted and made again under the same path gets everything again")
{
    owtest::loadPackages(true, true);
    if (!lila::ops::find("unrealPlaceInstances"))
        lila::link::registerCommands();
    lila::link::registerExporter("fbx:instanceSource", [](const std::vector<lila::scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    run("newScene");
    const fs::path cache = fs::temp_directory_path() / "lila_world_unreal_recreate";
    fs::remove_all(cache);
    lila::PrefStore::get().set("unreal", "/cacheDir", cache.generic_string());
    FakeLevel level;
    FakeServer server([&](const json& req) { return level.handle(req); });
    lila::link::Link::ConnectOptions o;
    o.port = server.port();
    lila::link::Link::get().connect(o);

    json s = spec(false, "");
    s["settlements"] = json::parse(R"([{"name": "Pueblo", "create": {"profile": "usTown", "seed": 2,
        "outline": [[-30000, -30000], [10000, -30000], [10000, 10000], [-30000, 10000]]}}])");
    const json r1 = run("worldBuild", {{"spec", s}, {"folder", (cache / "w1").generic_string()}});
    INFO(failedSteps(r1));
    REQUIRE(r1.value("failed", json()).is_null());
    size_t groups, cells, splines;
    std::string key;
    {
        std::scoped_lock lock(level.m);
        groups = level.groups.size();
        cells = level.cells.size();
        splines = level.splines.size();
        REQUIRE(level.zoneGraphs.size() == 1);
        key = *level.zoneGraphs.begin();
    }
    REQUIRE(groups > 0);
    REQUIRE(cells > 0);
    REQUIRE(splines > 0);

    // The same level again: the settlement's record holds, nothing of it is sent.
    const json r2 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w2").generic_string()}});
    INFO(failedSteps(r2));
    REQUIRE(r2.value("failed", json()).is_null());
    const json same = stepResult(r2, "export.unreal.settlement.Pueblo")["result"];
    CHECK_FALSE(same.contains("resentAll"));
    CHECK(same["added"] == 0);
    CHECK(same["changed"] == 0);
    CHECK_FALSE(stepResult(r2, "export.unreal.dropped")["result"].contains("recordIgnored"));

    // Deleted and made again under the same path: every group, cell, spline, lane and zone set arrives again.
    level.recreate();
    const json r3 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w3").generic_string()}});
    INFO(failedSteps(r3));
    REQUIRE(r3.value("failed", json()).is_null());
    const json again = stepResult(r3, "export.unreal.settlement.Pueblo")["result"];
    INFO(again.dump());
    CHECK(again.value("resentAll", std::string()).find("deleted and made again") != std::string::npos);
    CHECK(again["unchanged"] == 0);
    CHECK(again["removed"] == 0);
    CHECK(stepResult(r3, "export.unreal.dropped")["result"].contains("recordIgnored"));
    CHECK(stepResult(r3, "export.unreal.terrain")["result"]["unchanged"] == false);
    {
        std::scoped_lock lock(level.m);
        CHECK(level.groups.size() == groups);
        CHECK(level.cells.size() == cells);
        CHECK(level.splines.size() == splines);
        CHECK(level.zoneGraphs.count(key) == 1);
        CHECK(level.zoneSets.size() == 1);
    }

    // The record now names the new level, so the next build trusts it again.
    const json r4 = run("worldBuild", {{"spec", s}, {"replace", true}, {"folder", (cache / "w4").generic_string()}});
    REQUIRE(r4.value("failed", json()).is_null());
    CHECK_FALSE(stepResult(r4, "export.unreal.settlement.Pueblo")["result"].contains("resentAll"));
    CHECK(stepResult(r4, "export.unreal.settlement.Pueblo")["result"]["added"] == 0);

    lila::link::unregisterExporter("fbx:instanceSource");
    lila::link::Link::get().disconnect();
    fs::remove_all(cache);
}
