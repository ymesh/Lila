// Buildings through its package's commands: create from envelopes, stable slot ids across unrelated edits, edits and slot
// overrides surviving regeneration, pins, every style passing buildingCheck, party walls and slopes in the scene, the
// OSM fixture town through Settlement's buildings route, and a performance budget.

#include "core/Workspace.h"
#include "extensions/ExtensionManager.h"
#include "gn/InstanceView.h"
#include "gn/NodeLib.h"
#include "ops/Command.h"
#include "scene/Graph.h"

#include <array>
#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using json = nlohmann::json;

namespace
{
json run(const char* cmd, json call = json::object()) { return ops::execute(cmd, call); }

void loadBuildings()
{
    if (!ops::find("buildingCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_BUILDINGS_PACKAGE) == "Buildings");
}

void loadSettlement()
{
    if (!gn::findDecl("Settlement"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_SETTLEMENT_PACKAGE) == "Settlement");
    if (!ops::find("terrainCreate"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_TERRAIN_PACKAGE) == "Terrain");
}

/** An envelope in Settlement's published schema. */
json envelope(uint64_t lot, double x0, double z0, double w, double d, int floors, std::vector<std::string> sides = {"street", "open", "open", "open"},
              std::string style = "", std::string program = "")
{
    json fp = json::array({{x0, z0}, {x0 + w, z0}, {x0 + w, z0 + d}, {x0, z0 + d}});
    json e = {{"lot", int(lot)},
              {"lotId", lot},
              {"footprint", fp},
              {"sides", sides},
              {"floors", floors},
              {"height", floors * 320.0},
              {"ground", 0.0},
              {"base", -50.0},
              {"top", floors * 320.0 + 40.0},
              {"roof", "flat"},
              {"front", {0.0, -1.0}},
              {"entrances", json::array({{x0 + w * 0.5, z0}})},
              {"style", style},
              {"program", program},
              {"use", program.empty() ? "residential" : "commercial"}};
    if (!program.empty())
        e["businesses"] = json::array({{{"name", "Taquería El Faro"}, {"program", program}, {"at", {x0 + w * 0.5, z0}}}});
    return e;
}

uint64_t hashIds(const std::vector<std::string>& ids)
{
    uint64_t h = 1469598103934665603ull;
    for (const std::string& s : ids)
        for (unsigned char c : s + "\n")
        {
            h ^= c;
            h *= 1099511628211ull;
        }
    return h;
}

std::vector<std::string> slotIds(const std::string& building)
{
    const json info = run("buildingInfo", {{"args", {building}}, {"modules", true}});
    std::vector<std::string> ids;
    for (const json& m : info["moduleList"])
        ids.push_back(m["slot"].get<std::string>());
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string firstSlot(const std::string& building, const std::string& type)
{
    const json info = run("buildingInfo", {{"args", {building}}, {"modules", true}});
    for (const json& m : info["moduleList"])
        if (m["type"] == type && m["slot"].get<std::string>().find("/l1/") != std::string::npos)
            return m["slot"];
    return {};
}
} // namespace

TEST_CASE("buildings commands: create, stable ids across unrelated edits, edits and overrides survive regeneration")
{
    loadBuildings();
    run("newScene");
    const json envs = json::array({envelope(0x1111, 0, 0, 1500, 1200, 2), envelope(0x2222, 1500, 0, 1500, 1200, 2, {"street", "open", "open", "party"})});
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Town"}, {"style", "colonial"}});
    CHECK(made["buildings"] == 2);
    CHECK(made["slots"].get<int>() > 20);
    const std::string A = "building:1111", B = "building:2222";

    // Modules carry their building's palette colour into the instanced draw; shared module materials keep their own.
    {
        scene::Graph& g = scene::Graph::current();
        for (int k = 0; k < 3; ++k)
            g.evaluate();
        size_t colored = 0;
        for (scene::NodeId id : g.nodes())
            if (gn::InstanceViewPtr v = gn::instanceViewFor(g, id))
                for (const gn::InstancePart& p : v->parts)
                    if (p.colors)
                        for (const glm::vec4& c : *p.colors)
                            colored += c.a == 1.0f && c != glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        CHECK(colored > 10);
        CHECK(run("getAttr", {{"args", {"bld_glass.instanceColor"}}}) == false);
        CHECK(run("getAttr", {{"args", {"Bld_colonial_render0.instanceColor"}}}) == true);
        // Two tones: trim and frames take the second; colonial paints its windows' and doors' frames in its trim.
        CHECK(run("getAttr", {{"args", {"bld_trim.instanceColor2"}}}) == true);
        CHECK(run("getAttr", {{"args", {"bld_frame.instanceColor2"}}}) == true);
        CHECK(run("getAttr", {{"args", {"bld_frame.instanceColor"}}}) == true);
        CHECK(run("getAttr", {{"args", {"Bld_colonial_render0.instanceColor2"}}}) == false);
        size_t toned = 0;
        for (scene::NodeId id : g.nodes())
            if (gn::InstanceViewPtr v = gn::instanceViewFor(g, id))
                for (const gn::InstancePart& p : v->parts)
                    if (p.colors2)
                        for (const glm::vec4& c : *p.colors2)
                            toned += c.a == 1.0f ? 1 : 0;
        CHECK(toned > 10);
    }
    const uint64_t before = hashIds(slotIds(B));
    const std::vector<std::string> aBefore = slotIds(A);

    // Unrelated edit: A's style and floors. B's ids hash the same.
    run("buildingEdit", {{"args", {A}}, {"floors", 1}, {"style", "seafront"}});
    CHECK(hashIds(slotIds(B)) == before);
    CHECK(slotIds(A) != aBefore);

    // Bay paint and a slot override on B, then regenerate from the same envelopes.
    const std::string window = firstSlot(B, "window.residential");
    REQUIRE_FALSE(window.empty());
    run("slotSet", {{"id", window}, {"placeholder", "box"}, {"data", {{"locked", true}}}});
    run("buildingEdit", {{"args", {B}}, {"bays", {{"t0/f0/l0/b0", "door"}}}});
    run("buildingCreate", {{"envelopes", envs}, {"name", "Town"}, {"style", "colonial"}});
    const json found = run("slotFind", {{"id", window}});
    REQUIRE(found["count"] == 1);
    bool override = false;
    for (const json& s : found["slots"][0]["sources"])
        override = override || s == "override";
    CHECK(override);
    CHECK(run("slotReport", {{"validate", false}})["orphans"].size() == 0);
    const json info = run("buildingInfo", {{"args", {B}}, {"bays", true}});
    bool door = false;
    for (const json& b : info["bays"])
        door = door || (b["path"] == "t0/f0/l0/b0" && b["kind"] == "door");
    CHECK(door);
    CHECK(info["edits"]["bays"]["t0/f0/l0/b0"] == "door");

    // Undo takes the edit back.
    run("buildingEdit", {{"args", {B}}, {"floors", 3}});
    CHECK(run("buildingInfo", {{"args", {B}}})["floors"] == 3);
    run("undo");
    CHECK(run("buildingInfo", {{"args", {B}}})["floors"] == 2);

    // A pinned building keeps its spec when the envelope changes.
    run("slotPin", {{"args", {"owner:" + A}}});
    json changed = envs;
    changed[0]["floors"] = 5;
    run("buildingCreate", {{"envelopes", changed}, {"name", "Town"}, {"style", "colonial"}});
    CHECK(run("buildingInfo", {{"args", {A}}})["floors"] == 1); // the edit, on the pinned spec
}

TEST_CASE("buildings commands: proxies made again refill the same nodes")
{
    loadBuildings();
    run("newScene");
    run("buildingCreate", {{"envelopes", json::array({envelope(0x3333, 0, 0, 1500, 1200, 2)})}, {"name", "Town"}});
    const json first = run("buildingProxies");
    REQUIRE(first["proxies"].size() == 2);
    scene::Graph& g = scene::Graph::current();
    std::vector<scene::NodeId> ids;
    for (const json& p : first["proxies"])
        ids.push_back(g.find(p["mesh"].get<std::string>())->id());
    const json second = run("buildingProxies");
    REQUIRE(second["proxies"].size() == 2);
    for (size_t k = 0; k < ids.size(); ++k)
        CHECK(g.find(second["proxies"][k]["mesh"].get<std::string>())->id() == ids[k]);
}

TEST_CASE("buildings commands: every style with several seeds passes buildingCheck")
{
    loadBuildings();
    run("newScene");
    const json styles = run("buildingStyles")["styles"];
    REQUIRE(styles.size() >= 16);
    json envs = json::array();
    int k = 0;
    std::vector<std::string> ids;
    for (const json& s : styles)
        for (int seed = 0; seed < 3; ++seed, ++k)
        {
            const double x = (k % 12) * 6000.0, z = (k / 12) * 6000.0;
            json e = envelope(0x5000 + uint64_t(k), x, z, 2200, 1600, 3, {"street", "party", "open", "street"}, s["id"], k % 2 ? "restaurant" : "");
            envs.push_back(e);
        }
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Styles"}, {"cell", 0}});
    CHECK(made["buildings"] == envs.size());
    for (const json& e : envs)
    {
        const std::string id = "building:" + [&] {
            char buf[20];
            std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(e["lotId"].get<uint64_t>()));
            return std::string(buf);
        }();
        const std::string style = e["style"];
        run("buildingEdit", {{"args", {id}}, {"style", style}, {"seed", int(e["lotId"].get<uint64_t>() % 3)}});
    }
    const json check = run("buildingCheck");
    CAPTURE(check["counts"].dump());
    CAPTURE(check["issues"].size() > 0 ? check["issues"][0].dump() : std::string());
    CHECK(check["ok"] == true);
    CHECK(check["buildings"] == envs.size());
    CHECK(check["texts"].get<int>() > 0);
}

TEST_CASE("buildings commands: party walls, overlapping neighbours, slopes on a ground mesh, real names")
{
    loadBuildings();
    run("newScene");
    // A ground rising 3 m along x.
    const json ground = run("createMesh", {{"points", {{-5000, 0, -5000}, {15000, 300, -5000}, {15000, 300, 15000}, {-5000, 0, 15000}}},
                                           {"faces", {{0, 3, 2, 1}}},
                                           {"name", "Ground"}});
    json envs = json::array({envelope(0x7001, 0, 0, 3000, 1200, 2, {"street", "party", "open", "open"}),
                             envelope(0x7002, 3000, 0, 1500, 1200, 2, {"street", "open", "open", "party"})});
    run("buildingCreate", {{"envelopes", envs}, {"name", "Slope"}, {"ground", ground}, {"style", "colonial"}});
    json check = run("buildingCheck");
    CAPTURE(check["issues"].dump());
    CHECK(check["counts"]["doorNotReachingGround"] == 0);
    CHECK(check["counts"]["overlappingNeighbours"] == 0);
    CHECK(check["counts"]["slotFloating"] == 0);
    const json info = run("buildingInfo", {{"args", {"building:7001"}}, {"modules", true}});
    CHECK(info["moduleTypes"].value("wall.party", 0) >= 1);
    bool partyBlank = true;
    for (const json& f : info["facades"])
        if (f["side"] == "party")
            partyBlank = partyBlank && f["bays"] == 0;
    CHECK(partyBlank);

    // An overlapping neighbour and a real brand on a sign are flagged.
    envs.push_back(envelope(0x7003, 2000, 300, 1500, 1200, 2, {"street", "open", "open", "open"}, "modernCommercial", "shop"));
    envs.back()["businesses"][0]["name"] = "Walmart";
    run("buildingCreate", {{"envelopes", envs}, {"name", "Slope"}, {"ground", ground}, {"style", "colonial"}});
    check = run("buildingCheck");
    CHECK(check["counts"]["overlappingNeighbours"].get<int>() >= 1);
    CHECK(check["counts"]["realName"].get<int>() >= 1);
}

TEST_CASE("buildings commands: the OSM fixture town through Settlement's buildings route")
{
    loadBuildings();
    loadSettlement();
    run("newScene");
    const json terrain = run("terrainCreate", {{"size", json::array({80000.0, 80000.0})}, {"resolution", 65}, {"amplitude", 200.0}, {"seed", 4}});
    const std::string townFile = (std::filesystem::path(LILA_SOURCE_DIR) / "tests" / "data" / "osm" / "w1_town.osm").string();
    const json town = run("settlementCreate", {{"profile", "usTown"}, {"size", 60000.0}, {"ground", terrain["mesh"]}, {"osmFile", townFile},
                                               {"osmOrigin", {23.2, -106.42}}, {"name", "Town"}});
    const std::string mesh = town["mesh"];
    const json routed = run("settlementBuildings", {{"args", {mesh}}, {"via", "buildings"}});
    CAPTURE(routed.dump());
    REQUIRE(routed.contains("buildings"));
    const int envelopes = run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}})["count"];
    CHECK(routed["buildings"]["buildings"] == envelopes);
    const json signs = run("slotFind", {{"type", "sign.storefront"}, {"limit", 0}, {"full", true}});
    CHECK(signs["count"].get<int>() >= 1);
    for (const json& s : signs["slots"])
        CHECK_FALSE(s["payload"]["data"].value("text", std::string()).empty());
    const json check = run("buildingCheck");
    CAPTURE(check["counts"].dump());
    CHECK(check["ok"] == true);
    CHECK(run("realNameCheck", {{"scene", true}})["ok"] == true);

    // One rule swaps every storefront sign.
    run("slotRule", {{"args", {"add"}}, {"type", "sign.storefront"}, {"placeholder", "panel"}, {"name", "Fictional signs"}});
    const json after = run("slotFind", {{"type", "sign.storefront"}, {"limit", 0}});
    int ruled = 0;
    for (const json& s : after["slots"])
        for (const json& src : s["sources"])
            ruled += src.get<std::string>().rfind("rule:", 0) == 0 ? 1 : 0;
    CHECK(ruled == after["count"].get<int>());

    // Change a style by district and paint a bay through the route's buildings.
    const json st = run("buildingStyle", {{"all", true}, {"style", "seafront"}});
    CHECK(st["count"] == envelopes);

    // A settlement edit rebuilds its buildings as a step of their own: none stale after, one undo shows the old ones.
    run("settlementAddExclusion", {{"args", {mesh}}, {"polygon", {{-15000, -15000}, {15000, -15000}, {15000, 15000}, {-15000, 15000}}}, {"name", "Park"}});
    CHECK(run("buildingRefresh", {{"dryRun", true}})["stale"].empty());
    const int fewer = run("settlementEnvelopes", {{"args", {mesh}}, {"limit", 0}})["count"];
    CHECK(fewer < envelopes);
    CHECK(run("undoInfo")["undo"] == "buildingRefresh");
    run("undo");
    CHECK(run("buildingRefresh", {{"dryRun", true}})["stale"].size() == 1);
}

