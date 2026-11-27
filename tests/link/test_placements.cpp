#include <doctest/doctest.h>

#include "FakeServer.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "gn/Gn.h"
#include "gn/InstanceView.h"
#include "io/SceneFile.h"
#include "link/Exporters.h"
#include "link/UnrealLink.h"
#include "math/Unreal.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "scene/Transform.h"

#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using namespace lila;
using namespace lila::link;
using json = nlohmann::json;
namespace fs = std::filesystem;

namespace
{
json run(const char* name, json call = json::object()) { return ops::execute(name, call); }

/** The same registries test_link.cpp sets up; whichever file runs first does it. */
void setupPlacementTest()
{
    if (!ops::find("unrealPlaceInstances"))
    {
        static bool done = [] {
            mainq::init();
            ops::registerBuiltins();
            scene::types::registerBuiltins();
            scene::registerCommands();
            link::registerCommands();
            return true;
        }();
        (void)done;
    }
    if (!ops::find("saveScene"))
        io::registerCommands();
}

/** A bolt and two instances, one moved and one turned, plus a plain cube with no instances. */
void buildKit()
{
    run("newScene");
    run("polyCube", {{"name", "Bolt"}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltA"}});
    run("instance", {{"args", {"Bolt"}}, {"name", "BoltB"}});
    run("move", {{"args", {"BoltA"}}, {"by", {100, 0, 0}}});
    run("rotate", {{"args", {"BoltB"}}, {"by", {0, 90, 0}}});
    run("polyCube", {{"name", "Crate"}});
}

const json* placementOf(const json& group, const std::string& name)
{
    for (const json& p : group["placements"])
        if (p["name"] == name)
            return &p;
    return nullptr;
}
} // namespace

TEST_CASE("placement plan: one asset per instance root, every member placed in Unreal space")
{
    setupPlacementTest();
    buildKit();
    scene::Graph& g = scene::Graph::current();
    // Selecting one instance brings its whole group.
    json plan = placementPlan(g, {g.find("BoltA")->id(), g.find("Crate")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 2);
    const json& bolt = plan["groups"][0];
    CHECK(bolt["source"] == "|Bolt");
    CHECK(bolt["contentPath"] == "/Game/Lila/Instances/SM_Bolt");
    CHECK(bolt["objectPath"] == "/Game/Lila/Instances/SM_Bolt.SM_Bolt");
    CHECK(bolt["mode"] == "actors");
    REQUIRE(bolt["placements"].size() == 3);
    const json* a = placementOf(bolt, "BoltA");
    REQUIRE(a);
    // A dry run gives no link ids, so members go by node id.
    CHECK((*a)["id"] == std::to_string(g.find("BoltA")->id()));
    const math::Vector loc = math::toUnreal(math::Vector(100.0, 0.0, 0.0));
    CHECK((*a)["location"][0].get<double>() == doctest::Approx(loc.x));
    CHECK((*a)["location"][1].get<double>() == doctest::Approx(loc.y));
    CHECK((*a)["location"][2].get<double>() == doctest::Approx(loc.z));
    CHECK((*a)["rotation"][3].get<double>() == doctest::Approx(1.0));
    // A quarter turn about up.
    const json* b = placementOf(bolt, "BoltB");
    REQUIRE(b);
    CHECK(std::abs((*b)["rotation"][3].get<double>()) == doctest::Approx(std::sqrt(0.5)));
    CHECK(plan["groups"][1]["placements"].size() == 1);

    // Above the threshold auto goes to a hierarchical instanced component; an explicit mode wins.
    PlacementOptions o;
    o.threshold = 2;
    o.folder = "/Game/Kit/";
    json hism = placementPlan(g, {g.find("Bolt")->id()}, o);
    CHECK(hism["groups"][0]["mode"] == "hism");
    CHECK(hism["groups"][0]["contentPath"] == "/Game/Kit/SM_Bolt");
    o.mode = "ism";
    CHECK(placementPlan(g, {g.find("Bolt")->id()}, o)["groups"][0]["mode"] == "ism");
    o.mode = "bad";
    CHECK_THROWS_AS(placementPlan(g, {g.find("Bolt")->id()}, o), CommandError);

    CHECK(run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}})["groups"].size() == 1);
}

TEST_CASE("placement plan: lights become light actors and lighting channels Unreal's three")
{
    setupPlacementTest();
    run("newScene");
    run("polyCube", {{"name", "Crate"}});
    run("polyCube", {{"name", "Floor"}});
    run("light", {{"type", "spot"}, {"name", "Lamp"}, {"position", {0, 200, 0}}, {"skipSelect", true}});
    run("light", {{"type", "dome"}, {"name", "Sky"}, {"skipSelect", true}});
    run("lightingChannels", {{"args", {"Lamp", "Crate"}}, {"set", {"interior"}}});
    scene::Graph& g = scene::Graph::current();
    const std::vector<scene::NodeId> roots = {g.find("Crate")->id(), g.find("Floor")->id(), g.find("Lamp")->id(), g.find("Sky")->id()};

    json plan = placementPlan(g, roots, PlacementOptions{});
    REQUIRE(plan["lights"].size() == 1);
    const json& lamp = plan["lights"][0];
    CHECK(lamp["kind"] == "spot");
    CHECK(lamp["lightingChannels"] == json::array({false, true, false}));
    // Unturned, a spot shines down Lila's -Z; forward is that direction in Unreal space.
    const math::Vector down = math::toUnreal(math::Vector(0.0, 0.0, -1.0));
    CHECK(lamp["forward"][0].get<double>() == doctest::Approx(down.x));
    CHECK(lamp["forward"][1].get<double>() == doctest::Approx(down.y));
    CHECK(lamp["forward"][2].get<double>() == doctest::Approx(down.z));
    const math::Vector at = math::toUnreal(math::Vector(0.0, 200.0, 0.0));
    CHECK(lamp["location"][0].get<double>() == doctest::Approx(at.x));
    CHECK(lamp["location"][2].get<double>() == doctest::Approx(at.z));
    CHECK(plan["skipped"].size() == 1);

    REQUIRE(plan["groups"].size() == 2);
    CHECK(plan["groups"][0]["placements"][0]["lightingChannels"] == json::array({false, true, false}));
    CHECK(plan["groups"][1]["placements"][0]["lightingChannels"] == json::array({true, false, false}));
    const json& report = plan["lightingChannels"];
    CHECK(report["channels"] == json::array({"interior"}));
    CHECK(report["lights"] == json::array({"Lamp"}));
    CHECK(report["objects"] == json::array({"Crate"}));
    CHECK(report["unreal"] == json::array({"default", "interior", ""}));
    CHECK(report["written"] == true);
    CHECK_FALSE(report.contains("note"));

    // More than three: the fourth and later are dropped and named, and what is left in none goes on channel 0.
    for (const char* c : {"a", "b", "c"})
        run("lightingChannel", {{"args", {c}}, {"create", true}});
    run("lightingChannels", {{"args", {"Floor"}}, {"set", {"a", "b"}}});
    run("lightingChannels", {{"args", {"Lamp"}}, {"set", {"c"}}});
    plan = placementPlan(g, roots, PlacementOptions{});
    CHECK(plan["lightingChannels"]["unreal"] == json::array({"default", "interior", "a"}));
    CHECK(plan["lightingChannels"]["dropped"] == json::array({"b", "c"}));
    CHECK(plan["lightingChannels"]["note"].get<std::string>().find("b, c dropped") != std::string::npos);
    CHECK(plan["lights"][0]["lightingChannels"] == json::array({true, false, false}));

    PlacementOptions off;
    off.lightingChannels = false;
    plan = placementPlan(g, roots, off);
    CHECK_FALSE(plan["groups"][0]["placements"][0].contains("lightingChannels"));
    CHECK_FALSE(plan["lights"][0].contains("lightingChannels"));
    CHECK(plan["lightingChannels"]["written"] == false);
    CHECK(plan["lightingChannels"]["objects"].size() == 2);
}

