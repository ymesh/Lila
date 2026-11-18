#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"

using namespace textest;
namespace gp = lila::render::paint;
namespace tp = lila::tex::paint;

namespace
{
const json kView = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 800}, {"height", 600}};

/** A stroke right across the panel, both faces. */
json acrossStroke(json payload)
{
    json brush = brushJson();
    brush["radius"] = 25.0;
    return {{"points", {{250.0, 300.0, 1.0}, {320.0, 300.0, 1.0}, {400.0, 300.0, 1.0}, {480.0, 300.0, 1.0}, {550.0, 300.0, 1.0}}},
            {"view", kView},
            {"brush", brush},
            {"payload", std::move(payload)}};
}

std::string paintLayer()
{
    return run("texAddLayer", {{"args", {"Panel", "paint"}}})["id"].get<std::string>();
}
} // namespace

TEST_CASE("texture paint selection: a screen lasso masks what it encloses and the view sees, add and subtract follow")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);
    const std::string layer = paintLayer();
    // Screen x 300..390 is world x -24..-2: the left face only.
    run("texSelectionMask", {{"args", {"Panel", layer}}, {"points", {{300.0, 200.0}, {390.0, 200.0}, {390.0, 400.0}, {300.0, 400.0}}}, {"view", kView}});
    image::Image m = gp::readChannel(set.id(), layer, "mask");
    CHECK(at(m, seamUv(0, -15.0, 0.0))[3] > 0.99f);
    CHECK(at(m, seamUv(0, -40.0, 0.0))[3] == 0.0f);
    CHECK(at(m, seamUv(1, 15.0, 0.0))[3] == 0.0f);

    run("texSelectionMask", {{"args", {"Panel", layer}}, {"rect", {410.0, 200.0, 500.0, 400.0}}, {"view", kView}, {"mode", "add"}});
    m = gp::readChannel(set.id(), layer, "mask");
    CHECK(at(m, seamUv(0, -15.0, 0.0))[3] > 0.99f);
    CHECK(at(m, seamUv(1, 15.0, 0.0))[3] > 0.99f);

    run("texSelectionMask", {{"args", {"Panel", layer}}, {"rect", {300.0, 200.0, 390.0, 400.0}}, {"view", kView}, {"mode", "subtract"}});
    m = gp::readChannel(set.id(), layer, "mask");
    CHECK(at(m, seamUv(0, -15.0, 0.0))[3] == 0.0f);
    CHECK(at(m, seamUv(1, 15.0, 0.0))[3] > 0.99f);

    // In uv space and from a face group: whole triangles.
    run("texSelectionMask", {{"args", {"Panel", layer}}, {"uvPoints", {{0.5, 0.0}, {1.0, 0.0}, {1.0, 1.0}, {0.5, 1.0}}}, {"mesh", "PanelMesh"}});
    m = gp::readChannel(set.id(), layer, "mask");
    CHECK(at(m, seamUv(1, 25.0, 0.0))[3] > 0.99f);
    CHECK(at(m, seamUv(0, -25.0, 0.0))[3] == 0.0f);
    run("texFaceGroup", {{"args", {"Panel", "left"}}, {"faces", {0}}, {"mesh", "PanelMesh"}});
    run("texSelectionMask", {{"args", {"Panel", layer}}, {"group", "left"}, {"mesh", "PanelMesh"}});
    m = gp::readChannel(set.id(), layer, "mask");
    CHECK(at(m, seamUv(0, -25.0, 0.0))[3] > 0.99f);
    CHECK(at(m, seamUv(1, 25.0, 0.0))[3] == 0.0f);
}

TEST_CASE("texture paint selection: a quick mask limits strokes, locked faces stay untouched, and both replay")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 256);

    // Quick mask: only the left half of uv space (the left face's island).
    std::vector<float> rgba(64 * 64 * 4, 0.0f);
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 32; ++x)
            for (int c = 0; c < 4; ++c)
                rgba[(size_t(y) * 64 + x) * 4 + c] = 1.0f;
    const json limit = tp::PaintImage::fromRgba(64, 64, rgba)->toJson();
    json call = acrossStroke({{"channels", {{"baseColor", {0.0, 0.0, 1.0}}}}, {"limit", limit}});
    call["args"] = {"Panel"};
    call["layer"] = "Q";
    run("texPaintStroke", call);
    image::Image img = gp::readChannel(set.id(), "Q", "baseColor");
    CHECK(at(img, seamUv(0, -15.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 15.0, 0.0))[3] == 0.0f);
    const auto limited = gp::pageBytes(set.id(), "Q");
    run("texRasterize", {{"args", {"Panel", "Q"}}});
    CHECK(gp::pageBytes(set.id(), "Q") == limited);

    // Locked faces: the right face (1) keeps nothing of the stroke.
    call = acrossStroke({{"channels", {{"baseColor", {1.0, 1.0, 0.0}}}}, {"lockedFaces", {1}}});
    call["args"] = {"Panel"};
    call["layer"] = "L";
    run("texPaintStroke", call);
    img = gp::readChannel(set.id(), "L", "baseColor");
    CHECK(at(img, seamUv(0, -15.0, 0.0))[3] > 0.5f);
    CHECK(at(img, seamUv(1, 15.0, 0.0))[3] == 0.0f);
    CHECK_FALSE(tp::readPaint(set)->buffer("L")->strokes.front()->payload.locked.empty());
    const auto locked = gp::pageBytes(set.id(), "L");
    run("texRasterize", {{"args", {"Panel", "L"}}});
    CHECK(gp::pageBytes(set.id(), "L") == locked);
}
