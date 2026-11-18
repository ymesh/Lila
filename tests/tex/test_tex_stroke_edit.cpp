#include "TexPaintTest.h"

#include "render/PaintGpu.h"

using namespace textest;
namespace gp = lila::render::paint;

namespace
{
json dot(const std::string& mesh, double x, double z, glm::vec3 c)
{
    return {{"space", "surface"},
            {"mesh", mesh},
            {"brush", brushJson()},
            {"payload", {{"channels", {{"baseColor", {c.r, c.g, c.b}}}}}},
            {"dabs", {{{"position", {x, 0.0, z}}, {"radius", 5.0}}}}};
}
} // namespace

TEST_CASE("texture paint stroke editing: a stroke's colour changes only its texels; delete, reorder and pick work on the records")
{
    if (!gpu())
        return;
    newScene();
    const std::string panel = addMesh("Panel", seamPanel());
    scene::Node& set = makeSet("Set", 128);
    paintStroke("Set", "P", dot(panel, 30.0, 0.0, {1.0f, 0.0f, 0.0f}));
    paintStroke("Set", "P", dot(panel, 30.0, 30.0, {0.0f, 1.0f, 0.0f}));
    json strokes = run("texStrokes", {{"args", {"Set", "P"}}});
    REQUIRE(strokes.size() == 2);
    const uint64_t first = strokes[0]["id"], second = strokes[1]["id"];

    const image::Image before = gp::readChannel(set.id(), "P", "baseColor");
    const json edited = run("texEditStroke", {{"args", {"Set", "P"}}, {"stroke", first}, {"color", {0.0, 0.0, 1.0}}});
    const image::Image after = gp::readChannel(set.id(), "P", "baseColor");
    const float* a = at(after, seamUv(1, 30.0, 0.0));
    CHECK(a[2] > 0.9f);
    CHECK(a[0] < 0.1f);
    // Every texel outside the first stroke's dab is as it was.
    int changedOutside = 0;
    const glm::vec2 c = seamUv(1, 30.0, 0.0);
    for (int y = 0; y < after.height; ++y)
        for (int x = 0; x < after.width; ++x)
        {
            const glm::vec2 uv((x + 0.5f) / after.width, 1.0f - (y + 0.5f) / after.height);
            if (glm::distance(uv, c) < 0.08f)
                continue;
            const size_t i = (size_t(y) * after.width + x) * 4;
            for (int k = 0; k < 4; ++k)
                changedOutside += std::abs(after.rgba[i + k] - before.rgba[i + k]) > 1e-4f ? 1 : 0;
        }
    CHECK(changedOutside == 0);
    const uint64_t firstNow = edited["id"];
    CHECK(firstNow != first);

    // Picking finds the stroke under a point, topmost first.
    const json picked = run("texPickStroke", {{"args", {"Set", "P"}}, {"position", {30.0, 0.0, 30.0}}});
    REQUIRE(picked.is_object());
    CHECK(picked["id"] == second);
    CHECK(run("texPickStroke", {{"args", {"Set", "P"}}, {"position", {-40.0, 0.0, -40.0}}}).is_null());

    // Reordering keeps both; the moved stroke is now the bottom one.
    run("texMoveStroke", {{"args", {"Set", "P"}}, {"stroke", second}, {"index", 0}});
    strokes = run("texStrokes", {{"args", {"Set", "P"}}});
    CHECK(strokes[0]["id"] == second);

    // Deleting the green stroke clears its texels and leaves the blue one.
    run("texDeleteStrokes", {{"args", {"Set", "P"}}, {"strokes", {second}}});
    const image::Image gone = gp::readChannel(set.id(), "P", "baseColor");
    CHECK(at(gone, seamUv(1, 30.0, 30.0))[3] < 0.01f);
    CHECK(at(gone, seamUv(1, 30.0, 0.0))[2] > 0.9f);

    run("undo");
    CHECK(run("texStrokes", {{"args", {"Set", "P"}}}).size() == 2);
}

TEST_CASE("texture paint copy and paste: a painted layer's strokes come through the clipboard text")
{
    if (!gpu())
        return;
    newScene();
    const std::string panel = addMesh("Panel", seamPanel());
    scene::Node& set = makeSet("Set", 64);
    const std::string layer = run("texAddLayer", {{"args", {"Set", "paint"}}})["id"];
    paintStroke("Set", layer, dot(panel, 30.0, 0.0, {1.0f, 0.0f, 0.0f}));
    const std::string text = run("texCopyLayers", {{"args", {"Set"}}, {"layers", {layer}}});
    const json pasted = run("texPasteLayers", {{"args", {"Set", text}}});
    REQUIRE(pasted["layers"].size() == 1);
    // The original's buffer and the pasted copy's each hold the one stroke.
    int painted = 0;
    for (const auto& [name, buf] : tex::paint::readPaint(set)->buffers)
        painted += buf.strokes.size() == 1 ? 1 : 0;
    CHECK(painted == 2);
}
