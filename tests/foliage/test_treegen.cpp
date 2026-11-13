#include "TestUtil.h"
#include "TreeBuild.h"
#include "TreeSchema.h"

#include "extensions/ExtensionManager.h"
#include "geo/ColorSets.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/Menus.h"
#include "ui/Panels.h"

#include <doctest/doctest.h>

#include <QImage>

#include <filesystem>
#include <fstream>
#include <set>

using namespace lila;
using namespace lila::scene;
using namespace foliagetest;
namespace fs = std::filesystem;
using json = nlohmann::json;

namespace
{
fs::path scratch(const char* name)
{
    fs::path p = fs::temp_directory_path() / "lila_foliage_tests" / name;
    fs::remove_all(p);
    fs::create_directories(p);
    return p;
}

MeshPtr mesh(const std::string& name)
{
    Graph& g = Graph::current();
    g.evaluate();
    Node* n = g.find(name);
    REQUIRE_MESSAGE(n, name);
    REQUIRE(n->type().isA("mesh"));
    return n->as<MeshPtr>(MeshNode::kOutMesh);
}

std::vector<glm::dvec3> positions(const geo::Mesh& m) { return {m.positions().begin(), m.positions().end()}; }

/** A small tree's settings: few leaves so the command tests stay quick. */
json quick(int64_t seed = 3)
{
    return {{"leaf.count", 60}, {"trunk.seed", seed}, {"leaf.seed", seed}};
}

json readJson(const fs::path& p)
{
    std::ifstream in(p);
    return json::parse(in);
}
} // namespace

TEST_CASE("tree schema: defaults, coercion and presets")
{
    using namespace treegen;
    const json d = defaults();
    CHECK(d["trunk.height"] == 6.0);
    CHECK(d["leaf.color_set"] == "leafShade");
    CHECK(d["leaf.scale"] == json::array({0.8, 1.3}));
    CHECK(coerce("blob.blob_clip", 3.0) == 0.95);
    CHECK(coerce("blob.blob_offset", 50.0) == 50.0);
    CHECK(coerce("trunk.sides", 7.6) == 8);
    CHECK(coerce("leaf.scale", 2.0) == json::array({2.0, 2.0}));
    CHECK(coerce("global.up_axis", "W") == "auto");
    CHECK(coerce("leaf.tilt", json::array({1.0, 2.0, 3.0})) == d["leaf.tilt"]);
    CHECK(sanitize({{"bogus", 1}, {"trunk.levels", "3"}})["trunk.levels"] == 3);
    CHECK_FALSE(sanitize({{"bogus", 1}}).contains("bogus"));

    REQUIRE(builtInPresets().size() == 10);
    CHECK(builtInPresets().front() == "Default");
    for (const std::string& name : builtInPresets())
    {
        const json v = presetSettings(name);
        CHECK(v.size() == d.size());
        for (const auto& [k, _] : presetOverrides(name).items())
            CHECK_MESSAGE(field(k), k);
    }
    CHECK(presetSettings("Sugi Young")["trunk.height"] == 7.0);
    CHECK(presetSettings("Akamatsu Ancient")["blob.blob_clip"] == 0.3);
    CHECK(presetSettings("Kaede Mature")["leaf.align"] == json::array({0.0, 0.4}));
    CHECK(presetSettings("Mine", {{"Mine", {{"trunk.height", 2.0}}}})["trunk.height"] == 2.0);
    CHECK(presetNames({{"Mine", json::object()}, {"Default", json::object()}}).back() == "Mine");
    CHECK(speciesOf("Sugi Mature") == "sugi");
    CHECK(paletteFor("nope")["bark"]["base"] == "#594A39");
    CHECK(paletteFor("akamatsu")["leaves"]["lateAutumn"]["roughness"] == 0.46);

    const json styled = stylize(defaults(), 1.0);
    CHECK(styled["trunk.radius"].get<double>() == doctest::Approx(0.22 * 2.6));
    CHECK(styled["leaf.count"].get<int64_t>() < 400);
    CHECK(stylize(defaults(), 0.0) == defaults());
}

