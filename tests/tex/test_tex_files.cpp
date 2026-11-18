#include "TexTest.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/GraphFiles.h"
#include "tex/NodeDef.h"
#include "tex/TextureSet.h"

#include <filesystem>
#include <fstream>

using namespace textest;

namespace
{
namespace fs = std::filesystem;
} // namespace

TEST_CASE("tex: a set exports to .lilamat and imports as a new set")
{
    if (!ops::find("texExportGraph"))
        registerFileCommands();
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"settings", {{"width", 512}, {"height", 512}}}});
    run("texAddNode", {{"args", {"Rock", "perlin"}}, {"name", "Noise"}});
    run("texConnect", {{"args", {"Rock", "Noise.out", "Output.roughness"}}});
    run("texSetValue", {{"args", {"Rock", "Noise.scale", 9}}});

    fs::path dir = tempDir("files");
    std::string file = (dir / "rock.lilamat").generic_string();
    CHECK(run("texExportGraph", {{"args", {"Rock", file}}}).get<std::string>() == file);
    REQUIRE(fs::exists(file));

    json names = run("texImportGraph", {{"args", {file}}});
    REQUIRE(names.size() == 1);
    std::string copy = names[0];
    CHECK(copy != "Rock");
    scene::Node* n = findSet(scene::Graph::current(), copy);
    REQUIRE(n);
    Graph g = readGraph(*n);
    CHECK(g.name == copy);
    REQUIRE(g.node("Noise"));
    CHECK(g.node("Noise")->values["scale"] == 9);
    CHECK(g.links.size() == 1);
    CHECK(readSettings(*n).width == 512);

    run("undo");
    CHECK_FALSE(findSet(scene::Graph::current(), copy));

    CHECK(run("texImportGraph", {{"args", {file}}, {"name", "Granite"}})[0] == "Granite");
    CHECK_THROWS(run("texImportGraph", {{"args", {(dir / "missing.lilamat").generic_string()}}}));
}

TEST_CASE("tex: node groups export as a node library and import into the user's nodes")
{
    if (!ops::find("texExportGraph"))
        registerFileCommands();
    run("newScene");
    run("texCreateSet", {{"args", {"Lib"}}});
    fs::path dir = tempDir("library");
    std::string file = (dir / "lib.lilatexnodes").generic_string();
    // A set without node groups has nothing to put in a library.
    CHECK_THROWS(run("texExportGraph", {{"args", {"Lib", file}}}));

    Graph sub = Graph::makeGroup("tstDarken");
    GraphInput in;
    in.name = "in";
    in.port = PortType::Color;
    sub.inputs.push_back(in);
    add(sub, "Lv", "levels");
    link(sub, "GroupInput.in", "Lv.input");
    link(sub, "Lv.out", "GroupOutput.out");
    json def = readGraph(*findSet(scene::Graph::current(), "Lib")).toJson();
    def["subgroups"] = {{"tstDarken", sub.toJson()}};
    run("texSetDefinition", {{"args", {"Lib", def}}});
    CHECK(run("texExportGraph", {{"args", {"Lib", file}}}).get<std::string>() == file);
    std::vector<NodeDef> defs = readNodeFile(file);
    REQUIRE(defs.size() == 1);
    CHECK(defs[0].type == "tstDarken");
    CHECK(defs[0].kind == NodeKind::Graph);

    json lib = {{"format", "lilatexnode"},
                {"version", 1},
                {"type", "tstTint"},
                {"label", "Tint Test"},
                {"category", "Filter"},
                {"inputs", json::array({{{"id", "input"}, {"type", "color"}, {"default", json::array({1, 1, 1, 1})}}})},
                {"outputs", json::array({{{"id", "out"}, {"type", "color"}}})},
                {"body", {{"out", "return input.sample(uv) * 0.5;"}}}};
    fs::path single = dir / "tstTint.lilatexnode";
    std::ofstream(single) << lib.dump(2);
    CHECK(run("texImportGraph", {{"args", {single.generic_string()}}}).empty());
    CHECK(findNode("tstTint"));
    CHECK(fs::exists(nodeDirs().at(1) / "tstTint.lilatexnode"));
    fs::remove(nodeDirs().at(1) / "tstTint.lilatexnode");
    reloadLibrary();
    CHECK_FALSE(findNode("tstTint"));
}
