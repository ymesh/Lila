#include "TestUtil.h"

#include "scene/Diagnostics.h"
#include "scene/Skin.h"

#include <filesystem>

using namespace rigtest;
using namespace lila::scene;

namespace
{
json weightsAt(const std::string& mesh, int p) { return run("skinPercent", {{"args", {mesh}}, {"vertices", {p}}})[0]["weights"]; }

double sum(const json& w)
{
    double s = 0.0;
    for (auto& [k, v] : w.items())
        s += v.get<double>();
    return s;
}

/** A middle point given weight on all three bones, the case where removed weight could go to two places. */
int threeWay(const std::string& mesh)
{
    int i = 0;
    for (const json& p : run("skinPercent", {{"args", {mesh}}}))
    {
        if (p["weights"].value("bone_b", 0.0) > 0.5)
        {
            run("skinPercent", {{"args", {mesh}}, {"vertices", {i}}, {"transformValue", {{"bone_a", 0.3}, {"bone_b", 0.4}, {"bone_c", 0.3}}}});
            return i;
        }
        ++i;
    }
    return -1;
}

void lockJoint(const std::string& mesh, const std::string& joint, bool lock)
{
    NodeId sk = skin::find(graph(), id(mesh));
    for (const auto& i : skin::influences(graph(), sk))
        if (i.name == joint)
            run("setAttr", {{"args", {graph().node(i.link)->path() + ".lockWeights", lock}}});
}

const diagnostics::Issue* skinIssue()
{
    for (const auto& i : diagnostics::issues())
        if (i.source == "skinWeights")
            return &i;
    return nullptr;
}

/** Sum over edges of how far each edge's posed length is from its rest length. */
double stretch(const std::string& mesh)
{
    NodeId sk = skin::find(graph(), id(mesh));
    MeshPtr rest = skin::inputMesh(graph(), sk);
    MeshPtr posed = outMesh(mesh);
    double total = 0.0;
    for (geo::Index e = 0; e < rest->edgeCapacity(); ++e)
    {
        if (!rest->edgeValid(e))
            continue;
        const geo::Index a = rest->edgePoint(e, 0), b = rest->edgePoint(e, 1);
        total += std::abs(glm::length(posed->position(a) - posed->position(b)) - glm::length(rest->position(a) - rest->position(b)));
    }
    return total;
}
} // namespace

TEST_CASE("binding marks the weights automatic and warns until they are accepted")
{
    std::string mesh = skinnedCylinder();
    json info = run("skinInfo", {{"args", {mesh}}});
    CHECK(info["automatic"].get<int>() == info["points"].get<int>());
    const diagnostics::Issue* issue = skinIssue();
    REQUIRE(issue);
    CHECK(issue->severity == diagnostics::Severity::Warning);
    CHECK(issue->message.find("automatic weights") != std::string::npos);

    CHECK(run("selectAutomaticWeights", {{"args", {mesh}}}).get<int>() == info["points"].get<int>());
    CHECK(run("acceptSkinWeights", {{"args", {mesh}}, {"vertices", {0, 1, 2}}}).get<int>() == 3);
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"].get<int>() == info["points"].get<int>() - 3);
    // With no vertices given it takes the selected ones: all of them, from selectAutomaticWeights.
    run("acceptSkinWeights", {{"args", {mesh}}});
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"] == 0);
    CHECK_FALSE(skinIssue());

    // Weights survive save and load with their flags.
    run("undo");
    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "automatic.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"].get<int>() == info["points"].get<int>() - 3);
}

TEST_CASE("strict flood refuses split weight; a receiver takes it exactly")
{
    std::string mesh = skinnedCylinder(20);
    const int p = threeWay(mesh);
    REQUIRE(p >= 0);
    const json before = weightsAt(mesh, p);

    json r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"value", 0.0}, {"vertices", {p}}, {"strict", true}});
    CHECK(r["refused"]["ambiguous"] == 1);
    CHECK(r["refusedPoints"] == json::array({p}));
    CHECK(weightsAt(mesh, p) == before);

    // With a receiver only bone_c and bone_b move, by exactly bone_c's old weight.
    r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"receiver", "bone_b"}, {"value", 0.0}, {"vertices", {p}}});
    CHECK(r["painted"] == 1);
    const json after = weightsAt(mesh, p);
    CHECK(after.value("bone_c", 0.0) == doctest::Approx(0.0));
    CHECK(after.value("bone_a", 0.0) == doctest::Approx(before.value("bone_a", 0.0)));
    CHECK(after.value("bone_b", 0.0) == doctest::Approx(before.value("bone_b", 0.0) + before.value("bone_c", 0.0)));
    CHECK(sum(after) == doctest::Approx(1.0));

    // The one other unlocked joint takes it without a receiver.
    lockJoint(mesh, "bone_a", true);
    const json locked = weightsAt(mesh, p);
    r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"value", 0.1}, {"vertices", {p}}, {"strict", true}});
    CHECK(r["painted"] == 1);
    CHECK(weightsAt(mesh, p).value("bone_a", 0.0) == doctest::Approx(locked.value("bone_a", 0.0)));
}