TEST_CASE("tree geometry: skeleton counts and determinism")
{
    using namespace treegen;
    const json trunk = sectionValues(defaults(), "trunk");
    Rng rng(0);
    const Skeleton sk = skeleton(trunkSpec(trunk, {0, 1, 0}), rng);
    // Two levels of three branches: the trunk, three limbs and nine twigs.
    CHECK(sk.limbs.size() == 13);
    CHECK(sk.tips.size() == 9);

    const Trunk a = buildTrunk(trunk, {0, 1, 0});
    const Trunk b = buildTrunk(trunk, {0, 1, 0});
    json other = trunk;
    other["seed"] = 7;
    const Trunk c = buildTrunk(other, {0, 1, 0});
    CHECK(positions(a.mesh) == positions(b.mesh));
    CHECK(positions(a.mesh) != positions(c.mesh));
    // A tube per limb of 8 sides: rings of quads, a fan tip cap and an 8-gon base.
    CHECK(a.mesh.numFaces() > 13 * (8 + 1));
    CHECK(a.mesh.cornerAttrs().has(geo::attr::uvName(0)));
    CHECK(a.density > 0.0);

    const auto placements = placeBlobs(a.tips, 50.0, 0.25, 0.0, 0.35, 0.0, 0.0, {0, 1, 0}, 0);
    CHECK(placements.size() == a.tips.size());
    const auto blobs = buildBlobs(placements, "icosphere", 1, 0.0);
    CHECK(blobs.front().numFaces() == 80);
    CHECK(buildBlobs(placements, "quadsphere", 1, 0.0).front().numFaces() == 6 * 16);
    CHECK(buildBlobs(placements, "uvsphere", 1, 0.5).front().numFaces() > 0);
    CHECK_THROWS(buildBlobs(placements, "cube", 1, 0.0));

    const Arrays plane = leafPlaneArrays(0.0, 0.0, 6, 1, 0.6, true);
    const geo::Mesh leafMesh = toMesh(plane);
    CHECK(leafMesh.numFaces() == 12);
    std::vector<CanopyInput> inputs;
    for (const geo::Mesh& m : blobs)
        inputs.push_back({.mesh = &m});
    const Canopy canopy(inputs);
    const Leaf leaf = makeLeaf(leafMesh, "auto", "auto", "base");
    ScatterSettings s;
    s.count = 120;
    const auto plansA = planLeaves(canopy, leaf, s, {0, 1, 0});
    const auto plansB = planLeaves(canopy, leaf, s, {0, 1, 0});
    REQUIRE(plansA.size() == 120);
    const auto leavesA = emitLeaves(leaf, plansA, true, 1.0, "leafShade");
    const auto leavesB = emitLeaves(leaf, plansB, true, 1.0, "leafShade");
    CHECK(positions(leavesA.front()) == positions(leavesB.front()));
    CHECK(leavesA.front().numFaces() == 120 * 12);
    CHECK(geo::colors::has(leavesA.front(), "leafShade"));
    CHECK(emitLeaves(leaf, plansA, false, 0.0, "").size() == 120);

    s.clumps = 4;
    s.minDistance = 2.0;
    const auto clumped = planLeaves(canopy, leaf, s, {0, 1, 0});
    CHECK(!clumped.empty());
    CHECK(clumped.size() <= 120);

    const std::vector<uint8_t> mask = opacityMask(leafMesh, 64, 0.0);
    // The card's UVs fill the whole square, so the mask does too.
    CHECK(std::count(mask.begin(), mask.end(), uint8_t(255)) == 64 * 64);
    Arrays tri;
    tri.points = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}};
    tri.counts = {3};
    tri.connects = {0, 1, 2};
    tri.us = {0.0, 1.0, 0.0};
    tri.vs = {0.0, 0.0, 1.0};
    tri.uvIds = {0, 1, 2};
    const std::vector<uint8_t> half = opacityMask(toMesh(tri), 64, 1.0);
    const auto covered = std::count(half.begin(), half.end(), uint8_t(255));
    CHECK(covered > 64 * 64 * 4 / 10);
    CHECK(covered < 64 * 64 * 7 / 10);

    const json sp = spread({-2.0, 1.0, 3.0});
    CHECK(sp["count"] == 3);
    CHECK(sp["out"] == 3.0);
    CHECK(sp["in"] == 2.0);
}

