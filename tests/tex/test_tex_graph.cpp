#include "TexTest.h"

#include "core/ExtensionContext.h"
#include "tex/NodeDef.h"
#include "tex/Ports.h"
#include "tex/TextureSet.h"

using namespace textest;

TEST_CASE("tex: graph JSON round trips in the geometry-nodes shape")
{
    Graph g = Graph::makeMaterial("Rock");
    g.description = "a rock";
    add(g, "Noise", "perlin", {{"scale", 8}, {"seed", 3}});
    add(g, "Lv", "levels", {{"gamma", 2.2}});
    link(g, "Noise.out", "Lv.input");
    link(g, "Lv.out", "Output.baseColor");
    GraphInput gi;
    gi.name = "noiseScale";
    gi.param = true;
    gi.paramType = ParamType::Int;
    gi.defaultValue = 4;
    gi.min = 1.0;
    gi.targets = {"Noise.scale"};
    g.inputs.push_back(gi);
    GraphInput port;
    port.name = "mask";
    port.port = PortType::Gray;
    port.defaultValue = 1.0;
    g.inputs.push_back(port);
    g.outputs.push_back({"out", PortType::Color, "result"});
    g.subgroups["Inner"] = Graph::makeGroup("Inner").toJson();

    json j = g.toJson();
    CHECK(j["links"][0] == json::array({"Noise", "out", "Lv", "input"}));
    CHECK(j["nodes"].is_array());
    CHECK(j["inputs"][0]["kind"] == "param");
    CHECK(j["inputs"][1]["kind"] == "port");
    Graph back = Graph::fromJson(j);
    CHECK(back.toJson() == j);
    CHECK(back.input("noiseScale")->targets == std::vector<std::string>{"Noise.scale"});
    CHECK(back.input("mask")->port == PortType::Gray);
    CHECK(back.subgroups.contains("Inner"));
    CHECK_THROWS_AS(Graph::fromJson(json::array()), std::invalid_argument);
    CHECK_THROWS_AS(Graph::fromJson(json{{"name", "x"}, {"inputs", json::array({json{{"name", "a"}, {"kind", "param"}, {"type", "nope"}}})}}),
                    std::invalid_argument);
}

TEST_CASE("tex: the built-in node library loads")
{
    reloadLibrary();
    CHECK(libraryErrors().empty());
    for (const char* t : {"uv", "position", "constant", "color", "perlin", "valueNoise", "voronoi", "gradient", "bricks", "tile", "transform", "warp",
                          "levels", "blend", "math", "colorize", "heightToNormal", "normalBlend", "material", "materialOutput", "buffer", "blur",
                          "GroupInput", "GroupOutput", "Group"})
    {
        CAPTURE(t);
        const NodeDef* d = findNode(t);
        REQUIRE(d);
        if (d->tier == "builtin")
        {
            // Every definition survives its own JSON.
            NodeDef again = NodeDef::fromJson(d->toJson());
            CHECK(again.toJson() == d->toJson());
        }
    }
    for (const char* h : {"noise", "voronoi", "blend", "normals"})
        CHECK(findHelper(h));
    CHECK(findNode("voronoi")->outputs.size() == 5);
    CHECK(findNode("heightToNormal")->input("height")->taps == 4);
    CHECK(findNode("blur")->kind == NodeKind::Native);
}

TEST_CASE("tex: node definitions are validated")
{
    json ok = {{"type", "tint"}, {"inputs", json::array({json{{"id", "input"}, {"type", "color"}}})}, {"outputs", json::array({json{{"id", "out"}, {"type", "color"}}})},
               {"params", json::array({json{{"id", "amount"}, {"type", "float"}, {"default", 0.5}}})}, {"body", {{"out", "return input.sample(uv) * p.amount;"}}}};
    CHECK_NOTHROW(NodeDef::fromJson(ok));
    json reserved = ok;
    reserved["inputs"][0]["id"] = "in";
    CHECK_THROWS_AS(NodeDef::fromJson(reserved), std::invalid_argument);
    json noBody = ok;
    noBody.erase("body");
    CHECK_THROWS_AS(NodeDef::fromJson(noBody), std::invalid_argument);
    json badEnum = ok;
    badEnum["params"] = json::array({json{{"id", "mode"}, {"type", "enum"}}});
    CHECK_THROWS_AS(NodeDef::fromJson(badEnum), std::invalid_argument);
    json dup = ok;
    dup["params"][0]["id"] = "input";
    CHECK_THROWS_AS(NodeDef::fromJson(dup), std::invalid_argument);
}

