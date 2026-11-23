#include <doctest/doctest.h>

#include "TestUtil.h"

#include "io/SceneFile.h"

#include "scene/Graph.h"
#include "scene/SceneGraph.h"
#include "ui/GraphBackend.h"

#include <QApplication>
#include <QClipboard>
#include <QGraphicsItem>
#include <QGraphicsView>
#include <QMainWindow>

#include <algorithm>
#include <filesystem>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
/** Locators A, B and C with A.translateX driving B's and B's driving C's, B selected. */
void chainScene()
{
    run("newScene");
    for (const char* n : {"A", "B", "C"})
        run("spaceLocator", {{"name", n}});
    run("connectAttr", {{"args", {"A.translateX", "B.translateX"}}});
    run("connectAttr", {{"args", {"B.translateX", "C.translateX"}}});
    run("select", {{"args", {"B"}}});
}

std::vector<std::string> shownNames(const json& info)
{
    std::vector<std::string> out;
    for (const json& n : info["nodes"])
        out.push_back(n["name"].get<std::string>());
    std::sort(out.begin(), out.end());
    return out;
}

bool hasLink(const json& info, const json& link)
{
    for (const json& l : info["definition"]["links"])
        if (l == link)
            return true;
    return false;
}

json info(const char* view) { return run("sceneGraphInfo", {{"args", {view}}}); }

bool driven(const char* plug)
{
    scene::Graph& g = scene::Graph::current();
    return g.source(g.plug(plug)).has_value();
}

ui::NodeEditor* openScene(QMainWindow& win)
{
    auto* editor = new ui::NodeEditor(&win, ui::kSceneGraphBackend);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup("Selection");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    return editor;
}

void selectAllNodes(QGraphicsView* view)
{
    view->scene()->clearSelection();
    for (QGraphicsItem* i : view->scene()->items())
        if (i->flags() & QGraphicsItem::ItemIsSelectable)
            i->setSelected(true);
}
} // namespace

TEST_CASE("the scene backend copies, pastes and duplicates scene nodes with the connections among them")
{
    registerTexGraphUi();
    chainScene();
    run("setAttr", {{"args", {"B.translateY", 7.0}}});
    scene::Graph& g = scene::Graph::current();

    // Copied text stands alone: with the originals gone, paste rebuilds the nodes, their values and their link.
    const std::string text = run("sceneGraphCopyNodes", {{"args", {"Selection", "A", "B"}}}).get<std::string>();
    CHECK(text.find(R"("format": "lila.sceneNodes")") != std::string::npos);
    run("sceneGraphDeleteNode", {{"args", {"Selection", "A", "B"}}});
    REQUIRE_FALSE(g.find("B"));
    json pasted = run("sceneGraphPasteNodes", {{"args", {"Selection", text}}, {"position", {300, 0}}});
    REQUIRE(pasted.size() == 2);
    CHECK(g.find("A"));
    REQUIRE(g.find("B"));
    CHECK(driven("B.translateX"));
    CHECK(run("getAttr", {{"args", {"B.translateY"}}}).get<double>() == doctest::Approx(7.0));
    const size_t before = g.nodes().size();
    run("undo");
    CHECK(g.nodes().size() == before - 2);
    run("undo");
    CHECK(g.find("B"));

    // Bad text changes nothing and names the problem.
    CHECK_THROWS_WITH_AS(run("sceneGraphPasteNodes", {{"args", {"Selection", R"({"format": "lila.sceneNodes", "version": 1, "nodes": [{"name": "X", "type": "noSuchType"}]})"}}}),
                         doctest::Contains("unknown type 'noSuchType'"), std::exception);
    CHECK(g.nodes().size() == before);

    // Through the editor: copy and paste the whole view, then duplicate, each one undo step.
    QMainWindow win;
    ui::NodeEditor* editor = openScene(win);
    auto* view = editor->findChild<QGraphicsView*>();
    selectAllNodes(view);
    ui::actions::run(json("nodeEditor.copy"));
    CHECK(QApplication::clipboard()->text().contains("lila.sceneNodes"));
    ui::actions::run(json("nodeEditor.paste"));
    pump();
    CHECK(g.nodes().size() == before + 3);
    CHECK(driven("B1.translateX"));
    CHECK(driven("C1.translateX"));
    run("undo");
    pump();
    CHECK(g.nodes().size() == before);

    json dup = run("sceneGraphDuplicateNodes", {{"args", {"Selection", "B"}}, {"offset", {40, 40}}});
    REQUIRE(dup.size() == 1);
    CHECK(g.nodes().size() == before + 1);
    json shown = info("Selection");
    double bx = 0, copyX = 0;
    for (const json& n : shown["nodes"])
    {
        if (n["name"] == "B")
            bx = n["position"][0].get<double>();
        if (n["name"] == dup[0])
            copyX = n["position"][0].get<double>();
    }
    CHECK(copyX == doctest::Approx(bx + 40.0));
    run("undo");
    CHECK(g.nodes().size() == before);
}

