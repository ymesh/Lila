#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "fbx/Fbx.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/Rig.h"
#include "scene/RotationCurve.h"

#include <filesystem>

using namespace lila;
using namespace lila::scene;
using namespace fbxtest;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
std::vector<glm::dmat4> sampleJoint(const char* name, int frames, double fps)
{
    Graph& g = Graph::current();
    std::vector<glm::dmat4> out;
    for (int f = 0; f <= frames; ++f)
    {
        g.setTime(f / fps);
        g.evaluate();
        out.push_back(g.find(name)->as<glm::dmat4>(Transform::kWorldMatrix));
    }
    return out;
}
} // namespace

TEST_CASE("rotation curves round-trip through FBX, keyed and baked, and import as rotation curves")
{
    for (bool bake : {false, true})
    {
        CAPTURE(bake);
        run("newScene");
        run("playbackOptions", {{"fps", 24.0}, {"minTime", 0.0}, {"maxTime", 1.0}, {"animationStartTime", 0.0}, {"animationEndTime", 1.0}});
        run("joint", {{"name", "root"}, {"orientation", v3(0, 0, 30)}});
        run("joint", {{"name", "tip"}, {"position", v3(0, 20, 0)}});
        run("setAttr", {{"args", {"tip.rotateOrder", 2}}});
        Graph& g = Graph::current();
        PlugRef rotate{g.find("tip")->id(), Transform::kRotate, -1};
        anim::setRotationKey(g, rotate, 0.0, {0, 0, 0});
        anim::setRotationKey(g, rotate, 0.5, {80, 170, -30});
        anim::setRotationKey(g, rotate, 1.0, {-20, 260, 45});
        REQUIRE(anim::isRotationCurve(*g.find("tip_rotate")));
        std::vector<glm::dmat4> before = sampleJoint("tip", 24, 24.0);

        fs::path dir = fs::temp_directory_path() / "lila_fbx_tests";
        fs::create_directories(dir);
        fs::path file = dir / (bake ? "rotation_baked.fbx" : "rotation_keys.fbx");
        run("exportFbx", {{"args", {file.string(), "root"}}, {"kind", "animation"}, {"bakeAnimation", bake}});

        run("newScene");
        run("importFbx", {{"args", {file.string()}}});
        REQUIRE(g.find("tip"));
        auto src = g.source({g.find("tip")->id(), Transform::kRotate, -1});
        REQUIRE(src);
        CHECK(anim::isRotationCurve(*g.node(src->node)));
        std::vector<glm::dmat4> after = sampleJoint("tip", 24, 24.0);
        double worst = 0.0;
        for (size_t f = 0; f < before.size(); ++f)
            for (int c = 0; c < 4; ++c)
                worst = std::max(worst, glm::length(before[f][c] - after[f][c]));
        MESSAGE("worst matrix column error " << worst);
        CHECK(worst < 1e-3);
    }
}

TEST_CASE("the exported take spans the playback range in frames at the scene fps")
{
    // Frames 1 to 120 at 30 fps: Lila's times are seconds, so the take is 119 frames long, not 119 seconds.
    run("newScene");
    run("playbackOptions", {{"fps", 30.0}, {"minTime", 1.0 / 30.0}, {"maxTime", 120.0 / 30.0}, {"animationStartTime", 1.0 / 30.0},
                            {"animationEndTime", 120.0 / 30.0}});
    run("spaceLocator", {{"name", "Mover"}});
    run("setKeyframe", {{"args", {"Mover.translateX"}}, {"time", 1.0 / 30.0}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"Mover.translateX"}}, {"time", 120.0 / 30.0}, {"value", 100.0}});
    for (bool bake : {false, true})
    {
        CAPTURE(bake);
        const fs::path file = tempFile(bake ? "range_baked.fbx" : "range.fbx");
        run("exportFbx", {{"args", {file.string(), "Mover"}}, {"kind", "sequence"}, {"bakeAnimation", bake}});
        UfbxScene s(file);
        REQUIRE(s.scene);
        REQUIRE(s.scene->anim_stacks.count == 1);
        const ufbx_anim_stack* stack = s.scene->anim_stacks.data[0];
        CHECK(stack->time_begin == doctest::Approx(1.0 / 30.0));
        CHECK(stack->time_end == doctest::Approx(120.0 / 30.0));
        CHECK(s.scene->settings.frames_per_second == doctest::Approx(30.0));
    }
}
