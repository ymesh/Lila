#include "TexTest.h"

#include "tex/TextureSet.h"

using namespace textest;

namespace
{
void buildStack()
{
    run("newScene");
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 32}});
    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Base"}, {"material", {{"baseColor", {0.5, 0.5, 0.5}}, {"roughness", 0.6}}}});
    const std::string red = run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Red"}, {"material", {{"baseColor", {1.0, 0.0, 0.0}}, {"roughness", 0.2}}}})["id"];
    run("texAddMask", {{"args", {"Crate", red, "fill"}}, {"value", 0.4}});
    run("texAddAnchor", {{"args", {"Crate", red, "rust"}}});
    run("texAddLayer", {{"args", {"Crate", "fill"}}, {"name", "Top"}, {"material", {{"metallic", 1.0}}}});
}

std::map<std::string, image::Image> composite()
{
    scene::Node& set = *tex::findSet(scene::Graph::current(), "Crate");
    const tex::SetSettings s = tex::readSettings(set);
    return tex::evaluateOutputs(set, tex::channelRequests(tex::readGraph(set), s), 1001, 32, 32, "cpu");
}

json stackLayers() { return run("texLayerStack", {{"args", {"Crate"}}})["layers"]; }
} // namespace

TEST_CASE("tex layer clipboard: flattening keeps the composite, merge down too")
{
    buildStack();
    const auto before = composite();
    const json layers = stackLayers();
    const std::string base = layers[0]["id"], red = layers[1]["id"];
    const json r = run("texFlattenLayers", {{"args", {"Crate"}}, {"layers", {base, red}}});
    CHECK(r["channels"].contains("baseColor"));
    CHECK(r["channels"].contains("roughness"));
    const json after = stackLayers();
    REQUIRE(after.size() == 2);
    CHECK(after[0]["kind"] == "paint");
    const auto now = composite();
    for (const auto& [name, img] : before)
    {
        INFO(name);
        CHECK(maxDiff(img, now.at(name)) < 2e-3);
    }

    // Undo brings the layers back; merge down of Top into the stack below keeps it too.
    run("undo");
    CHECK(stackLayers().size() == 3);
    const std::string top = stackLayers()[2]["id"];
    run("texFlattenLayers", {{"args", {"Crate", top}}, {"down", true}, {"keep", true}});
    CHECK(stackLayers().size() == 4);
    const auto merged = composite();
    for (const auto& [name, img] : before)
        CHECK(maxDiff(img, merged.at(name)) < 2e-3);
}

TEST_CASE("tex layer clipboard: copied layers paste with their masks and anchors, into another set too")
{
    buildStack();
    const std::string red = stackLayers()[1]["id"];
    const std::string text = run("texCopyLayers", {{"args", {"Crate", red}}});
    const json pasted = run("texPasteLayers", {{"args", {"Crate", text}}});
    REQUIRE(pasted["layers"].size() == 1);
    const json l = stackLayers().back();
    CHECK(l["id"] != red);
    CHECK(l["name"] == "Red");
    REQUIRE(l["masks"].size() == 1);
    CHECK(l["masks"][0]["value"] == 0.4);
    CHECK(l.contains("anchor"));
    CHECK(l["anchor"] != "rust");

    run("texCreateSet", {{"args", {"Other"}}, {"resolution", 32}});
    run("texPasteLayers", {{"args", {"Other", text}}});
    const json other = run("texLayerStack", {{"args", {"Other"}}})["layers"];
    REQUIRE(!other.empty());
    CHECK(other.back()["masks"].size() == 1);
    CHECK(other.back()["anchor"] == "rust");
}

TEST_CASE("tex layer clipboard: exporting flattened layers writes what flattening would make")
{
    buildStack();
    const std::string red = stackLayers()[1]["id"];
    const json r = run("texExportFlattened", {{"args", {"Crate", red}}, {"folder", tempDir("flat").string()}, {"format", "exr"}});
    REQUIRE(r["files"].contains("baseColor"));
    std::string err;
    const image::Image img = image::read(r["files"]["baseColor"].get<std::string>(), &err);
    REQUIRE(img.width == 32);
    CHECK(stackLayers().size() == 3);
}