TEST_CASE("tree geometry: no preset's trunk has a zero-area face")
{
    using namespace treegen;
    for (const std::string& preset : builtInPresets())
    {
        const json values = presetSettings(preset);
        const json build = stylize(values, values["global.style"].get<double>());
        const Trunk t = buildTrunk(sectionValues(build, "trunk"), {0, 1, 0});
        // The same test meshCheck reports zero-area faces by.
        const double scale = glm::length(t.mesh.bounds().size());
        int degenerate = 0;
        for (geo::Index f = 0; f < t.mesh.faceCapacity(); ++f)
            degenerate += t.mesh.faceValid(f) && t.mesh.faceArea(f) < 1e-8 * scale * scale;
        CHECK_MESSAGE(degenerate == 0, preset, ": ", degenerate, " zero-area faces of ", t.mesh.numFaces());
    }
}

TEST_CASE("tree generator: package registers its commands, panel, actions and menu")
{
    ensureLoaded();
    for (const char* c : {"treeGrowTrunk", "treeCanopyBlobs", "treeScatterLeaves", "treeBuild", "treeRebuild", "treeLeafPlane", "treeStamp", "treeSettings",
                          "treeExport", "treeExportLeaf", "treeMeasureBake", "treeOpacityMap", "treeSplitMasks", "treePresets", "treeBakeHandoff"})
        CHECK_MESSAGE(ops::find(c), c);
    CHECK(ui::panels::registry().find("treeGenerator"));
    CHECK(ui::actions::find("treeGenerator.build"));
    CHECK(ui::actions::find("window.treeGenerator"));
    CHECK(ui::menus::itemRegistry().find("treeGenerator.menu"));
    CHECK(run("treePresets").size() == 10);
}

TEST_CASE("tree generator: every preset builds a stamped tree with its parts")
{
    fresh();
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    for (const std::string& preset : treegen::builtInPresets())
    {
        CAPTURE(preset);
        const std::string root = run("treeBuild", {{"preset", preset}, {"settings", quick()}, {"leaf", leaf}, {"name", "tree"}}).get<std::string>();
        Graph& g = Graph::current();
        Node* r = g.find(root);
        REQUIRE(r);
        CHECK(r->type().name == "treeGenerator");
        const json stored = run("treeSettings", {{"args", {root}}});
        CHECK(stored["preset"] == preset);
        CHECK(stored["sources"]["leaf"] == json::array({leaf}));
        std::set<std::string> parts;
        for (NodeId c : g.children(r->id()))
            parts.insert(g.node(c)->as<std::string>(g.node(c)->type().attrIndex("treeGenerator_part")));
        CHECK(parts.contains("trunk"));
        CHECK(parts.contains("leaves"));
        CHECK_FALSE(parts.contains("canopy"));
        MeshPtr leaves = mesh(root + "|tree_leaves");
        CHECK(leaves->numFaces() > 0);
        CHECK(leaves->numFaces() % 6 == 0);
        CHECK(leaves->numFaces() <= 60 * 6);
        CHECK(geo::colors::current(*leaves) == "leafShade");
        CHECK(mesh(root + "|tree_trunk")->cornerAttrs().has(geo::attr::uvName(0)));
        run("delete", {{"args", {root}}});
    }
}

