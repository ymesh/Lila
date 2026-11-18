#include "TexTest.h"

#include "core/ImageIO.h"

#include <filesystem>

using namespace textest;
namespace fs = std::filesystem;

namespace
{
json exportWith(const std::string& preset)
{
    run("newScene");
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 16}});
    run("texAddLayer", {{"args", {"Crate", "fill"}},
                        {"material", {{"baseColor", {0.5, 0.25, 0.125}}, {"roughness", 0.3}, {"metallic", 1.0}, {"ao", 0.6}, {"emissive", {0.0, 0.0, 0.0}}}}});
    return run("texExport", {{"args", {"Crate"}}, {"preset", preset}, {"folder", tempDir("presets_" + preset).string()}, {"backend", "cpu"}});
}

std::string fileWith(const json& r, const std::string& suffix)
{
    for (const json& f : r["files"])
    {
        const std::string name = fs::path(f.get<std::string>()).filename().string();
        if (name.find("_" + suffix + ".") != std::string::npos)
            return f.get<std::string>();
    }
    return {};
}

std::array<float, 4> pixel(const std::string& file)
{
    std::string err;
    const image::Image img = image::read(file, &err);
    REQUIRE_MESSAGE(img.width > 0, err);
    const float* t = texel(img, 4, 4);
    return {t[0], t[1], t[2], t[3]};
}
} // namespace

TEST_CASE("tex export presets: each preset's names and packing")
{
    {
        const json r = exportWith("unityHdrp");
        const std::string mask = fileWith(r, "MaskMap");
        REQUIRE(!mask.empty());
        CHECK(!fileWith(r, "BaseMap").empty());
        CHECK(!fileWith(r, "Normal").empty());
        const auto p = pixel(mask);
        CHECK(p[0] == doctest::Approx(1.0).epsilon(0.01));
        CHECK(p[1] == doctest::Approx(0.6).epsilon(0.01));
        CHECK(p[2] == doctest::Approx(1.0).epsilon(0.01));
        CHECK(p[3] == doctest::Approx(0.7).epsilon(0.01));
    }
    {
        const json r = exportWith("unityUrp");
        const auto p = pixel(fileWith(r, "MetallicSmoothness"));
        CHECK(p[0] == doctest::Approx(1.0).epsilon(0.01));
        CHECK(p[3] == doctest::Approx(0.7).epsilon(0.01));
        CHECK(!fileWith(r, "Occlusion").empty());
    }
    for (const char* preset : {"gltf", "godot"})
    {
        INFO(preset);
        const json r = exportWith(preset);
        const auto p = pixel(fileWith(r, std::string(preset) == "gltf" ? "occlusionRoughnessMetallic" : "orm"));
        CHECK(p[0] == doctest::Approx(0.6).epsilon(0.01));
        CHECK(p[1] == doctest::Approx(0.3).epsilon(0.01));
        CHECK(p[2] == doctest::Approx(1.0).epsilon(0.01));
        // OpenGL normals: a flat normal's green stays at the middle either way, so check the file is there.
        CHECK(!fileWith(r, "normal").empty());
    }
    {
        const json r = exportWith("openpbr");
        for (const char* s : {"base_color", "base_metalness", "specular_roughness", "geometry_normal"})
            CHECK_MESSAGE(!fileWith(r, s).empty(), s);
        CHECK(pixel(fileWith(r, "specular_roughness"))[0] == doctest::Approx(0.3).epsilon(0.01));
    }
}
