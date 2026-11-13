#include <doctest/doctest.h>

#include "AnimTestUtil.h"
#include "ops/Command.h"
#include "extensions/ExtensionManager.h"
#include "scene/Anim.h"
#include "scene/AnimCurve.h"
#include "scene/Graph.h"
#include "scene/RotationCurve.h"
#include "scene/Take.h"

#include <filesystem>
#include <map>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;
using namespace animtest;

namespace
{
void loadPuppeteer()
{
    if (!ops::find("stackerKeys"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PUPPETEER_PACKAGE) == "Puppeteer");
}

void newScene()
{
    loadPuppeteer();
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", sec(16)}, {"fps", 30.0}});
}

double valueAt(const std::string& plug, double frame)
{
    Graph& g = Graph::current();
    g.setTime(sec(frame));
    return std::get<double>(g.value(g.plug(plug)));
}

/** root, a chain j1 > j2, and an arm pair; j1.tx keyed at 0, 4, 10, 16 and j2's rotation at 0 and 10. */
void chainScene()
{
    newScene();
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "j1"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "j2"}});
    const double xs[] = {0, 5, 2, 8};
    const double fs[] = {0, 4, 10, 16};
    for (int k = 0; k < 4; ++k)
        key("j1.tx", fs[k], xs[k]);
    key("j2.rz", 0, 0);
    key("j2.rz", 10, 45);
}
} // namespace

TEST_CASE("stacker: keys are the union across joints; a gap edit retimes every curve after it")
{
    chainScene();
    json s = run("stackerKeys", {{"args", {"j1"}}});
    CHECK(frames(s) == std::vector<double>{0, 4, 10, 16});
    CHECK(s["rows"][1]["gap"].get<double>() == Approx(6));
    CHECK(s["total"].get<double>() == Approx(16));

    const double before = valueAt("j1.tx", 10);
    s = run("stackerSetGap", {{"args", {"j1"}}, {"index", 1}, {"gap", 8}});
    CHECK(frames(s) == std::vector<double>{0, 4, 12, 18});
    CHECK(valueAt("j1.tx", 12) == Approx(before));
    // j2's rotation key at 10 belongs to the same row, so it moved too.
    CHECK(valueAt("j2.rz", 12) == Approx(45));

    run("undo");
    CHECK(frames(run("stackerKeys", {{"args", {"j1"}}})) == std::vector<double>{0, 4, 10, 16});

    s = run("stackerNudgeGap", {{"args", {"j1"}}, {"keys", {0, 2}}, {"by", 2}});
    CHECK(frames(s) == std::vector<double>{0, 6, 12, 20});
    s = run("stackerNudgeGap", {{"args", {"j1"}}, {"keys", {0}}, {"by", -10}});
    CHECK(frames(s)[1] == Approx(1));
}

TEST_CASE("stacker: delete with timing, create after with the last gap")
{
    chainScene();
    const double at10 = valueAt("j1.tx", 10);
    json s = run("stackerDeleteKey", {{"args", {"j1"}}, {"keys", {1}}});
    CHECK(frames(s) == std::vector<double>{0, 4, 10});
    CHECK(valueAt("j1.tx", 4) == Approx(at10));

    chainScene();
    const double at4 = valueAt("j1.tx", 4);
    CHECK(run("stackerCreateKey", {{"args", {"j1"}}, {"index", 1}}) == 2);
    s = run("stackerKeys", {{"args", {"j1"}}});
    CHECK(frames(s) == std::vector<double>{0, 4, 8, 14, 20});
    CHECK(valueAt("j1.tx", 8) == Approx(at4));
}

TEST_CASE("stacker: loop key, marks, copy and paste, insert after")
{
    chainScene();
    CHECK(run("stackerLoopKey", {{"args", {"j1"}}, {"state", true}}).get<double>() == Approx(22));
    json s = run("stackerKeys", {{"args", {"j1"}}});
    CHECK(s["loop"] == true);
    CHECK(s["rows"].back()["loop"] == true);
    CHECK(valueAt("j1.tx", 22) == Approx(valueAt("j1.tx", 0)));
    CHECK(run("stackerSelect", {{"args", {"j1"}}, {"which", "secondHalf"}}) == json::array({2, 3}));
    CHECK_THROWS_AS(run("stackerDeleteKey", {{"args", {"j1"}}, {"keys", {4}}}), CommandError);
    CHECK_THROWS_AS(run("stackerCreateKey", {{"args", {"j1"}}, {"index", 4}}), CommandError);

    run("stackerMarkKey", {{"args", {"j1"}}, {"keys", {2}}, {"mark", "extreme"}});
    run("stackerSetGap", {{"args", {"j1"}}, {"index", 0}, {"gap", 2}});
    s = run("stackerKeys", {{"args", {"j1"}}});
    CHECK(s["rows"][2]["mark"] == "extreme");
    CHECK(s["rows"][2]["frame"].get<double>() == Approx(8));

    run("stackerCopyKeys", {{"args", {"j1"}}, {"keys", {1}}});
    run("stackerPasteKeys", {{"args", {"j1"}}, {"keys", {3}}});
    CHECK(valueAt("j1.tx", s["rows"][3]["frame"].get<double>()) == Approx(valueAt("j1.tx", s["rows"][1]["frame"].get<double>())));

    const json added = run("stackerInsertKeys", {{"args", {"j1"}}, {"index", 0}});
    CHECK(added == json::array({1}));
    CHECK(run("stackerKeys", {{"args", {"j1"}}})["rows"].size() == 6);

    run("stackerLoopKey", {{"args", {"j1"}}, {"state", false}});
    CHECK(run("stackerKeys", {{"args", {"j1"}}})["loop"] == false);
}

