#include "TexTest.h"

#include "bake/BakeNodes.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "tex/Generators.h"
#include "tex/Layers.h"
#include "tex/PaintStrokes.h"
#include "tex/SmartMaterial.h"
#include "tex/TextureSet.h"

#include <filesystem>

using namespace textest;

namespace
{
namespace fs = std::filesystem;

scene::Node& setNode(const std::string& name)
{
    scene::Node* n = findSet(scene::Graph::current(), name);
    REQUIRE(n);
    return *n;
}

image::Image flat(int res, float r, float g, float b)
{
    image::Image img{res, res, {}};
    for (int i = 0; i < res * res; ++i)
        img.rgba.insert(img.rgba.end(), {r, g, b, 1.0f});
    return img;
}

using Maps = std::map<std::string, image::Image>;

/** Mesh maps of a flat, open, upward-facing, thin, low and red surface. */
Maps mapsA(int res)
{
    return {{"curvature", flat(res, 0.5f, 0.5f, 0.5f)}, {"ao", flat(res, 1, 1, 1)},          {"cavity", flat(res, 1, 1, 1)},
            {"thickness", flat(res, 0.2f, 0.2f, 0.2f)}, {"position", flat(res, 0.5f, 0.1f, 0.5f)}, {"objectNormal", flat(res, 0.5f, 1.0f, 0.5f)},
            {"id", flat(res, 1, 0, 0)},                 {"uvIslands", flat(res, 1, 0, 0)},     {"height", flat(res, 0.5f, 0.5f, 0.5f)}};
}

/** Sharp convex, occluded, sideways-facing, thick, high and green. */
Maps mapsB(int res)
{
    return {{"curvature", flat(res, 0.9f, 0.9f, 0.9f)}, {"ao", flat(res, 0.2f, 0.2f, 0.2f)},      {"cavity", flat(res, 0.1f, 0.1f, 0.1f)},
            {"thickness", flat(res, 0.9f, 0.9f, 0.9f)}, {"position", flat(res, 0.5f, 0.9f, 0.5f)}, {"objectNormal", flat(res, 1.0f, 0.5f, 0.5f)},
            {"id", flat(res, 0, 1, 0)},                 {"uvIslands", flat(res, 0, 1, 0)},         {"height", flat(res, 0.5f, 0.5f, 0.5f)}};
}

std::map<std::string, image::Image> evalGraph(const Graph& g, const SetSettings& s, int res, const Maps& externals = {})
{
    PlanRequest r;
    r.width = r.height = res;
    r.outputs = channelRequests(g, s);
    Plan p = buildPlan(g, r);
    REQUIRE_MESSAGE(p.error.empty(), p.error);
    EvalOptions o;
    o.externals = externals;
    return evaluateCpu(p, o).outputs;
}

std::map<std::string, image::Image> evalSet(const std::string& set, int res, const Maps& externals = {})
{
    scene::Node& n = setNode(set);
    return evalGraph(readGraph(n), readSettings(n), res, externals);
}

void checkTexel(const image::Image& img, const std::vector<float>& want, double tol = 2e-3)
{
    const float* t = texel(img, img.width / 2, img.height / 2);
    for (size_t c = 0; c < want.size(); ++c)
        CHECK_MESSAGE(std::abs(double(t[c]) - double(want[c])) < tol, "component " << c << ": " << t[c] << " want " << want[c]);
}

double mean(const image::Image& img)
{
    double s = 0.0;
    for (size_t i = 0; i < img.rgba.size(); i += 4)
        s += img.rgba[i];
    return s / double(img.rgba.size() / 4);
}

json stack(const std::string& set) { return run("texLayerStack", {{"args", {set}}}); }
} // namespace

