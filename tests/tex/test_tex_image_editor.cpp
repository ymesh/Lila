#include "TexPaintTest.h"

#include "render/PaintGpu.h"

#include <filesystem>

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint image editor: an unedited capture projects back to the same paint, saved views recall exactly")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    const json view = {{"eye", {0.0, 200.0, 0.01}}, {"lookAt", {0.0, 0.0, 0.0}}, {"up", {0.0, 0.0, -1.0}}, {"fov", 40.0}, {"width", 512}, {"height", 384}};
    // A flat colour on both faces.
    run("texPaintStroke", {{"args", {"Panel", "P"}},
                           {"stroke", {{"mesh", "PanelMesh"}, {"payload", {{"kind", "fill"}, {"fillBy", "mesh"}, {"channels", {{"baseColor", {0.2, 0.55, 0.8}}}}}}}}});

    run("texProjectionView", {{"args", {"Panel", "front"}}, {"view", view}});
    CHECK(run("texProjectionView", {{"args", {"Panel", "front"}}}) == view);

    const std::string file = (tempDir("image_editor") / "capture.png").string();
    const json cap = run("texCaptureView", {{"args", {"Panel"}}, {"view", "front"}, {"file", file}, {"layer", "P"}});
    CHECK(cap["width"] == 512);
    REQUIRE(std::filesystem::exists(file));

    json payload = {{"kind", "project"}, {"image", file}, {"channels", json::object()}, {"colorChannel", "baseColor"}};
    run("texPaintStroke", {{"args", {"Panel", "Q"}}, {"stroke", {{"mesh", "PanelMesh"}, {"payload", payload}}}, {"view", "front"}});
    const image::Image p = gp::readChannel(set.id(), "P", "baseColor"), q = gp::readChannel(set.id(), "Q", "baseColor");
    for (const glm::vec2 uv : {seamUv(0, -20.0, 0.0), seamUv(1, 20.0, 10.0), seamUv(0, -5.0, -20.0)})
    {
        const float *a = at(p, uv), *b = at(q, uv);
        REQUIRE(b[3] > 0.9f);
        for (int c = 0; c < 3; ++c)
            CHECK(b[c] == doctest::Approx(a[c]).epsilon(0.01));
    }
}