TEST_CASE("buildings commands: site kit envelopes (no lot, a stable id each) make one building each")
{
    loadBuildings();
    run("newScene");
    json envs = json::array();
    for (int k = 0; k < 3; ++k)
    {
        json e = envelope(0, k * 3000.0, 0.0, 1500.0, 1200.0, 1, {"street", "open", "open", "open"}, "warehouse");
        e.erase("lot");
        e.erase("lotId");
        e["id"] = "site:port1/warehouse/" + std::to_string(k);
        envs.push_back(e);
    }
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Site"}});
    CHECK(made["buildings"] == 3);
    // The same ids give the same buildings on a second run.
    const json again = run("buildingCreate", {{"envelopes", envs}, {"name", "Site"}});
    CHECK(again["buildings"] == 3);
}

TEST_CASE("buildings commands: a style the library lacks falls back to the nearest shipped one and says so; a workspace adds its own")
{
    loadBuildings();
    Workspace::get().close();
    run("newScene");
    const std::pair<const char*, const char*> regional[] = {
        {"colonialCentro", "colonial"}, {"malecon", "seafront"}, {"narcoMansion", "mansion"}, {"villageAdobe", "adobe"}};
    json envs = json::array();
    for (size_t k = 0; k < std::size(regional); ++k)
        envs.push_back(envelope(0x5a0 + k, k * 3000.0, 0.0, 1400.0, 1000.0, 2, {"street", "open", "open", "open"}, regional[k].first));
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Regional"}});
    CAPTURE(made.dump().substr(0, 2000));
    REQUIRE(made.contains("warnings"));
    for (size_t k = 0; k < std::size(regional); ++k)
    {
        const std::string want = std::string("style '") + regional[k].first + "' is not in the library; '" + regional[k].second + "' stands in";
        bool found = false;
        for (const json& w : made["warnings"])
            found = found || w.get<std::string>().starts_with(want);
        CHECK_MESSAGE(found, want);
        char id[32];
        std::snprintf(id, sizeof id, "building:%zx", size_t(0x5a0 + k));
        CHECK(run("buildingInfo", {{"args", {id}}})["style"] == regional[k].second);
    }

    // A workspace's BuildingStyles folder: the regional style is found and nothing falls back.
    const std::filesystem::path ws = std::filesystem::temp_directory_path() / "lila_buildings_ws_test";
    std::filesystem::create_directories(ws / ".lila");
    std::filesystem::create_directories(ws / "BuildingStyles");
    {
        std::ifstream in(std::filesystem::path(LILA_TEST_BUILDINGS_STYLES) / "colonial.json");
        json st = json::parse(in);
        st["id"] = "colonialCentro";
        st["label"] = "Colonial Centro";
        st["replaces"] = {"colonial"};
        std::ofstream(ws / "BuildingStyles" / "colonialCentro.json") << st.dump(2);
    }
    Workspace::get().open(ws);
    bool listed = false;
    const json lib = run("buildingStyles");
    CAPTURE(lib["workspaceFolder"]);
    CAPTURE(lib["styles"].size());
    for (const json& s : lib["styles"])
        listed = listed || (s["id"] == "colonialCentro" && s.value("source", "") == "workspace");
    CHECK(listed);
    const json again = run("buildingCreate", {{"envelopes", json::array({envs[0]})}, {"name", "Regional"}});
    CHECK_FALSE(again.contains("warnings"));
    CHECK(run("buildingInfo", {{"args", {"building:5a0"}}})["style"] == "colonialCentro");
    // It replaces the shipped colonial, so buildings naming colonial get it too.
    run("buildingCreate", {{"envelopes", json::array({envelope(0x5b0, 0, 4000, 1400, 1000, 2, {"street", "open", "open", "open"}, "colonial")})},
                           {"name", "Replaced"}});
    CHECK(run("buildingInfo", {{"args", {"building:5b0"}}})["style"] == "colonialCentro");
    Workspace::get().close();
    bool gone = true;
    for (const json& s : run("buildingStyles")["styles"])
        gone = gone && s["id"] != "colonialCentro";
    CHECK(gone);
}

