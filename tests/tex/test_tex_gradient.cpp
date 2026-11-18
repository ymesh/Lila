#include "TexPaintTest.h"

#include "render/PaintGpu.h"

using namespace textest;
namespace gp = lila::render::paint;

namespace
{
json gradientStroke(json gradient)
{
    return {{"args", {"Panel"}},
            {"layer", "G"},
            {"stroke",
             {{"mesh", "PanelMesh"},
              {"brush", brushJson()},
              {"payload", {{"kind", "fill"}, {"fillBy", "mesh"}, {"channels", {{"roughness", 0.0}}}, {"gradient", std::move(gradient)}}}}}};
}
} // namespace

TEST_CASE("texture paint gradient: a uv gradient's ends are exact and it rises steadily between them")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    // Roughness 0 at u = 0.1 to 1 at u = 0.9, at full strength throughout.
    run("texPaintStroke", gradientStroke({{"type", "linear"},
                                          {"space", "uv"},
                                          {"start", {0.1, 0.5}},
                                          {"end", {0.9, 0.5}},
                                          {"stops", {{{"position", 0.0}, {"channels", {{"roughness", 0.0}}}},
                                                     {{"position", 1.0}, {"channels", {{"roughness", 1.0}}}}}}}));
    const image::Image img = gp::readChannel(set.id(), "G", "roughness");
    const int row = img.height / 2;
    auto value = [&](int x) { return img.rgba[(size_t(row) * img.width + x) * 4]; };
    auto covered = [&](int x) { return img.rgba[(size_t(row) * img.width + x) * 4 + 3] > 0.5f; };
    // Inside the left island before the start: exactly the start value; inside the right one past the end: exactly the end's.
    const int before = int(0.07 * img.width), past = int(0.93 * img.width);
    REQUIRE(covered(before));
    REQUIRE(covered(past));
    CHECK(value(before) == 0.0f);
    CHECK(value(past) == 1.0f);
    float last = -1.0f;
    for (int x = int(0.12 * img.width); x < int(0.88 * img.width); ++x)
        if (covered(x))
        {
            CHECK(value(x) >= last);
            last = value(x);
        }
    CHECK(last > 0.7f);
}

TEST_CASE("texture paint gradient: radial and screen gradients fade the amount and replay bit for bit")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json view = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};
    json call = gradientStroke({{"type", "radial"}, {"space", "screen"}, {"start", {400.0, 300.0}}, {"end", {500.0, 300.0}}, {"dither", true}});
    call["stroke"]["payload"]["channels"] = {{"baseColor", {1.0, 0.0, 0.0}}};
    call["stroke"]["view"] = view;
    run("texPaintStroke", call);
    const image::Image img = gp::readChannel(set.id(), "G", "baseColor");
    // Full at the press (world x 0), nothing a drag's length away (world x about 24).
    CHECK(at(img, seamUv(1, 1.0, 0.0))[3] > 0.9f);
    CHECK(at(img, seamUv(1, 35.0, 0.0))[3] == 0.0f);
    const auto painted = gp::pageBytes(set.id(), "G");
    run("texRasterize", {{"args", {"Panel", "G"}}});
    CHECK(gp::pageBytes(set.id(), "G") == painted);
}
