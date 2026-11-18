#include "TexTest.h"

#include "core/ImageIO.h"
#include "tex/MaterialFromMaps.h"

#include <filesystem>

using namespace textest;
namespace mm = lila::tex::maps;

TEST_CASE("tex material from maps: channel tokens in file names")
{
    struct Row
    {
        const char* file;
        const char* channel;
        bool directX = false;
        bool gloss = false;
    };
    const Row rows[] = {
        {"Rock01_BaseColor.png", "baseColor"},   {"rock_albedo_4k.jpg", "baseColor"},  {"Bricks_Diff.tga", "baseColor"},
        {"Bricks_Color.png", "baseColor"},       {"metal_Roughness.png", "roughness"}, {"Plank_rough.jpg", "roughness"},
        {"Plank_Glossiness.png", "roughness", false, true},                            {"x_Metalness.png", "metallic"},
        {"Tile_Normal_GL.png", "normal"},        {"Tile_nrm.png", "normal"},           {"Tile_Normal_DX.png", "normal", true},
        {"Tile_NormalDX.png", "normal", true},   {"Tile_Height.exr", "height"},        {"Tile_Disp.png", "height"},
        {"Tile_AO.png", "ao"},                   {"Tile_AmbientOcclusion.png", "ao"},  {"Lamp_Emissive.png", "emissive"},
        {"Leaf_Opacity.png", "opacity"},         {"Crate_ORM.png", "packed"},          {"Crate_arm.png", "packed"},
    };
    for (const Row& r : rows)
    {
        INFO(r.file);
        const std::optional<mm::MapRole> role = mm::classify(r.file);
        REQUIRE(role.has_value());
        CHECK(role->channel == r.channel);
        CHECK(role->directX == r.directX);
        CHECK(role->gloss == r.gloss);
    }
    CHECK_FALSE(mm::classify("readme.png").has_value());
    CHECK(mm::classify("Crate_ORM.png")->packed == std::vector<std::string>{"ao", "roughness", "metallic"});
    CHECK(mm::sizeFromName("Rock_200cm_BaseColor.png") == doctest::Approx(200.0));
    CHECK(mm::sizeFromName("Rock_2m_Normal.png") == doctest::Approx(200.0));
    CHECK(mm::sizeFromName("Rock_1.5x1.5m_AO.png") == doctest::Approx(150.0));
    CHECK(mm::sizeFromName("Rock_4k_AO.png") == 0.0);
}

TEST_CASE("tex material from maps: files group into materials, packed maps split and DirectX normals flip")
{
    const std::filesystem::path dir = tempDir("from_maps");
    auto write = [&](const std::string& name, float r, float g, float b) {
        std::vector<float> px(8 * 8 * 4);
        for (size_t i = 0; i < px.size(); i += 4)
            px[i] = r, px[i + 1] = g, px[i + 2] = b, px[i + 3] = 1.0f;
        image::writeExr(dir / name, 8, 8, px, 32);
        return dir / name;
    };
    const std::vector<std::filesystem::path> files = {write("Crate_BaseColor.exr", 0.5f, 0.25f, 0.125f), write("Crate_ORM.exr", 0.9f, 0.3f, 1.0f),
                                                      write("Crate_Normal_DX.exr", 0.5f, 0.75f, 1.0f), write("Rock_Roughness.exr", 0.6f, 0.6f, 0.6f)};
    const std::vector<mm::MapMaterial> mats = mm::group(files);
    REQUIRE(mats.size() == 2);
    const mm::MapMaterial& crate = mats[0].name == "crate" ? mats[0] : mats[1];
    CHECK(crate.maps.size() == 3);

    const Graph g = mm::buildGraph(crate);
    auto channel = [&](const std::string& name, PortType type) {
        PlanRequest r;
        r.width = r.height = 8;
        r.outputs = materialRequests(g, {{name, type}});
        Plan p = buildPlan(g, r);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        return evaluateCpu(p).outputs.at(name);
    };
    CHECK(texel(channel("ao", PortType::Gray), 3, 3)[0] == doctest::Approx(0.9).epsilon(0.01));
    CHECK(texel(channel("roughness", PortType::Gray), 3, 3)[0] == doctest::Approx(0.3).epsilon(0.01));
    CHECK(texel(channel("metallic", PortType::Gray), 3, 3)[0] == doctest::Approx(1.0).epsilon(0.01));
    // Green 0.75 in DirectX's convention is 0.25 in OpenGL's.
    CHECK(texel(channel("normal", PortType::Color), 3, 3)[1] == doctest::Approx(0.25).epsilon(0.01));
    CHECK(texel(channel("baseColor", PortType::Color), 3, 3)[0] == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("tex material from maps: texMaterialFromMaps writes a library material per group")
{
    const std::filesystem::path dir = tempDir("from_maps_cmd");
    std::vector<float> px(4 * 4 * 4, 0.5f);
    image::writeExr(dir / "Moss_200cm_Albedo.exr", 4, 4, px, 32);
    image::writeExr(dir / "Moss_200cm_Rough.exr", 4, 4, px, 32);
    const json r = run("texMaterialFromMaps", {{"folder", dir.string()}, {"path", (dir / "out").string()}});
    REQUIRE(r.size() == 1);
    CHECK(r[0]["maps"].size() == 2);
    CHECK(r[0]["scaleCm"].get<double>() == doctest::Approx(200.0));
    CHECK(std::filesystem::exists(r[0]["file"].get<std::string>()));
}