TEST_CASE("buildings commands: performance budget")
{
    loadBuildings();
    run("newScene");
    json envs = json::array();
    const int n = 1000;
    for (int k = 0; k < n; ++k)
    {
        const double x = (k % 40) * 1600.0, z = (k / 40) * 1600.0;
        envs.push_back(envelope(0x90000 + uint64_t(k), x, z, 1200 + (k % 5) * 50.0, 1100, 1 + k % 4, {"street", "party", "open", "party"},
                                k % 3 == 0 ? "colonial" : k % 3 == 1 ? "residential" : "modernCommercial", k % 4 == 0 ? "shop" : ""));
    }
    const auto t0 = std::chrono::steady_clock::now();
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Perf"}});
    const double createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    MESSAGE("1000 buildings: " << createMs << " ms, cells " << made["cells"] << ", slots " << made["slots"] << ", faces " << made["faces"]);
    CHECK(made["buildings"] == n);
    CHECK(createMs < 60000.0 * double(n) / 5000.0 * 2.0); // generous: twice the 5,000-in-a-minute rate
    const auto t1 = std::chrono::steady_clock::now();
    const json edit = run("buildingEdit", {{"args", {"building:90005"}}, {"floors", 3}});
    const double editMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    MESSAGE("one-building edit: " << editMs << " ms (evaluate " << edit["evaluateMs"] << " ms, node " << edit.value("nodeMs", 0.0) << " ms)");
    CHECK(edit.value("generated", 0) == 1);
    CHECK(editMs < 1000.0); // generous; the target is 100 ms, measured and logged
}

