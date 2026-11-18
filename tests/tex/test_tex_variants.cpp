#include "TexTest.h"

#include "core/ImageIO.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
image::Image variantComposite(const char* channel)
{
    scene::Node& set = *tex::findSet(scene::Graph::current(), "Crate");
    const tex::SetSettings s = tex::readSettings(set);
    return tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), 1001, 16, 16, "cpu").at(channel);
}
} // namespace

TEST_CASE("tex variants: overrides change only what they touch, edits go to the variant shown, export writes every variant")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 16}});
    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Base"}, {"material", {{"baseColor", {0.5, 0.5, 0.5}}, {"roughness", 0.6}}}});
    const std::string red =
        run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Paint"}, {"material", {{"baseColor", {1.0, 0.0, 0.0}}}}})["id"];

    const float baseRed = texel(variantComposite("baseColor"), 4, 4)[0];
    CHECK(baseRed == doctest::Approx(1.0f).epsilon(0.01));

    // A variant: edits while it shows are its overrides.
    run("texAddVariant", {{"args", {"Crate", "Blue"}}});
    run("texSetLayer", {{"args", {"Crate", red, {{"material", {{"baseColor", {0.0, 0.0, 1.0}}}}}}}});
    CHECK(texel(variantComposite("baseColor"), 4, 4)[2] == doctest::Approx(1.0f).epsilon(0.01));
    const json v = run("texVariants", {{"args", {"Crate"}}});
    CHECK(v["active"] == "Blue");
    CHECK(v["variants"]["Blue"].contains(red));

    // The base is untouched.
    run("texSwitchVariant", {{"args", {"Crate", ""}}});
    CHECK(texel(variantComposite("baseColor"), 4, 4)[0] == doctest::Approx(1.0f).epsilon(0.01));

    // An override set from outside: the second variant hides the paint layer.
    run("texAddVariant", {{"args", {"Crate", "Plain"}}});
    run("texSwitchVariant", {{"args", {"Crate", ""}}});
    run("texSetVariant", {{"args", {"Crate", "Plain", red, {{"enabled", false}}}}});
    CHECK(texel(variantComposite("baseColor"), 4, 4)[0] == doctest::Approx(1.0f).epsilon(0.01));

    const std::filesystem::path dir = tempDir("variants");
    const json out = run("texExport", {{"args", {"Crate"}}, {"preset", "gltf"}, {"folder", dir.string()}, {"variants", true}, {"backend", "cpu"}});
    REQUIRE(out["variants"].size() == 3);
    auto baseColorFile = [&](const char* variant) {
        for (const json& f : out["variants"][variant]["outputs"])
            if (f.value("map", std::string()) == "baseColor")
                return f["file"].get<std::string>();
        return std::string();
    };
    const std::string b = baseColorFile("base"), blue = baseColorFile("Blue"), plain = baseColorFile("Plain");
    REQUIRE(!b.empty());
    CHECK(b != blue);
    CHECK(blue.find("Blue") != std::string::npos);
    const image::Image ib = image::read(b), iblue = image::read(blue), iplain = image::read(plain);
    CHECK(texel(ib, 4, 4)[0] > 0.9f);
    CHECK(texel(iblue, 4, 4)[2] > 0.9f);
    CHECK(texel(iblue, 4, 4)[0] < 0.1f);
    CHECK(texel(iplain, 4, 4)[0] == doctest::Approx(texel(iplain, 4, 4)[2]).epsilon(0.02));
    // Maps without an override come out the same in every variant.
    for (const json& f : out["variants"]["base"]["outputs"])
        if (f.value("map", std::string()) != "baseColor")
            for (const json& g : out["variants"]["Blue"]["outputs"])
                if (g.value("map", std::string()) == f.value("map", std::string()))
                    CHECK(maxDiff(image::read(f["file"].get<std::string>()), image::read(g["file"].get<std::string>())) == 0.0);
    CHECK(run("texVariants", {{"args", {"Crate"}}})["active"] == "");

    run("texRemoveVariant", {{"args", {"Crate", "Plain"}}});
    CHECK_FALSE(run("texVariants", {{"args", {"Crate"}}})["variants"].contains("Plain"));
}