TEST_CASE("tex layers: the stack round-trips through the graph, picks up value edits and falls to graph mode on structural ones")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Wall"}}, {"resolution", 8}});
    CHECK(stack("Wall")["mode"] == "stack");
    const std::string base = run("texAddLayer", {{"args", {"Wall", "fill"}}, {"name", "Base"}})["id"];
    run("texAddAnchor", {{"args", {"Wall", base, "bump"}}});
    const std::string folder = run("texAddLayer", {{"args", {"Wall", "folder"}}})["id"];
    const std::string wear = run("texAddLayer", {{"args", {"Wall", "fill"}}, {"parent", folder}, {"material", {{"baseColor", {0.9, 0.9, 0.9, 1}}}}})["id"];
    run("texAddGenerator", {{"args", {"Wall", wear, "genEdgeWear"}}, {"values", {{"level", 0.4}}}});
    run("texAddMask", {{"args", {"Wall", wear, "levels"}}, {"values", {{"gamma", 1.5}}}});
    run("texAddMask", {{"args", {"Wall", folder, "anchor"}}, {"anchor", "bump"}, {"channel", "height"}});
    const std::string paint = run("texAddLayer", {{"args", {"Wall", "paint"}}})["id"];
    run("texAddLayer", {{"args", {"Wall", "instance"}}, {"source", paint}});
    run("texAddLayer", {{"args", {"Wall", "filter"}}, {"filter", "hsv"}, {"values", {{"saturation", 0.5}}}});
    Graph proc = Graph::makeGroup("p");
    proc.outputs = {{"roughness", PortType::Gray, {}}};
    proc.nodes.push_back({"N", "perlin", {0.0, 0.0}, json::object()});
    proc.links.push_back({"N", "out", "GroupOutput", "roughness"});
    run("texAddLayer", {{"args", {"Wall", "procedural"}}, {"graph", proc.toJson()}, {"below", folder}});

    json view = stack("Wall");
    CHECK(view["mode"] == "stack");
    REQUIRE(view["layers"].size() == 6);
    CHECK(view["layers"][0]["id"] == base);
    CHECK(view["layers"][1]["kind"] == "procedural");
    CHECK(view["layers"][2]["children"][0]["masks"].size() == 2);
    CHECK(view["anchors"]["bump"] == base);
    CHECK(std::find(view["maps"].begin(), view["maps"].end(), "curvature") != view["maps"].end());

    // The graph alone reproduces the stack (ids and all), and recompiling it gives the same graph.
    scene::Node& n = setNode("Wall");
    Graph g = readGraph(n);
    StackState st = readStack(g, readSettings(n));
    CHECK(st.layered);
    CHECK_FALSE(st.graphMode);
    CHECK(st.stack.layers.size() == 6);
    CHECK(compileStack(st.stack, readSettings(n), g).graph.toJson() == g.toJson());
    CHECK(st.stack.find(wear)->masks[0].values["level"] == 0.4);
    REQUIRE_NOTHROW(evalSet("Wall", 8, mapsA(8)));

    // A value edited as nodes lands in the stack and survives later stack edits.
    run("texSetValue", {{"args", {"Wall", base + "_baseColor.opacity", 0.25}}});
    view = stack("Wall");
    CHECK(view["mode"] == "stack");
    CHECK(view["layers"][0]["channels"]["baseColor"]["opacity"] == 0.25);
    run("texSetLayer", {{"args", {"Wall", paint, {{"name", "Scuffs"}}}}});
    view = stack("Wall");
    CHECK(view["layers"][0]["resolved"]["baseColor"]["opacity"] == 0.25);
    CHECK(view["layers"][3]["name"] == "Scuffs");

    // Moves and removals; instances keep their source alive.
    CHECK_THROWS_AS(run("texRemoveLayer", {{"args", {"Wall", paint}}}), CommandError);
    run("texMoveLayer", {{"args", {"Wall", paint}}, {"index", 0}});
    CHECK(stack("Wall")["layers"][0]["id"] == paint);
    run("undo");
    CHECK(stack("Wall")["layers"][3]["id"] == paint);

    // A structural node edit the stack can't show: graph mode, layer edits refuse until rebuilt.
    run("texAddNode", {{"args", {"Wall", "perlin"}}});
    view = stack("Wall");
    CHECK(view["mode"] == "graph");
    CHECK(view["edited"] == true);
    CHECK_THROWS_AS(run("texAddLayer", {{"args", {"Wall", "fill"}}}), CommandError);
    run("texRebuildLayers", {{"args", {"Wall"}}});
    CHECK(stack("Wall")["mode"] == "stack");
    CHECK(stack("Wall")["layers"].size() == 6);
    run("undo");
    CHECK(stack("Wall")["mode"] == "graph");
    run("undo");
    CHECK(stack("Wall")["mode"] == "stack");
}