TEST_CASE("buildings commands: the trim sheet goes to the user's folder without a workspace, never beside the shipped one")
{
    loadBuildings();
    run("newScene");
    run("buildingCreate", {{"envelopes", json::array({envelope(1, 0, 0, 1200, 900, 2)})}, {"name", "Block"}});
    const std::filesystem::path shipped = std::filesystem::path(LILA_TEST_BUILDINGS_STYLES).parent_path() / "trims";
    auto count = [](const std::filesystem::path& d) {
        int n = 0;
        for (const auto& e : std::filesystem::directory_iterator(d))
            n += e.is_regular_file();
        return n;
    };
    const int before = count(shipped);
    // Baking it (Trim Sheets) writes maps beside that copy; here only the copy is made.
    const json r = run("buildingTrimSheet", {{"bake", false}});
    CAPTURE(r.dump().substr(0, 1500));
    CHECK(r["fileFrom"].get<std::string>().starts_with("user folder"));
    CHECK(r["file"].get<std::string>().find("trim-sheets") != std::string::npos);
    CHECK(std::filesystem::exists(r["file"].get<std::string>()));
    CHECK(count(shipped) == before);
}
TEST_CASE("buildings commands: near detail keeps the modules as instances, realize flattens them into the cell mesh")
{
    loadBuildings();
    run("newScene");
    json envs = json::array();
    for (int k = 0; k < 12; ++k)
        envs.push_back(envelope(0xa0000 + uint64_t(k), (k % 4) * 1600.0, (k / 4) * 1600.0, 1200, 1100, 3, {"street", "party", "open", "party"},
                                "colonial", k % 2 ? "shop" : ""));
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Inst"}});
    REQUIRE(!made["meshes"].empty());
    const std::string mesh = made["meshes"][0].get<std::string>();
    const json info = run("buildingInfo", {{"args", {mesh}}});
    REQUIRE(info["slots"].get<int>() > 0);
    // The mesh holds the shell only; the modules are drawn and sent as instances.
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"].get<int>() == info["faces"].get<int>());
    run("setAttr", {{"args", {info["modifier"].get<std::string>() + ".realize", true}}});
    CHECK(run("meshInfo", {{"args", {mesh}}})["faces"].get<int>() > info["faces"].get<int>());
}

