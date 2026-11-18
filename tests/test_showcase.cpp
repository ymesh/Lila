#include <doctest/doctest.h>

#include "core/ExtensionContext.h"
#include "core/VideoWriter.h"
#include "ops/Command.h"
#include "render/CubeLut.h"
#include "render/FrameState.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "scene/PostProcess.h"
#include "scene/RenderSettings.h"
#include "ui/RenderOutput.h"
#include "ui/ViewportDisplay.h"

#include <filesystem>
#include <fstream>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
json showcaseRun(const char* name, json call = json::object()) { return ops::execute(name, call); }

fs::path showcaseScratch(const char* name)
{
    fs::path dir = fs::temp_directory_path() / "lila_showcase_tests";
    fs::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("render frame paths number the run of hashes")
{
    CHECK(ui::renderOutput::framePath("shot.####.png", 7) == "shot.0007.png");
    CHECK(ui::renderOutput::framePath("a/b_##.exr", 123) == "a/b_123.exr");
    CHECK(ui::renderOutput::framePath("shot.png", 12) == "shot.0012.png");
    CHECK(ui::renderOutput::framePath("dir.v2/shot", 3) == "dir.v2/shot.0003");
}

TEST_CASE("render suffixes go before the frame number or the extension")
{
    CHECK(ui::renderOutput::withSuffix("a/shot.####.png", "_turntable") == "a/shot_turntable.####.png");
    CHECK(ui::renderOutput::withSuffix("a/shot_####.exr", "_tt") == "a/shot_tt_####.exr");
    CHECK(ui::renderOutput::withSuffix("a.b/shot.mp4", "_turntable") == "a.b/shot_turntable.mp4");
    CHECK(ui::renderOutput::withSuffix("a.b/shot", "_t") == "a.b/shot_t");
    CHECK(ui::renderOutput::withSuffix("shot.mp4", "") == "shot.mp4");
    const std::string video = ui::renderOutput::defaultVideo();
    CHECK(video.ends_with(".mp4"));
    CHECK(video.find('#') == std::string::npos);
}

TEST_CASE("the first enabled post process is the active one")
{
    showcaseRun("newScene");
    Graph& g = Graph::current();
    CHECK(postProcess::active(g) == kNoNode);
    const std::string first = showcaseRun("postProcess").get<std::string>();
    const std::string second = showcaseRun("postProcess", {{"name", "grade"}}).get<std::string>();
    CHECK(g.node(postProcess::active(g))->name() == first);
    showcaseRun("setAttr", {{"args", {first + ".enabled", false}}});
    CHECK(g.node(postProcess::active(g))->name() == second);
    CHECK(showcaseRun("postProcess", {{"existing", true}}).get<std::string>() == second);
}

TEST_CASE("post process settings reach the frame")
{
    showcaseRun("newScene");
    const std::string pp = showcaseRun("postProcess").get<std::string>();
    showcaseRun("setAttr", {{"args", {pp + ".tonemapper", 1}}});
    showcaseRun("setAttr", {{"args", {pp + ".exposure", 1.0}}});
    showcaseRun("setAttr", {{"args", {pp + ".bloom", 0.2}}});
    render::FrameState s;
    s.exposure = 1.0f;
    ui::viewportDisplay::applyPostProcess(s, true);
    CHECK(s.post.tonemapper == render::PostSettings::AgX);
    CHECK(s.exposure == doctest::Approx(2.0f));
    CHECK(s.post.bloom == doctest::Approx(0.2f));
    CHECK(s.post.effects);
}

TEST_CASE("white balance leaves 6500 K alone and warms or cools away from it")
{
    const glm::dvec3 neutral = postProcess::whiteBalance(6500.0, 0.0);
    CHECK(neutral.r == doctest::Approx(1.0).epsilon(0.02));
    CHECK(neutral.g == doctest::Approx(1.0).epsilon(0.02));
    CHECK(neutral.b == doctest::Approx(1.0).epsilon(0.02));
    // White balanced for warm light pulls red down; for cool light, blue down.
    const glm::dvec3 warm = postProcess::whiteBalance(3200.0, 0.0), cool = postProcess::whiteBalance(10000.0, 0.0);
    CHECK(warm.b > warm.r);
    CHECK(cool.r > cool.b);
}

TEST_CASE("cube LUTs load 3D tables and refuse 1D ones")
{
    const fs::path cube = showcaseScratch("identity.cube");
    {
        std::ofstream f(cube);
        f << "TITLE \"identity\"\nLUT_3D_SIZE 2\n";
        for (int b = 0; b < 2; ++b)
            for (int g = 0; g < 2; ++g)
                for (int r = 0; r < 2; ++r)
                    f << r << ' ' << g << ' ' << b << '\n';
    }
    std::string err;
    auto lut = render::loadCubeLut(cube.string(), &err);
    REQUIRE(lut);
    CHECK(lut->size == 2);
    CHECK(lut->rgb.size() == 24);
    CHECK(lut->rgb[3] == doctest::Approx(1.0f)); // red varies fastest

    const fs::path oneD = showcaseScratch("ramp.cube");
    {
        std::ofstream f(oneD);
        f << "LUT_1D_SIZE 2\n0 0 0\n1 1 1\n";
    }
    CHECK_FALSE(render::loadCubeLut(oneD.string(), &err));
    CHECK(err.find("1D") != std::string::npos);
}

TEST_CASE("camera lens attributes give the aperture")
{
    showcaseRun("newScene");
    showcaseRun("createNode", {{"args", {"camera"}}, {"name", "shot"}});
    showcaseRun("setAttr", {{"args", {"shot.focalLength", 50.0}}});
    showcaseRun("setAttr", {{"args", {"shot.fStop", 2.0}}});
    showcaseRun("setAttr", {{"args", {"shot.depthOfField", true}}});
    const camera::Lens lens = camera::lens(*Graph::current().find("shot"));
    CHECK(lens.depthOfField);
    // 50 mm at f/2 is a 25 mm aperture: 1.25 cm radius.
    CHECK(lens.apertureRadius() == doctest::Approx(1.25));
    render::FrameState s;
    ui::viewportDisplay::applyLens(s, lens);
    CHECK(s.lensRadius == doctest::Approx(1.25f));
}

TEST_CASE("lights Lila owns reach the frame with shadow matrices")
{
    showcaseRun("newScene");
    ext::Context ctx("showcaseTest");
    lights::registerProvider(ctx, {.id = lights::kLilaProvider, .name = "Lila Lights"});
    showcaseRun("polyCube", {{"name", "box"}});
    const std::string sun = showcaseRun("light", {{"type", "directional"}, {"intensity", 2.0}}).get<std::string>();
    showcaseRun("light", {{"type", "point"}, {"position", {0.0, 50.0, 0.0}}});
    render::FrameState s;
    ui::viewportDisplay::applyLights(s);
    REQUIRE(s.lights.size() == 2);
    const render::LightDraw* dir = s.lights[0].kind == render::LightDraw::Directional ? &s.lights[0] : &s.lights[1];
    CHECK(dir->shadows);
    CHECK(dir->radiance.r == doctest::Approx(2.0f));
    // The box's centre lands inside the shadow map's clip space.
    const glm::vec4 c = dir->shadowViewProj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    CHECK(std::abs(c.x / c.w) <= 1.0f);
    CHECK(std::abs(c.y / c.w) <= 1.0f);
    CHECK(c.z / c.w > 0.0f);
    CHECK(c.z / c.w < 1.0f);
    ctx.unloadAll();
}

TEST_CASE("render output settings save with the scene")
{
    RenderSettings r;
    r.output = {{"samples", 128}, {"video", "shot.mp4"}};
    RenderSettings back;
    back.fromJson(r.toJson());
    CHECK(back.output.value("samples", 0) == 128);
    CHECK(back.output.value("video", std::string()) == "shot.mp4");
}

#ifdef _WIN32
TEST_CASE("video writer encodes an mp4")
{
    const fs::path path = showcaseScratch("frames.mp4");
    fs::remove(path);
    video::Options o;
    o.width = 64;
    o.height = 48;
    o.fps = 24.0;
    std::string err;
    auto w = video::Writer::open(path, o, &err);
    REQUIRE_MESSAGE(w, err);
    std::vector<uint8_t> frame(size_t(o.width) * o.height * 4);
    for (int i = 0; i < 12; ++i)
    {
        std::fill(frame.begin(), frame.end(), uint8_t(i * 20));
        CHECK(w->add(frame.data(), &err));
    }
    CHECK(w->finish(&err));
    CHECK(fs::file_size(path) > 0);
}
#endif
