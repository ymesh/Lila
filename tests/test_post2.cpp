#include <doctest/doctest.h>

#include "core/ImageIO.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "render/FrameState.h"
#include "render/Grade.h"
#include "render/Ocio.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/ColorManagement.h"
#include "scene/PostProcess.h"
#include "scene/Graph.h"
#include "ui/ViewportDisplay.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>

using namespace lila;
using namespace lila::scene;
namespace fs = std::filesystem;

namespace
{
json post2Run(const char* name, json call = json::object()) { return ops::execute(name, call); }

fs::path post2Scratch(const char* name)
{
    const fs::path dir = fs::temp_directory_path() / "lila_post2_tests";
    fs::create_directories(dir);
    return dir / name;
}
} // namespace

TEST_CASE("the reflections pref reaches the frame")
{
    PrefStore& p = PrefStore::get();
    const std::string before = p.value<std::string>("general", ui::viewportDisplay::kReflections, "off");
    p.set("general", ui::viewportDisplay::kReflections, std::string("rayTraced"));
    p.set("general", ui::viewportDisplay::kReflectionCutoff, 0.4);
    render::FrameState s;
    ui::viewportDisplay::apply(s);
    CHECK(s.reflections == render::FrameState::Reflections::RayTraced);
    CHECK(s.reflectionCutoff == doctest::Approx(0.4f));
    p.set("general", ui::viewportDisplay::kReflections, std::string("screenSpace"));
    ui::viewportDisplay::apply(s);
    CHECK(s.reflections == render::FrameState::Reflections::ScreenSpace);
    p.set("general", ui::viewportDisplay::kReflections, before);
}

TEST_CASE("the built-in OCIO config lists displays and views and maps Lila's working space")
{
    const render::ocio::ConfigInfo info = render::ocio::info(render::ocio::kBuiltinConfig);
    REQUIRE(info.ok);
    CHECK(!info.displays.empty());
    CHECK(!info.views.empty());
    CHECK(!info.colorSpaces.empty());
    CHECK(info.workingSpace != "scene_linear");
}

TEST_CASE("an OCIO view bakes to a shaped LUT that rises with the light")
{
    std::string err;
    auto view = render::ocio::viewTransform("", "", "", "", &err);
    REQUIRE_MESSAGE(view, err);
    REQUIRE(view->lut);
    CHECK(view->lut->size == 65);
    std::vector<float> px = {0.0f, 0.0f, 0.0f, 1.0f, 0.18f, 0.18f, 0.18f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 16.0f, 16.0f, 16.0f, 1.0f};
    render::ocio::applyView(*view, px);
    CHECK(px[0] < 0.05f);
    CHECK(px[4] > px[0]);
    CHECK(px[8] > px[4]);
    CHECK(px[12] >= px[8]);
    CHECK(px[12] <= 1.05f);
    CHECK(px[3] == 1.0f);
    // The same request is the same bake.
    CHECK(render::ocio::viewTransform("", "", "", "") == view);
}

TEST_CASE("a broken OCIO config falls back to Lila's tonemapper and says why")
{
    post2Run("newScene");
    post2Run("colorManagement", {{"mode", "ocio"}, {"configPath", post2Scratch("missing.ocio").string()}});
    render::FrameState s;
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(!s.post.view);
    CHECK(ui::viewportDisplay::colorNote().find("OCIO failed") != std::string::npos);
    post2Run("colorManagement", {{"configPath", ""}});
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(s.post.view);
    CHECK(ui::viewportDisplay::colorNote().empty());
    post2Run("colorManagement", {{"mode", "lila"}});
    ui::viewportDisplay::applyColorManagement(s);
    CHECK(!s.post.view);
}

TEST_CASE("colour management settings save on their node and a pref can force the config")
{
    post2Run("newScene");
    const std::string node = post2Run("colorManagement", {{"mode", "ocio"}, {"display", "sRGB - Display"}}).get<std::string>();
    CHECK(post2Run("colorManagement").get<std::string>() == node);
    const colorManagement::Settings s = colorManagement::settings(Graph::current());
    CHECK(s.ocio);
    CHECK(s.display == "sRGB - Display");
    CHECK(s.config.empty());
    const colorManagement::Settings forced = colorManagement::settings(Graph::current(), "X:/studio/config.ocio");
    CHECK(forced.forced);
    CHECK(forced.config == "X:/studio/config.ocio");
}

