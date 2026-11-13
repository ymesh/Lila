#include <doctest/doctest.h>

#include "AnimTestUtil.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Anim.h"
#include "scene/AnimLayer.h"
#include "scene/Graph.h"
#include "ui/Modes.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using json = nlohmann::json;
using Approx = doctest::Approx;
using namespace animtest;

namespace
{
double at(const std::string& plug, double frame)
{
    Graph& g = Graph::current();
    g.setTime(sec(frame));
    return std::get<double>(g.value(g.plug(plug)));
}

/** root > j1 > j2 and arm_l / arm_r under root. j1.tx keyed 0, 10, 20; j2.rz 0 and 20; arms at 0 and 20. */
void setupScene()
{
    if (!ops::find("keyTarget"))
        REQUIRE(extensions::ExtensionManager::get().load(LILA_TEST_PUPPETEER_PACKAGE) == "Puppeteer");
    run("newScene");
    run("autoKeyframe", {{"state", false}});
    run("playbackOptions", {{"minTime", 0.0}, {"maxTime", sec(30)}, {"fps", 30.0}});
    run("joint", {{"position", v3(0, 0, 0)}, {"name", "root"}});
    run("joint", {{"position", v3(0, 10, 0)}, {"name", "j1"}});
    run("joint", {{"position", v3(0, 20, 0)}, {"name", "j2"}});
    run("joint", {{"position", v3(10, 10, 0)}, {"name", "arm_l"}, {"parent", "root"}});
    run("joint", {{"position", v3(-10, 10, 0)}, {"name", "arm_r"}, {"parent", "root"}});
    key("j1.tx", 0, 0);
    key("j1.tx", 10, 6);
    key("j1.tx", 20, 2);
    key("j2.rz", 0, 0);
    key("j2.rz", 20, 40);
    for (const char* arm : {"arm_l.rz", "arm_r.rz"})
    {
        key(arm, 0, 0);
        key(arm, 20, 0);
    }
}

/** Poses a channel at frame the way a manipulator does. */
void pose(const std::string& plug, double frame, double v)
{
    Graph::current().setTime(sec(frame));
    run("setAttr", {{"args", {plug, v}}});
}

json tracks() { return run("listControlTracks", {{"args", {"root"}}}); }
} // namespace

TEST_CASE("control tracks: an offset adds one value over the whole take; mute and the all switch leave the keys")
{
    setupScene();
    const double base5 = at("j2.rz", 5), base15 = at("j2.rz", 15);
    CHECK(run("keyTarget", {{"args", {"root"}}, {"target", "control"}}) == "control");
    pose("j2.rz", 5, base5 + 10);

    json t = tracks();
    REQUIRE(t["tracks"].size() == 1);
    CHECK(t["tracks"][0]["offset"] == true);
    CHECK(at("j2.rz", 5) == Approx(base5 + 10));
    CHECK(at("j2.rz", 15) == Approx(base15 + 10));
    // The character keys are untouched: same rows, nothing at the posed frame.
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 10, 20});

    run("editControlTrack", {{"args", {"j2"}}, {"mute", true}});
    CHECK(at("j2.rz", 15) == Approx(base15));
    run("editControlTrack", {{"args", {"j2"}}, {"mute", false}});
    run("controlTracksOn", {{"args", {"root"}}, {"state", false}});
    CHECK(at("j2.rz", 15) == Approx(base15));
    run("controlTracksOn", {{"args", {"root"}}, {"state", true}});
    CHECK(at("j2.rz", 15) == Approx(base15 + 10));
    CHECK(tracks()["tracks"][0]["mute"] == false);

    run("undo");
    CHECK(at("j2.rz", 15) == Approx(base15));
}

TEST_CASE("control tracks: posing in the character target keys the character keys under the offset")
{
    setupScene();
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    pose("j2.rz", 10, at("j2.rz", 10) + 10);
    run("keyTarget", {{"args", {"root"}}, {"target", "character"}});
    run("autoKeyframe", {{"state", true}});
    pose("j2.rz", 20, 25);
    // What you pose is what you get, the offset still on top; turning it off shows the key underneath.
    CHECK(at("j2.rz", 20) == Approx(25));
    run("controlTracksOn", {{"args", {"root"}}, {"state", false}});
    CHECK(at("j2.rz", 20) == Approx(15));
    CHECK(tracks()["tracks"][0]["offset"] == true);
}

