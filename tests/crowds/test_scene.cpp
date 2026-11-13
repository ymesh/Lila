// The Crowds package through its commands, on gait cycles (no model needed): spawning, the cached simulation through the
// framework (deterministic, resumed after an edit), and bake onto real characters matching the simulated agents.

#include <doctest/doctest.h>

#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <cmath>

using namespace lila;
using json = nlohmann::json;

namespace
{
json runScene(const char* name, json call = json::object()) { return ops::execute(name, call); }

void loadPackages()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_RIGTEMPLATES_PACKAGE) == "RigTemplates");
    REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_CROWDS_PACKAGE) == "Crowds");
}

/** A Manny skeleton with a capsule skinned to each leg, the torso and the head, under a group named Manny. */
void makeCharacter()
{
    runScene("buildRigSkeleton", {{"args", {"UE5_Manny"}}, {"rig", false}, {"includeIk", false}});
    const std::vector<std::pair<std::vector<std::string>, glm::dvec3>> parts = {
        {{"thigh_l", "calf_l", "foot_l"}, {12, 50, 0}}, {{"thigh_r", "calf_r", "foot_r"}, {-12, 50, 0}},
        {{"pelvis", "spine_01", "spine_03", "spine_05"}, {0, 120, 0}}, {{"neck_01", "head"}, {0, 165, 0}}};
    json group = json::array({"root"});
    int k = 0;
    for (const auto& [joints, at] : parts)
    {
        const std::string name = "part" + std::to_string(k++);
        runScene("polyCapsule", {{"radius", 10.0}, {"height", 60.0}, {"name", name}});
        runScene("setAttr", {{"args", {name + ".translate", json::array({at.x, at.y, at.z})}}});
        json args = json::array();
        for (const std::string& j : joints)
            args.push_back(j);
        args.push_back(name);
        runScene("skinCluster", {{"args", args}, {"bindTo", "selected"}});
        group.push_back(name);
    }
    runScene("group", {{"args", group}, {"name", "Manny"}});
}

double gap(const json& a, const json& b) { return std::hypot(a[0].get<double>() - b[0].get<double>(), a[2].get<double>() - b[2].get<double>()); }
} // namespace

TEST_CASE("crowd scene: cached, deterministic, resumed after an edit, and baked onto characters")
{
    loadPackages();
    runScene("newScene");
    runScene("playbackOptions", {{"minTime", 0.0}, {"maxTime", 4.0}, {"fps", 30.0}});
    makeCharacter();
    const std::string crowd = runScene("crowdCreate", {{"characters", {"Manny"}}, {"startFrame", 0}}).get<std::string>();
    runScene("setAttr", {{"args", {crowd + ".detail", 2}}});
    const json spawned = runScene("crowdSpawn", {{"args", {crowd}}, {"count", 12}, {"behaviour", "wander"}, {"radius", 800.0}, {"seed", 5}});
    CHECK(spawned["agents"] == 12);

    const json first = runScene("crowdSimulate", {{"args", {crowd}}, {"frame", 90}});
    CHECK(first["agents"] == 12);
    CHECK(first["device"] == "none");
    CHECK(first["cachedTo"] == 90);
    const json at90 = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 90}});
    int moved = 0;
    const json at0 = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 0}});
    for (size_t i = 0; i < at90.size(); ++i)
        moved += gap(at0[i]["position"], at90[i]["position"]) > 50.0 ? 1 : 0;
    CHECK(moved >= 8);

    // A cleared cache simulates to the same crowd.
    runScene("clearSimCache", {{"args", {crowd}}});
    const json again = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 90}});
    for (size_t i = 0; i < at90.size(); ++i)
        CHECK(gap(at90[i]["position"], again[i]["position"]) < 1e-6);

    // A second group changes the setup: the cache starts again, and the first group's agents keep what they did alone
    // only where nobody else is near, so just check it re-simulates to the end.
    runScene("crowdSpawn", {{"args", {crowd}}, {"count", 4}, {"behaviour", "idle"}, {"center", {3000.0, 0.0, 3000.0}}});
    const json info = runScene("crowdSimulate", {{"args", {crowd}}, {"frame", 60}});
    CHECK(info["agents"] == 16);
    CHECK(info["cachedTo"] == 60);
    const json idle = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 60}});
    for (size_t i = 12; i < 16; ++i)
        CHECK(idle[i]["doing"] == "idle");

    // Bake two agents: each copy's pelvis follows its agent on the ground.
    const json baked = runScene("crowdBake", {{"args", {crowd}}, {"agents", {0, 5}}, {"start", 0}, {"end", 60}});
    REQUIRE(baked["agents"].size() == 2);
    CHECK(baked["agents"][0]["frames"] == 61);
    CHECK(baked["warning"].is_string()); // cycles throughout: the report says so
    const json agents = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 45}});
    runScene("currentTime", {{"args", {1.5}}});
    for (const json& b : baked["agents"])
    {
        const std::string pelvis = b["node"].get<std::string>() + "|root|pelvis";
        const json p = runScene("xform", {{"args", {pelvis}}, {"query", true}, {"worldSpace", true}, {"translation", true}});
        const json& want = agents[b["agent"].get<size_t>()]["position"];
        CHECK(gap(p, want) < 25.0);
    }
    // Undo takes the whole bake back.
    runScene("undo");
    CHECK(scene::Graph::current().find(baked["group"].get<std::string>()) == nullptr);
}

