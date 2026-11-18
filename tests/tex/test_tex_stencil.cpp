#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "scene/Graph.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

namespace
{
/** Looking straight down at the seam panel: screen x 300 to 500 runs along world x -24 to 24. */
const json kStencilView = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};

/** w x h texels, rgba per texel from fn(x, y). */
json picture(int w, int h, const std::function<std::array<float, 4>(int, int)>& fn)
{
    std::vector<float> rgba;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (float c : fn(x, y))
                rgba.push_back(c);
    return tp::PaintImage::fromRgba(w, h, rgba)->toJson();
}

json stencilStroke(const json& stencil)
{
    json brush = brushJson();
    brush["radius"] = 25.0;
    return {{"args", {"Panel"}},
            {"layer", "P"},
            {"points", {{300.0, 300.0, 1.0}, {350.0, 300.0, 1.0}, {400.0, 300.0, 1.0}, {450.0, 300.0, 1.0}, {500.0, 300.0, 1.0}}},
            {"view", kStencilView},
            {"brush", brush},
            {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}}},
            {"stencil", stencil}};
}

/** A stencil twice as wide as tall spans the 800 x 600 view's height and more than its width: its left half is screen x < 400. */
json leftHalfWhite()
{
    return picture(64, 32, [](int x, int) { return x < 32 ? std::array{1.0f, 1.0f, 1.0f, 1.0f} : std::array{0.0f, 0.0f, 0.0f, 1.0f}; });
}
} // namespace

TEST_CASE("texture paint stencil: a mask leaves its dark side unpainted and replays bit for bit")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    run("texPaintStroke", stencilStroke({{"image", leftHalfWhite()}}));
    const tp::PaintPtr data = tp::readPaint(set);
    const tp::Stroke& s = *data->buffer("P")->strokes.front();
    REQUIRE(s.stencil.has_value());
    CHECK(data->images.contains(s.stencil->image));

    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(0, -15.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 15.0, 0.0))[3] == 0.0f);

    const auto painted = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == painted);

    // Inverted, the other side.
    run("undo");
    run("texPaintStroke", stencilStroke({{"image", leftHalfWhite()}, {"invert", true}}));
    const image::Image inv = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(inv, seamUv(0, -15.0, 0.0))[3] == 0.0f);
    CHECK(at(inv, seamUv(1, 15.0, 0.0))[3] > 0.5f);
}

TEST_CASE("texture paint stencil: an all-black mask paints nothing; moved off screen, nothing either")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    run("texPaintStroke", stencilStroke({{"image", picture(8, 8, [](int, int) { return std::array{0.0f, 0.0f, 0.0f, 1.0f}; })}}));
    const image::Image black = gp::readChannel(set.id(), "P", "baseColor");
    for (size_t i = 3; i < black.rgba.size(); i += 4)
        REQUIRE(black.rgba[i] == 0.0f);

    run("undo");
    const json white = picture(8, 8, [](int, int) { return std::array{1.0f, 1.0f, 1.0f, 1.0f}; });
    run("texPaintStroke", stencilStroke({{"image", white}, {"offset", {2000.0, 0.0}}}));
    const image::Image away = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(away, seamUv(0, -15.0, 0.0))[3] == 0.0f);

    // Tiled, the same picture far away still covers the view.
    run("undo");
    run("texPaintStroke", stencilStroke({{"image", white}, {"offset", {2000.0, 0.0}}, {"tile", true}}));
    CHECK(at(gp::readChannel(set.id(), "P", "baseColor"), seamUv(0, -15.0, 0.0))[3] > 0.5f);
}

TEST_CASE("texture paint stencil: colour mode paints the picture's colour")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json teal = picture(8, 8, [](int, int) { return std::array{0.2f, 0.5f, 0.8f, 1.0f}; });
    run("texPaintStroke", stencilStroke({{"image", teal}, {"mode", "color"}}));
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    const float* t = at(img, seamUv(0, -10.0, 0.0));
    REQUIRE(t[3] > 0.5f);
    CHECK(t[0] == doctest::Approx(0.2).epsilon(0.02));
    CHECK(t[1] == doctest::Approx(0.5).epsilon(0.02));
    CHECK(t[2] == doctest::Approx(0.8).epsilon(0.02));
}

TEST_CASE("texture paint pictures: an 8-bit file decodes from sRGB in colour channels and reads raw in data channels")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const std::string file = (tempDir("paint_pictures") / "grey.png").string();
    image::writePng(file, 8, 8, std::vector<float>(8 * 8 * 4, 0.5f), 8);
    const float stored = 128.0f / 255.0f, decoded = std::pow((stored + 0.055f) / 1.055f, 2.4f);
    auto value = [&](const char* layer, const char* channel) {
        const image::Image img = gp::readChannel(set.id(), layer, channel);
        const float* t = at(img, seamUv(0, -10.0, 0.0));
        REQUIRE(t[3] > 0.5f);
        return t[0];
    };

    // A texture source sampled once feeds both channels: decoded for base colour, raw for roughness.
    json src = stencilStroke(json());
    src.erase("stencil");
    src["layer"] = "Source";
    src["payload"] = {{"channels", {{"baseColor", {1.0, 1.0, 1.0}}, {"roughness", 1.0}}},
                      {"source", {{"node", "layerImage"}, {"values", {{"file", file}}}, {"resolution", 64}}}};
    src["brush"]["falloff"] = "constant";
    run("texPaintStroke", src);
    CHECK(value("Source", "baseColor") == doctest::Approx(decoded).epsilon(0.02));
    CHECK(value("Source", "roughness") == doctest::Approx(stored).epsilon(0.02));

    // A colour stencil and a projection decode by the channel they paint.
    for (const char* channel : {"baseColor", "roughness"})
    {
        json st = stencilStroke({{"image", file}, {"mode", "color"}, {"colorChannel", channel}});
        st["layer"] = std::string("Stencil_") + channel;
        st["payload"] = {{"channels", {{channel, channel == std::string("baseColor") ? json{1.0, 1.0, 1.0} : json(1.0)}}}};
        st["brush"]["falloff"] = "constant";
        run("texPaintStroke", st);
        CHECK(value(st["layer"].get<std::string>().c_str(), channel) == doctest::Approx(channel == std::string("baseColor") ? decoded : stored).epsilon(0.02));

        const std::string layer = std::string("Project_") + channel;
        const json payload = {{"kind", "project"}, {"image", file}, {"channels", json::object()}, {"colorChannel", channel}};
        run("texPaintStroke", {{"args", {"Panel", layer}}, {"stroke", {{"mesh", "PanelMesh"}, {"payload", payload}}}, {"view", kStencilView}});
        CHECK(value(layer.c_str(), channel) == doctest::Approx(channel == std::string("baseColor") ? decoded : stored).epsilon(0.02));
    }
}
