#include <doctest/doctest.h>

#include "TestUtil.h"
#include "ops/Command.h"
#include "gn/Clipboard.h"
#include "gn/Gn.h"
#include "geo/Mesh.h"
#include "scene/Graph.h"
#include "scene/Node.h"

#include <string>

using namespace lila;
using namespace gntest;
using json = nlohmann::json;

namespace
{
/** Grid and Combine XYZ both feed Set Position, which feeds the output; Combine XYZ's y is exposed as "height". */
void buildTiedGrid(const char* name)
{
    run("gnCreateGroup", args({name}));
    run("gnAddNode", {{"args", {name, "MeshGrid"}}, {"position", {0, 0}}});
    run("gnAddNode", {{"args", {name, "SetPosition"}}, {"position", {300, 0}}});
    run("gnAddNode", {{"args", {name, "CombineXYZ"}}, {"position", {0, 200}}});
    run("gnConnect", args({name, "MeshGrid.mesh", "SetPosition.geometry"}));
    run("gnConnect", args({name, "CombineXYZ.vector", "SetPosition.offset"}));
    run("gnConnect", args({name, "SetPosition.geometry", "GroupOutput.geometry"}));
    run("gnExposeInput", args({name, "CombineXYZ.y", "height"}));
}

json tie(std::initializer_list<json> links, double x = 150, double y = 50, double angle = 0, const char* color = "")
{
    json t = {{"links", json(links)}, {"position", {x, y}}, {"angle", angle}};
    if (*color)
        t["color"] = color;
    return t;
}

const json kGrid = {"MeshGrid", "mesh", "SetPosition", "geometry"};
const json kXyz = {"CombineXYZ", "vector", "SetPosition", "offset"};
const json kOut = {"SetPosition", "geometry", "GroupOutput", "geometry"};
} // namespace

TEST_CASE("zip ties round trip through the group definition and undo")
{
    run("newScene");
    buildTiedGrid("T");
    CHECK(run("gnSetTies", args({"T", json::array({tie({kGrid, kXyz}, 150, 60, 30, "teal")})})) == 1);
    gn::Group g = group("T");
    REQUIRE(g.ties.size() == 1);
    CHECK(g.ties[0].links.size() == 2);
    CHECK(g.ties[0].links[0] == gn::GLink{"MeshGrid", "mesh", "SetPosition", "geometry"});
    CHECK(g.ties[0].position.x == doctest::Approx(150.0));
    CHECK(g.ties[0].angle == doctest::Approx(30.0));
    CHECK(g.ties[0].color == "teal");

    // The JSON text round trips, and the info the canvas reads carries it.
    gn::Group back = gn::Group::fromJson(g.toJson());
    CHECK(back.ties == g.ties);
    json info = run("gnGroupInfo", args({"T"}));
    CHECK(info["definition"]["ties"].size() == 1);

    // Links the group lacks, repeats and one-link ties are dropped.
    json bogus = {"Nope", "out", "SetPosition", "offset"};
    CHECK(run("gnSetTies", args({"T", json::array({tie({kGrid, kXyz, kGrid, bogus}), tie({kOut})})})) == 1);
    CHECK(group("T").ties[0].links.size() == 2);

    run("undo");
    CHECK(group("T").ties[0].color == "teal");
    run("undo");
    CHECK(group("T").ties.empty());
    run("redo");
    CHECK(group("T").ties.size() == 1);
}

TEST_CASE("removing or reconnecting a tied link drops it, and a tie under two links dissolves")
{
    run("newScene");
    buildTiedGrid("T");
    run("gnSetTies", args({"T", json::array({tie({kGrid, kXyz, kOut}), tie({kGrid, kXyz}, 250)})}));
    REQUIRE(group("T").ties.size() == 2);

    // Reconnecting the output input replaces its link, so the old one leaves the first tie.
    run("gnConnect", args({"T", "MeshGrid.mesh", "GroupOutput.geometry"}));
    gn::Group g = group("T");
    REQUIRE(g.ties.size() == 2);
    CHECK(g.ties[0].links.size() == 2);

    // Disconnecting one of the two remaining dissolves both ties that held it.
    run("gnDisconnect", args({"T", "SetPosition.offset"}));
    CHECK(group("T").ties.empty());
    run("undo");
    CHECK(group("T").ties.size() == 2);

    // Deleting a node takes its links out of every tie.
    run("gnDeleteNode", args({"T", "MeshGrid"}));
    CHECK(group("T").ties.empty());
}

TEST_CASE("renaming an exposed input keeps the tie on its link")
{
    run("newScene");
    buildTiedGrid("T");
    json fromInput = {"GroupInput", "height", "CombineXYZ", "y"};
    run("gnSetTies", args({"T", json::array({tie({fromInput, kXyz})})}));
    REQUIRE(group("T").ties.size() == 1);
    run("gnRenameInput", args({"T", "height", "lift"}));
    gn::Group g = group("T");
    REQUIRE(g.ties.size() == 1);
    CHECK(g.ties[0].links[0] == gn::GLink{"GroupInput", "lift", "CombineXYZ", "y"});
}