TEST_CASE("crowd scene: spawn sources and errors")
{
    loadPackages();
    runScene("newScene");
    const std::string crowd = runScene("crowdCreate").get<std::string>();
    runScene("curveEP", {{"points", json::array({json::array({0.0, 0.0, 0.0}), json::array({2000.0, 0.0, 0.0})})}, {"degree", 1}, {"name", "line"}});
    const json along = runScene("crowdSpawn", {{"args", {crowd}}, {"count", 5}, {"behaviour", "followPath"}, {"path", "line"}, {"source", "line"}});
    CHECK(along["agents"] == 5);
    runScene("polyPlane", {{"width", 1000.0}, {"depth", 1000.0}, {"name", "floor"}});
    CHECK(runScene("crowdSpawn", {{"args", {crowd}}, {"count", 10}, {"source", "floor"}})["agents"] == 10);
    CHECK_THROWS_AS(runScene("crowdSpawn", {{"args", {crowd}}, {"behaviour", "queue"}}), CommandError);
    CHECK_THROWS_AS(runScene("crowdSpawn", {{"args", {crowd}}, {"behaviour", "goTo"}}), CommandError);
    CHECK_THROWS_AS(runScene("crowdSpawn", {{"args", {crowd}}, {"behaviour", "dance"}}), CommandError);
    const json groups = runScene("crowdGroups", {{"args", {crowd}}});
    CHECK(groups.size() == 2);
    runScene("crowdGroups", {{"args", {crowd}}, {"remove", groups[0]["name"]}});
    CHECK(runScene("crowdGroups", {{"args", {crowd}}}).size() == 1);
}