TEST_CASE("control tracks: a keyed track has its own timing; base retimes carry it, its own retime stays on it")
{
    setupScene();
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    Graph& g = Graph::current();
    g.setTime(sec(4));
    run("characterKey", {{"args", {"arm_l"}}});
    pose("arm_l.rz", 14, 30);
    run("characterKey", {{"args", {"arm_l"}}});

    json t = tracks()["tracks"][0];
    CHECK(t["offset"] == false);
    CHECK(t["keys"] == json::array({4.0, 14.0}));
    CHECK(at("arm_l.rz", 14) == Approx(30));
    CHECK(at("arm_l.rz", 4) == Approx(0));
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 10, 20});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}, {"track", "arm_l"}})) == std::vector<double>{4, 14});

    // The track alone: its gap grows, the character keys stay.
    run("stackerSetGap", {{"args", {"root"}}, {"track", "arm_l"}, {"index", 0}, {"gap", 12}});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}, {"track", "arm_l"}})) == std::vector<double>{4, 16});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 10, 20});
    CHECK(at("arm_l.rz", 16) == Approx(30));

    // The character keys: the first gap doubles, the track's keys inside it stretch with it.
    run("stackerSetGap", {{"args", {"root"}}, {"index", 0}, {"gap", 20}});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 20, 30});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}, {"track", "arm_l"}})) == std::vector<double>{8, 26});
    CHECK(at("arm_l.rz", 26) == Approx(30));
}

TEST_CASE("control tracks: merge sums into the character keys; undo brings the track back; solo")
{
    setupScene();
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    pose("j2.rz", 10, at("j2.rz", 10) + 12);
    pose("j1.tx", 10, at("j1.tx", 10) - 2);
    const double sum10 = at("j2.rz", 10), sum20 = at("j2.rz", 20), tx10 = at("j1.tx", 10);
    REQUIRE(tracks()["tracks"].size() == 2);

    run("editControlTrack", {{"args", {"j2"}}, {"solo", true}});
    CHECK(at("j2.rz", 10) == Approx(sum10));
    CHECK(at("j1.tx", 10) == Approx(tx10 + 2));
    run("editControlTrack", {{"args", {"j2"}}, {"solo", false}});

    const json r = run("mergeControlTrack", {{"args", {"j2"}}});
    REQUIRE(r.size() == 1);
    // An offset merges onto the control's own keys, so the curve keeps its shape.
    CHECK(r[0]["keys"] == 2);
    CHECK(r[0]["rotateError"].get<double>() < 1e-6);
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 10, 20});
    CHECK(tracks()["tracks"].size() == 1);
    CHECK(at("j2.rz", 10) == Approx(sum10));
    CHECK(at("j2.rz", 20) == Approx(sum20));
    CHECK(tracks()["tracks"][0]["control"].get<std::string>().ends_with("j1"));
    run("undo");
    CHECK(tracks()["tracks"].size() == 2);
    CHECK(at("j2.rz", 20) == Approx(sum20));
    run("controlTracksOn", {{"args", {"root"}}, {"state", false}});
    CHECK(at("j2.rz", 20) == Approx(40));
}

TEST_CASE("control tracks: a keyed track merges exactly, with keys between where the curve would leave the sum")
{
    setupScene();
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    Graph& g = Graph::current();
    g.setTime(sec(4));
    run("characterKey", {{"args", {"arm_l"}}});
    pose("arm_l.rz", 14, 30);
    run("characterKey", {{"args", {"arm_l"}}});
    pose("arm_l.rz", 25, 12);
    run("characterKey", {{"args", {"arm_l"}}});
    std::vector<double> layered;
    for (int f = 0; f <= 30; ++f)
        layered.push_back(at("arm_l.rz", f));
    const size_t ownKeys = run("keyframe", {{"args", {"arm_l.rz"}}, {"query", true}}).size();

    const json r = run("mergeControlTrack", {{"args", {"arm_l"}}});
    REQUIRE(r.size() == 1);
    // Own keys, the track's 4, 14 and 25, and keys between where the spans would stray.
    CHECK(r[0]["addedKeys"].get<int>() > 0);
    CHECK(r[0]["keys"].get<int>() >= 5 + r[0]["addedKeys"].get<int>());
    CHECK(r[0]["rotateError"].get<double>() <= 0.01);
    CHECK(tracks()["tracks"].size() == 0);
    double worst = 0.0;
    for (int f = 0; f <= 25; ++f)
        worst = std::max(worst, std::abs(at("arm_l.rz", f) - layered[size_t(f)]));
    CHECK(worst <= 0.01);
    // Fewer keys than frames: only the spans that needed one got one.
    CHECK(r[0]["keys"].get<int>() < 26);

    run("undo");
    CHECK(tracks()["tracks"].size() == 1);
    CHECK(run("keyframe", {{"args", {"arm_l.rz"}}, {"query", true}}).size() == ownKeys);
}