TEST_CASE("refusals name their reason and strict off marks the split automatic")
{
    std::string mesh = skinnedCylinder(20);
    run("acceptSkinWeights", {{"args", {mesh}}});
    const int p = threeWay(mesh);
    REQUIRE(p >= 0);
    // Every other joint locked: lowering bone_c has nowhere to go.
    lockJoint(mesh, "bone_a", true);
    lockJoint(mesh, "bone_b", true);
    json r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"value", 0.0}, {"vertices", {p}}});
    CHECK(r["refused"]["nowhereToGo"] == 1);
    CHECK(r["hint"].get<std::string>().find("unlock") != std::string::npos);
    r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_a"}, {"value", 0.0}, {"vertices", {p}}});
    CHECK(r["refused"]["locked"] == 1);
    lockJoint(mesh, "bone_a", false);
    lockJoint(mesh, "bone_b", false);

    r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"value", 0.0}, {"vertices", {p}}, {"strict", false}});
    CHECK(r["painted"] == 1);
    CHECK(sum(weightsAt(mesh, p)) == doctest::Approx(1.0));
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"] == 1);
    REQUIRE(skinIssue());
    CHECK(skinIssue()->message.find("1 vertex") == 0);
}

TEST_CASE("a receiver past the influence limit is refused")
{
    run("newScene");
    std::string mesh = run("polyCylinder", {{"name", "Tube"}, {"radius", 10}, {"height", 100}, {"subdivisionsHeight", 20}, {"caps", false}})[0];
    run("select", {{"args", json::array()}});
    run("joint", {{"position", v3(0, -50, 0)}, {"name", "bone_a"}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "bone_b"}});
    run("joint", {{"position", v3(0, 50, 0)}, {"name", "bone_c"}});
    run("skinCluster", {{"args", {"bone_a", mesh}}, {"maxInfluences", 2}});
    // A point weighted by a and b only; giving c's share to it needs a third slot.
    int p = -1, i = 0;
    for (const json& pt : run("skinPercent", {{"args", {mesh}}}))
    {
        if (pt["weights"].size() == 2 && !pt["weights"].contains("bone_c"))
            p = i;
        ++i;
    }
    REQUIRE(p >= 0);
    json r = run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_a"}, {"receiver", "bone_c"}, {"value", 0.0}, {"strength", 0.5}, {"vertices", {p}}});
    CHECK(r["refused"]["influenceLimit"] == 1);
}

TEST_CASE("brush samples keep sums and locks")
{
    std::string mesh = skinnedCylinder(20);
    NodeId sk = skin::find(graph(), id(mesh));
    WeightTable t = skin::table(graph(), sk);
    auto infs = skin::influences(graph(), sk);
    std::vector<char> locked(3, 0);
    const auto a = uint32_t(skin::indexOf(infs, "bone_a"));
    locked[a] = 1;
    skin::PaintOptions o{.influence = uint32_t(skin::indexOf(infs, "bone_c")), .receiver = -1, .strict = false, .maxInfluences = 4};
    for (size_t p = 0; p < t.points.size(); ++p)
    {
        const float lockedBefore = t.get(p, a);
        const auto outcome = skin::paintPoint(t, p, skin::PaintMode::Add, false, 0.5f, 1.0f, locked, o);
        CHECK(outcome != skin::PaintOutcome::Ambiguous);
        float s = 0.0f;
        for (auto& [i, w] : t.points[p])
            s += w;
        CHECK(s == doctest::Approx(1.0f).epsilon(1e-4));
        CHECK(t.get(p, a) == doctest::Approx(lockedBefore));
    }
}

TEST_CASE("relax keeps authored weights and settles the rest between them")
{
    std::string mesh = skinnedCylinder(20);
    const json all = run("skinPercent", {{"args", {mesh}}});
    // Accept the bottom and top rings as authored; the middle stays automatic.
    json ends = json::array();
    for (size_t k = 0; k < all.size(); ++k)
    {
        const json& w = all[k]["weights"];
        if (w.value("bone_a", 0.0) > 0.9 || w.value("bone_c", 0.0) > 0.9)
            ends.push_back(k);
    }
    REQUIRE(ends.size() > 4);
    run("acceptSkinWeights", {{"args", {mesh}}, {"vertices", ends}});
    run("relaxSkinWeights", {{"args", {mesh}}, {"iterations", 60}});
    const json relaxed = run("skinPercent", {{"args", {mesh}}});
    for (const json& k : ends)
        CHECK(relaxed[k.get<size_t>()]["weights"] == all[k.get<size_t>()]["weights"]);
    bool moved = false;
    for (size_t k = 0; k < all.size(); ++k)
    {
        CHECK(sum(relaxed[k]["weights"]) == doctest::Approx(1.0).epsilon(1e-4));
        moved |= relaxed[k]["weights"] != all[k]["weights"];
    }
    CHECK(moved);
    CHECK(run("skinInfo", {{"args", {mesh}}})["automatic"] == 0);
}

