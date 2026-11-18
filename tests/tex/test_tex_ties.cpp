#include "TexTest.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "tex/TextureSet.h"

using namespace textest;

namespace
{
tex::Graph graph(const char* set) { return tex::readGraph(*tex::findSet(scene::Graph::current(), set)); }

void build()
{
    run("newScene");
    run("texCreateSet", {{"args", {"Rock"}}, {"resolution", 32}});
    run("texAddNode", {{"args", {"Rock", "bricks"}}, {"name", "Bricks"}});
    run("texAddNode", {{"args", {"Rock", "colorize"}}, {"name", "Tint"}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Tint.input"}}});
    run("texConnect", {{"args", {"Rock", "Tint.out", "Output.baseColor"}}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Output.height"}}});
}

const json kTint = {"Bricks", "mask", "Tint", "input"};
const json kHeight = {"Bricks", "mask", "Output", "height"};
} // namespace

TEST_CASE("tex: frames store, copy and undo")
{
    build();
    json frames = json::array({{{"label", "Base"}, {"position", {0, 0}}, {"size", {300, 200}}, {"nodes", {"Bricks", "Tint"}}}});
    CHECK(run("texSetFrames", args({"Rock", frames})) == 1);
    CHECK(graph("Rock").frames[0].nodes.size() == 2);
    CHECK(tex::Graph::fromJson(graph("Rock").toJson()).frames == graph("Rock").frames);
    json clip = json::parse(run("texCopyNodes", args({"Rock", "Tint"})).get<std::string>());
    REQUIRE(clip["frames"].size() == 1);
    run("texPasteNodes", {{"args", {"Rock", clip.dump()}}, {"position", {400, 400}}});
    CHECK(graph("Rock").frames.size() == 2);
    run("texDeleteNode", args({"Rock", "Bricks"}));
    CHECK(graph("Rock").frames[0].nodes == std::vector<std::string>{"Tint"});
    run("undo");
    run("undo");
    run("undo");
    CHECK(graph("Rock").frames.empty());
}

TEST_CASE("tex: node labels and note text store, copy and undo")
{
    build();
    CHECK(run("texSetNodeLabel", args({"Rock", "Tint", "Mortar Tint"})) == "Mortar Tint");
    CHECK(graph("Rock").node("Tint")->label == "Mortar Tint");
    CHECK(tex::Graph::fromJson(graph("Rock").toJson()).node("Tint")->label == "Mortar Tint");
    CHECK(graph("Rock").linksInto("Output", "baseColor").front().fromNode == "Tint");
    json info = run("texGraphInfo", args({"Rock"}));
    for (const json& n : info["nodes"])
        if (n["name"] == "Tint")
            CHECK(n["title"] == "Mortar Tint");
    json clip = json::parse(run("texCopyNodes", args({"Rock", "Tint"})).get<std::string>());
    run("texPasteNodes", {{"args", {"Rock", clip.dump()}}, {"position", {400, 400}}});
    CHECK(graph("Rock").node("Tint1")->label == "Mortar Tint");
    run("undo");

    json frames = json::array({{{"label", "Note"}, {"position", {0, 0}}, {"size", {240, 100}}, {"text", "Mortar colour"}, {"nodes", json::array()}}});
    run("texSetFrames", args({"Rock", frames}));
    CHECK(graph("Rock").frames[0].text == "Mortar colour");
    CHECK(tex::Graph::fromJson(graph("Rock").toJson()).frames == graph("Rock").frames);
    run("undo");
    run("undo");
    CHECK(graph("Rock").node("Tint")->label.empty());
    CHECK(graph("Rock").frames.empty());
}

TEST_CASE("tex: zip ties store, prune, copy and undo like graph edits")
{
    build();
    json ties = json::array({{{"links", {kTint, kHeight}}, {"position", {100, 40}}, {"angle", 15}, {"color", "blue"}}});
    CHECK(run("texSetTies", args({"Rock", ties})) == 1);
    tex::Graph g = graph("Rock");
    REQUIRE(g.ties.size() == 1);
    CHECK(g.ties[0].color == "blue");
    CHECK(tex::Graph::fromJson(g.toJson()).ties == g.ties);
    CHECK(run("texGraphInfo", args({"Rock"}))["definition"]["ties"].size() == 1);

    std::string text = run("texCopyNodes", args({"Rock", "Bricks", "Tint", "Output"})).get<std::string>();
    json clip = json::parse(text);
    // The material output stays behind, so only the link into Tint would travel: not enough for a tie.
    CHECK_FALSE(clip.contains("ties"));

    run("texAddNode", {{"args", {"Rock", "colorize"}}, {"name", "Tint2"}});
    run("texConnect", {{"args", {"Rock", "Bricks.mask", "Tint2.input"}}});
    json kTint2 = {"Bricks", "mask", "Tint2", "input"};
    run("texSetTies", args({"Rock", json::array({{{"links", {kTint, kTint2}}, {"position", {100, 40}}, {"angle", 0}}})}));
    clip = json::parse(run("texCopyNodes", args({"Rock", "Bricks", "Tint", "Tint2"})).get<std::string>());
    REQUIRE(clip["ties"].size() == 1);
    json names = run("texPasteNodes", {{"args", {"Rock", clip.dump()}}, {"position", {500, 500}}});
    REQUIRE(names.size() == 3);
    g = graph("Rock");
    REQUIRE(g.ties.size() == 2);
    CHECK(g.ties[1].links[0].fromNode == names[0].get<std::string>());

    // A removed link leaves its ties; the tie it leaves with one link goes.
    run("texDisconnect", args({"Rock", "Tint2.input"}));
    CHECK(graph("Rock").ties.size() == 1);
    run("undo");
    CHECK(graph("Rock").ties.size() == 2);
    run("texDeleteNode", args({"Rock", "Bricks"}));
    CHECK(graph("Rock").ties.size() == 1);
}