TEST_CASE("tree generator: deterministic per seed, undoable, rebuildable")
{
    fresh();
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    const std::string a = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", quick(5)}, {"leaf", leaf}, {"name", "a"}}).get<std::string>();
    const std::string b = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", quick(5)}, {"leaf", leaf}, {"name", "b"}}).get<std::string>();
    const std::string c = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", quick(6)}, {"leaf", leaf}, {"name", "c"}}).get<std::string>();
    CHECK(positions(*mesh(a + "|a_leaves")) == positions(*mesh(b + "|b_leaves")));
    CHECK(positions(*mesh(a + "|a_trunk")) == positions(*mesh(b + "|b_trunk")));
    CHECK(positions(*mesh(a + "|a_trunk")) != positions(*mesh(c + "|c_trunk")));

    const std::string rebuilt = run("treeRebuild", {{"args", {a}}}).get<std::string>();
    CHECK(rebuilt == "a_rebuilt");
    CHECK(positions(*mesh(rebuilt + "|a_rebuilt_leaves")) == positions(*mesh(a + "|a_leaves")));

    run("undo");
    CHECK_FALSE(Graph::current().find("a_rebuilt"));
    run("undo");
    CHECK_FALSE(Graph::current().find("c"));

    json kept = quick(5);
    kept["blob.keep_canopy"] = true;
    const std::string k = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", kept}, {"leaf", leaf}, {"name", "k"}}).get<std::string>();
    Node* canopy = Graph::current().find(k + "|k_canopy");
    REQUIRE(canopy);
    CHECK_FALSE(canopy->as<bool>(Transform::kVisibility));
}

TEST_CASE("tree generator: step by step build links the parts to the trunk")
{
    fresh();
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    const std::string trunk = run("treeGrowTrunk", {{"settings", quick()}}).get<std::string>();
    const json tips = run("treeBranchTips", {{"args", {trunk}}});
    CHECK(tips.size() == 9);
    const std::string canopy = run("treeCanopyBlobs", {{"args", {trunk}}, {"settings", quick()}}).get<std::string>();
    Graph& g = Graph::current();
    CHECK(g.children(g.find(canopy)->id()).size() == 9);
    json blobs = json::array();
    for (NodeId id : g.children(g.find(canopy)->id()))
        blobs.push_back(g.node(id)->path());
    const std::string leaves = run("treeScatterLeaves", {{"settings", quick()}, {"leaf", leaf}, {"canopy", blobs}}).get<std::string>();
    // Settings resolve through the part links back to the trunk.
    run("select", {{"args", {leaves}}});
    CHECK(run("treeSettings")["tree"] == trunk);

    // Two leaf meshes give detail levels placed identically.
    const std::string lod1 = run("treeLeafPlane", {{"settings", {{"plane.segments", 2}, {"plane.name", "leafLow"}}}}).get<std::string>();
    const std::string group = run("treeScatterLeaves", {{"settings", quick()}, {"leaf", json::array({leaf, lod1})}, {"canopy", blobs}, {"name", "lods"}}).get<std::string>();
    CHECK(g.children(g.find(group)->id()).size() == 2);
    CHECK(mesh(group + "|lods_lod0")->numFaces() > mesh(group + "|lods_lod1")->numFaces());
}