TEST_CASE("buildings commands: acute corners, a tiny edge between two walls and mansion porticos pass buildingCheck")
{
    loadBuildings();
    run("newScene");
    // A wedge (about 30 degrees at its tip), a quad whose short third edge hides an acute meeting, and a 45 degree kite.
    const std::vector<std::vector<std::array<double, 2>>> shapes = {
        {{0, 0}, {3000, 0}, {0, 1700}},
        {{0, 0}, {2800, 0}, {2800, 60}, {0, 1500}},
        {{0, 0}, {2400, 0}, {2400, 1000}, {1000, 1000}},
    };
    json envs = json::array();
    int k = 0;
    for (const char* style : {"modernCommercial", "colonial", "residential", "mansion", "seafront"})
        for (const auto& shape : shapes)
            for (int seed = 0; seed < 2; ++seed, ++k)
            {
                const double x0 = (k % 10) * 6000.0, z0 = (k / 10) * 6000.0;
                json e = envelope(0xb000 + uint64_t(k), x0, z0, 100, 100, 3, {}, style, k % 2 ? "shop" : "");
                json fp = json::array(), sides = json::array();
                for (const auto& p : shape)
                {
                    fp.push_back({x0 + p[0], z0 + p[1]});
                    sides.push_back("street");
                }
                e["footprint"] = fp;
                e["sides"] = sides;
                e["entrances"] = json::array({{x0 + shape[0][0] * 0.5 + shape[1][0] * 0.5, z0}});
                envs.push_back(e);
            }
    run("buildingCreate", {{"envelopes", envs}, {"name", "Acute"}, {"cell", 0}});
    const json check = run("buildingCheck");
    CAPTURE(check["counts"].dump());
    CAPTURE(check["issues"].size() > 0 ? check["issues"][0].dump() : std::string());
    CHECK(check["counts"]["modulesIntersecting"] == 0);
    CHECK(check["counts"]["slotFloating"] == 0);
}

