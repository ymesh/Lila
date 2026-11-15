#include "TexTest.h"

#include "scene/Graph.h"
#include "core/PrefStore.h"
#include "tex/PainterImport.h"
#include "tex/TextureSet.h"

#include <array>
#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** Made by tests/data/tex/make_painter_spsm.py: one preset in both of Painter's layouts, and an embedded 2x2 image. */
fs::path tinyFile()
{
    return fs::path(LILA_TEST_TEX_DATA) / "painter_tiny.spsm";
}

const json* child(const json& folder, const std::string& name)
{
    auto it = folder.find("children");
    if (it == folder.end())
        return nullptr;
    for (const json& c : *it)
        if (c.value("name", "") == name)
            return &c;
    return nullptr;
}

std::string layerId(const json& layers, const std::string& name)
{
    for (const json& l : layers)
    {
        if (l.value("name", "") == name)
            return l.value("id", "");
        if (std::string id = layerId(l.value("children", json::array()), name); !id.empty())
            return id;
    }
    return {};
}

/** A Qt property header (big-endian QVariantMap) in front of pixels, as Painter stores images. */
std::vector<uint8_t> qtImage(const std::vector<std::pair<std::string, std::pair<uint32_t, std::vector<uint8_t>>>>& props, const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> out;
    auto be32 = [&](uint32_t v) {
        for (int i = 3; i >= 0; --i)
            out.push_back(uint8_t(v >> (i * 8)));
    };
    be32(uint32_t(props.size()));
    for (const auto& [key, value] : props)
    {
        be32(uint32_t(key.size() * 2));
        for (char c : key)
            out.insert(out.end(), {0, uint8_t(c)});
        be32(value.first);
        out.push_back(0);
        out.insert(out.end(), value.second.begin(), value.second.end());
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::vector<uint8_t> be(uint64_t v, int n)
{
    std::vector<uint8_t> b;
    for (int i = n - 1; i >= 0; --i)
        b.push_back(uint8_t(v >> (i * 8)));
    return b;
}

std::vector<uint8_t> qstr(const std::string& s)
{
    std::vector<uint8_t> b = be(s.size() * 2, 4);
    for (char c : s)
        b.insert(b.end(), {0, uint8_t(c)});
    return b;
}
} // namespace

TEST_CASE("tex painter: the HDF5 container, both preset layouts and the embedded image read back")
{
    painter::SpsmFile f = painter::readSpsm(tinyFile());
    REQUIRE(f.presets.size() == 2);
    CHECK(f.presets[0]["layerGroup"]["label"] == "Tiny Old");
    CHECK(f.presets[1]["layerGroup"]["label"] == "Tiny");
    CHECK(f.presets[1]["preset_dataversion"] == 88);
    // Both layouts decode to the same shape.
    const json& a = f.presets[0]["layerGroup"]["subStack"]["items"][1];
    const json& b = f.presets[1]["layerGroup"]["subStack"]["items"][1];
    CHECK(a["@"] == "DataLayerColor");
    CHECK(a["perChannelBlending"] == b["perChannelBlending"]);
    REQUIRE(f.resources.contains("bb.image"));
    image::Image img = painter::decodeImage(f.resources.at("bb.image"));
    REQUIRE(img.width == 2);
    CHECK(img.rgba[0] == doctest::Approx(1.0f));
    CHECK(img.rgba[5] == doctest::Approx(1.0f));
    CHECK(img.rgba[15] == doctest::Approx(128.0f / 255.0f));
}

TEST_CASE("tex painter: LZ4 image data, and damaged presets refuse to decode")
{
    // "abcabcabcabc" as LZ4: three literals, then a match of 9 at offset 3.
    const std::vector<uint8_t> lz4 = {0x35, 'a', 'b', 'c', 3, 0};
    std::vector<uint8_t> bytes = qtImage({{"width", {3, be(3, 4)}},
                                          {"height", {3, be(1, 4)}},
                                          {"format", {10, qstr("LUM8")}},
                                          {"dataSizeRaw", {5, be(12, 8)}},
                                          {"dataSizeLZ4", {5, be(lz4.size(), 8)}},
                                          {"compression", {10, qstr("lz4")}}},
                                         lz4);
    image::Image img = painter::decodeImage(bytes);
    REQUIRE(img.width == 3);
    CHECK(img.rgba[0] == doctest::Approx(float('a') / 255.0f));
    CHECK(img.rgba[8] == doctest::Approx(float('c') / 255.0f));

    std::vector<uint8_t> preset = {0xdd, 0x2f, 0x7c, 0x1b, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 200, 0, 0, 0};
    CHECK_THROWS(painter::decodePreset(preset));
    CHECK_THROWS(painter::decodePreset({1, 2, 3}));
}

TEST_CASE("tex painter: a smart material converts into layers, masks and blend modes, and reports what it couldn't carry")
{
    const fs::path images = fs::temp_directory_path() / "lila_tex_tests" / "painter_images";
    painter::Conversion c = painter::convertSpsm(painter::readSpsm(tinyFile()), "Tiny", images);
    // One uuid in two layouts: only the newest stands.
    REQUIRE(c.materials.size() == 1);
    const json& root = c.materials[0].layer;
    CHECK(root["kind"] == "folder");
    CHECK(root["name"] == "Tiny");

    const json* base = child(root, "Base");
    REQUIRE(base);
    CHECK((*base)["kind"] == "fill");
    CHECK((*base)["material"]["baseColor"][2] == doctest::Approx(0.4));
    CHECK((*base)["material"]["roughness"] == doctest::Approx(0.6));
    // Painter's signed height 0.1 around 0 is 0.55 around Lila's 0.5.
    CHECK((*base)["material"]["height"] == doctest::Approx(0.55));
    CHECK(c.dropped.contains("channel specular"));

    const json* edges = child(root, "Edges");
    REQUIRE(edges);
    // Linear Dodge on height is Lila's default signed add; Color Burn on base colour; no setting for unwritten height.
    CHECK((*edges)["channels"]["baseColor"]["mode"] == "colorBurn");
    CHECK_FALSE((*edges)["channels"].contains("height"));
    const json& masks = (*edges)["masks"];
    REQUIRE(masks.size() == 2);
    CHECK(masks[0]["kind"] == "generator");
    CHECK(masks[0]["type"] == "genEdgeWear");
    CHECK(masks[1]["kind"] == "paint");
    CHECK(masks[1]["mode"] == "multiply");
    CHECK(c.images.contains(fs::path(masks[1]["image"].get<std::string>())));
    CHECK(c.approximated.contains("mg_curvature as genEdgeWear"));
    CHECK(c.dropped.contains("painted strokes (in a mask)"));
}

TEST_CASE("tex painter: the import command writes smart materials that apply and evaluate")
{
    run("newScene");
    const fs::path dir = fs::temp_directory_path() / "lila_tex_tests" / "painter_import";
    fs::remove_all(dir);
    json r = run("texImportPainterSmartMaterials", {{"args", {tinyFile().string()}}, {"folder", dir.string()}});
    REQUIRE(r.size() == 1);
    REQUIRE_MESSAGE(!r[0].contains("error"), r[0].dump());
    const std::string file = r[0]["materials"][0]["file"];
    CHECK(fs::exists(file));
    CHECK(fs::exists(dir / "painter_tiny.images"));

    run("texCreateSet", {{"args", {"Imported"}}, {"resolution", 4}});
    run("texApplySmartMaterial", {{"args", {"Imported", file}}});
    const json st = run("texLayerStack", {{"args", {"Imported"}}});
    CHECK(st["mode"] == "stack");
    const std::string edges = layerId(st["layers"], "Edges");
    REQUIRE(!edges.empty());
    run("texSetLayer", {{"args", {"Imported", edges, {{"enabled", false}}}}});
    scene::Node* n = findSet(scene::Graph::current(), "Imported");
    REQUIRE(n);
    PlanRequest req;
    req.width = req.height = 4;
    req.outputs = channelRequests(readGraph(*n), readSettings(*n));
    Plan p = buildPlan(readGraph(*n), req);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    auto out = evaluateCpu(p).outputs;
    CHECK(texel(out.at("baseColor"), 2, 2)[2] == doctest::Approx(0.4f).epsilon(0.02));
    CHECK(texel(out.at("roughness"), 2, 2)[0] == doctest::Approx(0.6f).epsilon(0.02));
}

TEST_CASE("tex painter: the added layer blend modes composite as their formulas say")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Modes"}}, {"resolution", 4}});
    run("texAddLayer", {{"args", {"Modes", "fill"}}, {"material", {{"baseColor", {0.5, 0.25, 0.8, 1}}}}});
    const std::string top = run("texAddLayer", {{"args", {"Modes", "fill"}}, {"material", {{"baseColor", {0.5, 0.5, 0.25, 1}}}}})["id"];
    scene::Node* n = findSet(scene::Graph::current(), "Modes");
    REQUIRE(n);
    auto colour = [&](const char* mode) {
        run("texSetLayer", {{"args", {"Modes", top, {{"channels", {{"baseColor", {{"mode", mode}}}}}}}}});
        PlanRequest req;
        req.width = req.height = 4;
        req.outputs = channelRequests(readGraph(*n), readSettings(*n));
        Plan p = buildPlan(readGraph(*n), req);
        REQUIRE_MESSAGE(p.error.empty(), p.error);
        const float* t = texel(evaluateCpu(p).outputs.at("baseColor"), 2, 2);
        return std::array<float, 3>{t[0], t[1], t[2]};
    };
    auto lb = colour("linearBurn");
    CHECK(lb[0] == doctest::Approx(0.0f).epsilon(0.01));
    CHECK(lb[2] == doctest::Approx(0.05f).epsilon(0.01));
    auto ex = colour("exclusion");
    CHECK(ex[0] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(ex[1] == doctest::Approx(0.5f).epsilon(0.01));
    auto dv = colour("divide");
    CHECK(dv[1] == doctest::Approx(0.5f).epsilon(0.01));
    auto ll = colour("linearLight");
    CHECK(ll[2] == doctest::Approx(0.3f).epsilon(0.01));
}

TEST_CASE("tex painter: Painter's stored preview reads back for file pickers")
{
    image::Image img = painter::readSpsmPreview(tinyFile());
    REQUIRE(img.width == 4);
    CHECK(img.rgba[0] == doctest::Approx(1.0f));
    CHECK(img.rgba[1] == doctest::Approx(128.0f / 255.0f).epsilon(0.01));
    CHECK(img.rgba[2] == doctest::Approx(0.0f));
}

TEST_CASE("tex painter: smart material tags filter the library and the user's retagging overrides the file's")
{
    const fs::path dir = PrefStore::get().userDir() / "smartmaterials";
    fs::remove(dir / "tags.json");
    run("texImportPainterSmartMaterials", {{"args", {tinyFile().string()}}});
    auto names = [](const json& list) {
        std::vector<std::string> out;
        for (const json& m : list)
            out.push_back(m["name"]);
        return out;
    };
    // Imports carry "imported"; filters need every tag given.
    CHECK(names(run("texSmartMaterials", {{"tags", {"imported"}}})) == std::vector<std::string>{"painter_tiny"});
    CHECK(run("texSmartMaterials", {{"tags", {"imported", "metal"}}}).empty());
    CHECK(run("texSetSmartMaterialTags", {{"args", {"painter_tiny"}}, {"add", {" Metal ", "stylized"}}}) ==
          json::array({"imported", "metal", "stylized"}));
    CHECK(names(run("texSmartMaterials", {{"tags", {"imported", "metal", "stylized"}}})) == std::vector<std::string>{"painter_tiny"});
    // Built-in ones retag too, without touching their files.
    const std::string builtin = run("texSmartMaterials", {{"tags", json::array()}})[0]["name"];
    run("texSetSmartMaterialTags", {{"args", {builtin}}, {"tags", {"mine"}}});
    CHECK(names(run("texSmartMaterials", {{"tags", {"mine"}}})) == std::vector<std::string>{builtin});
    run("texSetSmartMaterialTags", {{"args", {builtin}}, {"remove", {"mine"}}});
    CHECK(run("texSmartMaterials", {{"tags", {"mine"}}}).empty());
    fs::remove(dir / "tags.json");
}

TEST_CASE("tex painter: smart masks become Lila mask stacks and brush presets keep their stamp settings")
{
    const fs::path dir = fs::path(LILA_TEST_TEX_DATA);
    painter::Conversion m = painter::convertSpsm(painter::readSpsm(dir / "painter_tiny.spmsk"), "Tiny Mask", {});
    REQUIRE(m.materials.size() == 1);
    CHECK(m.materials[0].kind == "mask");
    const json& masks = m.materials[0].masks;
    REQUIRE(masks.size() == 2);
    CHECK(masks[0]["type"] == "genEdgeWear");
    CHECK(masks[1]["kind"] == "levels");
    CHECK(masks[1]["values"]["inLow"] == doctest::Approx(0.2));

    painter::Conversion b = painter::convertSpsm(painter::readSpsm(dir / "painter_tiny.sppr"), "Tiny Brush", {});
    REQUIRE(b.brushes.size() == 1);
    const json& brush = b.brushes[0].brush;
    CHECK(b.brushes[0].id == "Imported/Tiny_Brush");
    CHECK(brush["flow"] == doctest::Approx(0.5));
    // Painter spaces by diameter, Lila by radius.
    CHECK(brush["spacing"] == doctest::Approx(0.2));
    CHECK(brush["followStroke"] == true);
    CHECK(brush["jitter"]["angle"] == doctest::Approx(180.0).epsilon(0.01));
    CHECK(brush["jitter"]["position"] == doctest::Approx(0.5));
    // The tip is the library alpha sharing a word with Painter's, reported as approximate.
    if (brush.contains("alpha"))
        CHECK(brush["alpha"].get<std::string>().find("scratch") != std::string::npos);
    CHECK((b.approximated.size() + b.dropped.size()) == 1);
}