TEST_CASE("tex layers: a graph made by hand joins the stack as its bottom layer")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Hand"}}, {"resolution", 4}});
    run("texAddNode", {{"args", {"Hand", "constant"}}, {"name", "R"}});
    run("texSetValue", {{"args", {"Hand", "R.value", 0.3}}});
    run("texConnect", {{"args", {"Hand", "R.out", "Output.roughness"}}});
    run("texSetValue", {{"args", {"Hand", "Output.metallic", 0.6}}});
    CHECK(stack("Hand")["mode"] == "graph");
    CHECK(stack("Hand")["edited"] == false);
    run("texAddLayer", {{"args", {"Hand", "fill"}}, {"material", {{"baseColor", {1, 1, 1, 1}}}}});
    json view = stack("Hand");
    CHECK(view["mode"] == "stack");
    REQUIRE(view["layers"].size() == 2);
    CHECK(view["layers"][0]["kind"] == "procedural");
    auto out = evalSet("Hand", 4);
    checkTexel(out.at("roughness"), {0.3f});
    checkTexel(out.at("metallic"), {0.6f});
    checkTexel(out.at("baseColor"), {1, 1, 1});
}

TEST_CASE("tex layers: blend modes and opacity per channel (CPU goldens)")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Gold"}}, {"resolution", 4}});
    run("texAddLayer", {{"args", {"Gold", "fill"}},
                        {"material", {{"baseColor", {1, 0, 0, 1}}, {"roughness", 0.8}, {"height", 0.7}, {"metallic", 0.9}}},
                        {"channels", {{"baseColor", {{"opacity", 0.5}}}, {"metallic", {{"enabled", false}}}}}});
    run("texAddLayer", {{"args", {"Gold", "fill"}},
                        {"material", {{"roughness", 0.5}, {"height", 0.6}, {"metallic", 1.0}}},
                        {"channels", {{"roughness", {{"mode", "multiply"}}}, {"metallic", {{"opacity", 0.25}}}}}});
    const std::string off = run("texAddLayer", {{"args", {"Gold", "fill"}}, {"material", {{"baseColor", {1, 1, 1, 1}}}}})["id"];
    run("texSetLayer", {{"args", {"Gold", off, {{"enabled", false}}}}});
    run("texAddLayer", {{"args", {"Gold", "fill"}}, {"material", {{"normal", {0.8, 0.5, 0.9, 1}}, {"emissive", {0, 0, 1, 1}}}},
                        {"channels", {{"emissive", {{"mode", "add"}, {"opacity", 0.5}}}}}});
    const std::string half = run("texAddLayer", {{"args", {"Gold", "fill"}}, {"material", {{"ao", 0.2}}}})["id"];
    run("texAddMask", {{"args", {"Gold", half, "fill"}}, {"value", 0.5}});

    auto out = evalSet("Gold", 4);
    checkTexel(out.at("baseColor"), {0.75f, 0.25f, 0.25f});
    checkTexel(out.at("roughness"), {0.4f});
    checkTexel(out.at("metallic"), {0.25f});
    checkTexel(out.at("height"), {0.8f});
    checkTexel(out.at("emissive"), {0.0f, 0.0f, 0.5f});
    checkTexel(out.at("ao"), {0.6f});
    // The layer's normal over flat normals, and a constant height adds no slope.
    checkTexel(out.at("normal"), {0.8f, 0.5f, 0.9f}, 5e-3);

    // Height-to-normal: a height ramp bends the composited normal.
    run("texAddLayer", {{"args", {"Gold", "procedural"}}, {"graph", [] {
                            Graph p = Graph::makeGroup("ramp");
                            p.outputs = {{"height", PortType::Gray, {}}};
                            p.nodes.push_back({"G", "gradient", {0.0, 0.0}, json::object()});
                            p.links.push_back({"G", "out", "GroupOutput", "height"});
                            return p.toJson();
                        }()}});
    out = evalSet("Gold", 16);
    CHECK(texel(out.at("normal"), 8, 8)[0] != doctest::Approx(0.8f).epsilon(0.01));
}

