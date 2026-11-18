#include "TexPaintTest.h"

#include "render/PaintGpu.h"
#include "tex/PaintStrokes.h"
#include "tex/TextureSet.h"

using namespace textest;
namespace gp = lila::render::paint;

TEST_CASE("texture paint picker: picked values equal the evaluated channels and the layer's own paint at a texel")
{
    if (!gpu())
        return;
    newScene();
    addMesh("PanelMesh", seamPanel());
    scene::Node& set = makeSet("Panel", 128);
    json brush = brushJson();
    run("texPaintStroke", {{"args", {"Panel"}},
                           {"layer", "P"},
                           {"stroke",
                            {{"space", "surface"},
                             {"mesh", "PanelMesh"},
                             {"brush", brush},
                             {"payload", {{"channels", {{"baseColor", {0.25, 0.5, 0.75}}, {"roughness", 0.3}}}}},
                             {"dabs", {{{"position", {-25.0, 0.0, 0.0}}, {"radius", 15.0}}}}}}});
    const glm::vec2 uv = seamUv(0, -24.0, 1.0);

    // The layer's own paint: exactly what the stroke wrote there.
    const json layer = run("texSampleChannels", {{"args", {"Panel"}}, {"uv", {uv.x, uv.y}}, {"source", "layer"}, {"layer", "P"}});
    const image::Image painted = gp::readChannel(set.id(), "P", "baseColor");
    const float* t = at(painted, uv);
    REQUIRE(layer["channels"].contains("baseColor"));
    CHECK(layer["channels"]["baseColor"][0].get<float>() == doctest::Approx(t[0]));
    CHECK(layer["channels"]["baseColor"][2].get<float>() == doctest::Approx(t[2]));
    CHECK_FALSE(layer["channels"].contains("metallic"));

    // The set: every channel as evaluated, the same texel as texEvaluate's images.
    const json all = run("texSampleChannels", {{"args", {"Panel"}}, {"uv", {uv.x, uv.y}}});
    const tex::SetSettings s = tex::readSettings(set);
    const auto images = tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), 1001, s.width, s.height);
    for (const auto& [name, img] : images)
    {
        INFO(name);
        REQUIRE(all["channels"].contains(name));
        const json& v = all["channels"][name];
        const float* e = at(img, uv);
        CHECK((v.is_array() ? v[0].get<float>() : v.get<float>()) == doctest::Approx(e[0]));
    }

    // Picked by position on the mesh, the same uv.
    const json byPos = run("texSampleChannels", {{"args", {"Panel"}}, {"mesh", "PanelMesh"}, {"position", {-24.0, 0.0, 1.0}}});
    CHECK(byPos["uv"][0].get<double>() == doctest::Approx(uv.x).epsilon(1e-4));
    CHECK(byPos["channels"] == all["channels"]);
}
