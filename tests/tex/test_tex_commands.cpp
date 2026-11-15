#include "TexTest.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/TextureSet.h"

#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

/** A small set: bricks feed base colour (through colorize) and height, height feeds the normal; roughness is a constant. */
void buildRock()
{
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 64}});
    run("texAddNode", {{"args", {"Rock", "bricks"}}, {"name", "Bricks"}});
    run("texAddNode", {{"args", {"Rock", "colorize"}}, {"name", "Tint"}});
    run("texAddNode", {{"args", {"Rock", "heightToNormal"}}, {"name", "Nrm"}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Tint.input"}}});
    run("texConnect", {{"args", {"Rock", "Tint.out", "Output.baseColor"}}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Output.height"}}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Nrm.height"}}});
    run("texConnect", {{"args", {"Rock", "Nrm.normal", "Output.normal"}}});
    run("texSetValue", {{"args", {"Rock", "Output.roughness", 0.7}}});
    run("texSetValue", {{"args", {"Rock", "Output.metallic", 0.2}}});
    run("texSetValue", {{"args", {"Rock", "Output.ao", 0.9}}});
}
} // namespace

TEST_CASE("tex: commands build, edit and undo a texture set")
{
    buildRock();
    CHECK(run("texSets") == json::array({"Rock"}));
    json info = run("texGraphInfo", {{"args", {"Rock"}}});
    CHECK_FALSE(info.contains("error"));
    CHECK(info["nodes"].size() == 4);
    CHECK(info["plan"]["passes"].size() >= 1);
    const std::string hash = info["plan"]["structureHash"];

    run("texSetValue", {{"args", {"Rock", "Bricks.rows", 12}}});
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] == hash);
    run("undo");
    json node = run("texGraphInfo", {{"args", {"Rock"}}})["nodes"];
    for (const json& n : node)
        if (n["name"] == "Bricks")
            for (const json& p : n["params"])
                if (p["id"] == "rows")
                    CHECK(p["value"] == 8);

    CHECK_THROWS_AS(run("texConnect", {{"args", {"Rock", "Tint.out", "Bricks.nothing"}}}), CommandError);
    CHECK_THROWS_AS(run("texConnect", {{"args", {"Rock", "Nrm.normal", "Nrm.height"}}}), CommandError);
    CHECK_THROWS_AS(run("texSetValue", {{"args", {"Rock", "Bricks.rows", "many"}}}), CommandError);
    CHECK_THROWS_AS(run("texSetValue", {{"args", {"Rock", "Tint.gradient", 3}}}), CommandError);
    CHECK_THROWS_AS(run("texAddNode", {{"args", {"Rock", "nope"}}}), CommandError);

    CHECK(run("texDisconnect", {{"args", {"Rock", "Output.height"}}}) == 1);
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] != hash);
    run("undo");
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["plan"]["structureHash"] == hash);

    json dup = run("texDuplicateNodes", {{"args", {"Rock", "Bricks", "Tint"}}});
    CHECK(dup.size() == 2);
    std::string text = run("texCopyNodes", {{"args", {"Rock", "Bricks", "Tint"}}});
    json pasted = run("texPasteNodes", {{"args", {"Rock", text}}});
    CHECK(pasted.size() == 2);
    run("texDeleteNode", {{"args", {"Rock", dup[0], dup[1], pasted[0], pasted[1]}}});
    CHECK(run("texGraphInfo", {{"args", {"Rock"}}})["nodes"].size() == 4);

    json types = run("texNodeTypes", {{"linkType", "material"}});
    CHECK(std::any_of(types.begin(), types.end(), [](const json& t) { return t["type"] == "materialOutput"; }));
    CHECK(std::none_of(types.begin(), types.end(), [](const json& t) { return t["type"] == "levels"; }));
}