TEST_CASE("tree generator: export writes the pair, the sidecar and a palette that is never overwritten")
{
    fresh();
    const fs::path out = scratch("export");
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    json settings = quick();
    settings["export.path"] = out.string();
    settings["export.name"] = "Maple";
    const std::string root = run("treeBuild", {{"preset", "Kaede Mature"}, {"settings", settings}, {"leaf", leaf}, {"name", "tree"}}).get<std::string>();
    const json written = run("treeExport", {{"args", {root}}, {"preset", "Kaede Mature"}, {"settings", settings}});
    for (const char* f : {"Maple_Hi.fbx", "Maple_Lo.fbx", "Maple_tree.json", "Maple_palette.json"})
        CHECK_MESSAGE(fs::exists(out / f), f);
    CHECK(fs::file_size(out / "Maple_Hi.fbx") > fs::file_size(out / "Maple_Lo.fbx"));
    CHECK(written["high"] == (out / "Maple_Hi.fbx").string());

    const json side = readJson(out / "Maple_tree.json");
    std::set<std::string> keys;
    for (const auto& [k, _] : side.items())
        keys.insert(k);
    CHECK(keys == std::set<std::string>{"tree", "kind", "preset", "species", "version", "colourSet", "materials", "faces"});
    CHECK(side["tree"] == "Maple");
    CHECK(side["kind"] == "tree");
    CHECK(side["species"] == "kaede");
    CHECK(side["colourSet"] == "leafShade");
    CHECK(side["materials"] == json({{"trunk", "Trunk"}, {"canopy", "Canopy"}, {"leaves", "Leaves"}}));
    CHECK(side["faces"]["trunk"].get<int64_t>() > 0);
    CHECK(side["faces"]["leaves"].get<int64_t>() > 0);

    json palette = readJson(out / "Maple_palette.json");
    CHECK(palette["leaves"]["summer"]["base"] == "#789447");
    CHECK(palette["bark"].contains("deep"));
    palette["bark"]["base"] = "#123456";
    std::ofstream(out / "Maple_palette.json") << palette.dump(4);
    run("treeExport", {{"args", {root}}, {"settings", settings}});
    CHECK(readJson(out / "Maple_palette.json")["bark"]["base"] == "#123456");

    // The export assigns the part materials and leaves no temporary node behind.
    CHECK(run("listMaterials").size() >= 2);
    CHECK_FALSE(Graph::current().find("Maple_Hi"));

    const json handoff = run("treeBakeHandoff", {{"args", {root}}, {"settings", settings}});
    CHECK(handoff["low"] == (out / "Maple_Lo.fbx").string());
    CHECK(handoff["rays"].contains("trunk"));
    CHECK(handoff["materials"]["leaves"] == "Leaves");

    const json rays = run("treeMeasureBake", {{"args", {root}}, {"settings", settings}});
    CHECK(rays["trunk"]["count"].get<int64_t>() > 0);

    // The low poly carries the part materials and the leaf colour set through the file.
    run("newScene");
    run("importFbx", {{"args", {(out / "Maple_Lo.fbx").string()}}});
    std::set<std::string> slotNames, setNames;
    Graph& g = Graph::current();
    g.evaluate();
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            if (MeshPtr m = n->as<MeshPtr>(MeshNode::kOutMesh))
            {
                for (const std::string& s : m->materialNames())
                    slotNames.insert(s);
                for (const std::string& s : geo::colors::sets(*m))
                    setNames.insert(s);
            }
    CHECK(slotNames.contains("Trunk"));
    CHECK(slotNames.contains("Leaves"));
    CHECK(setNames.contains("leafShade"));
}