TEST_CASE("tex layers: anchors feed masks and generator inputs above them")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Anc"}}, {"resolution", 4}});
    const std::string bump = run("texAddLayer", {{"args", {"Anc", "fill"}}, {"material", {{"height", 0.8}}}})["id"];
    run("texAddAnchor", {{"args", {"Anc", bump, "bump"}}});
    const std::string top = run("texAddLayer", {{"args", {"Anc", "fill"}}, {"material", {{"baseColor", {1, 1, 1, 1}}}}})["id"];
    run("texAddMask", {{"args", {"Anc", top, "anchor"}}, {"anchor", "bump"}});
    checkTexel(evalSet("Anc", 4).at("baseColor"), {0.9f, 0.9f, 0.9f});

    // A generator input bound to the anchor instead of its mesh map.
    const std::string rough = run("texAddLayer", {{"args", {"Anc", "fill"}}, {"material", {{"roughness", 1.0}}}})["id"];
    run("texAddGenerator", {{"args", {"Anc", rough, "genThickness"}}, {"bindings", {{"thickness", "anchor:bump.height"}}},
                            {"values", {{"level", 0.5}, {"softness", 0.2}}}});
    checkTexel(evalSet("Anc", 4).at("roughness"), {1.0f});
    CHECK_THROWS_AS(run("texAddMask", {{"args", {"Anc", top, "anchor"}}, {"anchor", "nope"}}), CommandError);
    CHECK_THROWS_AS(run("texAddAnchor", {{"args", {"Anc", top, "bump"}}}), CommandError);
}

TEST_CASE("tex layers: every generator responds to its mesh maps")
{
    const std::vector<std::string> types = generatorTypes();
    CHECK(types.size() >= 17);
    SetSettings s;
    s.channels = {{"baseColor", PortType::Color, 8}};
    const int res = 32;
    for (const std::string& type : types)
    {
        CAPTURE(type);
        LayerStack st;
        st.base["baseColor"] = json::array({0, 0, 0, 1});
        Layer l;
        l.id = "L1";
        l.material["baseColor"] = json::array({1, 1, 1, 1});
        Mask m;
        m.id = "M2";
        m.kind = MaskKind::Generator;
        m.type = type;
        l.masks.push_back(m);
        st.layers.push_back(l);
        StackCompile c = compileStack(st, s, Graph::makeMaterial("gen"));
        CHECK(c.warnings.empty());
        auto a = evalGraph(c.graph, s, res, mapsA(res)).at("baseColor");
        auto b = evalGraph(c.graph, s, res, mapsB(res)).at("baseColor");
        CHECK(meanDiff(a, b) > 1e-3);
        if (type == "genEdgeWear" || type == "genDirt" || type == "genCavityGrime" || type == "genRustSpread" || type == "genThickness" ||
            type == "genAoDarken" || type == "genPaintChips")
            CHECK(mean(b) > mean(a));
        if (type == "genDust" || type == "genMoss" || type == "genIdSelect" || type == "genPolygonFill" || type == "genWetness")
            CHECK(mean(a) > mean(b));
    }
}