TEST_CASE("stacker: reset to the bind pose by channel, and mirror")
{
    chainScene();
    run("joint", {{"position", v3(10, 10, 0)}, {"name", "arm_l"}, {"parent", "root"}});
    run("joint", {{"position", v3(-10, 10, 0)}, {"name", "arm_r"}, {"parent", "root"}});
    Graph::current().setTime(0);
    run("savePose", {{"args", {"bind"}}, {"nodes", {"root"}}, {"thumbnail", false}});
    key("arm_l.rz", 0, 30);
    key("arm_l.rz", 4, 10);
    key("arm_r.rz", 0, -5);
    key("arm_r.rz", 4, -5);
    key("arm_l.tx", 4, 12);

    run("stackerMirrorKeys", {{"args", {"root"}}, {"keys", {1}}});
    // Sided joints swap values; the centre joint reflects, so its X offset flips.
    CHECK(valueAt("arm_r.rz", 4) == Approx(10));
    CHECK(valueAt("arm_l.rz", 4) == Approx(-5));
    CHECK(valueAt("arm_r.tx", 4) == Approx(-12));
    CHECK(valueAt("j1.tx", 4) == Approx(-5));

    run("stackerResetKeys", {{"args", {"root"}}, {"keys", {0}}, {"channels", {"rotate"}}});
    CHECK(valueAt("arm_l.rz", 0) == Approx(0));
    CHECK(valueAt("arm_r.rz", 0) == Approx(0));
    CHECK(valueAt("j1.tx", 0) == Approx(0));
    CHECK(valueAt("j1.tx", 10) == Approx(2));
}

TEST_CASE("cycle side: the other side gets the motion half a cycle later; odd counts are refused")
{
    newScene();
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "leg_l"}, {"parent", "root"}});
    run("joint", {{"position", v3(-10, 0, 0)}, {"name", "leg_r"}, {"parent", "root"}});
    const double rz[] = {10, 20, 30, 40};
    for (int k = 0; k < 4; ++k)
        key("leg_l.rz", k * 8, rz[k]);
    key("leg_r.rz", 0, 0);
    run("stackerLoopKey", {{"args", {"root"}}, {"state", true}, {"gap", 8}});

    run("cycleSide", {{"args", {"leg_l"}}});
    for (int k = 0; k < 4; ++k)
        CHECK(valueAt("leg_r.rz", k * 8) == Approx(rz[(k + 2) % 4]));
    CHECK(valueAt("leg_r.rz", 32) == Approx(valueAt("leg_r.rz", 0)));

    run("stackerDeleteKey", {{"args", {"root"}}, {"keys", {3}}});
    CHECK_THROWS_WITH_AS(run("cycleSide", {{"args", {"leg_l"}}}), doctest::Contains("even number"), CommandError);
}

TEST_CASE("cycle step: the first half's keys are mirrored into the second half")
{
    newScene();
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "hips"}});
    run("joint", {{"position", v3(10, 0, 0)}, {"name", "leg_l"}, {"parent", "hips"}});
    run("joint", {{"position", v3(-10, 0, 0)}, {"name", "leg_r"}, {"parent", "hips"}});
    key("hips.tx", 0, 5);
    key("hips.tx", 4, 2);
    key("hips.tx", 8, 0);
    key("leg_l.rz", 0, 25);
    key("leg_l.rz", 4, 15);
    key("leg_r.rz", 0, -20);
    run("stackerLoopKey", {{"args", {"hips"}}, {"state", true}, {"gap", 8}});

    const json r = run("cycleStep", {{"args", {"hips"}}});
    CHECK(r["made"] == 2);
    CHECK(r["replaced"] == 1);
    // The centre joint reflects across the YZ plane; the sides swap.
    CHECK(valueAt("hips.tx", 8) == Approx(-5));
    CHECK(valueAt("hips.tx", 12) == Approx(-2));
    CHECK(valueAt("leg_r.rz", 8) == Approx(25));
    CHECK(valueAt("leg_l.rz", 8) == Approx(-20));
    CHECK(frames(run("stackerKeys", {{"args", {"hips"}}})) == std::vector<double>{0, 4, 8, 12, 16});
}