TEST_CASE("tree generator: single leaf export, opacity map and mask split")
{
    fresh();
    const fs::path out = scratch("leaf");
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    json settings = {{"export.path", out.string()}, {"export.name", "Maple"}, {"export.leaf_divisions", 1}};
    const json written = run("treeExportLeaf", {{"preset", "Kaede Young"}, {"settings", settings}, {"leaf", leaf}});
    for (const char* f : {"Maple_Leaf_Hi.fbx", "Maple_Leaf_Lo.fbx", "Maple_Leaf_tree.json", "Maple_Leaf_palette.json", "Maple_Leaf_opacity.png"})
        CHECK_MESSAGE(fs::exists(out / f), f);
    const json side = readJson(out / "Maple_Leaf_tree.json");
    CHECK(side["kind"] == "leaf");
    CHECK(side["materials"] == json({{"leaves", "Leaves"}}));
    CHECK(side["faces"]["leaves"] == 6);
    QImage opacity(QString::fromStdString((out / "Maple_Leaf_opacity.png").string()));
    CHECK(opacity.width() == 1024);

    // A tree part is not a leaf.
    const std::string root = run("treeBuild", {{"settings", quick()}, {"leaf", leaf}}).get<std::string>();
    CHECK_THROWS_AS(run("treeExportLeaf", {{"settings", settings}, {"leaf", root + "|tree_leaves"}}), CommandError);

    QImage bake(8, 8, QImage::Format_RGBA8888);
    for (int i = 0; i < 64; ++i)
        bake.setPixelColor(i % 8, i / 8, QColor(i * 4 % 256, 90, 180, 255));
    REQUIRE(bake.save(QString::fromStdString((out / "Maple_leafShade.png").string())));
    const json masks = run("treeSplitMasks", {{"settings", settings}});
    REQUIRE(masks.size() == 3);
    CHECK(masks[0] == (out / "Maple_maskR.png").string());
    QImage r(QString::fromStdString(masks[0].get<std::string>())), gch(QString::fromStdString(masks[1].get<std::string>()));
    CHECK(QColor(r.pixel(1, 0)).red() == 4);
    CHECK(QColor(gch.pixel(3, 3)).red() == 90);
}

TEST_CASE("tree generator: presets save to the preferences")
{
    fresh();
    run("treeSavePreset", {{"args", {"Bonsai"}}, {"preset", "Kaede Young"}, {"settings", {{"trunk.height", 0.6}}}});
    CHECK(run("treePresets").back() == "Bonsai");
    CHECK(run("treePresetSettings", {{"args", {"Bonsai"}}})["trunk.height"] == 0.6);
    CHECK_THROWS_AS(run("treeSavePreset", {{"args", {"Default"}}}), CommandError);
    CHECK(run("treeDeletePreset", {{"args", {"Bonsai"}}}) == true);
    CHECK(run("treePresets").size() == 10);
}

TEST_CASE("tree generator: native texture sets from the species palette")
{
    fresh();
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    const std::string tree = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", quick(5)}, {"leaf", leaf}, {"name", "maple"}}).get<std::string>();
    const json r = run("treeTextureSet", {{"args", {tree}}, {"bake", false}, {"season", "autumn"}});
    REQUIRE(r["sets"].size() == 2);
    CHECK(r["species"] == "kaede");
    const std::string leaves = r["sets"][1].get<std::string>();
    const json stack = run("texLayerStack", {{"args", {leaves}}});
    CHECK(stack["layers"].size() == 6);
    CHECK(stack["layers"][0]["material"]["baseColor"] == "#C86A22");
    const json e = run("texEvaluate", {{"args", {leaves}}, {"resolution", 16}, {"backend", "cpu"}, {"channels", {"baseColor"}},
                                       {"folder", (std::filesystem::temp_directory_path() / "lila_foliage_tests" / "tex").string()}});
    CHECK(e["files"].size() == 1);
}

TEST_CASE("tree generator: an exported tree's bake project comes from the paths the export stored")
{
    fresh();
    const fs::path out = scratch("bakeproject");
    const std::string leaf = run("treeLeafPlane").get<std::string>();
    const std::string tree = run("treeBuild", {{"preset", "Kaede Young"}, {"settings", quick(5)}, {"leaf", leaf}, {"name", "maple"}}).get<std::string>();
    json settings = json::object();
    settings["export.path"] = out.string();
    settings["export.name"] = "maple";
    run("treeExport", {{"args", {tree}}, {"settings", settings}});

    CHECK(run("treeBakeHandoff", {{"args", {tree}}})["high"] == (out / "maple_Hi.fbx").string());
    const json r = run("treeTextureSet", {{"args", {tree}}, {"season", "autumn"}});
    CHECK_FALSE(r["bakeProject"].get<std::string>().empty());
    CHECK(r["warnings"].empty());
    CHECK_FALSE(run("createBakeFromGenerator", {{"args", {tree}}, {"kind", "tree"}})["project"].get<std::string>().empty());
}
