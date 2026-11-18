#include "TexPaintTest.h"

#include "curve/CurveNodes.h"
#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

namespace
{
json pathCall(json points)
{
    return {{"args", {"Panel"}},
            {"layer", "P"},
            {"mesh", "PanelMesh"},
            {"points", std::move(points)},
            {"radius", 5.0},
            {"brush", brushJson()},
            {"payload", {{"channels", {{"baseColor", {0.0, 0.0, 1.0}}}}}}};
}
} // namespace

TEST_CASE("texture paint path: a path paints along its points and stays on the stroke")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json r = run("texPaintPath", pathCall({{-40.0, 0.0, -20.0}, {-10.0, 0.0, 0.0}, {-40.0, 0.0, 20.0}}));
    CHECK(r["dabs"].get<int>() > 10);
    const tp::Stroke& s = *tp::readPaint(set)->buffer("P")->strokes.front();
    REQUIRE(s.path.is_object());
    CHECK(s.path["points"].size() == 3);

    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(0, -40.0, -20.0))[3] > 0.5f);
    CHECK(at(img, seamUv(0, -40.0, 20.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 30.0, 0.0))[3] == 0.0f);

    // Rebuilt from its records, the same pixels.
    const auto painted = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == painted);
}

TEST_CASE("texture paint path: editing a path repaints only from the new points, deterministically, and undoes")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const json r = run("texPaintPath", pathCall({{-40.0, 0.0, -30.0}, {-40.0, 0.0, 30.0}}));
    const auto before = gp::pageBytes(set.id(), "P");
    const json e = run("texEditPath", {{"args", {"Panel", "P"}}, {"stroke", r["stroke"]}, {"points", {{30.0, 0.0, -30.0}, {30.0, 0.0, 30.0}}}});
    CHECK(e["stroke"].get<uint64_t>() != r["stroke"].get<uint64_t>());
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(0, -40.0, 0.0))[3] == 0.0f);
    CHECK(at(img, seamUv(1, 30.0, 0.0))[3] > 0.5f);

    // The same edit on a fresh copy of the first path gives the same bytes.
    const auto edited = gp::pageBytes(set.id(), "P");
    run("texRasterize", {{"args", {"Panel", "P"}}});
    CHECK(gp::pageBytes(set.id(), "P") == edited);

    run("undo");
    gp::sync(sg(), set);
    CHECK(gp::pageBytes(set.id(), "P") == before);
}

TEST_CASE("texture paint path: Paint Along Curve projects a scene curve onto the mesh")
{
    if (!gpu())
        return;
    lila::curve::registerTypes();
    lila::curve::registerCommands();
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    // A curve hovering 10 above the panel along z at x = 25.
    const std::string curve = run("curve", {{"points", {{25.0, 10.0, -30.0}, {25.0, 10.0, 0.0}, {25.0, 10.0, 30.0}}}, {"type", "poly"}}).get<std::string>();
    json call = pathCall(json());
    call.erase("points");
    call["curve"] = curve;
    run("texPaintPath", call);
    const image::Image img = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(img, seamUv(1, 25.0, -25.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 25.0, 25.0))[3] > 0.5f);
    CHECK(at(img, seamUv(0, -25.0, 0.0))[3] == 0.0f);
    CHECK(tp::readPaint(set)->buffer("P")->strokes.front()->path.value("curve", std::string()) == curve);
}