TEST_CASE("character keys: Set Key and auto key key every control the character animates")
{
    setupScene();
    run("characterKey", {{"args", {"j2"}}, {"time", sec(5)}});
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 5, 10, 20});
    CHECK(run("keyframe", {{"args", {"arm_r.rz"}}, {"query", true}}).size() == 3);
    CHECK(run("keyframe", {{"args", {"j1.tx"}}, {"query", true}}).size() == 4);

    // Auto key in Animate mode does the same; outside it only the posed channel is keyed.
    run("autoKeyframe", {{"state", true}});
    pose("j2.rz", 15, 7);
    CHECK(run("keyframe", {{"args", {"arm_r.rz"}}, {"query", true}}).size() == 3);
    REQUIRE(ui::modes::activate("puppeteerAnimate"));
    pose("j2.rz", 17, 9);
    CHECK(run("keyframe", {{"args", {"arm_r.rz"}}, {"query", true}}).size() == 4);
    CHECK(frames(run("stackerKeys", {{"args", {"root"}}})) == std::vector<double>{0, 5, 10, 15, 17, 20});
    // Posing on a track in Animate mode keys only the track.
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    run("characterKey", {{"args", {"j1"}}, {"time", sec(3)}});
    CHECK(run("keyframe", {{"args", {"arm_r.rz"}}, {"query", true}}).size() == 4);
    ui::modes::activate(ui::modes::kDefaultMode);
}

TEST_CASE("control tracks: copy and paste between track keys, mirror to the other side, save and load")
{
    setupScene();
    run("keyTarget", {{"args", {"root"}}, {"target", "control"}});
    Graph& g = Graph::current();
    pose("arm_l.rz", 4, 20);
    run("characterKey", {{"args", {"arm_l"}}, {"time", sec(4)}});
    pose("arm_l.rz", 12, 5);
    run("characterKey", {{"args", {"arm_l"}}, {"time", sec(12)}});
    CHECK(run("stackerCopyKeys", {{"args", {"root"}}, {"track", "arm_l"}, {"keys", {0}}}) == 1);
    run("stackerPasteKeys", {{"args", {"root"}}, {"track", "arm_l"}, {"keys", {1}}});
    // Off the posed frame first: a posed value is held until the time moves.
    CHECK(at("arm_l.rz", 8) == Approx(20));
    CHECK(at("arm_l.rz", 12) == Approx(20));
    CHECK_THROWS_AS(run("stackerPasteKeys", {{"args", {"root"}}, {"keys", {1}}}), CommandError);

    run("stackerMirrorKeys", {{"args", {"root"}}, {"track", "arm_l"}, {"keys", {0, 1}}});
    REQUIRE(tracks()["tracks"].size() == 2);
    CHECK(at("arm_r.rz", 4) == Approx(20));
    CHECK(at("arm_r.rz", 12) == Approx(20));

    const std::string path = (std::filesystem::temp_directory_path() / "lila_control_tracks.lila").string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    run("openScene", {{"args", {path}}, {"force", true}});
    std::filesystem::remove(path);
    CHECK(tracks()["target"] == "control");
    CHECK(tracks()["tracks"].size() == 2);
    CHECK(at("arm_l.rz", 4) == Approx(20));
    run("controlTracksOn", {{"args", {"root"}}, {"state", false}});
    CHECK(at("arm_l.rz", 4) == Approx(0));
    (void)g;
}
