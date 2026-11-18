#include "TexTest.h"

#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/Library.h"
#include "tex/Lint.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <fstream>

using namespace textest;
namespace lib = lila::tex::library;

namespace
{
namespace fs = std::filesystem;

void registerLibrary()
{
    if (!ops::find("materialLibrary"))
        lib::registerCommands();
}

/** A material graph with constant channel values on its Material Output. */
json constantMaterial(const std::string& name, json meta, json values)
{
    Graph g = Graph::makeMaterial(name);
    g.nodes.front().values = std::move(values);
    return {{"format", "lilamat"}, {"version", 1}, {"name", name}, {"meta", std::move(meta)}, {"graph", g.toJson()}};
}

void writeMaterial(const fs::path& file, const json& doc)
{
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << doc.dump(2);
}

const json kGood = {{"baseColor", {0.2, 0.18, 0.15, 1.0}}, {"roughness", 0.6}, {"metallic", 0.0}};
} // namespace

TEST_CASE("material library: later tiers win on the same id, and scene sets are listed")
{
    registerLibrary();
    run("newScene");
    fs::path user = lib::tierDir("user");
    REQUIRE_FALSE(user.empty());
    fs::remove_all(user);
    fs::path ws = tempDir("library_ws");
    Workspace::get().open(ws);
    lib::reload();

    ext::Context pack("TestPack");
    lib::Entry e;
    e.id = "Test/shared";
    e.meta.name = "From Pack";
    e.document = constantMaterial("Shared", {{"name", "From Pack"}}, kGood);
    lib::registerExtensionMaterial(pack, e);
    REQUIRE(lib::find("Test/shared"));
    CHECK(lib::find("Test/shared")->tier == "extension");
    CHECK(lib::find("Test/shared")->owner == "TestPack");

    writeMaterial(user / "Test" / "shared.lilamat", constantMaterial("Shared", {{"name", "From User"}, {"category", "Test"}}, kGood));
    lib::reload();
    CHECK(lib::find("Test/shared")->tier == "user");
    CHECK(lib::find("Test/shared")->meta.name == "From User");

    fs::path wsDir = lib::tierDir("workspace");
    REQUIRE_FALSE(wsDir.empty());
    writeMaterial(wsDir / "Test" / "shared.lilamat", constantMaterial("Shared", {{"name", "From Workspace"}}, kGood));
    lib::reload();
    CHECK(lib::find("Test/shared")->tier == "workspace");

    run("texCreateSet", {{"args", {"Crate"}}});
    std::optional<lib::Entry> scene = lib::find("scene/Crate");
    REQUIRE(scene);
    CHECK(scene->tier == "scene");
    CHECK(lib::document(*scene)["graph"]["nodes"].size() == 1);

    fs::remove_all(wsDir / "Test");
    fs::remove_all(user / "Test");
    lib::reload();
    CHECK(lib::find("Test/shared")->tier == "extension");
    // Unloading the pack takes its material away.
    pack.unloadAll();
    CHECK_FALSE(lib::find("Test/shared"));
    Workspace::get().close();
    lib::reload();
}

