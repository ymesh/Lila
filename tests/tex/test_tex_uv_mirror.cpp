#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint uv mirror: a dab mirrored across u paints the mirrored texels")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    // The panel's two islands mirror each other across u = 0.5.
    for (const json& mirror : {json{{"axis", "u"}}, json{{"axis", "u"}, {"perIsland", false}, {"center", 0.5}}})
    {
        run("texPaintStroke", {{"args", {"Panel", "M"}},
                               {"stroke",
                                {{"space", "surface"},
                                 {"mesh", "PanelMesh"},
                                 {"brush", brushJson()},
                                 {"uvMirror", mirror},
                                 {"dabs", {{{"position", {-20.0, 0.0, 10.0}}, {"radius", 8.0}}}},
                                 {"payload", {{"channels", {{"baseColor", {1.0, 0.5, 0.0}}}}}}}}});
        const tex::paint::Stroke& s = *tex::paint::readPaint(set)->buffer("M")->strokes.back();
        CHECK(s.dabs.size() == 2);
        CHECK(s.symmetry.contains("uvMirror"));
        const image::Image img = gp::readChannel(set.id(), "M", "baseColor");
        const glm::vec2 here = seamUv(0, -20.0, 10.0);
        const int y = std::clamp(int((1.0f - here.y) * img.height), 0, img.height - 1);
        const int x0 = int(here.x * img.width);
        for (int dx = -10; dx <= 10; ++dx)
        {
            const int x = x0 + dx, mx = img.width - 1 - x;
            const float a = texel(img, x, y)[3], b = texel(img, mx, y)[3];
            CHECK(b == doctest::Approx(a).epsilon(2e-3));
        }
        CHECK(texel(img, img.width - 1 - x0, y)[3] > 0.5f);
        run("texClearLayer", {{"args", {"Panel", "M"}}});
    }

    // Per island: mirrored within the island it starts on, so the other island stays clean.
    run("texPaintStroke", {{"args", {"Panel", "I"}},
                           {"stroke",
                            {{"space", "surface"},
                             {"mesh", "PanelMesh"},
                             {"brush", brushJson()},
                             {"uvMirror", {{"axis", "u"}, {"perIsland", true}}},
                             {"dabs", {{{"position", {-40.0, 0.0, 0.0}}, {"radius", 4.0}}}},
                             {"payload", {{"channels", {{"baseColor", {1.0, 0.5, 0.0}}}}}}}}});
    const image::Image img = gp::readChannel(set.id(), "I", "baseColor");
    CHECK(at(img, seamUv(0, -40.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(0, -10.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 10.0, 0.0))[3] == 0.0f);
}