TEST_CASE("tex: extension nodes are owner-tagged and leave with their extension")
{
    json def = {{"type", "extTint"},
                {"category", "Ext"},
                {"inputs", json::array({json{{"id", "input"}, {"type", "color"}}})},
                {"outputs", json::array({json{{"id", "out"}, {"type", "color"}}})},
                {"body", {{"out", "return input.sample(uv) * 0.5;"}}}};
    {
        ext::Context ctx("texExtension");
        registerExtensionNode(ctx, NodeDef::fromJson(def));
        REQUIRE(findNode("extTint"));
        CHECK(findNode("extTint")->tier == "extension");
        CHECK(findNode("extTint")->owner == "texExtension");
        ctx.unloadAll();
        CHECK_FALSE(findNode("extTint"));
    }
}

TEST_CASE("tex: ports follow the graph for group nodes and the set's channels for the output")
{
    Graph inner = Graph::makeGroup("Inner");
    inner.inputs.push_back({"src", false, PortType::Gray, ParamType::Float, 0.5});
    GraphInput sc;
    sc.name = "scale";
    sc.param = true;
    sc.paramType = ParamType::Int;
    sc.defaultValue = 3;
    inner.inputs.push_back(sc);
    Graph g = Graph::makeMaterial("Set");
    g.subgroups["Inner"] = inner.toJson();
    GNode& grp = add(g, "G", kGroupType, {{"group", "Inner"}});
    NodePorts p = portsOf(g, grp);
    CHECK(p.error.empty());
    REQUIRE(p.inputs.size() == 1);
    CHECK(p.inputs[0].id == "src");
    REQUIRE(p.params.size() == 1);
    CHECK(p.params[0].id == "scale");
    CHECK(p.outputs.size() == 1);

    ChannelList extra = {{"wetness", PortType::Gray}};
    NodePorts out = portsOf(g, *g.node("Output"), &extra);
    CHECK(out.input("baseColor"));
    CHECK(out.input("wetness"));
    CHECK(out.input("material")->type == PortType::Material);
}

TEST_CASE("tex: texture set settings round trip and reject bad values")
{
    SetSettings s = SetSettings::makeDefault();
    s.tiles = {{1001, 0, 0}, {1002, 512, 512}};
    s.channels.push_back({"wetness", PortType::Gray, 16});
    s.normalConvention = "directx";
    s.bakeProject = "Bake1";
    SetSettings back = SetSettings::fromJson(s.toJson());
    CHECK(back.toJson() == s.toJson());
    CHECK(back.tileSize(back.tiles[1]) == std::pair{512, 512});
    CHECK(back.tileSize(back.tiles[0]) == std::pair{2048, 2048});
    CHECK(extraChannels(back) == ChannelList{{"wetness", PortType::Gray}});
    CHECK_THROWS_AS(SetSettings::fromJson({{"normalConvention", "sideways"}}), std::invalid_argument);
    CHECK_THROWS_AS(SetSettings::fromJson({{"channels", json::array({json{{"name", "a"}, {"bits", 12}}})}}), std::invalid_argument);
    CHECK_THROWS_AS(SetSettings::fromJson({{"tiles", json::array({999})}}), std::invalid_argument);
    CHECK(udimOrigin(1001) == std::pair{0, 0});
    CHECK(udimOrigin(1012) == std::pair{1, 1});
}