TEST_CASE("the scene backend keeps zip ties and bookmarks per view")
{
    registerTexGraphUi();
    chainScene();
    run("connectAttr", {{"args", {"A.translateY", "B.translateY"}}});
    const json ties = json::array({{{"links", {{"A", "translateX", "B", "translateX"}, {"A", "translateY", "B", "translateY"}}}, {"position", {10, 20}}}});
    CHECK(run("sceneGraphSetTies", {{"args", {"Selection", ties}}}) == 1);
    json shown = info("Selection");
    REQUIRE(shown["definition"]["ties"].size() == 1);
    CHECK(shown["definition"]["ties"][0]["links"].size() == 2);
    CHECK(info("Upstream")["definition"]["ties"].empty());

    // Ties follow a rename; a tie left with one link isn't shown.
    run("sceneGraphRenameNode", {{"args", {"Selection", "A", "Driver"}}});
    shown = info("Selection");
    REQUIRE(shown["definition"]["ties"].size() == 1);
    CHECK(shown["definition"]["ties"][0]["links"][0][0] == "Driver");
    run("sceneGraphDisconnect", {{"args", {"Selection", "B.translateY"}}});
    CHECK(info("Selection")["definition"]["ties"].empty());
    run("undo");
    CHECK(info("Selection")["definition"]["ties"].size() == 1);

    CHECK(run("sceneGraphSetBookmarks", {{"args", {"Selection", {{"1", {10, 20, 1.5}}, {"2", "bad"}}}}}) == 1);
    shown = info("Selection");
    CHECK(shown["definition"]["bookmarks"]["1"] == json::array({10, 20, 1.5}));
    CHECK_FALSE(shown["definition"]["bookmarks"].contains("2"));
    CHECK(info("Hierarchy")["definition"]["bookmarks"].empty());
    run("undo");
    CHECK(info("Selection")["definition"]["bookmarks"].empty());

    QMainWindow win;
    ui::NodeEditor* editor = openScene(win);
    CHECK(editor->backend().supports("SetTies"));
    CHECK(editor->backend().supports("SetBookmarks"));
    CHECK(editor->shownTies().size() == 1);
}