TEST_CASE("settle eases the stretch of a bent joint, posed and simulated")
{
    for (const char* target : {"pose", "simulate"})
    {
        CAPTURE(target);
        std::string mesh = skinnedCylinder(20);
        run("setAttr", {{"args", {"bone_b.rotate", v3(0, 0, 100)}}});
        const double before = stretch(mesh);
        json r = run("settleSkinWeights", {{"args", {mesh}}, {"target", target}});
        CHECK(r["samples"] == 1);
        const double after = stretch(mesh);
        // Simulate also pulls stretched edges back to rest length, so it goes further than the delta mush target.
        CHECK(after < before * (std::string(target) == "pose" ? 0.9 : 0.8));
        CHECK(run("skinInfo", {{"args", {mesh}}})["unnormalized"] == 0);
        CHECK(run("skinInfo", {{"args", {mesh}}})["maxUsed"].get<int>() <= 4);
        run("undo");
        CHECK(stretch(mesh) == doctest::Approx(before));
    }
}

TEST_CASE("settle over a frame range samples each frame")
{
    std::string mesh = skinnedCylinder(10);
    key("bone_b.rotateZ", 0.0, 0.0);
    key("bone_b.rotateZ", 10.0 / kFps, 90.0);
    json r = run("settleSkinWeights", {{"args", {mesh}}, {"start", 0}, {"end", 10}, {"step", 5}});
    CHECK(r["samples"] == 3);
}

TEST_CASE("weight layers blend over the base by opacity and mask, save, load and undo")
{
    std::string mesh = skinnedCylinder(20);
    const int p = threeWay(mesh);
    REQUIRE(p >= 0);
    const json base = run("skinPercent", {{"args", {mesh}}});

    CHECK(run("addSkinLayer", {{"args", {mesh}}, {"name", "Fix"}}) == "Fix");
    json layers = run("skinLayers", {{"args", {mesh}}});
    REQUIRE(layers.size() == 2);
    CHECK(layers[0]["name"] == "Base");
    CHECK(layers[1]["active"] == true);
    // A fresh layer shows nothing.
    CHECK(run("skinPercent", {{"args", {mesh}}}) == base);

    run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"receiver", "bone_b"}, {"value", 0.0}, {"vertices", {p}}});
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) == doctest::Approx(0.0));
    CHECK(run("skinLayers", {{"args", {mesh}}})[1]["maskedPoints"] == 1);
    // Only the painted point differs from the base.
    const json painted = run("skinPercent", {{"args", {mesh}}});
    for (size_t k = 0; k < base.size(); ++k)
        if (int(k) != p)
            CHECK(painted[k] == base[k]);

    run("editSkinLayer", {{"args", {mesh}}, {"layer", "Fix"}, {"opacity", 0.5}});
    const double half = weightsAt(mesh, p).value("bone_c", 0.0);
    CHECK(half == doctest::Approx(base[size_t(p)]["weights"].value("bone_c", 0.0) * 0.5).epsilon(1e-3));
    run("editSkinLayer", {{"args", {mesh}}, {"layer", "Fix"}, {"enabled", false}});
    CHECK(run("skinPercent", {{"args", {mesh}}}) == base);
    run("editSkinLayer", {{"args", {mesh}}, {"layer", "Fix"}, {"enabled", true}, {"opacity", 1.0}});
    run("setSkinLayerMask", {{"args", {mesh}}, {"value", 0.0}, {"vertices", {p}}});
    CHECK(run("skinPercent", {{"args", {mesh}}}) == base);
    run("undo");
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) == doctest::Approx(0.0));

    auto path = (std::filesystem::temp_directory_path() / "lila_rig_tests" / "layers.lilaa").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}});
    CHECK(run("skinLayers", {{"args", {mesh}}}).size() == 2);
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) == doctest::Approx(0.0));

    // Painting the base under a layer leaves the layer's point on top.
    run("editSkinLayer", {{"args", {mesh}}, {"layer", 0}, {"active", true}});
    run("floodSkinWeights", {{"args", {mesh}}, {"influence", "bone_c"}, {"receiver", "bone_b"}, {"value", 1.0}});
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) == doctest::Approx(0.0));

    run("flattenSkinLayers", {{"args", {mesh}}});
    CHECK(run("skinLayers", {{"args", {mesh}}}).empty());
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) == doctest::Approx(0.0));
    run("undo");
    CHECK(run("skinLayers", {{"args", {mesh}}}).size() == 2);
    run("removeSkinLayer", {{"args", {mesh}}, {"layer", "Fix"}});
    CHECK(run("skinLayers", {{"args", {mesh}}}).empty());
    CHECK(weightsAt(mesh, p).value("bone_c", 0.0) > base[size_t(p)]["weights"].value("bone_c", 0.0));
}