TEST_CASE("tex layers: smart materials apply to different sets and adapt to their bakes")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Crate"}}, {"resolution", 16}});
    run("texCreateSet", {{"args", {"Rock"}},
                         {"settings", {{"width", 8}, {"height", 8},
                                       {"channels", {{{"name", "baseColor"}, {"type", "color"}}, {{"name", "roughness"}, {"type", "gray"}},
                                                     {{"name", "height"}, {"type", "gray"}, {"bits", 16}}, {{"name", "wetness"}, {"type", "gray"}}}}}}});
    bake::ProjectSettings ps = bake::ProjectSettings::defaults();
    ps.maps.clear();
    for (const char* m : {"normal", "ao"})
    {
        bake::MapSettings ms;
        ms.map = m;
        ps.maps.push_back(ms);
    }
    bake::createProject(scene::Graph::current(), "Bakes", ps);
    run("texSetSettings", {{"args", {"Crate", {{"bakeProject", "Bakes"}}}}});

    std::vector<std::string> names;
    for (const SmartMaterial& m : listSmartMaterials())
        if (m.tier == "builtin")
            names.push_back(m.name);
    CHECK(names.size() >= 5);
    for (const char* smart : {"wornPaintedMetal", "rustyIron", "dustyWood", "mossyStone", "edgeWornPlastic"})
    {
        CAPTURE(smart);
        for (const char* set : {"Crate", "Rock"})
        {
            CAPTURE(set);
            json r = run("texApplySmartMaterial", {{"args", {set, smart}}});
            CHECK_FALSE(r.contains("warnings"));
            CHECK(stack(set)["mode"] == "stack");
            auto out = evalSet(set, 8, mapsB(8));
            CHECK(out.contains("baseColor"));
            run("undo");
        }
    }

    json r = run("texApplySmartMaterial", {{"args", {"Crate", "wornPaintedMetal"}}});
    std::vector<std::string> added = r["addedMaps"];
    CHECK(std::find(added.begin(), added.end(), "curvature") != added.end());
    CHECK(std::find(added.begin(), added.end(), "ao") == added.end());
    json view = stack("Crate");
    REQUIRE(view["layers"].size() == 1);
    CHECK(view["layers"][0]["kind"] == "folder");
    CHECK(view["layers"][0]["children"].size() == 4);
    const std::string folder = view["layers"][0]["id"];

    // Mesh maps decide the result: worn edges show bare metal on convex areas only.
    Maps edges = mapsA(8);
    edges["curvature"] = flat(8, 0.9f, 0.9f, 0.9f);
    edges["objectNormal"] = flat(8, 1.0f, 0.5f, 0.5f);
    CHECK(mean(evalSet("Crate", 8, mapsA(8)).at("metallic")) < 0.05);
    CHECK(mean(evalSet("Crate", 8, edges).at("metallic")) > 0.95);

    // Saved and applied elsewhere, ids fresh and anchors kept unique.
    fs::path file = fs::temp_directory_path() / "lila_tex_tests" / "crate.lilasmart";
    CHECK(fs::path(run("texSaveSmartMaterial", {{"args", {"Crate", folder, file.string()}}, {"name", "Crate Paint"}}).get<std::string>()) == file);
    run("texApplySmartMaterial", {{"args", {"Rock", "rustyIron"}}});
    json second = run("texApplySmartMaterial", {{"args", {"Rock", file.string()}}});
    json rock = stack("Rock");
    CHECK(rock["layers"].size() == 2);
    CHECK(rock["layers"][1]["id"] == second["layer"]);
    CHECK(rock["anchors"].size() == 1);
    run("texApplySmartMaterial", {{"args", {"Rock", "rustyIron"}}});
    CHECK(stack("Rock")["anchors"].contains("rust2"));
    REQUIRE_NOTHROW(evalSet("Rock", 8, mapsA(8)));

    // Smart masks: a layer's mask stack, applied to another layer.
    const std::string paintLayer = view["layers"][0]["children"][1]["id"];
    fs::path maskFile = fs::temp_directory_path() / "lila_tex_tests" / "wear.lilasmart";
    run("texSaveSmartMaterial", {{"args", {"Crate", paintLayer, maskFile.string()}}, {"mask", true}});
    const std::string target = run("texAddLayer", {{"args", {"Rock", "fill"}}})["id"];
    json masks = run("texApplySmartMaterial", {{"args", {"Rock", maskFile.string()}}, {"layer", target}})["masks"];
    CHECK(masks.size() == 3);
    fs::remove(file);
    fs::remove(maskFile);
}

TEST_CASE("tex layers: paint covers the stack below only where and on the channels it was painted")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Painted"}}, {"resolution", 4}});
    run("texAddLayer", {{"args", {"Painted", "fill"}}, {"material", {{"baseColor", {1, 0, 0, 1}}, {"roughness", 0.5}}}});
    const std::string id = run("texAddLayer", {{"args", {"Painted", "paint"}}, {"material", {{"baseColor", {0.2, 0.4, 0.6, 1}}, {"roughness", 0.5}}}})["id"];
    // Roughness painted (value 0.9) on half coverage; base colour never painted.
    fs::path file = fs::temp_directory_path() / "lila_tex_tests" / "paint_rough.exr";
    std::vector<float> px;
    for (int i = 0; i < 16; ++i)
        px.insert(px.end(), {0.9f, 0.9f, 0.9f, 0.5f});
    image::writeExr(file, 4, 4, px, 32);
    run("texSetLayer", {{"args", {"Painted", id, {{"paint", {{"roughness", file.string()}}}, {"masks", json::array({{{"kind", "fill"}, {"id", "M90"}}})}}}}});
    auto out = evalSet("Painted", 4);
    checkTexel(out.at("roughness"), {0.7f});
    checkTexel(out.at("baseColor"), {1.0f, 0.0f, 0.0f});
    fs::remove(file);
}

