#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint limits: a stroke over a height ramp paints only where the height is in range, and replays")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    // Height rising from 0 at u = 0.05 to 1 at u = 0.95, in the stack.
    const std::string ramp = run("texAddLayer", {{"args", {"Panel", "paint"}}})["id"];
    const json stops = {{{"position", 0.0}, {"channels", {{"height", 0.0}}}}, {{"position", 1.0}, {"channels", {{"height", 1.0}}}}};
    const json gradient = {{"type", "linear"}, {"space", "uv"}, {"start", {0.05, 0.5}}, {"end", {0.95, 0.5}}, {"stops", stops}};
    const json rampPayload = {{"kind", "fill"}, {"fillBy", "mesh"}, {"channels", {{"height", 0.0}}}, {"gradient", gradient}};
    run("texPaintStroke", {{"args", {"Panel", ramp}}, {"stroke", {{"mesh", "PanelMesh"}, {"payload", rampPayload}}}});

    // A stroke right across both faces, limited to height 0.75 and up: the right face nearest the seam.
    json dabs = json::array();
    for (double x = -45.0; x <= 45.0; x += 5.0)
        dabs.push_back({{"position", {x, 0.0, 0.0}}, {"radius", 6.0}});
    run("texPaintStroke", {{"args", {"Panel", "L"}},
                           {"stroke",
                            {{"space", "surface"},
                             {"mesh", "PanelMesh"},
                             {"brush", brushJson()},
                             {"dabs", dabs},
                             {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}, {"limits", {{"height", {0.75, 1.0}}}}}}}}});
    const image::Image img = gp::readChannel(set.id(), "L", "baseColor");
    // u of the right face runs 0.95 at x = 0 down to 0.55 at x = 50: height 1 at x = 0, 0.56 at x = 45.
    CHECK(at(img, seamUv(1, 5.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 45.0, 0.0))[3] == 0.0f);
    CHECK(at(img, seamUv(0, -25.0, 0.0))[3] == 0.0f);
    const tex::paint::Stroke& st = *tex::paint::readPaint(set)->buffer("L")->strokes.front();
    CHECK(!st.payload.limit.empty());
    CHECK(st.payload.limits.contains("height"));
    const auto painted = gp::pageBytes(set.id(), "L");
    run("texRasterize", {{"args", {"Panel", "L"}}});
    CHECK(gp::pageBytes(set.id(), "L") == painted);
}

TEST_CASE("texture paint limits: a group limit keeps the stroke on the group's faces")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    run("texFaceGroup", {{"args", {"Panel", "left"}}, {"faces", {0}}, {"mesh", "PanelMesh"}});
    json dabs = json::array();
    for (double x = -30.0; x <= 30.0; x += 5.0)
        dabs.push_back({{"position", {x, 0.0, 0.0}}, {"radius", 6.0}});
    run("texPaintStroke", {{"args", {"Panel", "L"}},
                           {"stroke",
                            {{"space", "surface"},
                             {"mesh", "PanelMesh"},
                             {"brush", brushJson()},
                             {"dabs", dabs},
                             {"payload", {{"channels", {{"baseColor", {1.0, 0.0, 0.0}}}}, {"limits", {{"group", "left"}}}}}}}});
    const image::Image img = gp::readChannel(set.id(), "L", "baseColor");
    CHECK(at(img, seamUv(0, -20.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 20.0, 0.0))[3] == 0.0f);
}