TEST_CASE("unrealPlaceInstances exports each source once, imports it and places its members")
{
    setupPlacementTest();
    buildKit();
    fs::path dir = fs::temp_directory_path() / "lila_link_placements";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());

    std::mutex mx;
    std::vector<std::vector<scene::NodeId>> exported;
    registerExporter("fbx:instanceSource", [&](const std::vector<scene::NodeId>& nodes, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        std::scoped_lock lock(mx);
        exported.push_back(nodes);
        return true;
    });
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"].get<std::string>() + ".Imported"}},
                                           {"warnings", json::array()},
                                           {"errors", json::array()}});
        if (m == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
        if (m == "removePlacements")
            return FakeServer::reply(req, {{"removed", 3}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    scene::Graph& g = scene::Graph::current();
    json done = run("unrealPlaceInstances", {{"args", {"BoltB"}}, {"wait", true}, {"folder", "/Game/Kit"}});
    REQUIRE(done["groups"].size() == 1);
    CHECK(done["groups"][0]["placed"]["spawned"] == 3);
    {
        std::scoped_lock lock(mx);
        REQUIRE(exported.size() == 1);
        CHECK(exported[0] == std::vector<scene::NodeId>{g.find("Bolt")->id()});
    }
    json import, place;
    for (const json& r : server.requests())
    {
        if (r["method"] == "importAsset")
            import = r["params"];
        if (r["method"] == "placeInstances")
            place = r["params"];
    }
    REQUIRE(import.is_object());
    CHECK(import["contentPath"] == "/Game/Kit/SM_Bolt");
    CHECK(import["kind"] == "static");
    // Static level meshes collide by default, without Nanite unless asked.
    CHECK(import["options"]["collision"] == "complexAsSimple");
    CHECK(import["options"]["nanite"] == false);
    REQUIRE(place.is_object());
    const std::string key = std::get<std::string>(g.find("Bolt")->value(scene::Transform::kLinkId));
    REQUIRE(key.size() == 16);
    CHECK(place["group"] == key);
    CHECK(place["label"] == "|Bolt");
    // The placements reference the asset the import reported.
    CHECK(place["mesh"] == "/Game/Kit/SM_Bolt.Imported");
    CHECK(place["mode"] == "actors");
    CHECK(place["collision"] == "complexAsSimple");
    CHECK(place["placements"].size() == 3);

    CHECK(run("unrealRemovePlacements", {{"args", {"Bolt"}}})["removed"] == 3);
    json removal;
    for (const json& r : server.requests())
        if (r["method"] == "removePlacements")
            removal = r["params"];
    CHECK(removal["groups"] == json::array({key}));

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("unrealPlaceInstances: assets using a material named in naniteOffMaterials go without Nanite")
{
    setupPlacementTest();
    buildKit();
    run("createMaterial", {{"args", {"Glass"}}, {"blendMode", "translucent"}, {"opacity", 0.3}});
    run("assignMaterial", {{"args", {"Glass", "Bolt"}}});
    fs::path dir = fs::temp_directory_path() / "lila_link_placements_nanite";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [&](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"].get<std::string>() + ".Imported"}},
                                           {"warnings", json::array()},
                                           {"errors", json::array()}});
        if (m == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    const json done = run("unrealPlaceInstances", {{"args", {"BoltB", "Crate"}}, {"wait", true}, {"folder", "/Game/Kit"}, {"nanite", true},
                                                   {"naniteOffMaterials", {"Glass"}}, {"resend", true}});
    CHECK(done["naniteOff"] == 1);
    std::map<std::string, bool> nanite;
    for (const json& r : server.requests())
        if (r["method"] == "importAsset")
            nanite[r["params"]["contentPath"].get<std::string>()] = r["params"]["options"]["nanite"].get<bool>();
    CHECK(nanite["/Game/Kit/SM_Bolt"] == false);
    CHECK(nanite["/Game/Kit/SM_Crate"] == true);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("unrealPlaceInstances mode=customHlod: hidden proxies placed as Custom HLOD actors in the named layers")
{
    setupPlacementTest();
    run("newScene");
    run("polyCube", {{"name", "Block_HLOD"}});
    run("hide", {{"args", {"Block_HLOD"}}});
    fs::path dir = fs::temp_directory_path() / "lila_link_custom_hlod";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    auto serve = [](json features) {
        return [features](const json& req) -> std::optional<json> {
            const std::string m = req["method"];
            if (m == "hello")
                return FakeServer::reply(req, {{"project", "TestGame"}, {"features", features}});
            if (m == "importAsset")
                return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
            if (m == "placeInstances")
                return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
            return std::nullopt;
        };
    };
    const json flags = {{"args", {"Block_HLOD"}}, {"mode", "customHlod"}, {"hlodLayer", "/Game/Lila/HLOD/HL_Near"},
                        {"linkedHlodLayer", "/Game/Lila/HLOD/HL_Base"}, {"validate", false}, {"wait", true}};
    {
        // A plugin without Custom HLOD actors is told so instead of getting plain actors.
        FakeServer old(serve({"progress", "placements"}));
        Link::ConnectOptions o;
        o.port = old.port();
        Link::get().connect(o);
        CHECK_THROWS_AS(run("unrealPlaceInstances", flags), CommandError);
        Link::get().disconnect();
    }
    FakeServer server(serve({"progress", "placements", "customHlod"}));
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    // Hidden in Lila (a proxy) and still placed.
    const json done = run("unrealPlaceInstances", flags);
    REQUIRE(done["groups"].size() == 1);
    CHECK(done["groups"][0]["mode"] == "customHlod");
    json place;
    for (const json& r : server.requests())
        if (r["method"] == "placeInstances")
            place = r["params"];
    REQUIRE(place.is_object());
    CHECK(place["mode"] == "customHlod");
    CHECK(place["hlodLayer"] == "/Game/Lila/HLOD/HL_Near");
    CHECK(place["linkedHlodLayer"] == "/Game/Lila/HLOD/HL_Base");
    CHECK(place["placements"].size() == 1);
    CHECK_THROWS_AS(run("unrealPlaceInstances", {{"args", {"Block_HLOD"}}, {"mode", "lod"}, {"dryRun", true}}), CommandError);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("slot table: a send replaces only its own sources' rows")
{
    setupPlacementTest();
    run("newScene");
    const json lamp = {{"unrealClass", "/Game/BP/BP_Lamp.BP_Lamp_C"}};
    run("slotPublish", {{"args", {"Lamps"}},
                        {"slots", {{{"id", "street/1/lamp/0"}, {"type", "lamp.street"}, {"position", {0, 0, 0}}, {"payload", lamp}},
                                   {{"id", "street/1/lamp/1"}, {"type", "lamp.street"}, {"position", {500, 0, 0}}, {"payload", lamp}}}}});
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements", "slots"}}});
        if (m == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
        if (m == "writeSlotTable")
            return FakeServer::reply(req, {{"rows", req["params"]["rows"].size()}});
        if (m == "removePlacements")
            return FakeServer::reply(req, {{"removed", 0}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    run("unrealPlaceInstances", {{"args", {"Lamps"}}, {"wait", true}, {"validate", false}, {"folder", "/Game/Kit"}, {"mode", "actors"}});
    json table;
    for (const json& r : server.requests())
        if (r["method"] == "writeSlotTable")
            table = r["params"];
    REQUIRE(table.is_object());
    REQUIRE(table["rows"].size() == 2);
    REQUIRE(table["sources"].size() == 1);
    // Every row's group starts with a source the send names, so Unreal keeps every other send's rows.
    for (const json& row : table["rows"])
        CHECK(row["group"].get<std::string>().starts_with(table["sources"][0].get<std::string>()));

    // A rule swap sends the lamps again by itself (auto-sync), with the flags of the send that carried them.
    PrefStore::get().set("unreal", "/autoSync", true, PrefScope::Workspace);
    const size_t before = server.requests().size();
    run("slotRule", {{"args", {"add"}}, {"type", "lamp"}, {"unrealClass", "/Game/BP/BP_LampB.BP_LampB_C"}});
    json resent;
    for (int k = 0; k < 200 && resent.is_null(); ++k)
    {
        mainq::drain();
        const std::vector<json> reqs = server.requests();
        for (size_t i = before; i < reqs.size(); ++i)
            if (reqs[i]["method"] == "placeInstances")
                resent = reqs[i]["params"];
        if (resent.is_null())
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    REQUIRE(resent.is_object());
    CHECK(resent["actorClass"] == "/Game/BP/BP_LampB.BP_LampB_C");
    CHECK(resent["mode"] == "actors");
    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
}

TEST_CASE("placement plan: a live array's copies are placements of their source's asset")
{
    setupPlacementTest();
    run("newScene");
    run("polyCube", {{"name", "Post"}});
    run("array", {{"args", {"Post"}}, {"count", 4}, {"offset", {200, 0, 0}}, {"name", "Fence"}});
    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Fence")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 1);
    const json& post = plan["groups"][0];
    CHECK(post["source"] == "|Fence|Post");
    REQUIRE(post["placements"].size() == 4);
    const json* last = placementOf(post, "Post_3");
    REQUIRE(last);
    const std::string key = std::to_string(g.find("Post")->id()) + ":3";
    CHECK((*last)["id"] == key);
    const math::Vector loc = math::toUnreal(math::Vector(600.0, 0.0, 0.0));
    CHECK((*last)["location"][0].get<double>() == doctest::Approx(loc.x));
    CHECK((*last)["location"][1].get<double>() == doctest::Approx(loc.y));

    // The copies count toward the threshold.
    PlacementOptions o;
    o.threshold = 3;
    CHECK(placementPlan(g, {g.find("Fence")->id()}, o)["groups"][0]["mode"] == "hism");
}

TEST_CASE("placement keys: link ids survive renames, and copies get their own")
{
    setupPlacementTest();
    buildKit();
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    o.assignIds = true;
    json first = placementPlan(g, {g.find("Bolt")->id()}, o);
    const std::string key = first["groups"][0]["key"];
    const std::string boltA = std::get<std::string>(g.find("BoltA")->value(scene::Transform::kLinkId));
    REQUIRE(key.size() == 16);
    REQUIRE(boltA.size() == 16);
    CHECK(boltA != key);

    run("rename", {{"args", {"Bolt", "Rivet"}}});
    json renamed = placementPlan(g, {g.find("Rivet")->id()}, o);
    CHECK(renamed["groups"][0]["key"] == key);
    CHECK(renamed["groups"][0]["source"] == "|Rivet");

    // Duplicates start without one; a copy that arrives carrying one (pasted, imported twice) gets a new one.
    run("duplicate", {{"args", {"BoltA"}}, {"name", "BoltC"}});
    CHECK(std::get<std::string>(g.find("BoltC")->value(scene::Transform::kLinkId)).empty());
    g.setValue({g.find("BoltC")->id(), scene::Transform::kLinkId, -1}, boltA);
    placementPlan(g, {g.find("BoltC")->id()}, o);
    CHECK(std::get<std::string>(g.find("BoltA")->value(scene::Transform::kLinkId)) == boltA);
    const std::string boltC = std::get<std::string>(g.find("BoltC")->value(scene::Transform::kLinkId));
    CHECK(boltC.size() == 16);
    CHECK(boltC != boltA);
}

TEST_CASE("placement assets: names unique across same-named sources, saved and kept after reopening")
{
    setupPlacementTest();
    run("newScene");
    run("group", {{"empty", true}, {"name", "North"}});
    run("group", {{"empty", true}, {"name", "South"}});
    run("polyCube", {{"name", "Wall"}});
    run("parent", {{"args", {"Wall", "North"}}});
    run("polyCube", {{"name", "Gate"}});
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    o.assignIds = true;
    json first = placementPlan(g, {g.find("Wall")->id(), g.find("Gate")->id()}, o);
    REQUIRE(first["groups"].size() == 2);
    CHECK(first["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    CHECK(first["groups"][1]["contentPath"] == "/Game/Lila/Instances/SM_Gate");

    // A second Wall elsewhere gets its own asset; the first keeps its name.
    run("polyCube", {{"name", "Wall"}});
    run("parent", {{"args", {"|Wall", "South"}}});
    const scene::NodeId south = g.find("|South|Wall")->id();
    json both = placementPlan(g, {g.find("|North|Wall")->id(), south}, o);
    REQUIRE(both["groups"].size() == 2);
    CHECK(both["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    const std::string key = both["groups"][1]["key"];
    const std::string second = both["groups"][1]["contentPath"];
    CHECK(second == "/Game/Lila/Instances/SM_Wall_" + key.substr(0, 8));
    CHECK(both["groups"][1]["objectPath"] == second + "." + second.substr(second.rfind('/') + 1));

    const fs::path path = fs::temp_directory_path() / "lila_link_asset_names.lila";
    run("saveScene", {{"args", {path.generic_string()}}});
    run("newScene");
    run("openScene", {{"args", {path.generic_string()}}});
    json again = placementPlan(g, {g.find("|North|Wall")->id(), g.find("|South|Wall")->id()}, o);
    CHECK(again["groups"][0]["contentPath"] == "/Game/Lila/Instances/SM_Wall");
    CHECK(again["groups"][1]["contentPath"] == second);
    // Renaming the source keeps the asset it was sent as.
    run("rename", {{"args", {"|South|Wall", "Rampart"}}});
    CHECK(placementPlan(g, {g.find("Rampart")->id()}, o)["groups"][0]["contentPath"] == second);
    fs::remove(path);
}

TEST_CASE("placement collision: options checked, UCX_ proxies go with their source and are never placed")
{
    setupPlacementTest();
    run("newScene");
    run("polyCube", {{"name", "Arch"}});
    run("polyCube", {{"name", "UCX_Arch_00"}});
    run("parent", {{"args", {"UCX_Arch_00", "Arch"}}});
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    json plan = placementPlan(g, {g.find("Arch")->id()}, o);
    REQUIRE(plan["groups"].size() == 1);
    CHECK(plan["groups"][0]["source"] == "|Arch");
    // auto: a source with UCX_ children collides with them, and goes with its children.
    CHECK(plan["groups"][0]["collision"] == "ucx");
    CHECK(plan["groups"][0]["ucxChildren"] == true);
    CHECK(plan["groups"][0]["nanite"] == false);
    o.collision = "ucx";
    o.nanite = true;
    plan = placementPlan(g, {g.find("Arch")->id()}, o);
    CHECK(plan["groups"][0]["collision"] == "ucx");
    CHECK(plan["groups"][0]["nanite"] == true);
    o.collision = "sphere";
    CHECK_THROWS_AS(placementPlan(g, {g.find("Arch")->id()}, o), CommandError);
    CHECK(run("unrealPlaceInstances", {{"args", {"Arch"}}, {"dryRun", true}, {"collision", "box"}})["groups"][0]["collision"] == "box");

    // unrealImport passes collision through as an import option.
    CHECK(importParams("C:/x.fbx", {{"contentPath", "/Game/A"}, {"collision", "convex"}})["options"]["collision"] == "convex");
}

TEST_CASE("verification commands: unrealCall, unrealListPlacements, unrealAssetInfo, unrealSaveAssets return errors as data")
{
    setupPlacementTest();
    buildKit();
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "ping")
            return FakeServer::reply(req, "pong");
        if (m == "listPlacements")
            return FakeServer::reply(req, json::array({{{"actor", "Bolt"}, {"group", "g1"}, {"node", "n1"}},
                                                       {{"actor", "Lila_Crate"}, {"group", "other"}, {"instances", 40}}}));
        if (m == "assetInfo")
            return FakeServer::reply(req, {{"path", req["params"]["path"]}, {"class", "StaticMesh"}, {"collision", {{"simple", 1}}}});
        if (m == "saveAssets")
            return FakeServer::reply(req, {{"saved", false}, {"errors", {"Saving the level failed"}}});
        return json{{"jsonrpc", "2.0"}, {"id", req["id"]}, {"error", {{"code", -32601}, {"message", "no method " + m}}}};
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    json r = run("unrealCall", {{"args", {"ping"}}});
    CHECK(r["ok"] == true);
    CHECK(r["result"] == "pong");
    r = run("unrealCall", {{"args", {"noSuchMethod"}}, {"params", {{"x", 1}}}});
    CHECK(r["ok"] == false);
    CHECK(r["code"] == -32601);
    CHECK(r["error"].get<std::string>().find("noSuchMethod") != std::string::npos);

    // Every placement, or only the groups of the given meshes (their keys sent along).
    r = run("unrealListPlacements");
    REQUIRE(r["ok"] == true);
    CHECK(r["placements"].size() == 2);
    r = run("unrealListPlacements", {{"args", {"Crate"}}});
    REQUIRE(r["ok"] == true);
    CHECK(r["placements"].empty());
    json listed;
    for (const json& q : server.requests())
        if (q["method"] == "listPlacements")
            listed = q["params"];
    CHECK(listed["groups"] == json::array({std::to_string(scene::Graph::current().find("Crate")->id())}));

    r = run("unrealAssetInfo", {{"args", {"/Game/Lila/Instances/SM_Bolt"}}});
    CHECK(r["ok"] == true);
    CHECK(r["collision"]["simple"] == 1);
    r = run("unrealSaveAssets", {{"level", true}});
    CHECK(r["ok"] == false);
    CHECK(r["errors"].size() == 1);
    json saved;
    for (const json& q : server.requests())
        if (q["method"] == "saveAssets")
            saved = q["params"];
    CHECK(saved["level"] == true);
    CHECK_THROWS_AS(run("unrealSaveAssets"), CommandError);
    Link::get().disconnect();
}

TEST_CASE("placement timeouts scale with the count; a km-wide grid of thousands plans as one hism group")
{
    setupPlacementTest();
    CHECK(placementTimeoutMs(0) == 30000);
    CHECK(placementTimeoutMs(10000) == 230000);
    CHECK(placementTimeoutMs(100000000) == 1800000);

    run("newScene");
    run("polyCube", {{"name", "Stone"}, {"width", 300.0}, {"height", 300.0}, {"depth", 300.0}});
    // 60 x 60 stones 100 m apart: 5.9 km across.
    run("array", {{"args", {"Stone"}}, {"mode", "grid"}, {"count", 60}, {"countY", 1}, {"countZ", 60}, {"spacing", {10000, 0, 10000}}, {"name", "Field"}});
    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Field")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 1);
    const json& grp = plan["groups"][0];
    CHECK(grp["mode"] == "hism");
    REQUIRE(grp["placements"].size() == 3600);
    double far = 0.0;
    for (const json& p : grp["placements"])
        far = std::max(far, std::hypot(p["location"][0].get<double>(), p["location"][1].get<double>()));
    CHECK(far == doctest::Approx(std::hypot(590000.0, 590000.0)));
}

namespace
{
std::string gnAdd(const std::string& grp, const char* type) { return run("gnAddNode", {{"args", {grp, type}}}).get<std::string>(); }
void gnLink(const std::string& grp, const std::string& from, const std::string& to) { run("gnConnect", {{"args", {grp, from, to}}}); }
void gnSet(const std::string& grp, const std::string& plug, json v) { run("gnSetValue", {{"args", {grp, plug, v}}}); }

/** A km-wide ground scattering two sizes of boulder as geometry nodes instances, nothing realized. */
void buildScatter()
{
    run("newScene");
    run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Ground"}});
    run("gnCreateGroup", {{"args", {"Boulders"}}});
    const std::string grid = gnAdd("Boulders", "MeshGrid"), dist = gnAdd("Boulders", "DistributePointsOnFaces"),
                      big = gnAdd("Boulders", "MeshCube"), small = gnAdd("Boulders", "MeshCube"), a = gnAdd("Boulders", "InstanceOnPoints"),
                      b = gnAdd("Boulders", "InstanceOnPoints"), join = gnAdd("Boulders", "JoinGeometry");
    // 1 km square at one boulder per 10000 m2.
    gnSet("Boulders", grid + ".sizeX", 100000);
    gnSet("Boulders", grid + ".sizeY", 100000);
    gnSet("Boulders", dist + ".density", 0.0001);
    gnSet("Boulders", dist + ".seed", 7);
    gnSet("Boulders", big + ".size", json::array({400, 300, 400}));
    gnSet("Boulders", small + ".size", json::array({80, 60, 80}));
    gnLink("Boulders", grid + ".mesh", dist + ".mesh");
    gnLink("Boulders", dist + ".points", a + ".points");
    gnLink("Boulders", dist + ".points", b + ".points");
    gnLink("Boulders", big + ".mesh", a + ".instance");
    gnLink("Boulders", small + ".mesh", b + ".instance");
    gnLink("Boulders", a + ".instances", join + ".geometry");
    gnLink("Boulders", b + ".instances", join + ".geometry");
    gnLink("Boulders", join + ".geometry", "GroupOutput.geometry");
    run("gnApply", {{"args", {"Ground", "Boulders"}}});
}
} // namespace

TEST_CASE("geometry nodes instances: one asset per referenced mesh, placed per instance, keyed stably")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    scene::Graph& g = scene::Graph::current();
    g.evaluate();
    const gn::InstanceViewPtr view = gn::instanceViewFor(g, g.find("Ground")->id());
    REQUIRE(view);
    REQUIRE(view->parts.size() == 2);
    const size_t each = view->parts[0].transforms->size();
    REQUIRE(each > 50);

    PlacementOptions o;
    o.assignIds = true;
    json plan = placementPlan(g, {g.find("Ground")->id()}, o);
    REQUIRE(plan["groups"].size() == 3);
    const json& own = plan["groups"][0];
    const std::string key = own["key"];
    // The ground itself is all instances, so its own placements go.
    CHECK(own["remove"] == true);
    CHECK(own["placements"].empty());
    std::set<std::string> assets;
    for (size_t k = 1; k < 3; ++k)
    {
        const json& grp = plan["groups"][k];
        const std::string gk = grp["key"];
        CHECK(gk.starts_with(key + ":gn:"));
        CHECK(gk.size() == key.size() + 4 + 8);
        // Named by content, so the same module under many sources is one asset.
        const std::string content = grp["contentPath"];
        CHECK(content.starts_with("/Game/Lila/Instances/GN/SM_GN_" + gk.substr(gk.size() - 8)));
        CHECK(grp["objectPath"] == content + "." + content.substr(content.rfind('/') + 1));
        CHECK(grp["mode"] == "hism");
        CHECK(grp["placements"].size() == each);
        CHECK(grp.contains("gnMesh"));
        assets.insert(grp["contentPath"].get<std::string>());
    }
    CHECK(assets.size() == 2);
    REQUIRE(plan["gnSources"].size() == 1);
    CHECK(plan["gnSources"][0]["prefix"] == key + ":gn:");
    CHECK(plan["gnSources"][0]["keep"].size() == 2);

    // Instance 0 of the first part sits where the viewport draws it, in Unreal space, km out.
    const json& first = plan["groups"][1]["placements"][0];
    const glm::dvec3 at = glm::dvec3(glm::dmat4(view->parts[0].transforms->front())[3]);
    const math::Vector ue = math::toUnreal(math::Vector(at));
    CHECK(first["location"][0].get<double>() == doctest::Approx(ue.x).epsilon(1e-6));
    CHECK(first["location"][1].get<double>() == doctest::Approx(ue.y).epsilon(1e-6));
    CHECK(first["id"] == plan["groups"][1]["placements"][0]["id"]);
    CHECK(first["customData"].size() == 1);

    // Same scene, same keys: evaluating again or moving the ground keeps every group and instance key.
    run("move", {{"args", {"Ground"}}, {"by", {0, 500, 0}}});
    json again = placementPlan(g, {g.find("Ground")->id()}, o);
    REQUIRE(again["groups"].size() == 3);
    for (size_t k = 1; k < 3; ++k)
    {
        CHECK(again["groups"][k]["key"] == plan["groups"][k]["key"]);
        CHECK(again["groups"][k]["placements"][5]["id"] == plan["groups"][k]["placements"][5]["id"]);
    }

    // Joining the input back in gives the ground a base mesh of its own to place.
    const std::string join = "JoinGeometry";
    gnLink("Boulders", "GroupInput.geometry", join + ".geometry");
    json based = placementPlan(g, {g.find("Ground")->id()}, o);
    CHECK_FALSE(based["groups"][0].contains("remove"));
    CHECK(based["groups"][0]["placements"].size() == 1);
    CHECK(based["groups"][0].contains("gnMesh"));
}

TEST_CASE("geometry nodes instances reach Unreal: their meshes written and imported, stale meshes removed")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    fs::path dir = fs::temp_directory_path() / "lila_link_gn_placements";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    FakeServer server([](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements"}}});
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        if (m == "placeInstances")
            return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
        if (m == "removePlacements")
            return FakeServer::reply(req, {{"removed", 0}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);

    // A second ground with the same geometry nodes shares their two meshes: still two imports for both.
    run("polyPlane", {{"width", 100.0}, {"depth", 100.0}, {"name", "Ground2"}});
    run("gnApply", {{"args", {"Ground2", "Boulders"}}});
    json done = run("unrealPlaceInstances", {{"args", {"Ground", "Ground2"}}, {"wait", true}});
    REQUIRE(done["groups"].size() == 6);
    std::vector<json> imports, places, removals;
    for (const json& r : server.requests())
    {
        if (r["method"] == "importAsset")
            imports.push_back(r["params"]);
        if (r["method"] == "placeInstances")
            places.push_back(r["params"]);
        if (r["method"] == "removePlacements")
            removals.push_back(r["params"]);
    }
    REQUIRE(imports.size() == 2);
    for (const json& i : imports)
    {
        // A real FBX of the referenced mesh, not the realized scatter.
        const fs::path file = i["file"].get<std::string>();
        REQUIRE(fs::exists(file));
        CHECK(fs::file_size(file) > 1000);
        CHECK(fs::file_size(file) < 100000);
    }
    REQUIRE(places.size() == 4);
    CHECK(places[0]["mesh"] == places[2]["mesh"]);
    CHECK(places[0]["mode"] == "hism");
    CHECK(places[0]["label"].get<std::string>().starts_with("|Ground_"));
    // The holder's ULilaInstanceData learns what each custom data slot holds; it shares no components here.
    CHECK(places[0]["customData"] == json::parse(R"([{"name":"random","slot":0,"width":1}])"));
    CHECK(places[0]["components"] == json::array());
    REQUIRE(removals.size() == 4);
    CHECK(removals[0]["groups"] == json::array({done["groups"][0]["key"]}));
    CHECK(removals[2]["prefixes"] == json::array({done["groups"][0]["key"].get<std::string>() + ":gn:"}));
    CHECK(removals[2]["keep"].size() == 2);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("placement signatures: a second send of the same geometry nodes result imports and places nothing")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    fs::path dir = fs::temp_directory_path() / "lila_link_signatures";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    // The level as Unreal would hold it: group -> {hash, actors}.
    auto level = std::make_shared<json>(json::object());
    auto mx = std::make_shared<std::mutex>();
    FakeServer server([level, mx](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements", "placementHashes"}}});
        if (m == "placementHashes")
        {
            std::scoped_lock lock(*mx);
            return FakeServer::reply(req, {{"groups", *level}});
        }
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        if (m == "placeInstances")
        {
            const json& p = req["params"];
            std::scoped_lock lock(*mx);
            (*level)[p["group"].get<std::string>()] = {{"hash", p.value("hash", "")}, {"actors", p["mode"] == "hism" ? size_t(1) : p["placements"].size()}};
            return FakeServer::reply(req, {{"spawned", p["placements"].size()}, {"updated", 0}, {"removed", 0}});
        }
        if (m == "removePlacements")
            return FakeServer::reply(req, {{"removed", 0}});
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    auto count = [&server](const char* method) {
        return std::ranges::count_if(server.requests(), [method](const json& r) { return r["method"] == method; });
    };

    json first = run("unrealPlaceInstances", {{"args", {"Ground"}}, {"wait", true}});
    CHECK(first["unchanged"] == 0);
    CHECK(count("importAsset") == 2);
    CHECK(count("placeInstances") == 2);
    json second = run("unrealPlaceInstances", {{"args", {"Ground"}}, {"wait", true}});
    CHECK(second["unchanged"] == 2);
    CHECK(count("importAsset") == 2);
    CHECK(count("placeInstances") == 2);

    // A changed scatter changes the placements: both groups go again.
    gnSet("Boulders", "DistributePointsOnFaces.seed", 8);
    json third = run("unrealPlaceInstances", {{"args", {"Ground"}}, {"wait", true}});
    CHECK(third["unchanged"] == 0);
    CHECK(count("placeInstances") == 4);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("placement signatures: a plain mesh sent again unchanged is skipped, an edited one goes")
{
    setupPlacementTest();
    buildKit();
    fs::path dir = fs::temp_directory_path() / "lila_link_plain_signatures";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    auto level = std::make_shared<json>(json::object());
    auto mx = std::make_shared<std::mutex>();
    FakeServer server([level, mx](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements", "placementHashes"}}});
        if (m == "placementHashes")
        {
            std::scoped_lock lock(*mx);
            return FakeServer::reply(req, {{"groups", *level}});
        }
        if (m == "importAsset")
            return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
        if (m == "placeInstances")
        {
            const json& p = req["params"];
            std::scoped_lock lock(*mx);
            (*level)[p["group"].get<std::string>()] = {{"hash", p.value("hash", "")}, {"actors", p["mode"] == "hism" ? size_t(1) : p["placements"].size()}};
            return FakeServer::reply(req, {{"spawned", p["placements"].size()}, {"updated", 0}, {"removed", 0}});
        }
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    auto count = [&server](const char* method) {
        return std::ranges::count_if(server.requests(), [method](const json& r) { return r["method"] == method; });
    };

    json first = run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    CHECK(first["unchanged"] == 0);
    CHECK(count("importAsset") == 1);
    json second = run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    CHECK(second["unchanged"] == 1);
    CHECK(count("importAsset") == 1);
    CHECK(count("placeInstances") == 1);

    run("selectComponents", {{"args", {"Crate"}}, {"faces", {0}}});
    run("deleteComponents");
    json third = run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    CHECK(third["unchanged"] == 0);
    CHECK(count("importAsset") == 2);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("content hashes: a send into a fresh level names what each asset is made from, so the project's copy is reused")
{
    setupPlacementTest();
    buildKit();
    fs::path dir = fs::temp_directory_path() / "lila_link_content_hash";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    // A project's assets (path -> content hash) outlive its levels; the level's groups do not.
    auto level = std::make_shared<json>(json::object());
    auto assets = std::make_shared<std::map<std::string, std::string>>();
    auto imported = std::make_shared<int>(0);
    auto mx = std::make_shared<std::mutex>();
    FakeServer server([level, assets, imported, mx](const json& req) -> std::optional<json> {
        const std::string m = req["method"];
        const json& p = req["params"];
        if (m == "hello")
            return FakeServer::reply(req, {{"project", "TestGame"}, {"features", {"progress", "placements", "placementHashes"}}});
        std::scoped_lock lock(*mx);
        if (m == "placementHashes")
            return FakeServer::reply(req, {{"groups", *level}});
        if (m == "importAsset")
        {
            const std::string path = p["contentPath"], hash = p.value("contentHash", "");
            if (!hash.empty() && assets->contains(path) && assets->at(path) == hash)
                return FakeServer::reply(req, {{"assets", {path}}, {"reused", true}, {"method", "reused"}, {"warnings", json::array()}, {"errors", json::array()}});
            ++*imported;
            (*assets)[path] = hash;
            return FakeServer::reply(req, {{"assets", {path}}, {"warnings", json::array()}, {"errors", json::array()}});
        }
        if (m == "placeInstances")
        {
            (*level)[p["group"].get<std::string>()] = {{"hash", p.value("hash", "")}, {"actors", p["mode"] == "hism" ? size_t(1) : p["placements"].size()}};
            return FakeServer::reply(req, {{"spawned", p["placements"].size()}, {"updated", 0}, {"removed", 0}});
        }
        return std::nullopt;
    });
    Link::ConnectOptions o;
    o.port = server.port();
    Link::get().connect(o);
    auto hashes = [&server]() {
        std::vector<std::string> out;
        for (const json& r : server.requests())
            if (r["method"] == "importAsset")
                out.push_back(r["params"].value("contentHash", ""));
        return out;
    };

    run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    REQUIRE(hashes().size() == 1);
    CHECK(hashes()[0].size() == 16);
    CHECK(*imported == 1);

    // A new level: nothing placed there yet, so the group goes again, but its asset is the project's already.
    {
        std::scoped_lock lock(*mx);
        *level = json::object();
    }
    json fresh = run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    REQUIRE(hashes().size() == 2);
    CHECK(hashes()[1] == hashes()[0]);
    CHECK(*imported == 1);
    CHECK(fresh["groups"][0]["import"].value("reused", false));

    // Edited content is a different hash, so it imports over the old asset.
    run("selectComponents", {{"args", {"Crate"}}, {"faces", {0}}});
    run("deleteComponents");
    run("unrealPlaceInstances", {{"args", {"Crate"}}, {"wait", true}});
    REQUIRE(hashes().size() == 3);
    CHECK(hashes()[2] != hashes()[0]);
    CHECK(*imported == 2);

    unregisterExporter("fbx:instanceSource");
    Link::get().disconnect();
    fs::remove_all(dir);
}

TEST_CASE("custom primitive data: names declared on the source map member attrs to slots, saved and undoable")
{
    setupPlacementTest();
    buildKit();
    scene::Graph& g = scene::Graph::current();
    run("addAttr", {{"args", {"BoltA"}}, {"longName", "wear"}, {"defaultValue", 0.75}});
    run("addAttr", {{"args", {"BoltA"}}, {"longName", "tint"}, {"attributeType", "double3"}});
    run("setAttr", {{"args", {"BoltA.tint", 0.1, 0.2, 0.3}}});
    run("addAttr", {{"args", {"Bolt"}}, {"longName", "tint"}, {"attributeType", "double3"}});
    CHECK(run("unrealCustomData", {{"args", {"BoltB"}}, {"query", true}}) == json::array({"random"}));
    run("unrealCustomData", {{"args", {"BoltB"}}, {"data", {"wear", "random", "tint"}}});
    // Declared on the group's source, whichever member was named.
    CHECK(customDataNames(*g.find("Bolt")) == std::vector<std::string>{"wear", "random", "tint"});

    json plan = placementPlan(g, {g.find("Bolt")->id()}, PlacementOptions{});
    const json& bolt = plan["groups"][0];
    CHECK(bolt["customData"] == json::parse(R"([{"name":"wear","slot":0,"width":1},{"name":"random","slot":1,"width":1},{"name":"tint","slot":2,"width":3}])"));
    const json* a = placementOf(bolt, "BoltA");
    const json* b = placementOf(bolt, "BoltB");
    REQUIRE(a);
    REQUIRE(b);
    REQUIRE((*a)["customData"].size() == 5);
    CHECK((*a)["customData"][0].get<double>() == doctest::Approx(0.75));
    CHECK((*a)["customData"][1].get<double>() >= 0.0);
    CHECK((*a)["customData"][3].get<double>() == doctest::Approx(0.2));
    // A member without the attr sends zeros in its slots.
    CHECK((*b)["customData"][0] == 0.0);
    CHECK((*b)["customData"][4] == 0.0);

    run("undo");
    CHECK(customDataNames(*g.find("Bolt")) == std::vector<std::string>{"random"});
    run("redo");
    const fs::path path = fs::temp_directory_path() / "lila_link_custom_data.lila";
    run("saveScene", {{"args", {path.generic_string()}}});
    run("newScene");
    run("openScene", {{"args", {path.generic_string()}}});
    CHECK(run("unrealCustomData", {{"args", {"Bolt"}}, {"query", true}}) == json::array({"wear", "random", "tint"}));
    run("unrealCustomData", {{"args", {"Bolt"}}, {"data", json::array()}});
    CHECK(placementPlan(g, {g.find("Bolt")->id()}, PlacementOptions{})["groups"][0]["placements"][0]["customData"].empty());
    CHECK_THROWS_AS(run("unrealCustomData", {{"args", {"Bolt"}}, {"data", {1, 2}}}), CommandError);
    fs::remove(path);
}

TEST_CASE("custom primitive data: geometry nodes instances send their instance attributes")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    // Store each instance's index as "wear" on the instance domain, after joining.
    const std::string store = gnAdd("Boulders", "StoreNamedAttribute"), index = gnAdd("Boulders", "Index");
    gnSet("Boulders", store + ".domain", "instance");
    gnSet("Boulders", store + ".name", "wear");
    run("gnDisconnect", {{"args", {"Boulders", "GroupOutput.geometry"}}});
    gnLink("Boulders", "JoinGeometry.geometry", store + ".geometry");
    gnLink("Boulders", index + ".value", store + ".value");
    gnLink("Boulders", store + ".geometry", "GroupOutput.geometry");
    run("unrealCustomData", {{"args", {"Ground"}}, {"data", {"wear", "random"}}});

    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Ground")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 3);
    const json& part = plan["groups"][1];
    CHECK(part["customData"][0]["width"] == 1);
    REQUIRE(part["placements"].size() > 10);
    // Instance k of the first joined part is top-level instance k.
    for (size_t k : {0u, 5u, 10u})
        CHECK(part["placements"][k]["customData"][0].get<double>() == doctest::Approx(double(k)));
    const size_t each = part["placements"].size();
    CHECK(plan["groups"][2]["placements"][3]["customData"][0].get<double>() == doctest::Approx(double(each + 3)));
}

TEST_CASE("instance colours: geometry nodes instances send their colour as custom data 0-3, uncoloured meshes get M_LilaInstanceColor")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    const std::string store = gnAdd("Boulders", "StoreNamedAttribute");
    gnSet("Boulders", store + ".dataType", "color");
    gnSet("Boulders", store + ".domain", "instance");
    gnSet("Boulders", store + ".name", "color");
    gnSet("Boulders", store + ".value", json::array({0.5, 0.25, 0.125, 1.0}));
    run("gnDisconnect", {{"args", {"Boulders", "GroupOutput.geometry"}}});
    gnLink("Boulders", "JoinGeometry.geometry", store + ".geometry");
    gnLink("Boulders", store + ".geometry", "GroupOutput.geometry");
    // A declared slot follows the colour's four.
    run("unrealCustomData", {{"args", {"Ground"}}, {"data", {"random"}}});

    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Ground")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 3);
    const json& part = plan["groups"][1];
    CHECK(part["customData"] == json::parse(R"([{"name":"color","slot":0,"width":4},{"name":"random","slot":4,"width":1}])"));
    CHECK(part["instanceColor"] == true);
    CHECK(part["instanceColorMaterial"] == true);
    const json& c = part["placements"][0]["customData"];
    REQUIRE(c.size() == 5);
    CHECK(c[0].get<double>() == doctest::Approx(0.5));
    CHECK(c[1].get<double>() == doctest::Approx(0.25));
    CHECK(c[2].get<double>() == doctest::Approx(0.125));
    CHECK(c[3].get<double>() == doctest::Approx(1.0));

    fs::path dir = fs::temp_directory_path() / "lila_link_instance_color";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    for (const bool feature : {true, false})
    {
        FakeServer server([feature](const json& req) -> std::optional<json> {
            const std::string m = req["method"];
            if (m == "hello")
                return FakeServer::reply(req, {{"project", "TestGame"},
                                               {"features", feature ? json{"progress", "placements", "instanceColor"} : json{"progress", "placements"}}});
            if (m == "importAsset")
                return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
            if (m == "placeInstances")
                return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
            if (m == "removePlacements")
                return FakeServer::reply(req, {{"removed", 0}});
            if (m == "instanceColorMaterials")
                return FakeServer::reply(req, {{"assigned", req["params"]["assets"].size()}});
            return std::nullopt;
        });
        Link::ConnectOptions o;
        o.port = server.port();
        Link::get().connect(o);
        json done = run("unrealPlaceInstances", {{"args", {"Ground"}}, {"wait", true}, {"resend", true}});
        int calls = 0;
        for (const json& r : server.requests())
            if (r["method"] == "instanceColorMaterials")
            {
                ++calls;
                CHECK(r["params"]["assets"].size() == 2);
            }
        CHECK(calls == (feature ? 1 : 0));
        REQUIRE(done.contains("instanceColor"));
        CHECK(done["instanceColor"]["groups"] == 2);
        if (feature)
            CHECK(done["instanceColor"]["materials"]["assigned"] == 2);
        else
            CHECK(done["instanceColor"].contains("note"));
        Link::get().disconnect();
    }
    unregisterExporter("fbx:instanceSource");
    fs::remove_all(dir);
}

TEST_CASE("instance colours: a second tone follows the colour as custom data 4-7, declared slots after it")
{
    setupPlacementTest();
    if (!ops::find("gnAddNode"))
    {
        gn::registerTypes();
        gn::registerCommands();
    }
    buildScatter();
    // Only the second tone is stored: the colour's four stay (zero) so the second tone is always at 4.
    const std::string store = gnAdd("Boulders", "StoreNamedAttribute");
    gnSet("Boulders", store + ".dataType", "color");
    gnSet("Boulders", store + ".domain", "instance");
    gnSet("Boulders", store + ".name", "color2");
    gnSet("Boulders", store + ".value", json::array({0.1, 0.2, 0.3, 1.0}));
    run("gnDisconnect", {{"args", {"Boulders", "GroupOutput.geometry"}}});
    gnLink("Boulders", "JoinGeometry.geometry", store + ".geometry");
    gnLink("Boulders", store + ".geometry", "GroupOutput.geometry");
    run("unrealCustomData", {{"args", {"Ground"}}, {"data", {"random"}}});

    scene::Graph& g = scene::Graph::current();
    json plan = placementPlan(g, {g.find("Ground")->id()}, PlacementOptions{});
    REQUIRE(plan["groups"].size() == 3);
    const json& part = plan["groups"][1];
    CHECK(part["customData"] ==
          json::parse(R"([{"name":"color","slot":0,"width":4},{"name":"color2","slot":4,"width":4},{"name":"random","slot":8,"width":1}])"));
    CHECK(part["instanceColor"] == true);
    CHECK(part["instanceColor2"] == true);
    const json& c = part["placements"][0]["customData"];
    REQUIRE(c.size() == 9);
    for (int k = 0; k < 4; ++k)
        CHECK(c[k].get<double>() == 0.0);
    CHECK(c[4].get<double>() == doctest::Approx(0.1));
    CHECK(c[5].get<double>() == doctest::Approx(0.2));
    CHECK(c[6].get<double>() == doctest::Approx(0.3));
    CHECK(c[7].get<double>() == doctest::Approx(1.0));

    // Both tones: the colour keeps 0-3.
    const std::string store1 = gnAdd("Boulders", "StoreNamedAttribute");
    gnSet("Boulders", store1 + ".dataType", "color");
    gnSet("Boulders", store1 + ".domain", "instance");
    gnSet("Boulders", store1 + ".name", "color");
    gnSet("Boulders", store1 + ".value", json::array({0.9, 0.8, 0.7, 0.5}));
    run("gnDisconnect", {{"args", {"Boulders", "GroupOutput.geometry"}}});
    gnLink("Boulders", store + ".geometry", store1 + ".geometry");
    gnLink("Boulders", store1 + ".geometry", "GroupOutput.geometry");
    plan = placementPlan(g, {g.find("Ground")->id()}, PlacementOptions{});
    const json& both = plan["groups"][1]["placements"][0]["customData"];
    REQUIRE(both.size() == 9);
    CHECK(both[0].get<double>() == doctest::Approx(0.9));
    CHECK(both[3].get<double>() == doctest::Approx(0.5));
    CHECK(both[4].get<double>() == doctest::Approx(0.1));

    fs::path dir = fs::temp_directory_path() / "lila_link_instance_color2";
    fs::remove_all(dir);
    PrefStore::get().set("unreal", "/cacheDir", dir.generic_string());
    registerExporter("fbx:instanceSource", [](const std::vector<scene::NodeId>&, const fs::path& path, std::string*) {
        std::ofstream(path) << "fbx";
        return true;
    });
    for (const bool feature : {true, false})
    {
        FakeServer server([feature](const json& req) -> std::optional<json> {
            const std::string m = req["method"];
            if (m == "hello")
                return FakeServer::reply(req, {{"project", "TestGame"},
                                               {"features", feature ? json{"progress", "placements", "instanceColor", "instanceColor2"}
                                                                    : json{"progress", "placements", "instanceColor"}}});
            if (m == "importAsset")
                return FakeServer::reply(req, {{"assets", {req["params"]["contentPath"]}}, {"warnings", json::array()}, {"errors", json::array()}});
            if (m == "placeInstances")
                return FakeServer::reply(req, {{"spawned", req["params"]["placements"].size()}, {"updated", 0}, {"removed", 0}});
            if (m == "removePlacements")
                return FakeServer::reply(req, {{"removed", 0}});
            if (m == "instanceColorMaterials")
                return FakeServer::reply(req, {{"assigned", req["params"]["assets"].size()}});
            return std::nullopt;
        });
        Link::ConnectOptions o;
        o.port = server.port();
        Link::get().connect(o);
        json done = run("unrealPlaceInstances", {{"args", {"Ground"}}, {"wait", true}, {"resend", true}});
        REQUIRE(done.contains("instanceColor"));
        CHECK(done["instanceColor"]["secondToneGroups"] == 2);
        CHECK(done["instanceColor"].contains("secondToneNote") == !feature);
        Link::get().disconnect();
    }
    unregisterExporter("fbx:instanceSource");
    fs::remove_all(dir);
}

TEST_CASE("instanced groups: the holder takes the components every member shares, each instance keeps its own")
{
    setupPlacementTest();
    buildKit();
    for (const char* m : {"Bolt", "BoltA", "BoltB"})
    {
        run("addAttr", {{"args", {m}}, {"longName", "kind"}, {"dataType", "string"}, {"category", "Prop"}});
        run("setAttr", {{"args", {std::string(m) + ".kind", "bolt"}}});
        run("addAttr", {{"args", {m}}, {"longName", "hp"}, {"defaultValue", 10.0}, {"category", "Health"}});
    }
    run("setAttr", {{"args", {"BoltB.hp", 25.0}}});
    scene::Graph& g = scene::Graph::current();
    PlacementOptions o;
    o.mode = "hism";
    json plan = placementPlan(g, {g.find("Bolt")->id()}, o);
    const json& bolt = plan["groups"][0];
    REQUIRE(bolt["components"].size() == 1);
    CHECK(bolt["components"][0]["name"] == "Prop");
    const json* b = placementOf(bolt, "BoltB");
    REQUIRE(b);
    CHECK((*b)["components"][1]["attrs"]["hp"] == 25.0);
    // Actors carry their own components; the group has none to share.
    CHECK_FALSE(placementPlan(g, {g.find("Bolt")->id()}, PlacementOptions{})["groups"][0].contains("components"));
}

TEST_CASE("validation gates Unreal sends: blocking issues stop them, the rest come back as issues")
{
    setupPlacementTest();
    buildKit();
    namespace dg = scene::diagnostics;
    int runs = 0;
    bool block = true;
    dg::validators().put("testUnrealGate", {"test", [&] {
                                                ++runs;
                                                dg::Issue i;
                                                i.target = "BoltA";
                                                i.message = "no collision proxy";
                                                if (block)
                                                    i.blocks = {"unreal"};
                                                dg::report(std::move(i));
                                                dg::Issue other;
                                                other.target = "Crate";
                                                other.message = "not sent, so not reported";
                                                other.blocks = {"unreal"};
                                                dg::report(std::move(other));
                                            }});
    try
    {
        run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}});
        FAIL("the blocking issue let the send through");
    }
    catch (const CommandError& e)
    {
        CHECK(std::string(e.what()).find("no collision proxy") != std::string::npos);
    }
    CHECK(runs == 1);

    block = false;
    json plan = run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}});
    CHECK(runs == 2);
    REQUIRE(plan["issues"].size() == 1);
    CHECK(plan["issues"][0]["target"] == "BoltA");
    CHECK(plan["issues"][0]["severity"] == "warning");
    // validate=False keeps the issues already reported without running the validators again.
    plan = run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}, {"validate", false}});
    CHECK(runs == 2);
    CHECK(plan["issues"].size() == 1);

    // An issue blocking only another command is reported, not obeyed.
    dg::clear();
    dg::Issue save;
    save.target = "Bolt";
    save.message = "unsaved texture";
    save.blocks = {"exportFbx"};
    dg::report(std::move(save));
    plan = run("unrealPlaceInstances", {{"args", {"Bolt"}}, {"dryRun", true}, {"validate", false}});
    REQUIRE(plan["issues"].size() == 1);
    CHECK(plan["issues"][0]["blocks"] == json::array({"exportFbx"}));
    dg::validators().remove("testUnrealGate");
    dg::clear();
}