TEST_CASE("material library: search by words, category, tag, physical and favourites")
{
    registerLibrary();
    run("newScene");
    fs::path user = lib::tierDir("user");
    fs::remove_all(user);
    writeMaterial(user / "Metal" / "steel.lilamat",
                  constantMaterial("Steel", {{"name", "Brushed Steel"}, {"category", "Metal/Steel"}, {"tags", {"brushed", "industrial"}}, {"physical", "metal"}},
                                   kGood));
    writeMaterial(user / "Wood" / "oak.lilamat",
                  constantMaterial("Oak", {{"name", "Oak Planks"}, {"category", "Wood"}, {"tags", {"planks"}}, {"description", "Weathered industrial floor"}},
                                   kGood));
    lib::reload();

    auto ids = [](const json& list) {
        std::vector<std::string> out;
        for (const json& e : list)
            if (e["tier"] == "user")
                out.push_back(e["id"]);
        return out;
    };
    CHECK(ids(run("materialLibrary", {{"search", "steel"}})) == std::vector<std::string>{"Metal/steel"});
    CHECK(ids(run("materialLibrary", {{"search", "industrial"}})).size() == 2);
    CHECK(ids(run("materialLibrary", {{"search", "industrial oak"}})) == std::vector<std::string>{"Wood/oak"});
    CHECK(ids(run("materialLibrary", {{"category", "Metal"}})) == std::vector<std::string>{"Metal/steel"});
    CHECK(ids(run("materialLibrary", {{"category", "Met"}})).empty());
    CHECK(ids(run("materialLibrary", {{"tag", "planks"}})) == std::vector<std::string>{"Wood/oak"});
    CHECK(ids(run("materialLibrary", {{"physical", "metal"}})) == std::vector<std::string>{"Metal/steel"});

    run("materialFavourite", {{"args", {"Wood/oak"}}});
    CHECK(ids(run("materialLibrary", {{"favourites", true}})) == std::vector<std::string>{"Wood/oak"});
    run("materialFavourite", {{"args", {"Wood/oak"}}, {"on", false}});
    CHECK(ids(run("materialLibrary", {{"favourites", true}})).empty());

    json cats = run("materialLibrary", {{"categories", true}});
    auto has = [](const json& a, const std::string& v) { return std::find(a.begin(), a.end(), json(v)) != a.end(); };
    CHECK(has(cats["categories"], "Metal"));
    CHECK(has(cats["categories"], "Metal/Steel"));
    CHECK(has(cats["tags"], "brushed"));
}

TEST_CASE("material lint flags a bad material, fixes what it can, and passes a good one")
{
    registerLibrary();
    run("newScene");
    library::Meta meta;
    meta.physical = "dielectric";

    Graph good = Graph::makeMaterial("Good");
    good.nodes.front().values = kGood;
    lint::Report rg = lint::run(good, SetSettings::makeDefault(), meta, {.size = 32, .backend = "cpu"});
    INFO(rg.toJson().dump(2));
    CHECK(rg.passed());
    CHECK(rg.count("warn") == 0);

    // Pure black, half-metallic, a mirror, and a normal lying in the surface.
    Graph bad = Graph::makeMaterial("Bad");
    bad.nodes.front().values = {{"baseColor", {0.0, 0.0, 0.0, 1.0}}, {"roughness", 0.0}, {"metallic", 0.5}, {"normal", {1.0, 0.5, 0.5, 1.0}}};
    lint::Report rb = lint::run(bad, SetSettings::makeDefault(), meta, {.size = 32, .backend = "cpu"});
    INFO(rb.toJson().dump(2));
    CHECK_FALSE(rb.passed());
    CHECK(rb.check("albedo")->status == "fail");
    CHECK(rb.check("blackWhite")->status == "fail");
    CHECK(rb.check("metallic")->status == "fail");
    CHECK(rb.check("roughness")->status == "warn");
    CHECK(rb.check("normal")->status == "pass");

    // Through the command on a set, with a linked base colour so the fix inserts a Levels node.
    run("texCreateSet", {{"args", {"Bad"}}, {"resolution", 32}});
    run("texAddNode", {{"args", {"Bad", "color"}}, {"name", "Black"}});
    run("texConnect", {{"args", {"Bad", "Black.out", "Output.baseColor"}}});
    run("texSetValue", {{"args", {"Bad", "Black.color", json::array({0.0, 0.0, 0.0, 1.0})}}});
    run("texSetValue", {{"args", {"Bad", "Output.metallic", 0.5}}});
    json r = run("materialLint", {{"args", {"Bad"}}, {"size", 32}, {"backend", "cpu"}, {"fix", true}});
    INFO(r.dump(2));
    CHECK(r["passed"] == false);
    REQUIRE(r.contains("after"));
    CHECK(r["after"]["passed"] == true);
    Graph fixed = readGraph(*findSet(scene::Graph::current(), "Bad"));
    REQUIRE(fixed.node("LintFix_baseColor"));
    CHECK(fixed.node("LintFix_baseColor")->type == "levels");
    CHECK(fixed.node("Output")->values["metallic"] == 0.0);
    run("undo");
    CHECK_FALSE(readGraph(*findSet(scene::Graph::current(), "Bad")).node("LintFix_baseColor"));

    // A stylised material may be dark.
    library::Meta stylised;
    stylised.physical = "stylised";
    Graph dark = Graph::makeMaterial("Dark");
    dark.nodes.front().values = {{"baseColor", {0.005, 0.005, 0.005, 1.0}}, {"roughness", 0.6}};
    CHECK(lint::run(dark, SetSettings::makeDefault(), stylised, {.size = 32, .backend = "cpu"}).check("albedo")->status == "pass");
    CHECK(lint::run(dark, SetSettings::makeDefault(), meta, {.size = 32, .backend = "cpu"}).check("albedo")->status == "fail");
}