TEST_CASE("tex layers: set evaluation reads the bake project's live mesh maps, worldNormal as objectNormal")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Live"}}, {"resolution", 8}, {"settings", {{"bakeProject", "Proj"}}}});
    const std::string id = run("texAddLayer", {{"args", {"Live", "fill"}}, {"material", {{"baseColor", {1, 1, 1, 1}}}}})["id"];
    run("texAddGenerator", {{"args", {"Live", id, "genDust"}}, {"values", {{"breakup", 0.0}, {"level", 0.5}, {"softness", 0.1}}}});
    std::vector<std::string> asked;
    setLiveMeshMapReader([&](const std::string& project, const std::string& map, int) -> image::Image {
        asked.push_back(project + ":" + map);
        if (map == "objectNormal")
            return flat(8, 0.5f, 1.0f, 0.5f);
        if (map == "ao")
            return flat(8, 1, 1, 1);
        return {};
    });
    scene::Node& n = setNode("Live");
    auto up = evaluateOutputs(n, channelRequests(readGraph(n), readSettings(n)), 1001, 8, 8, "cpu");
    setLiveMeshMapReader({});
    CHECK(std::find(asked.begin(), asked.end(), "Proj:objectNormal") != asked.end());
    checkTexel(up.at("baseColor"), {1, 1, 1});
    CHECK(bakeMapName("worldNormal") == "objectNormal");
}

TEST_CASE("tex layers: texAddIdLayers adds a fill layer per ID map entry, masked to it")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Ids"}}, {"resolution", 8}, {"settings", {{"bakeProject", "Proj"}}}});
    CHECK_THROWS(run("texAddIdLayers", {{"args", {"Ids"}}}));
    setLiveMeshMapMetaReader([](const std::string& project, const std::string& map) -> json {
        if (project == "Proj" && map == "id")
            return {{"source", "material"}, {"legend", {{"matA", {255, 0, 0}}, {"matB", {0, 255, 0}}}}};
        return json();
    });
    const json r = run("texAddIdLayers", {{"args", {"Ids"}}});
    CHECK(r.size() == 2);
    const json again = run("texAddIdLayers", {{"args", {"Ids"}}, {"names", {"matB"}}});
    CHECK(again.size() == 1);
    setLiveMeshMapMetaReader({});
    const json view = stack("Ids");
    int folders = 0;
    for (const json& l : view["layers"])
        folders += l.value("kind", "") == "folder";
    CHECK(folders == 1);
    run("undo");
    checkTexel(evalSet("Ids", 8, {{"id", flat(8, 1, 0, 0)}}).at("baseColor"), {1, 0, 0});
    checkTexel(evalSet("Ids", 8, {{"id", flat(8, 0, 1, 0)}}).at("baseColor"), {0, 1, 0});
}