TEST_CASE("tex: texBuildGraph builds a set from one spec, or reports every problem and changes nothing")
{
    run("newScene");
    run("polyCube", {{"name", "Box"}});
    json spec = {
        {"material", "Brick"},
        {"settings", {{"width", 64}, {"height", 64}}},
        {"nodes",
         {{{"name", "Bricks"}, {"type", "bricks"}, {"params", {{"rows", 12}}}},
          {{"name", "Tint"}, {"type", "colorize"}},
          {{"name", "Nrm"}, {"type", "heightToNormal"}},
          {{"name", "Output"}, {"type", "materialOutput"}, {"values", {{"roughness", 0.8}}}}}},
        {"links", {"Bricks.mask -> Tint.input", "Tint.out -> Output.baseColor", json::array({"Bricks.mask", "Nrm.height"}),
                   json::array({"Nrm", "normal", "Output", "normal"})}},
    };
    json r = run("texBuildGraph", {{"args", {spec}}, {"assign", {"Box"}}});
    CHECK(r["set"] == "BrickTex");
    CHECK(r["material"] == "Brick");
    CHECK_FALSE(r.contains("error"));
    CHECK(r["channels"] == json::array({"baseColor", "normal"}));
    json info = run("texGraphInfo", {{"args", {"BrickTex"}}});
    CHECK(info["settings"]["width"] == 64);
    for (const json& n : info["nodes"])
        if (n["name"] == "Bricks")
            for (const json& p : n["params"])
                if (p["id"] == "rows")
                    CHECK(p["value"] == 12);
    CHECK(run("materialInfo", {{"args", {"Brick"}}}).is_object());

    // Rebuilding replaces the graph; the Material Output is added when the spec leaves it out.
    run("texBuildGraph", {{"args", {{{"material", "Brick"}, {"nodes", {{{"name", "N"}, {"type", "bricks"}}}}, {"links", {"N.mask -> Output.height"}}}}}});
    CHECK(run("texGraphInfo", {{"args", {"BrickTex"}}})["nodes"].size() == 2);

    // A set is not a material, so it drops the material's M_ prefix; a rebuild finds the bound set whatever its name.
    json bricks = {{"nodes", {{{"name", "N"}, {"type", "bricks"}}}}, {"links", {"N.mask -> Output.height"}}};
    json prefixed = bricks;
    prefixed["material"] = "M_Wall";
    CHECK(run("texBuildGraph", {{"args", {prefixed}}})["set"] == "WallTex");
    run("texCreateSet", {{"args", {"OddName"}}, {"settings", {{"material", "M_Floor"}}}});
    prefixed["material"] = "M_Floor";
    CHECK(run("texBuildGraph", {{"args", {prefixed}}})["set"] == "OddName");

    json bad = {{"material", "Brick"},
                {"nodes", {{{"name", "A"}, {"type", "brick"}}, {{"name", "B"}, {"type", "colorize"}, {"values", {{"nothing", 1}}}}}},
                {"links", {"B.out -> Output.nope", "B.out"}}};
    try
    {
        run("texBuildGraph", {{"args", {bad}}});
        FAIL("expected an error");
    }
    catch (const CommandError& e)
    {
        const std::string msg = e.what();
        CHECK(msg.find("4 problems") != std::string::npos);
        CHECK(msg.find("did you mean") != std::string::npos);
        CHECK(msg.find("no parameter or input 'nothing'") != std::string::npos);
        CHECK(msg.find("no input 'nope'") != std::string::npos);
    }
    CHECK(run("texGraphInfo", {{"args", {"BrickTex"}}})["nodes"].size() == 2);
    CHECK_THROWS_AS(run("texBuildGraph", {{"args", {{{"set", "Same"}, {"material", "Same"}}}}}), CommandError);
    CHECK_THROWS_AS(run("texBuildGraph", {{"args", {{{"set", "Lone"}}}}, {"assign", {"Box"}}}), CommandError);
}

TEST_CASE("tex: texture sets save and load in .lila and .lilaa")
{
    buildRock();
    run("texSetSettings", {{"args", {"Rock", {{"tiles", json::array({1001, json{{"udim", 1002}, {"width", 32}}})}, {"bakeProject", "RockBake"}}}}});
    json before = run("texGraphInfo", {{"args", {"Rock"}}});
    for (const char* ext : {".lila", ".lilaa"})
    {
        CAPTURE(ext);
        fs::path path = tempDir("files") / (std::string("rock") + ext);
        run("saveScene", {{"args", {path.string()}}});
        run("newScene");
        CHECK(run("texSets").empty());
        run("openScene", {{"args", {path.string()}}});
        json after = run("texGraphInfo", {{"args", {"Rock"}}});
        CHECK(after["definition"] == before["definition"]);
        CHECK(after["settings"] == before["settings"]);
        CHECK(after["plan"]["structureHash"] == before["plan"]["structureHash"]);
    }
}