TEST_CASE("crowd scene: relocating the scene moves the crowd and its simulation rigidly")
{
    loadPackages();
    runScene("newScene");
    runScene("playbackOptions", {{"minTime", 0.0}, {"maxTime", 3.0}, {"fps", 30.0}});
    const std::string crowd = runScene("crowdCreate", {{"startFrame", 0}}).get<std::string>();
    runScene("setAttr", {{"args", {crowd + ".detail", 2}}});
    runScene("crowdSpawn", {{"args", {crowd}}, {"count", 10}, {"behaviour", "wander"}, {"center", {400.0, 0.0, -300.0}}, {"radius", 600.0}, {"seed", 2}});
    runScene("crowdSpawn", {{"args", {crowd}}, {"count", 4}, {"behaviour", "goTo"}, {"goal", {1500.0, 0.0, 800.0}}, {"center", {0.0, 0.0, 0.0}}, {"radius", 200.0}});
    const json before0 = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 0}});
    const json before = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 75}});

    const glm::dvec3 offset(12000.0, 0.0, -3500.0);
    runScene("relocateScene", {{"offset", {offset.x, offset.y, offset.z}}});
    const json groups = runScene("crowdGroups", {{"args", {crowd}}});
    CHECK(groups[0]["center"][0].get<double>() == doctest::Approx(400.0 + offset.x));
    CHECK(groups[1]["goal"][2].get<double>() == doctest::Approx(800.0 + offset.z));

    const json after0 = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 0}});
    const json after = runScene("crowdAgents", {{"args", {crowd}}, {"frame", 75}});
    REQUIRE(after.size() == before.size());
    double worst = 0.0;
    for (size_t i = 0; i < after.size(); ++i)
        for (const auto* pair : {&before0, &before})
        {
            const json& b = (*pair)[i]["position"];
            const json& a = (pair == &before0 ? after0 : after)[i]["position"];
            worst = std::max({worst, std::abs(a[0].get<double>() - b[0].get<double>() - offset.x), std::abs(a[2].get<double>() - b[2].get<double>() - offset.z)});
        }
    CHECK(worst < 0.5);

    // Undo puts the groups back where they were.
    runScene("undo");
    CHECK(runScene("crowdGroups", {{"args", {crowd}}})[0]["center"][0].get<double>() == doctest::Approx(400.0));
}

TEST_CASE("crowdSendToUnreal mass: the network's lanes in Unreal space and the network groups with their speeds and shares")
{
    loadPackages();
    runScene("newScene");
    runScene("curveEP", {{"points", json::array({json::array({0.0, 0.0, 0.0}), json::array({2000.0, 0.0, 0.0}), json::array({2000.0, 0.0, 1500.0})})},
                    {"degree", 1},
                    {"name", "walk"}});
    const std::string crowd = runScene("crowdCreate", {{"network", {"walk"}}}).get<std::string>();
    runScene("crowdSpawn", {{"args", {crowd}}, {"count", 6}, {"behaviour", "wander"}, {"onNetwork", true}, {"speedMin", 1.0}, {"speedMax", 1.0}, {"seed", 2}});
    runScene("crowdSpawn", {{"args", {crowd}}, {"count", 2}, {"behaviour", "idle"}, {"center", {500.0, 0.0, 500.0}}});
    const json sent = runScene("crowdSendToUnreal", {{"args", {crowd}}, {"mass", true}, {"dryRun", true}})["params"];
    REQUIRE(sent["lanes"].size() >= 1);
    // Whatever the axes, a 2000 cm straight run stays 2000 cm long.
    const json& pts = sent["lanes"][0]["points"];
    REQUIRE(pts.size() >= 2);
    const glm::dvec3 a(pts[0][0].get<double>(), pts[0][1].get<double>(), pts[0][2].get<double>());
    const glm::dvec3 b(pts[1][0].get<double>(), pts[1][1].get<double>(), pts[1][2].get<double>());
    CHECK(glm::length(b - a) == doctest::Approx(2000.0).epsilon(1e-6));
    // Only the group on the network goes, at the walk's 130 cm/s.
    REQUIRE(sent["groups"].size() == 1);
    CHECK(sent["groups"][0]["share"] == 6);
    CHECK(sent["groups"][0]["speed"].get<double>() == doctest::Approx(130.0));
    CHECK(sent["count"] == 6);
    CHECK(sent["minGap"].get<double>() >= 100.0);
    CHECK(runScene("crowdSendToUnreal", {{"args", {crowd}}, {"mass", true}, {"count", 40}, {"density", 10.0}, {"dryRun", true}})["params"]["count"] == 40);

    runScene("newScene");
    const std::string bare = runScene("crowdCreate").get<std::string>();
    runScene("crowdSpawn", {{"args", {bare}}, {"count", 3}, {"behaviour", "wander"}});
    CHECK_THROWS(runScene("crowdSendToUnreal", {{"args", {bare}}, {"mass", true}, {"dryRun", true}}));
}