TEST_CASE("ties copy, paste and duplicate with their nodes")
{
    run("newScene");
    buildTiedGrid("T");
    run("gnSetTies", args({"T", json::array({tie({kGrid, kXyz}, 150, 50), tie({kXyz, kOut}, 400, 50)})}));

    // Only the tie whose links both have their ends among the copied nodes travels.
    std::string text = run("gnCopyNodes", args({"T", "MeshGrid", "CombineXYZ", "SetPosition"})).get<std::string>();
    json clip = json::parse(text);
    REQUIRE(clip["ties"].size() == 1);
    CHECK(clip["ties"][0]["links"].size() == 2);

    json names = run("gnPasteNodes", {{"args", {"T", text}}, {"position", {1000, 1000}}});
    REQUIRE(names.size() == 3);
    gn::Group g = group("T");
    REQUIRE(g.ties.size() == 3);
    const gn::GTie& pasted = g.ties.back();
    CHECK(pasted.links[0] == gn::GLink{"MeshGrid1", "mesh", "SetPosition1", "geometry"});
    CHECK(pasted.links[1] == gn::GLink{"CombineXYZ1", "vector", "SetPosition1", "offset"});
    // The tie sits where it did relative to the pasted nodes.
    glm::dvec2 shift = g.node("MeshGrid1")->position - g.node("MeshGrid")->position;
    CHECK(pasted.position.x == doctest::Approx(150.0 + shift.x));
    CHECK(pasted.position.y == doctest::Approx(50.0 + shift.y));
    run("undo");
    CHECK(group("T").ties.size() == 2);

    // Copying one end of a tie's links leaves the tie behind.
    json lone = json::parse(run("gnCopyNodes", args({"T", "MeshGrid"})).get<std::string>());
    CHECK_FALSE(lone.contains("ties"));

    run("gnDuplicateNodes", {{"args", {"T", "MeshGrid", "CombineXYZ", "SetPosition"}}, {"offset", {40, 40}}});
    g = group("T");
    REQUIRE(g.ties.size() == 3);
    CHECK(g.ties.back().position.x == doctest::Approx(190.0));
}

TEST_CASE("frames store, lose deleted members, copy, duplicate and undo")
{
    run("newScene");
    buildTiedGrid("T");
    json frames = json::array({{{"label", "Shape"}, {"position", {-20, -40}}, {"size", {400, 300}}, {"color", "blue"},
                                {"nodes", {"MeshGrid", "SetPosition", "Nope"}}},
                               {{"label", "Empty"}, {"position", {800, 0}}, {"size", {200, 100}}, {"collapsed", true}, {"nodes", json::array()}}});
    CHECK(run("gnSetFrames", args({"T", frames})) == 2);
    gn::Group g = group("T");
    REQUIRE(g.frames.size() == 2);
    CHECK(g.frames[0].label == "Shape");
    CHECK(g.frames[0].nodes == std::vector<std::string>{"MeshGrid", "SetPosition"});
    CHECK(g.frames[0].color == "blue");
    CHECK(g.frames[1].collapsed);
    CHECK(gn::Group::fromJson(g.toJson()).frames == g.frames);

    // Duplicates join their originals' frame; copies carry the frame along with the members copied.
    run("gnDuplicateNodes", {{"args", {"T", "MeshGrid"}}, {"offset", {40, 40}}});
    CHECK(group("T").frames[0].nodes.size() == 3);
    run("undo");
    json clip = json::parse(run("gnCopyNodes", args({"T", "MeshGrid", "CombineXYZ"})).get<std::string>());
    REQUIRE(clip["frames"].size() == 1);
    CHECK(clip["frames"][0]["nodes"] == json::array({"MeshGrid"}));
    run("gnPasteNodes", {{"args", {"T", clip.dump()}}, {"position", {0, 1000}}});
    g = group("T");
    REQUIRE(g.frames.size() == 3);
    CHECK(g.frames[2].nodes == std::vector<std::string>{"MeshGrid1"});
    run("undo");

    // Deleting a member leaves the frame with the rest; an empty frame stays as a comment.
    run("gnDeleteNode", args({"T", "MeshGrid", "SetPosition"}));
    g = group("T");
    REQUIRE(g.frames.size() == 2);
    CHECK(g.frames[0].nodes.empty());
    run("undo");
    CHECK(group("T").frames[0].nodes.size() == 2);
}

