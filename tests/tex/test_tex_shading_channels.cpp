#include "TexTest.h"

#include "bake/Targets.h"
#include "core/ImageIO.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "tex/ShadingChannels.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <map>

using namespace textest;
namespace sh = lila::tex::shading;

TEST_CASE("shading channels: added once, with neutral Material Output values, and undone")
{
    run("newScene");
    UndoStack::get().clear();
    run("texCreateSet", {{"args", {"Glass"}}, {"resolution", 8}});
    json added = run("texAddShadingChannels", {{"args", {"Glass"}}});
    CHECK(added.size() == sh::channels().size());
    CHECK(run("texAddShadingChannels", {{"args", {"Glass"}}}).empty());
    scene::Node* set = findSet(scene::Graph::current(), "Glass");
    REQUIRE(set);
    const SetSettings s = readSettings(*set);
    for (const sh::ChannelInfo& c : sh::channels())
        CHECK(s.channel(c.name) != nullptr);
    const Graph g = readGraph(*set);
    const GNode* out = g.firstOfType(kMaterialOutputType);
    REQUIRE(out);
    CHECK(out->values["specularWeight"].get<double>() == doctest::Approx(1.0));
    CHECK(out->values["ior"].get<double>() == doctest::Approx(0.25));
    CHECK(sh::iorFromChannel(0.25f) == doctest::Approx(1.5f));
    UndoStack::get().undo();
    CHECK(readSettings(*findSet(scene::Graph::current(), "Glass")).channel("coat") == nullptr);
    CHECK_THROWS(run("texAddShadingChannels", {{"args", {"Glass"}}, {"channels", {"glitter"}}}));
}

TEST_CASE("shading channels: names follow the export preset's renderer")
{
    bake::TargetProfile t;
    t.name = "openpbr";
    std::vector<bake::TargetMap> maps;
    sh::addExportMaps(maps, t);
    REQUIRE(maps.size() == sh::channels().size());
    std::map<std::string, const bake::TargetMap*> byMap;
    for (const bake::TargetMap& m : maps)
    {
        byMap[m.settings.map] = &m;
        CHECK(m.optional);
    }
    CHECK(byMap["coat"]->settings.suffix == "coat_weight");
    CHECK(byMap["sheenColor"]->slot == "fuzz_color");
    CHECK(byMap["subsurfaceRadius"]->settings.bits == 16);
    t.name = "gltfOrm";
    CHECK(sh::familyOf(t) == "gltf");
    CHECK(sh::nameFor(*sh::find("coatRoughness"), "gltf") == "KHR_materials_clearcoat.clearcoatRoughnessTexture.g");
    t.name = "unrealCrm";
    CHECK(sh::familyOf(t) == "unreal");
    CHECK(sh::nameFor(*sh::find("anisotropyDirection"), "unreal") == "Tangent");
    // A preset that already lists a channel keeps its own entry.
    maps.clear();
    bake::TargetMap own;
    own.settings.map = "coat";
    own.settings.suffix = "Mine";
    maps.push_back(own);
    sh::addExportMaps(maps, t);
    CHECK(maps.front().settings.suffix == "Mine");
    CHECK(maps.size() == sh::channels().size());
}

TEST_CASE("shading channels round-trip through the export presets")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Glass"}}, {"resolution", 8}});
    run("texAddShadingChannels", {{"args", {"Glass"}}});
    const std::map<std::string, std::vector<float>> values = {
        {"sheenColor", {0.2f, 0.4f, 0.6f}}, {"sheenRoughness", {0.3f}}, {"coat", {0.7f}},           {"coatRoughness", {0.15f}},
        {"coatNormal", {0.5f, 0.5f, 1.0f}}, {"subsurfaceColor", {0.8f, 0.3f, 0.2f}},              {"subsurfaceRadius", {0.6f}},
        {"anisotropy", {0.45f}},           {"anisotropyDirection", {0.9f, 0.6f, 0.5f}},           {"transmission", {0.35f}},
        {"ior", {0.25f}},                  {"specularWeight", {0.8f}},                            {"specularColor", {0.9f, 0.8f, 0.7f}},
    };
    for (const auto& [name, v] : values)
    {
        json value = v.size() == 1 ? json(v[0]) : json{v[0], v[1], v[2], 1.0f};
        run("texSetValue", {{"args", {"Glass", "Output." + name, value}}});
    }
    for (const char* preset : {"unreal", "substancePainter"})
    {
        CAPTURE(preset);
        const std::filesystem::path dir = tempDir(std::string("shading_") + preset);
        json r = run("texExport", {{"args", {"Glass"}}, {"preset", preset}, {"folder", dir.string()}, {"backend", "cpu"}});
        const std::optional<bake::TargetProfile> target = bake::findTarget(preset);
        REQUIRE(target);
        const std::string family = sh::familyOf(*target);
        int found = 0;
        for (const json& o : r["outputs"])
        {
            const std::string map = o["map"];
            auto it = values.find(map);
            if (it == values.end())
                continue;
            ++found;
            const sh::ChannelInfo* c = sh::find(map);
            const std::string file = o["file"];
            CHECK(file.find(family == "openpbr" ? std::string(c->openPbr) : std::string(c->suffix)) != std::string::npos);
            std::string err;
            image::Image img = image::read(std::filesystem::path(file), &err);
            REQUIRE_MESSAGE(img.width == 8, err);
            const float* t = texel(img, 3, 3);
            for (size_t k = 0; k < it->second.size(); ++k)
                CHECK(t[k] == doctest::Approx(it->second[k]).epsilon(0.01));
        }
        CHECK(found == int(values.size()));
    }
}
