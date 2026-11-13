#include <doctest/doctest.h>

#include "TestUtil.h"
#include "extensions/ExtensionManager.h"
#include "scene/Grapple.h"
#include "scene/Graph.h"
#include "scene/Simulation.h"
#include "scene/Transform.h"

#include <filesystem>
#include <map>

using namespace lila;
using namespace lila::scene;
using namespace simtest;

namespace grapplephysicstest
{
void loadPhysics()
{
    if (!sim::solverTypes().find("physicsSolver"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PHYSICS_PACKAGE) == "PhysicsAnimation");
}

/** A UE-named stick biped under its own root joint; facing -1 turns it round. Returns joint paths by name. */
std::map<std::string, std::string> biped(const std::string& rootName, glm::dvec3 offset, double facing)
{
    Graph& g = Graph::current();
    auto at = [&](double x, double y, double z) { return v3(offset.x + x * facing, offset.y + y, offset.z + z * facing); };
    std::map<std::string, std::string> path;
    const std::string root = run("joint", {{"position", at(0, 0, 0)}, {"name", rootName}, {"parent", ""}, {"skipSelect", true}});
    path["root"] = "|" + root.substr(root.rfind('|') + 1);
    auto j = [&](const std::string& name, double x, double y, double z, const std::string& parent) {
        const std::string made = run("joint", {{"position", at(x, y, z)}, {"name", name}, {"parent", path.at(parent)}, {"skipSelect", true}});
        const std::string leaf = made.substr(made.rfind('|') + 1);
        for (NodeId c : g.children(g.find(path.at(parent))->id()))
            if (g.node(c)->name() == leaf)
                path[name] = path.at(parent) + "|" + leaf;
    };
    j("pelvis", 0, 100, 0, "root");
    j("spine_01", 0, 112, 0, "pelvis");
    j("spine_02", 0, 126, 0, "spine_01");
    j("neck_01", 0, 146, 0, "spine_02");
    j("head", 0, 160, 0, "neck_01");
    for (const std::string side : {"l", "r"})
    {
        const double s = side == "l" ? 1.0 : -1.0;
        j("upperarm_" + side, 18 * s, 140, 0, "spine_02");
        j("lowerarm_" + side, 46 * s, 140, -2, "upperarm_" + side);
        j("hand_" + side, 72 * s, 140, 0, "lowerarm_" + side);
        j("thigh_" + side, 10 * s, 95, 0, "pelvis");
        j("calf_" + side, 10 * s, 52, 2, "thigh_" + side);
        j("foot_" + side, 10 * s, 8, 0, "calf_" + side);
    }
    return path;
}
} // namespace grapplephysicstest

using namespace grapplephysicstest;

TEST_CASE("grapple ragdoll: contacts pull the two bodies together and the bake is deterministic")
{
    loadPhysics();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 1.0}, {"fps", 30.0}});
    const auto a = biped("a_root", {0, 0, 0}, 1.0);
    const auto b = biped("b_root", {0, 0, 80}, -1.0);
    const json made = run("grappleContact", {{"args", {a.at("hand_r"), b.at("lowerarm_l")}}, {"kind", "hold"}, {"blend", 0.0}});
    const std::string contact = made["contact"].get<std::string>(), grapple = made["grapple"].get<std::string>();
    // Let go half way through: the bodies carry on with what the contact gave them.
    run("setKeyframe", {{"args", {contact + ".weight"}}, {"time", 0.5}, {"value", 1.0}});
    run("setKeyframe", {{"args", {contact + ".weight"}}, {"time", 0.5 + 1.0 / 30.0}, {"value", 0.0}});

    const auto [s0, t0] = grapple::contactPoints(Graph::current(), Graph::current().find(contact)->id());
    const double gap = glm::length(t0 - s0);
    REQUIRE(gap > 20.0);

    const json rag = run("grappleRagdoll", {{"args", {grapple}}});
    const std::string solver = rag["solver"].get<std::string>();
    for (const auto& [root, count] : rag["markers"].items())
        CHECK(count.get<int>() >= 12);
    run("setAttr", {{"args", {solver + ".gravity", v3(0, 0, 0)}}});

    const std::string first = run("recordPhysics", {{"args", {solver}}}).get<std::string>();
    Graph& g = Graph::current();
    g.setTime(12.0 / 30.0);
    g.evaluate();
    const auto [s1, t1] = grapple::contactPoints(g, g.find(contact)->id());
    CHECK(glm::length(t1 - s1) < gap * 0.5);
    const std::string keys = layerKeys(first);
    CHECK_FALSE(keys.empty());

    run("deleteAnimLayer", {{"args", {first}}});
    run("clearSimCache", {{"args", {solver}}});
    const std::string second = run("recordPhysics", {{"args", {solver}}}).get<std::string>();
    CHECK(layerKeys(second) == keys);
}

TEST_CASE("physics: a skeleton-only Manny's template ragdoll stands for 2 s under gravity, a body's mass")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    loadPhysics();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", 2.0}, {"fps", 30.0}});
    run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}, {"setPlayback", false}});
    const json markers = run("physicsMarkersFromTemplate", {{"args", {"root"}}});
    REQUIRE(markers.size() >= 20);
    Graph& g = Graph::current();
    double mass = 0.0;
    for (const json& m : markers)
    {
        const Node& n = *g.find(m.get<std::string>());
        auto num = [&](const char* a) { return n.as<double>(n.type().attrIndex(a)); };
        const glm::dvec3 e = n.as<glm::dvec3>(n.type().attrIndex("extents"));
        const double r = num("radius");
        const int64_t kind = n.as<int64_t>(n.type().attrIndex("shape"));
        const double volume = kind == 0 ? 3.14159265 * r * r * (num("length") + 4.0 / 3.0 * r) : kind == 1 ? e.x * e.y * e.z : 4.0 / 3.0 * 3.14159265 * r * r * r;
        mass += volume * num("density") / 1e6;
    }
    CHECK(mass > 50.0);
    CHECK(mass < 140.0);

    const std::string physics = run("recordPhysics", {{"args", {markers[0]}}}).get<std::string>();
    REQUIRE(g.find(physics));
    auto height = [&](const char* joint) { return glm::dvec3(g.find(joint)->as<glm::dmat4>(Transform::kWorldMatrix)[3]).y; };
    g.setTime(0.0);
    g.evaluate();
    const double head0 = height("head"), pelvis0 = height("pelvis"), hand0 = height("hand_r");
    for (int f = 0; f <= 60; f += 5)
    {
        g.setTime(f / 30.0);
        g.evaluate();
        CAPTURE(f);
        CHECK(height("head") > head0 * 0.9);
        CHECK(height("pelvis") > pelvis0 * 0.9);
        CHECK(height("hand_r") > hand0 * 0.8);
    }
}