TEST_CASE("the scene backend groups nodes into one node with boundary sockets, opens it, and ungroups")
{
    registerTexGraphUi();
    if (!ops::find("saveScene"))
        io::registerCommands();
    chainScene();
    scene::Graph& g = scene::Graph::current();

    json made = run("sceneGraphGroupNodes", {{"args", {"Selection", "B"}}, {"name", "Mid"}});
    CHECK(made == json({{"group", "Mid"}, {"node", "@Mid"}}));
    CHECK(scene::sceneGraph::hasGroup("Mid"));
    json shown = info("Selection");
    CHECK(shownNames(shown) == std::vector<std::string>{"@Mid", "A", "C"});
    CHECK(hasLink(shown, {"A", "translateX", "@Mid", "B.translateX"}));
    CHECK(hasLink(shown, {"@Mid", "B.translateX", "C", "translateX"}));

    // The group node's sockets are B's plugs.
    run("sceneGraphDisconnect", {{"args", {"Selection", "@Mid.B.translateX"}}});
    CHECK_FALSE(driven("B.translateX"));
    run("undo");
    CHECK(driven("B.translateX"));
    run("sceneGraphConnect", {{"args", {"Selection", "A.translateY", "@Mid.B.translateY"}}});
    CHECK(driven("B.translateY"));
    CHECK(hasLink(info("Selection"), {"A", "translateY", "@Mid", "B.translateY"}));
    run("undo");

    // Inside, the members with what is wired to them.
    CHECK(shownNames(info("Mid")) == std::vector<std::string>{"A", "B", "C"});

    // A group nested inside Mid shows inside Mid as its own node, and Mid still stands for both outside.
    run("sceneGraphGroupNodes", {{"args", {"Mid", "B"}}, {"name", "Inner"}});
    CHECK(scene::sceneGraph::parentGroup("Inner") == "Mid");
    CHECK(shownNames(info("Mid")) == std::vector<std::string>{"@Inner", "A", "C"});
    CHECK(shownNames(info("Selection")) == std::vector<std::string>{"@Mid", "A", "C"});
    run("undo");
    CHECK_FALSE(scene::sceneGraph::hasGroup("Inner"));

    // Group names survive save and load.
    const std::string path = (std::filesystem::temp_directory_path() / "lila_scene_graph_groups.lilaa").generic_string();
    run("saveScene", {{"args", {path}}});
    run("newScene");
    CHECK_FALSE(scene::sceneGraph::hasGroup("Mid"));
    run("openScene", {{"args", {path}}, {"force", true}});
    REQUIRE(scene::sceneGraph::hasGroup("Mid"));
    run("select", {{"args", {"B"}}});
    CHECK(shownNames(info("Selection")) == std::vector<std::string>{"@Mid", "A", "C"});

    // In the editor: open the group node, come back out, ungroup it.
    QMainWindow win;
    ui::NodeEditor* editor = openScene(win);
    editor->focusNode("@Mid");
    ui::actions::run(json("nodeEditor.openGroup"));
    pump();
    CHECK(editor->group() == "Mid");
    editor->openParentGroup();
    pump();
    CHECK(editor->group() == "Selection");
    editor->setGroup("Mid");
    pump();
    CHECK(editor->parentGroup().isEmpty() == false);

    editor->setGroup("Selection");
    pump();
    editor->focusNode("@Mid");
    ui::actions::run(json("nodeEditor.ungroup"));
    pump();
    CHECK_FALSE(scene::sceneGraph::hasGroup("Mid"));
    CHECK(shownNames(info("Selection")) == std::vector<std::string>{"A", "B", "C"});
    CHECK(g.find("B"));
    run("undo");
    CHECK(scene::sceneGraph::hasGroup("Mid"));

    // Copying the group node brings its members along under a new group.
    const size_t before = g.nodes().size();
    json dup = run("sceneGraphDuplicateNodes", {{"args", {"Selection", "@Mid"}}});
    CHECK(dup == json::array({"@Mid1"}));
    CHECK(scene::sceneGraph::hasGroup("Mid1"));
    CHECK(g.nodes().size() == before + 1);
    run("undo");
    CHECK_FALSE(scene::sceneGraph::hasGroup("Mid1"));

    // Through the editor's Group key path.
    editor->focusNode("C");
    ui::actions::run(json("nodeEditor.groupNodes"));
    pump();
    CHECK(scene::sceneGraph::hasGroup("NodeGroup"));
    std::filesystem::remove(path);
}