TEST_CASE("tex layers: a procedural layer's tiling repeats its content and its exposed settings reach their nodes")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Tiled"}}, {"resolution", 16}});
    Graph p = Graph::makeGroup("ramp");
    p.outputs = {{"roughness", PortType::Gray, {}}, {"metallic", PortType::Gray, {}}};
    p.nodes.push_back({"G", "gradient", {0.0, 0.0}, json::object()});
    p.nodes.push_back({"C", "constant", {0.0, 0.0}, {{"value", 0.5}}});
    p.links.push_back({"G", "out", "GroupOutput", "roughness"});
    p.links.push_back({"C", "out", "GroupOutput", "metallic"});
    json graph = p.toJson();
    graph["inputs"] = json::array({{{"name", "Shine"}, {"kind", "param"}, {"type", "float"}, {"default", 0.5}, {"targets", {"C.value"}}}});
    const std::string id = run("texAddLayer", {{"args", {"Tiled", "procedural"}}, {"graph", graph}})["id"];

    auto at = [](const image::Image& img, int x) { return double(texel(img, x, 8)[0]); };
    auto out = evalSet("Tiled", 16);
    // One ramp across: a quarter and three quarters differ.
    CHECK(at(out.at("roughness"), 4) != doctest::Approx(at(out.at("roughness"), 12)).epsilon(0.05));

    run("texSetLayer", {{"args", {"Tiled", id, {{"tiling", {{"scale", 2.0}}}, {"params", {{"Shine", 0.2}}}}}}});
    out = evalSet("Tiled", 16);
    // Two ramps across: the same point of each tile matches.
    CHECK(at(out.at("roughness"), 4) == doctest::Approx(at(out.at("roughness"), 12)).epsilon(0.02));
    checkTexel(out.at("metallic"), {0.2f});
    const json l = run("texLayerStack", {{"args", {"Tiled"}}})["layers"][0];
    CHECK(l["tiling"]["scale"] == doctest::Approx(2.0));
    CHECK(l["graph"]["inputs"][0]["default"] == doctest::Approx(0.2));
    CHECK_THROWS(run("texSetLayer", {{"args", {"Tiled", id, {{"params", {{"Nope", 1}}}}}}}));
}

TEST_CASE("tex layers: scrolling a field folds its edits into one undo step")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Scroll"}}, {"resolution", 4}});
    const std::string id = run("texAddLayer", {{"args", {"Scroll", "fill"}}, {"material", {{"roughness", 0.5}}}})["id"];
    // No setting left on the channel reads as full opacity.
    auto opacity = [&] {
        const json l = run("texLayerStack", {{"args", {"Scroll"}}})["layers"][0];
        const json ch = l.contains("channels") && l["channels"].is_object() ? l["channels"].value("roughness", json::object()) : json::object();
        return ch.is_object() ? ch.value("opacity", 1.0) : 1.0;
    };
    UndoStack& undo = UndoStack::get();
    const uint64_t before = undo.newestId();
    ops::setCoalesceKey("wheel:field");
    for (double v : {0.9, 0.8, 0.7})
        ops::execute("texSetLayer", {{"args", {"Scroll", id, {{"channels", {{"roughness", {{"opacity", v}}}}}}}}}, CallSource::UI);
    ops::setCoalesceKey({});
    CHECK(opacity() == doctest::Approx(0.7));
    REQUIRE(undo.undo());
    CHECK(undo.newestId() == before);
    CHECK(opacity() == doctest::Approx(1.0));
    // Without a scrolled field every edit is its own step.
    ops::execute("texSetLayer", {{"args", {"Scroll", id, {{"channels", {{"roughness", {{"opacity", 0.6}}}}}}}}}, CallSource::UI);
    ops::execute("texSetLayer", {{"args", {"Scroll", id, {{"channels", {{"roughness", {{"opacity", 0.4}}}}}}}}}, CallSource::UI);
    REQUIRE(undo.undo());
    CHECK(opacity() == doctest::Approx(0.6));
}

TEST_CASE("tex layers: a new paint layer points every channel and its mask at its buffers, so strokes never change the graph")
{
    run("newScene");
    run("texCreateSet", {{"args", {"Wired"}}, {"resolution", 4}});
    const std::string id = run("texAddLayer", {{"args", {"Wired", "paint"}}})["id"].get<std::string>();
    scene::Node& set = *tex::findSet(scene::Graph::current(), "Wired");
    const tex::SetSettings settings = tex::readSettings(set);
    const tex::StackState st = tex::readStack(tex::readGraph(set), settings);
    const tex::Layer* l = st.stack.find(id);
    REQUIRE(l);
    for (const tex::Channel& c : settings.channels)
        CHECK(l->paint.value(c.name, std::string()) == tex::paint::imageRef(id, c.name));
    CHECK(l->paint.value("mask", std::string()) == tex::paint::imageRef(id, "mask"));
    // Unpainted, the layer changes nothing.
    auto out = evalSet("Wired", 4);
    REQUIRE(out.contains("baseColor"));
}