TEST_CASE("material lint: per-metal reflectance and tile seams")
{
    library::Meta gold;
    gold.name = "Gold";
    gold.physical = "metal";
    Graph g = Graph::makeMaterial("Gold");
    g.nodes.front().values = {{"baseColor", {1.0, 0.766, 0.336, 1.0}}, {"metallic", 1.0}, {"roughness", 0.3}};
    lint::Report ok = lint::run(g, SetSettings::makeDefault(), gold, {.size = 32, .backend = "cpu"});
    CHECK(ok.check("metal")->status == "pass");
    CHECK(ok.check("metal")->data["metal"] == "gold");
    g.nodes.front().values["baseColor"] = {0.55, 0.556, 0.554, 1.0};
    CHECK(lint::run(g, SetSettings::makeDefault(), gold, {.size = 32, .backend = "cpu"}).check("metal")->status == "warn");

    // A uv gradient is not periodic: its edges jump from 1 back to 0.
    library::Meta tiles;
    Graph s = Graph::makeMaterial("Seam");
    add(s, "Ramp", "gradient");
    link(s, "Ramp.out", "Output.roughness");
    lint::Report seam = lint::run(s, SetSettings::makeDefault(), tiles, {.size = 64, .backend = "cpu"});
    INFO(seam.toJson().dump(2));
    CHECK(seam.check("seams")->status == "fail");
    tiles.tileable = false;
    CHECK(lint::run(s, SetSettings::makeDefault(), tiles, {.size = 64, .backend = "cpu"}).check("seams")->status == "skip");
}