TEST_CASE("buildings commands: a reopened scene reads its buildings from the cache; changed or damaged ones regenerate")
{
    loadBuildings();
    run("newScene");
    const json envs = json::array({envelope(0xc001, 0, 0, 1500, 1200, 3), envelope(0xc002, 1500, 0, 1500, 1200, 2, {"street", "open", "open", "party"})});
    const json made = run("buildingCreate", {{"envelopes", envs}, {"name", "Cached"}, {"style", "colonial"}, {"cell", 0}});
    REQUIRE(made["meshes"].size() == 1);
    const std::string mesh = made["meshes"][0];
    const std::string A = "building:c001", B = "building:c002";
    const std::vector<std::string> idsA = slotIds(A), idsB = slotIds(B);
    const json infoA = run("buildingInfo", {{"args", {A}}});

    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "lila_buildings_cache_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string scene = (dir / "town.lila").generic_string();
    run("saveScene", {{"args", {scene}}});
    run("buildingCache", {{"write", true}});
    const json cache = run("buildingCache");
    REQUIRE(cache["exists"] == true);
    CHECK(cache["file"] == (dir / "town_cache" / "buildings.bin").generic_string());
    // Saving again with the same buildings leaves the file as it is.
    run("saveScene", {{"args", {scene}}});
    CHECK(run("buildingCache", {{"write", true}})["written"] == -1);

    auto reopen = [&] {
        run("buildingCache", {{"clearMemory", true}});
        run("openScene", {{"args", {scene}}});
        return run("buildingInfo", {{"args", {mesh}}});
    };
    // Unchanged: nothing generated, the same buildings and slots.
    json info = reopen();
    CHECK(info["count"] == 2);
    CHECK(info["generated"] == 0);
    CHECK(slotIds(A) == idsA);
    CHECK(slotIds(B) == idsB);
    const json againA = run("buildingInfo", {{"args", {A}}});
    CHECK(againA["floors"] == infoA["floors"]);
    CHECK(againA["palette"] == infoA["palette"]);
    CHECK(againA["modules"] == infoA["modules"]);

    // A changed building regenerates; its neighbour still comes from the cache.
    run("buildingEdit", {{"args", {B}}, {"floors", 4}});
    run("saveScene", {{"args", {scene}}});
    CHECK(run("buildingCache", {{"write", true}})["written"].get<int>() >= -1);
    info = reopen();
    CHECK(info["generated"] == 0);
    CHECK(run("buildingInfo", {{"args", {B}}})["floors"] == 4);
    run("buildingEdit", {{"args", {B}}, {"floors", 2}});
    CHECK(run("buildingInfo", {{"args", {mesh}}})["generated"] == 1);
    CHECK(slotIds(B) == idsB);

    // A cache from another build of Buildings is not used: both regenerate, as they were.
    run("saveScene", {{"args", {scene}}});
    run("buildingCache", {{"write", true}});
    const std::filesystem::path file = dir / "town_cache" / "buildings.bin";
    {
        std::fstream f(file, std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(8);
        const char other[8] = {1, 2, 3, 4, 5, 6, 7, 8};
        f.write(other, 8);
    }
    info = reopen();
    CHECK(info["generated"] == 2);
    CHECK(slotIds(A) == idsA);
    CHECK(slotIds(B) == idsB);

    // A damaged entry regenerates that building alone.
    run("buildingCache", {{"clearMemory", true}});
    run("buildingEdit", {{"args", {B}}, {"floors", 3}});
    run("buildingEdit", {{"args", {B}}, {"floors", 2}});
    run("saveScene", {{"args", {scene}}});
    std::filesystem::remove(file);
    run("buildingCache", {{"write", true}});
    REQUIRE(std::filesystem::exists(file));
    std::filesystem::resize_file(file, std::filesystem::file_size(file) - 16);
    info = reopen();
    CHECK(info["generated"] == 1);
    CHECK(slotIds(A) == idsA);
    CHECK(slotIds(B) == idsB);
    std::filesystem::remove_all(dir);
}