TEST_CASE("collapsed and hide-unused flags store, copy and undo")
{
    run("newScene");
    buildTiedGrid("T");
    run("gnSetNodeFlags", {{"args", {"T", "MeshGrid", "CombineXYZ"}}, {"collapsed", true}});
    run("gnSetNodeFlags", {{"args", {"T", "CombineXYZ"}}, {"hideUnused", true}});
    gn::Group g = group("T");
    CHECK(g.node("MeshGrid")->collapsed);
    CHECK_FALSE(g.node("MeshGrid")->hideUnused);
    CHECK(g.node("CombineXYZ")->hideUnused);
    gn::Group back = gn::Group::fromJson(g.toJson());
    CHECK(back.node("CombineXYZ")->collapsed);
    CHECK(back.node("CombineXYZ")->hideUnused);
    json clip = json::parse(run("gnCopyNodes", args({"T", "CombineXYZ"})).get<std::string>());
    CHECK(clip["nodes"][0]["collapsed"] == true);
    run("gnPasteNodes", {{"args", {"T", clip.dump()}}, {"position", {0, 900}}});
    CHECK(group("T").node("CombineXYZ1")->hideUnused);
    run("undo");
    run("undo");
    CHECK_FALSE(group("T").node("CombineXYZ")->hideUnused);
    CHECK_THROWS(run("gnSetNodeFlags", {{"args", {"T", "GroupOutput"}}, {"muted", true}}));
}

TEST_CASE("node labels show on the header, keep the name, store, copy and undo")
{
    run("newScene");
    buildTiedGrid("T");
    CHECK(run("gnSetNodeLabel", args({"T", "CombineXYZ", "  Height\nOffset "})) == "Height Offset");
    gn::Group g = group("T");
    CHECK(g.node("CombineXYZ")->label == "Height Offset");
    CHECK(gn::Group::fromJson(g.toJson()).node("CombineXYZ")->label == "Height Offset");
    // The name is untouched, so links, exposed inputs and scripts still find the node by it.
    CHECK(g.linksInto("SetPosition", "offset").size() == 1);
    json info = run("gnGroupInfo", args({"T"}));
    for (const json& n : info["nodes"])
        if (n["name"] == "CombineXYZ")
            CHECK(n["title"] == "Height Offset");

    json clip = json::parse(run("gnCopyNodes", args({"T", "CombineXYZ"})).get<std::string>());
    run("gnPasteNodes", {{"args", {"T", clip.dump()}}, {"position", {0, 900}}});
    CHECK(group("T").node("CombineXYZ1")->label == "Height Offset");
    run("undo");

    run("gnSetNodeLabel", args({"T", "CombineXYZ", ""}));
    CHECK(group("T").node("CombineXYZ")->label.empty());
    run("undo");
    run("undo");
    CHECK(group("T").node("CombineXYZ")->label.empty());
    CHECK_THROWS(run("gnSetNodeLabel", args({"T", "Nope", "X"})));
}

TEST_CASE("a frame's text makes it a note that stores, copies and undoes")
{
    run("newScene");
    buildTiedGrid("T");
    json frames = json::array({{{"label", "Note"}, {"position", {0, -200}}, {"size", {240, 100}}, {"text", "Tweak height here.\nSecond line."},
                                {"nodes", {"CombineXYZ"}}},
                               {{"label", "Plain"}, {"position", {600, 0}}, {"size", {200, 100}}, {"nodes", json::array()}}});
    run("gnSetFrames", args({"T", frames}));
    gn::Group g = group("T");
    CHECK(g.frames[0].text == "Tweak height here.\nSecond line.");
    CHECK(g.frames[1].text.empty());
    CHECK_FALSE(gn::framesToJson(g.frames)[1].contains("text"));
    CHECK(gn::Group::fromJson(g.toJson()).frames == g.frames);
    json clip = json::parse(run("gnCopyNodes", args({"T", "CombineXYZ"})).get<std::string>());
    CHECK(clip["frames"][0]["text"] == "Tweak height here.\nSecond line.");
    run("undo");
    CHECK(group("T").frames.empty());
}

TEST_CASE("bookmarks store with the group and undo")
{
    run("newScene");
    buildTiedGrid("T");
    CHECK(run("gnSetBookmarks", args({"T", {{"1", {10, 20, 1.5}}, {"2", "bad"}, {"3", {0, 0, -1}}}})) == 1);
    gn::Group g = group("T");
    CHECK(g.bookmarks == json{{"1", {10, 20, 1.5}}});
    CHECK(gn::Group::fromJson(g.toJson()).bookmarks == g.bookmarks);
    run("undo");
    CHECK(group("T").bookmarks.empty());
}

TEST_CASE("ties never change what the group computes")
{
    run("newScene");
    buildTiedGrid("T");
    run("gnSetValue", args({"T", "MeshGrid.verticesX", 5}));
    std::string mesh = run("polyPlane", {{"name", "P"}})[0].get<std::string>();
    run("gnApply", args({mesh, "T"}));
    scene::Graph& g = scene::Graph::current();
    auto points = [&] {
        g.evaluate();
        scene::Node* n = g.find(mesh);
        return n->as<scene::MeshPtr>(n->type().attrIndex("outMesh"))->numPoints();
    };
    size_t before = points();
    CHECK(before > 0);
    run("gnSetTies", args({"T", json::array({tie({kGrid, kXyz, kOut}, 100, 100, 45, "red")})}));
    CHECK(points() == before);
}