TEST_CASE("applyLibraryMaterial binds a set to the mesh's slot and tiles it to the material's real-world size")
{
    registerLibrary();
    run("newScene");
    fs::path user = lib::tierDir("user");
    fs::remove_all(user);
    json doc = constantMaterial("Tiles", {{"name", "Floor Tiles"}, {"category", "Stone"}, {"scaleCm", 50}, {"seedParam", "Output.roughness"}}, kGood);
    doc["presets"] = {{"Rough", {{"Output.roughness", 0.9}}}};
    writeMaterial(user / "Stone" / "floorTiles.lilamat", doc);
    lib::reload();

    // A 200 cm plane with 0..1 uvs: 1/200 uv per cm, so a 50 cm material repeats 4 times.
    std::string plane = run("polyPlane", {{"width", 200.0}, {"depth", 200.0}})[0];
    json r = run("applyLibraryMaterial", {{"args", {"Stone/floorTiles", plane}}, {"preset", "Rough"}});
    INFO(r.dump(2));
    CHECK(r["tiles"].get<double>() == doctest::Approx(4.0).epsilon(1e-3));
    CHECK(r["uvPerCm"].get<double>() == doctest::Approx(1.0 / 200.0).epsilon(1e-3));
    std::string setName = r["set"];
    std::string material = r["material"];
    scene::Node* set = findSet(scene::Graph::current(), setName);
    REQUIRE(set);
    CHECK(materialsUsing(scene::Graph::current(), setName) == std::vector<std::string>{material});
    Graph g = readGraph(*set);
    REQUIRE(g.node(lib::kTilingNode));
    CHECK(g.node(lib::kTilingNode)->values["tilesX"].get<double>() == doctest::Approx(4.0).epsilon(1e-3));
    REQUIRE(g.node("LibraryChannels"));
    CHECK(g.node("LibraryChannels")->values["roughness"] == 0.9);
    json slots = run("listMaterials", {{"args", {plane}}});
    INFO(slots.dump());
    CHECK(slots[0]["slots"][0]["slot"] == material);

    // The tiled graph compiles and evaluates, and the 4x4 repeat keeps the constant values.
    PlanRequest req;
    req.width = req.height = 16;
    req.outputs = channelRequests(g, readSettings(*set));
    Plan p = buildPlan(g, req);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalResult ev = evaluateCpu(p);
    CHECK(texel(ev.outputs.at("roughness"), 3, 5)[0] == doctest::Approx(0.9f).epsilon(1e-3));

    // Applying again to the same object reuses the slot and its set; scale=False drops back to one tile.
    json again = run("applyLibraryMaterial", {{"args", {"Stone/floorTiles", plane}}, {"scale", false}});
    CHECK(again["set"] == setName);
    CHECK(again["material"] == material);

    // Saving leaves the mesh-specific tiling out.
    run("applyLibraryMaterial", {{"args", {"Stone/floorTiles", plane}}});
    json saved = run("saveLibraryMaterial", {{"args", {setName}}, {"id", "Stone/mine"}, {"scaleCm", 75}});
    std::optional<lib::Entry> mine = lib::find("Stone/mine");
    REQUIRE(mine);
    CHECK(mine->meta.scaleCm == 75.0);
    CHECK_FALSE(lib::graphOf(lib::document(*mine)).node(lib::kTilingNode));

    json presets = run("materialPresets", {{"args", {"Stone/floorTiles"}}, {"save", "Mine"}, {"set", setName}});
    CHECK(presets.contains("Rough"));
    REQUIRE(presets.contains("Mine"));
    CHECK(presets["Mine"].contains("Output.roughness"));
    run("materialPresets", {{"args", {"Stone/floorTiles"}}, {"delete", "Mine"}});
    CHECK_FALSE(run("materialPresets", {{"args", {"Stone/floorTiles"}}}).contains("Mine"));

    CHECK(lib::tilesFor(100.0, 0.0) == 1.0);
}

TEST_CASE("material presets set exposed parameters by name, through their targets")
{
    Graph g = Graph::makeMaterial("Exposed");
    GraphInput wear;
    wear.name = "wear";
    wear.param = true;
    wear.paramType = ParamType::Float;
    wear.defaultValue = 0.3;
    wear.targets = {"Output.roughness"};
    g.inputs.push_back(wear);
    std::vector<std::string> missed = lib::applyValues(g, {{"wear", 0.7}, {"Output.metallic", 1.0}, {"nothing", 1}});
    CHECK(missed == std::vector<std::string>{"nothing"});
    CHECK(g.input("wear")->defaultValue == 0.7);
    CHECK(g.node("Output")->values["roughness"] == 0.7);
    CHECK(g.node("Output")->values["metallic"] == 1.0);
    library::Meta m;
    m.seedParam = "wear";
    m.variationParams = {"Output.metallic"};
    json captured = lib::captureValues(g, m);
    CHECK(captured["wear"] == 0.7);
    CHECK(captured["Output.metallic"] == 1.0);
}