namespace
{
/** Every animated channel's keys under root, as text, by plug name. */
std::map<std::string, std::string> keysUnder(const std::string& rootName)
{
    Graph& g = Graph::current();
    std::map<std::string, std::string> out;
    const NodeId root = g.find(rootName)->id();
    std::vector<NodeId> nodes = g.descendants(root);
    nodes.push_back(root);
    for (NodeId id : nodes)
        for (const anim::AnimatedPlug& ap : anim::animatedPlugs(g, id))
        {
            const Node& c = *g.node(ap.curve);
            out[g.plugName(ap.plug)] = anim::isRotationCurve(c) ? anim::rotationCurveOf(c).keysToJson().dump() : anim::curveOf(c).keysToJson().dump();
        }
    return out;
}

json walkManny()
{
    newScene();
    run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}});
    return run("walkCycle", {{"args", {"pelvis"}}, {"cycleLength", 32}});
}
} // namespace

TEST_CASE("walk cycle: keys a labelled UE5 skeleton into a looping take, the same every time")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    newScene();
    run("importFbx", {{"args", {LILA_TEST_MANNY_FBX}}, {"animation", false}});
    const json labels = run("characterLabels", {{"args", {"pelvis"}}});
    CHECK(labels["thigh_l"] == "thigh_l");
    CHECK(labels["spine_03"] == "spine_03");
    CHECK(!labels.contains("index_01_l"));

    const json first = walkManny();
    CHECK(first["take"] == "Walk");
    CHECK(first["frames"] == 32);
    const auto keysA = keysUnder("root");
    CHECK(keysA.size() > 30);

    const json takes = run("listTakes");
    bool looping = false;
    for (const json& t : takes)
        if (t["name"] == "Walk")
            looping = t["loop"].get<bool>() && t["active"].get<bool>();
    CHECK(looping);

    // The loop key repeats the first, and the frame after it equals frame 1.
    for (const char* p : {"thigh_l.rx", "thigh_l.ry", "thigh_l.rz", "calf_r.rx", "pelvis.ty", "upperarm_l.rz"})
    {
        CHECK(valueAt(p, 32) == Approx(valueAt(p, 0)).epsilon(1e-9));
        CHECK(valueAt(p, 33) == Approx(valueAt(p, 1)).epsilon(1e-6));
    }
    // The legs actually move.
    CHECK(std::abs(valueAt("thigh_l.rx", 0) - valueAt("thigh_l.rx", 16)) + std::abs(valueAt("thigh_l.ry", 0) - valueAt("thigh_l.ry", 16)) +
              std::abs(valueAt("thigh_l.rz", 0) - valueAt("thigh_l.rz", 16)) >
          5.0);

    walkManny();
    CHECK(keysUnder("root") == keysA);
}

TEST_CASE("anim bank: numbered duplicates, folders, export and import")
{
    if (!std::filesystem::exists(LILA_TEST_MANNY_FBX))
        return;
    walkManny();
    CHECK(run("bankDuplicateTake", {{"args", {"Walk"}}}) == "Walk_01");
    CHECK(run("bankDuplicateTake", {{"args", {"Walk"}}}) == "Walk_02");
    run("editTake", {{"args", {"Walk_01"}}, {"folder", "loco/walks"}});
    std::map<std::string, json> byName;
    for (const json& t : run("listTakes"))
        byName[t["name"].get<std::string>()] = t;
    CHECK(byName["Walk_01"]["folder"] == "loco/walks");
    CHECK(byName["Walk"]["active"] == true);

    const auto path = (std::filesystem::temp_directory_path() / "lila_puppeteer_walk.lilatake").string();
    const json out = run("exportTake", {{"args", {"Walk"}}, {"path", path}});
    CHECK(out["curves"].get<int>() > 15);
    CHECK(out["skipped"] == 0);
    const auto keysWalk = keysUnder("root");

    CHECK(run("importTake", {{"args", {path}}, {"name", "Imported"}}) == "Imported");
    byName.clear();
    for (const json& t : run("listTakes"))
        byName[t["name"].get<std::string>()] = t;
    CHECK(byName["Walk"]["active"] == true);
    CHECK(byName["Imported"]["active"] == false);
    CHECK(byName["Imported"]["loop"] == true);
    CHECK(byName["Imported"]["channels"] == byName["Walk"]["channels"]);
    std::map<std::string, std::vector<double>> before;
    for (const auto& [plug, keys] : keysWalk)
        for (int f = 0; f <= 32; f += 4)
            before[plug].push_back(valueAt(plug, f));
    run("setActiveTake", {{"args", {"Imported"}}});
    // The same channels, evaluating the same (key text may differ in the last bit of a re-derived quaternion).
    const auto keysImported = keysUnder("root");
    CHECK(keysImported.size() == keysWalk.size());
    for (const auto& [plug, values] : before)
    {
        INFO(plug);
        CHECK(keysImported.count(plug) == 1);
        for (int f = 0, i = 0; f <= 32; f += 4, ++i)
            CHECK(valueAt(plug, f) == Approx(values[size_t(i)]).epsilon(1e-9));
    }
    std::filesystem::remove(path);
}
