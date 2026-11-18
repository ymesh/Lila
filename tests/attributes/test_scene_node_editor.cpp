#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "ui/NodeEditor.h"

#include <QApplication>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QMouseEvent>

using namespace lila;

namespace sceneNodeEditorTests
{
json sneCmd(const char* name, json call = json::object()) { return ops::execute(name, call); }

void snePump()
{
    for (int i = 0; i < 4; ++i)
    {
        mainq::drain();
        QApplication::processEvents();
    }
}

/** Nodes on the canvas: the movable, selectable items that aren't wires. */
int canvasNodes(QGraphicsView* view)
{
    int n = 0;
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsMovable) && (i->flags() & QGraphicsItem::ItemIsSelectable) && !dynamic_cast<QGraphicsPathItem*>(i))
            ++n;
    return n;
}

/** Wires: path items behind the nodes. */
int canvasWires(QGraphicsView* view)
{
    int n = 0;
    for (QGraphicsItem* i : view->scene()->items())
        if (auto* p = dynamic_cast<QGraphicsPathItem*>(i); p && p->zValue() < -0.5 && p->isVisible())
            ++n;
    return n;
}
} // namespace sceneNodeEditorTests
using namespace sceneNodeEditorTests;

TEST_CASE("Scene node editor: the selection's nodes and wires, rebuilt as connections and the selection change")
{
    sneCmd("newScene");
    for (const char* n : {"A", "B", "C"})
        sneCmd("spaceLocator", {{"name", n}});
    sneCmd("connectAttr", {{"args", {"A.translateX", "B.translateY"}}});
    sneCmd("connectAttr", {{"args", {"B.translate", "C.rotate"}}});
    sneCmd("select", {{"args", {"B"}}});

    QWidget host;
    auto* editor = new ui::NodeEditor(&host, ui::kSceneGraphBackend);
    host.resize(1000, 700);
    host.show();
    snePump();
    REQUIRE(editor->backend().id() == ui::kSceneGraphBackend);
    CHECK(editor->group() == "Selection");
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    CHECK(canvasNodes(view) == 3);
    CHECK(canvasWires(view) == 2);

    sneCmd("disconnectAttr", {{"args", {"B.translateY"}}});
    snePump();
    CHECK(canvasNodes(view) == 2);
    CHECK(canvasWires(view) == 1);
    sneCmd("undo");
    snePump();
    CHECK(canvasNodes(view) == 3);

    sneCmd("select", {{"args", {"C"}}});
    snePump();
    CHECK(canvasNodes(view) == 2);
    CHECK(canvasWires(view) == 1);

    editor->setGroup("Upstream");
    snePump();
    CHECK(canvasNodes(view) == 3);
    CHECK(canvasWires(view) == 2);
}

TEST_CASE("Scene node editor: a modifier shows its sockets, goes into the selected mesh's stack, and edits on the node")
{
    sneCmd("newScene");
    const std::string cube = sneCmd("polyCube", {{"name", "Box"}})[0];
    sneCmd("select", {{"args", {cube}}});
    QWidget host;
    auto* editor = new ui::NodeEditor(&host, ui::kSceneGraphBackend);
    host.resize(1000, 700);
    host.show();
    snePump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    auto stack = [&] { return sneCmd("modifiers", {{"args", {cube}}}); };

    // Added with the mesh selected, a modifier goes on top of its stack, as the Modifiers panel puts it: one undo step.
    const std::string onTop = sneCmd("sceneGraphAddNode", {{"args", {"Selection", "subdivisionModifier"}}, {"position", {400, 0}}});
    CHECK(stack() == json::array({onTop}));
    sneCmd("undo");
    CHECK(stack().empty());
    CHECK_FALSE(scene::Graph::current().find(onTop));

    // insert=False leaves it unlinked, with its mesh sockets and parameters showing.
    const std::string loose = sneCmd("sceneGraphAddNode", {{"args", {"Selection", "subdivisionModifier"}}, {"position", {600, 0}}, {"insert", false}});
    snePump();
    CHECK(stack().empty());
    json info = sneCmd("sceneGraphInfo", {{"args", {"Selection"}}});
    const json* mod = nullptr;
    std::string from;
    for (const json& n : info["nodes"])
        if (n["name"] == loose)
            mod = &n;
    for (const json& l : info["definition"]["links"])
        if (l[2] == cube && l[3] == "inMesh")
            from = l[0].get<std::string>() + "." + l[1].get<std::string>();
    REQUIRE(mod);
    REQUIRE(!from.empty());
    CHECK((*mod)["inputs"][0]["id"] == "inMesh");
    CHECK(std::any_of((*mod)["outputs"].begin(), (*mod)["outputs"].end(), [](const json& s) { return s["id"] == "outMesh" && s["type"] == "mesh"; }));
    QGraphicsItem* item = nullptr;
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && i->pos() == QPointF(600, 0))
            item = i;
    REQUIRE(item);
    const size_t rows = (*mod)["outputs"].size() + (*mod)["inputs"].size();
    CHECK(item->boundingRect().height() >= 24.0 + 20.0 * double(rows));

    // Clicking its Enabled check turns it off, undoably.
    int enabled = -1;
    for (size_t i = 0; i < (*mod)["inputs"].size(); ++i)
        if ((*mod)["inputs"][i]["id"] == "enabled")
            enabled = int(i);
    REQUIRE(enabled >= 0);
    const QPointF check(600 + 85, 24 + 20.0 * double((*mod)["outputs"].size() + size_t(enabled)) + 10);
    for (QEvent::Type t : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease})
    {
        QPoint p = view->mapFromScene(check);
        QMouseEvent e(t, QPointF(p), QPointF(view->viewport()->mapToGlobal(p)), Qt::LeftButton,
                      t == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(view->viewport(), &e);
        snePump();
    }
    CHECK(sneCmd("getAttr", {{"args", {loose + ".enabled"}}}) == false);
    sneCmd("undo");
    CHECK(sneCmd("getAttr", {{"args", {loose + ".enabled"}}}) == true);

    // Dropped on the mesh's history wire, it goes into the stack the same way.
    CHECK(sneCmd("sceneGraphInsertNode", {{"args", {"Selection", loose, from, cube + ".inMesh"}}}) == true);
    CHECK(stack() == json::array({loose}));
    sneCmd("undo");
    CHECK(stack().empty());
    // Anything else is left for the editor to wire.
    CHECK(sneCmd("sceneGraphInsertNode", {{"args", {"Selection", cube, from, cube + ".inMesh"}}}) == false);
}