TEST_CASE("tex: texEvaluate writes every channel of every tile")
{
    buildRock();
    run("texSetSettings", {{"args", {"Rock", {{"tiles", json::array({1001, 1002})}}}}});
    fs::path dir = tempDir("evaluate");
    json r = run("texEvaluate", {{"args", {"Rock"}}, {"folder", dir.string()}, {"channels", {"baseColor", "roughness"}}});
    CHECK(r["files"].size() == 4);
    CHECK(fs::exists(dir / "Rock_baseColor.1001.png"));
    CHECK(fs::exists(dir / "Rock_roughness.1002.png"));
    image::Image rough = image::read(dir / "Rock_roughness.1001.png");
    REQUIRE(rough.width == 64);
    CHECK(rough.rgba[0] == doctest::Approx(0.7).epsilon(0.01));
}

TEST_CASE("tex: 8-bit colour channels are written sRGB-encoded, float files and data channels linear")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Srgb"}}, {"resolution", 16}});
    run("texSetValue", {{"args", {"Srgb", "Output.baseColor", json::array({0.214, 0.214, 0.214, 1.0})}}});
    run("texSetValue", {{"args", {"Srgb", "Output.roughness", 0.214}}});
    fs::path dir = tempDir("srgb");
    run("texEvaluate", {{"args", {"Srgb"}}, {"folder", dir.string()}, {"channels", {"baseColor", "roughness"}}});
    CHECK(image::read(dir / "Srgb_baseColor.png").rgba[0] == doctest::Approx(0.5).epsilon(0.01));
    CHECK(image::read(dir / "Srgb_roughness.png").rgba[0] == doctest::Approx(0.214).epsilon(0.01));
    run("texEvaluate", {{"args", {"Srgb"}}, {"folder", dir.string()}, {"channels", {"baseColor"}}, {"format", "exr"}});
    CHECK(image::read(dir / "Srgb_baseColor.exr").rgba[0] == doctest::Approx(0.214).epsilon(0.01));

    json r = run("texExport", {{"args", {"Srgb"}}, {"preset", "unreal"}, {"folder", dir.string()}});
    CAPTURE(r.dump());
    CHECK(image::read(dir / "T_Srgb_BC.png").rgba[0] == doctest::Approx(0.5).epsilon(0.01));
}

TEST_CASE("tex: texExport packs channels through the Unreal preset")
{
    buildRock();
    fs::path dir = tempDir("export");
    json r = run("texExport", {{"args", {"Rock"}}, {"preset", "unreal"}, {"folder", dir.string()}});
    CAPTURE(r.dump());
    CHECK(fs::exists(dir / "T_Rock_ORM.png"));
    CHECK(fs::exists(dir / "T_Rock_N.png"));
    CHECK(fs::exists(dir / "T_Rock_BC.png"));
    CHECK(fs::exists(dir / "T_Rock_H.png"));
    // Only the normal map carries a green convention: a reader flips the green of any map marked opengl.
    for (const json& o : r["outputs"])
        CHECK(o["normalFormat"] == (o["map"] == "normal" ? "directx" : ""));
    image::Image orm = image::read(dir / "T_Rock_ORM.png");
    REQUIRE(orm.width == 64);
    CHECK(orm.rgba[0] == doctest::Approx(0.9).epsilon(0.01));
    CHECK(orm.rgba[1] == doctest::Approx(0.7).epsilon(0.01));
    CHECK(orm.rgba[2] == doctest::Approx(0.2).epsilon(0.01));

    // Unreal wants DirectX normals: the graph's OpenGL green is flipped on the way out.
    fs::path raw = tempDir("rawNormal");
    run("texEvaluate", {{"args", {"Rock"}}, {"folder", raw.string()}, {"channels", {"normal"}}, {"bits", 16}});
    image::Image gl = image::read(raw / "Rock_normal.png");
    image::Image dx = image::read(dir / "T_Rock_N.png");
    REQUIRE(gl.width == dx.width);
    double worst = 0.0;
    for (size_t i = 0; i < gl.rgba.size(); i += 4)
        worst = std::max(worst, std::abs(double(gl.rgba[i + 1]) - (1.0 - double(dx.rgba[i + 1]))));
    CHECK(worst < 0.01);
}