TEST_CASE("multichannel EXRs write sorted layers with chromaticities and read back")
{
    const fs::path path = post2Scratch("layers.exr");
    std::vector<image::ExrChannel> channels;
    for (const char* name : {"R", "G", "B", "A", "depth.Z"})
        channels.push_back({name, std::vector<float>(4 * 2, name[0] == 'd' ? 5.0f : 0.5f), name[0] == 'd'});
    image::writeExrChannels(path, 4, 2, channels, {image::exrChromaticitiesRec709(), image::exrString("lila/test", "yes")});
    std::string err;
    const image::Image img = image::readExr(path, &err);
    REQUIRE_MESSAGE(img.width == 4, err);
    CHECK(img.rgba[0] == doctest::Approx(0.5f));
    std::ifstream f(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    CHECK(bytes.find("depth.Z") != std::string::npos);
    CHECK(bytes.find("chromaticities") != std::string::npos);
    CHECK(bytes.find("lila/test") != std::string::npos);
}

TEST_CASE("a neutral grade leaves display values alone")
{
    render::PostSettings p;
    for (float v : {0.0f, 0.18f, 0.5f, 1.0f})
    {
        const glm::vec3 out = render::grade::applyDisplay(glm::vec3(v, v * 0.5f, 1.0f - v), p);
        CHECK(out.r == doctest::Approx(v).epsilon(1e-4));
        CHECK(out.g == doctest::Approx(v * 0.5f).epsilon(1e-4));
    }
    std::array<std::string, render::grade::kCurveCount> neutral;
    neutral[0] = "0 0, 1 1";
    neutral[4] = "0 0.5, 0.5 0.5";
    CHECK(!render::grade::curves(neutral));
}

TEST_CASE("grade curves pass through their points without overshoot and hue curves wrap")
{
    using namespace render::grade;
    const std::vector<glm::vec2> pts = parseCurve("1 1, 0 0, 0.5 0.3");
    REQUIRE(pts.size() == 3);
    CHECK(pts[0].x == 0.0f);
    CHECK(evaluate(kMaster, pts, 0.5f) == doctest::Approx(0.3f));
    float last = -1.0f;
    for (int i = 0; i <= 50; ++i)
    {
        const float y = evaluate(kMaster, pts, i / 50.0f);
        CHECK(y >= last - 1e-5f);
        CHECK(y <= 1.0f + 1e-5f);
        last = y;
    }
    const std::vector<glm::vec2> hue = parseCurve("0.1 0.8, 0.6 0.2");
    CHECK(evaluate(kHueVsSat, hue, 0.0f) == doctest::Approx(evaluate(kHueVsSat, hue, 1.0f)).epsilon(1e-4));
    CHECK(formatCurve(parseCurve("0 0, 0.5 0.25")) == "0.000 0.000, 0.500 0.250");
}

TEST_CASE("wheels, tonal balance and curves move the display picture as their names say")
{
    render::PostSettings p;
    p.gainMaster = 1.5f;
    CHECK(render::grade::applyDisplay(glm::vec3(0.4f), p).g == doctest::Approx(0.6f).epsilon(1e-3));
    p = {};
    p.liftMaster = 0.2f;
    CHECK(render::grade::applyDisplay(glm::vec3(0.0f), p).r == doctest::Approx(0.2f).epsilon(1e-3));
    p = {};
    p.shadows = glm::vec3(1.0f, 1.0f, 2.0f);
    const glm::vec3 dark = render::grade::applyDisplay(glm::vec3(0.05f), p);
    CHECK(dark.b > dark.r);
    const glm::vec3 bright = render::grade::applyDisplay(glm::vec3(0.95f), p);
    CHECK(bright.b == doctest::Approx(bright.r).epsilon(1e-3));
    p = {};
    std::array<std::string, render::grade::kCurveCount> texts;
    texts[render::grade::kHueVsSat] = "0 0, 0.5 0, 1 0";
    p.curves = render::grade::curves(texts);
    REQUIRE(p.curves);
    const glm::vec3 grey = render::grade::applyDisplay(glm::vec3(0.8f, 0.2f, 0.1f), p);
    CHECK(grey.r == doctest::Approx(grey.b).epsilon(1e-3));
}

TEST_CASE("lens presets set the flare and the lens dirt reaches the frame")
{
    post2Run("newScene");
    const json names = post2Run("lensPreset");
    REQUIRE(names.size() == 4);
    post2Run("lensPreset", {{"args", {"Vintage Glass"}}});
    const std::string pp = post2Run("postProcess", {{"existing", true}}).get<std::string>();
    CHECK(post2Run("getAttr", {{"args", {pp + ".lensFlare"}}}).get<double>() > 0.0);
    CHECK(post2Run("getAttr", {{"args", {pp + ".lensDirt"}}}).get<std::string>() == "dust");
    render::FrameState s;
    ui::viewportDisplay::applyPostProcess(s, true);
    CHECK(s.post.flare.intensity > 0.0f);
    REQUIRE(s.post.flare.dirt);
    CHECK(s.post.flare.dirt->width > 0);
    CHECK(s.post.flare.dirt->rgba.size() == size_t(s.post.flare.dirt->width) * s.post.flare.dirt->height * 4);
    CHECK_THROWS(post2Run("lensPreset", {{"args", {"No Such Lens"}}}));
}

TEST_CASE("post volumes blend over the global post process by where the camera is")
{
    post2Run("newScene");
    const std::string global = post2Run("postProcess", {{"name", "Global"}}).get<std::string>();
    const std::string room = post2Run("postProcess", {{"name", "Room"}}).get<std::string>();
    post2Run("setAttr", {{"args", {room + ".volume", 1}}});
    post2Run("setAttr", {{"args", {room + ".volumeCenter", json::array({0.0, 0.0, 0.0})}}});
    post2Run("setAttr", {{"args", {room + ".volumeSize", json::array({200.0, 200.0, 200.0})}}});
    post2Run("setAttr", {{"args", {room + ".volumeBlend", 100.0}}});
    post2Run("setAttr", {{"args", {room + ".saturation", 0.0}}});
    const Node* r = Graph::current().find(room);
    REQUIRE(r);
    CHECK(postProcess::volumeWeight(*r, {0.0, 0.0, 0.0}) == doctest::Approx(1.0));
    CHECK(postProcess::volumeWeight(*r, {150.0, 0.0, 0.0}) == doctest::Approx(0.5));
    CHECK(postProcess::volumeWeight(*r, {400.0, 0.0, 0.0}) == doctest::Approx(0.0));
    CHECK(postProcess::active(Graph::current()) == Graph::current().find(global)->id());
    render::FrameState s;
    s.eye = {150.0, 0.0, 0.0};
    ui::viewportDisplay::applyPostProcess(s, true);
    CHECK(s.post.saturation == doctest::Approx(0.5f));
    s.eye = {0.0, 0.0, 0.0};
    ui::viewportDisplay::applyPostProcess(s, true);
    CHECK(s.post.saturation == doctest::Approx(0.0f));
}

TEST_CASE("a camera's own post process and physical exposure reach its frames")
{
    post2Run("newScene");
    post2Run("postProcess", {{"name", "Global"}});
    const std::string shot = post2Run("postProcess", {{"name", "Shot"}}).get<std::string>();
    post2Run("setAttr", {{"args", {shot + ".exposure", 1.0}}});
    const std::string cam = post2Run("createNode", {{"args", {"camera"}}, {"name", "Cam"}}).get<std::string>();
    post2Run("setAttr", {{"args", {cam + ".postProcess", shot}}});
    render::FrameState s;
    s.exposure = 1.0f;
    ui::viewportDisplay::applyPostProcess(s, true);
    CHECK(s.exposure == doctest::Approx(1.0f));
    ui::viewportDisplay::applyLens(s, camera::lens(*Graph::current().find(cam)));
    CHECK(s.exposure == doctest::Approx(2.0f));
    // Applying it again replaces the scene's exposure share rather than stacking.
    ui::viewportDisplay::applyLens(s, camera::lens(*Graph::current().find(cam)));
    CHECK(s.exposure == doctest::Approx(2.0f));
    post2Run("setAttr", {{"args", {cam + ".postProcess", ""}}});
    post2Run("setAttr", {{"args", {cam + ".physicalExposure", true}}});
    post2Run("setAttr", {{"args", {cam + ".fStop", 2.8}}});
    render::FrameState p;
    p.exposure = 1.0f;
    ui::viewportDisplay::applyPostProcess(p, true);
    const float before = p.exposure;
    ui::viewportDisplay::applyLens(p, camera::lens(*Graph::current().find(cam)));
    const double fps = std::max(anim::playback().fps, 1.0);
    // Two stops wider than f/5.6, with the shutter at this frame rate against the 24 fps reference.
    CHECK(p.exposure / before == doctest::Approx(4.0 * 24.0 / fps).epsilon(1e-3));
}

TEST_CASE("render passes take known names only")
{
    post2Run("newScene");
    CHECK_THROWS(post2Run("render", {{"path", post2Scratch("bad.exr").string()}, {"passes", "depth,nonsense"}}));
}
