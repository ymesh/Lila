#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

TEST_CASE("texture paint tilt: tilted dabs replay bit for bit, and tilt roundness narrows the tip")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    json brush = brushJson();
    brush["tilt"] = {{"roundness", true}, {"curve", {{"floor", 0.2}, {"exponent", 1.0}}}};
    // Dabs as the paint tool records them, with the pen leaning by 0.9 and turned.
    tp::Stroke rec;
    rec.mesh = "PanelMesh";
    rec.brush = brush;
    rec.payload.channels["baseColor"] = {1.0f, 1.0f, 0.0f};
    for (double x : {-35.0, -25.0})
    {
        const auto r = run("texPaintStroke", {{"args", {"Panel"}},
                                              {"layer", "Scratch"},
                                              {"stroke", {{"space", "surface"}, {"mesh", "PanelMesh"}, {"brush", brush}, {"dabs", {{{"position", {x, 0.0, 0.0}}, {"radius", 8.0}}}}}}});
        (void)r;
    }
    const tp::Buffer* scratch = tp::readPaint(set)->buffer("Scratch");
    REQUIRE(scratch);
    for (const auto& s : scratch->strokes)
        for (tp::Dab d : s->dabs)
        {
            d.tilt = 0.9f;
            d.angle = 0.6f;
            rec.dabs.push_back(d);
        }
    run("texPaintStroke", {{"args", {"Panel"}}, {"layer", "P"}, {"stroke", rec.toJson(true)}});
    const tp::Stroke& stored = *tp::readPaint(set)->buffer("P")->strokes.front();
    REQUIRE(stored.dabs.size() == 2);
    CHECK(stored.dabs.front().tilt == doctest::Approx(0.9f));
    const auto painted = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == painted);

    // The same dabs upright cover more: the tilt narrowed the tip.
    auto covered = [&](const std::string& layer) {
        const image::Image img = gp::readChannel(set.id(), layer, "baseColor");
        size_t n = 0;
        for (size_t i = 3; i < img.rgba.size(); i += 4)
            n += img.rgba[i] > 0.5f;
        return n;
    };
    tp::Stroke upright = rec;
    for (tp::Dab& d : upright.dabs)
        d.tilt = 0.0f;
    run("texPaintStroke", {{"args", {"Panel"}}, {"layer", "U"}, {"stroke", upright.toJson(true)}});
    CHECK(covered("P") < covered("U") * 3 / 4);
}