TEST_CASE("tex: normalFromHeight exports the normal as detail over a 16-bit height")
{
    buildRock();
    fs::path dir = tempDir("exportHeightNormal");
    run("texSetSettings", {{"args", {"Rock", {{"normalFade", 0.0}}}}});
    json r = run("texExport", {{"args", {"Rock"}}, {"preset", "unreal"}, {"folder", dir.string()}, {"optional", false}, {"normalFromHeight", true}});
    CAPTURE(r.dump());
    // The height goes out even with optional maps off, at 16 bits, and both carry what rebuilds the normal.
    bool sawHeight = false;
    for (const json& o : r["outputs"])
    {
        if (o["map"] == "height")
        {
            sawHeight = true;
            CHECK(o["bits"] == 16);
            CHECK(o["meta"]["heightRange"].get<double>() == doctest::Approx(2.5));
        }
        if (o["map"] == "normal")
            CHECK(o["meta"]["normalFromHeight"] == true);
    }
    CHECK(sawHeight);
    // The Rock's normal is all its height's, so the detail left is flat.
    image::Image n = image::read(dir / "T_Rock_N.png");
    REQUIRE(n.width == 64);
    double worst = 0.0;
    for (size_t i = 0; i < n.rgba.size(); i += 4)
        worst = std::max({worst, std::abs(n.rgba[i] - 0.5), std::abs(n.rgba[i + 1] - 0.5)});
    CHECK(worst < 0.02);
}

TEST_CASE("tex: the stack's height normal follows the set's depth and UV size")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Wall"}}, {"resolution", 32}});
    run("texAddLayer", {{"args", {"Wall", "fill"}}, {"material", {{"height", 0.7}}}});
    run("texSetSettings", {{"args", {"Wall", {{"heightDepth", 2.0}, {"uvWidth", 400.0}, {"uvHeight", 200.0}}}}});
    bool found = false;
    const json info = run("texGraphInfo", {{"args", {"Wall"}}});
    CAPTURE(info["definition"].dump());
    for (const json& node : info["nodes"])
        if (node["type"] == "heightToNormal")
            for (const json& p : node["params"])
            {
                found = true;
                if (p["id"] == "depth")
                    CHECK(p["value"] == 2.0);
                if (p["id"] == "sizeU")
                    CHECK(p["value"] == 400.0);
                if (p["id"] == "sizeV")
                    CHECK(p["value"] == 200.0);
            }
    CHECK(found);
}

TEST_CASE("tex: normalFromHeight shows the normal rebuilt from the height channel, or the baked height")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Wall"}}, {"resolution", 32}});
    run("texAddLayer", {{"args", {"Wall", "fill"}}, {"material", {{"height", 0.7}}}});
    auto types = [] {
        std::vector<std::string> out;
        const json info = run("texGraphInfo", {{"args", {"Wall"}}});
        for (const json& node : info["nodes"])
            out.push_back(node["type"]);
        return out;
    };
    auto has = [](const std::vector<std::string>& v, const char* t) { return std::ranges::find(v, t) != v.end(); };
    CHECK_FALSE(has(types(), "heightNormalRebuild"));
    run("texSetSettings", {{"args", {"Wall", {{"normalFromHeight", true}}}}});
    CHECK(has(types(), "heightNormalRebuild"));
    CHECK_FALSE(has(types(), "meshHeight"));
    run("texSetSettings", {{"args", {"Wall", {{"heightSource", "mesh"}}}}});
    CHECK(has(types(), "meshHeight"));
    CHECK_THROWS_AS(run("texSetSettings", {{"args", {"Wall", {{"heightSource", "nope"}}}}}), CommandError);
}
