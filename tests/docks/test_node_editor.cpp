#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "gn/Clipboard.h"
#include "gn/Gn.h"
#include "gn/Routing.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/GraphEditor.h"
#include "ui/Hotkeys.h"
#include "ui/NodeEditor.h"
#include "ui/ThemeModel.h"
#include "core/PrefStore.h"

#include <QApplication>
#include <QClipboard>
#include <QCursor>
#include <QElapsedTimer>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMainWindow>
#include <QMouseEvent>

#include <cmath>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
/** A key press as a user types it: to the focus widget, through the app-wide hotkey filter. */
void press(QWidget* w, Qt::Key key, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QKeyEvent down(QEvent::KeyPress, key, mods);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods);
    QApplication::sendEvent(w, &up);
    pump();
}

size_t nodeCount(const char* group) { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), group)).nodes.size(); }

void selectNode(QGraphicsView* view, const std::string& name)
{
    view->scene()->clearSelection();
    // Items keep their node name internally, so match by position.
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    const gn::GNode* n = g.node(name);
    REQUIRE(n);
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), QPointF(n->position.x, n->position.y)).length() < 0.5)
            i->setSelected(true);
}
} // namespace

TEST_CASE("node editor keys act on the graph, not the scene selection")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    gn::registerTypes();
    gn::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerNodeEditorActions();
    ui::Hotkeys::get().install();

    run("newScene");
    run("spaceLocator", {{"name", "keepMe"}});
    run("select", {{"args", {"keepMe"}}});
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 300}}});

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win);
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    win.activateWindow();
    editor->setGroup("G");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    REQUIRE(QApplication::focusWidget() == view);
    REQUIRE(nodeCount("G") == 3);

    selectNode(view, "Value");
    press(view, Qt::Key_Delete);
    CHECK(nodeCount("G") == 2);
    CHECK(scene::Graph::current().find("keepMe")); // edit.delete didn't run
    run("undo");
    pump();
    CHECK(nodeCount("G") == 3);

    // Copy as text, paste it back as a new node.
    selectNode(view, "Value");
    press(view, Qt::Key_C, Qt::ControlModifier);
    QString text = QApplication::clipboard()->text();
    CHECK(text.contains(gn::kClipboardFormat));
    press(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(nodeCount("G") == 4);

    // Text that isn't node data changes nothing.
    QApplication::clipboard()->setText("just some words");
    press(view, Qt::Key_V, Qt::ControlModifier);
    CHECK(nodeCount("G") == 4);

    selectNode(view, "Value");
    press(view, Qt::Key_D, Qt::ControlModifier);
    CHECK(nodeCount("G") == 5);

    // Tab opens the add-node search rather than moving focus.
    press(view, Qt::Key_Tab);
    QWidget* popup = QApplication::activePopupWidget();
    CHECK(popup);
    if (popup)
        popup->close();
    pump();
}

namespace
{
/** Geometry nodes under another id, as an extension's backend would sit beside it. */
class AliasBackend final : public ui::GraphBackend
{
public:
    std::string id() const override { return "tstAlias"; }
    QString title() const override { return "Alias"; }
    std::string commandPrefix() const override { return "gn"; }
    std::vector<std::string> groupNames(scene::Graph& g) const override { return gn::groupNames(g); }
    bool isGroupNode(const scene::Node& n) const override { return n.type().name == gn::kGroupNodeType; }
};
} // namespace

TEST_CASE("a node editor opens on a registered backend and falls back when it goes")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 300}}});
    ui::graphBackends::add(std::make_shared<AliasBackend>(), "tstOwner");

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win, "tstAlias");
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    editor->setGroup("G");
    pump();
    CHECK(editor->backend().id() == "tstAlias");
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();
    selectNode(view, "Value");
    press(view, Qt::Key_Delete);
    CHECK(nodeCount("G") == 2);

    ui::graphBackends::remove("tstAlias");
    pump();
    CHECK(editor->backend().id() == ui::kGeometryNodesBackend);
    CHECK_FALSE(editor->setBackend("tstAlias"));
}

namespace
{
void key(QWidget* w, QEvent::Type type, Qt::Key k)
{
    QKeyEvent e(type, k, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
    pump();
}

void mouse(QGraphicsView* view, QEvent::Type type, QPointF scenePos, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    QPoint p = view->mapFromScene(scenePos);
    QMouseEvent e(type, QPointF(p), QPointF(view->viewport()->mapToGlobal(p)), type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton, buttons, mods);
    QApplication::sendEvent(view->viewport(), &e);
    pump();
}

void dragInView(QGraphicsView* view, QPointF from, QPointF to, Qt::KeyboardModifiers mods = Qt::NoModifier)
{
    mouse(view, QEvent::MouseButtonPress, from, Qt::LeftButton, mods);
    for (int i = 1; i <= 4; ++i)
        mouse(view, QEvent::MouseMove, from + (to - from) * (i / 4.0), Qt::LeftButton, mods);
    mouse(view, QEvent::MouseButtonRelease, to, Qt::NoButton, mods);
}
} // namespace

TEST_CASE("holding T and dragging across wires ties them, and the tie drags")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", "MeshGrid"}}, {"position", {0, 0}}});
    run("gnAddNode", {{"args", {"G", "CombineXYZ"}}, {"position", {0, 200}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {300, 60}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {650, 0}}, {"GroupInput", {-400, 0}}}}});
    run("gnConnect", {{"args", {"G", "MeshGrid.mesh", "SetPosition.geometry"}}});
    run("gnConnect", {{"args", {"G", "CombineXYZ.vector", "SetPosition.offset"}}});
    run("gnConnect", {{"args", {"G", "SetPosition.geometry", "GroupOutput.geometry"}}});

    QMainWindow win;
    auto* editor = new ui::NodeEditor(&win);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup("G");
    pump();
    auto* view = editor->findChild<QGraphicsView*>();
    REQUIRE(view);
    view->setFocus();
    pump();

    // Without the key, the same drag is a rubber band and ties nothing.
    dragInView(view, {235, -30}, {235, 330});
    CHECK(editor->shownTies().empty());

    key(view, QEvent::KeyPress, Qt::Key_T);
    dragInView(view, {235, -30}, {235, 330});
    key(view, QEvent::KeyRelease, Qt::Key_T);
    pump();
    REQUIRE(editor->shownTies().size() == 1);
    CHECK(editor->shownTies()[0].size() == 2);
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    REQUIRE(g.ties.size() == 1);
    CHECK(g.ties[0].position.x == doctest::Approx(235.0).epsilon(0.02));
    CHECK(g.ties[0].angle == doctest::Approx(0.0));

    // E turns the tie under the cursor in 15 degree steps; Esc puts it back, a click places it.
    QPointF at(g.ties[0].position.x, g.ties[0].position.y);
    const double px = 1.0 / view->transform().m11(); // scene units per screen pixel
    const QPointF down = at + QPointF(3, 100) * px;
    auto turn = [&] {
        mouse(view, QEvent::MouseMove, at, Qt::NoButton);
        press(view, Qt::Key_E);
        mouse(view, QEvent::MouseMove, at + QPointF(100, 0) * px, Qt::NoButton);
        mouse(view, QEvent::MouseMove, down, Qt::NoButton);
    };
    turn();
    CHECK(editor->shownTies().size() == 1);
    press(view, Qt::Key_Escape);
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).ties[0].angle == doctest::Approx(0.0));
    turn();
    mouse(view, QEvent::MouseButtonPress, down, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, down, Qt::NoButton);
    g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    CHECK(std::fmod(g.ties[0].angle, 15.0) == doctest::Approx(0.0));
    REQUIRE(g.ties[0].angle != doctest::Approx(0.0));
    run("undo");
    pump();
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).ties[0].angle == doctest::Approx(0.0));

    // E away from any tie waits: pressing a tie then turns it while dragged, and letting go places it.
    mouse(view, QEvent::MouseMove, at + QPointF(0, -400) * px, Qt::NoButton);
    press(view, Qt::Key_E);
    mouse(view, QEvent::MouseButtonPress, at, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, at + QPointF(100, 0) * px, Qt::LeftButton);
    mouse(view, QEvent::MouseMove, down, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, down, Qt::NoButton);
    REQUIRE(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).ties.size() == 1);
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).ties[0].angle != doctest::Approx(0.0));
    run("undo");
    pump();
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).ties[0].angle == doctest::Approx(0.0));

    // Dragging the band moves the tie, undoably.
    dragInView(view, at, at + QPointF(30, 20));
    g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    CHECK(g.ties[0].position.x == doctest::Approx(at.x() + 30).epsilon(0.02));
    run("undo");
    pump();
    g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    CHECK(g.ties[0].position.x == doctest::Approx(at.x()).epsilon(0.02));
    run("undo");
    pump();
    CHECK(editor->shownTies().empty());
}

namespace
{
/** The wires on the canvas: path items behind the nodes. */
std::vector<QGraphicsPathItem*> wires(QGraphicsView* view)
{
    std::vector<QGraphicsPathItem*> out;
    for (QGraphicsItem* i : view->scene()->items())
        if (auto* p = dynamic_cast<QGraphicsPathItem*>(i); p && p->zValue() < -0.5 && p->isVisible())
            out.push_back(p);
    return out;
}

ui::NodeEditor* openOn(QMainWindow& win, const char* group)
{
    auto* editor = new ui::NodeEditor(&win);
    win.setCentralWidget(editor);
    win.resize(1000, 700);
    win.show();
    win.activateWindow();
    editor->setGroup(group);
    pump();
    editor->findChild<QGraphicsView*>()->setFocus();
    pump();
    return editor;
}
} // namespace

TEST_CASE("double-clicking a wire adds a reroute, and selecting it lights its wires")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {400, 0}}, {"GroupInput", {-300, 0}}}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    std::vector<QGraphicsPathItem*> before = wires(view);
    REQUIRE(before.size() == 1);
    QPointF mid = before[0]->path().pointAtPercent(0.5);
    mouse(view, QEvent::MouseButtonPress, mid, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, mid, Qt::NoButton);
    mouse(view, QEvent::MouseButtonDblClick, mid, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, mid, Qt::NoButton);
    pump();
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    REQUIRE(g.nodes.size() == 3);
    const gn::GNode* r = g.firstOfType(gn::kRerouteType);
    REQUIRE(r);
    CHECK(QLineF(QPointF(r->position.x, r->position.y), mid).length() < 3.0);
    CHECK(g.links.size() == 2);
    std::vector<QGraphicsPathItem*> after = wires(view);
    REQUIRE(after.size() == 2);

    // Selecting the reroute draws both of its wires as selected.
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), mid).length() < 3.0)
            i->setSelected(true);
    pump();
    for (QGraphicsPathItem* w : wires(view))
        CHECK(w->pen().widthF() > 2.5);

    run("undo");
    pump();
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).nodes.size() == 2);
}

TEST_CASE("dropping an unlinked node on a wire wires it in and slides the rest aside")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {200, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {0, 300}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    dragInView(view, {85, 312}, {85 - 60, 12});
    pump();
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    auto has = [&](gn::GLink l) { return std::find(g.links.begin(), g.links.end(), l) != g.links.end(); };
    CHECK(has({"GroupInput", "geometry", "SetPosition", "geometry"}));
    CHECK(has({"SetPosition", "geometry", "GroupOutput", "geometry"}));
    CHECK(g.links.size() == 2);
    // The output sat under the dropped node, so it moved clear of it.
    CHECK(g.node("GroupOutput")->position.x >= g.node("SetPosition")->position.x + 170.0);

    // One undo takes back the move, the wiring and the slide.
    run("undo");
    g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    CHECK(g.links.size() == 1);
    CHECK(g.node("SetPosition")->position.y == doctest::Approx(300.0));
    CHECK(g.node("GroupOutput")->position.x == doctest::Approx(200.0));
}

TEST_CASE("a wire let go on empty canvas offers only nodes that fit, and wires the pick")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {500, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnDisconnect", {{"args", {"G", "GroupOutput.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    // From Group Input's geometry socket out into empty canvas.
    dragInView(view, {-130, 34}, {100, 150});
    QWidget* popup = QApplication::activePopupWidget();
    REQUIRE(popup);
    auto* search = popup->findChild<QLineEdit*>();
    REQUIRE(search);
    search->setText("Set Position");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(search, &enter);
    pump();
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    REQUIRE(g.node("SetPosition"));
    CHECK(std::find(g.links.begin(), g.links.end(), gn::GLink{"GroupInput", "geometry", "SetPosition", "geometry"}) != g.links.end());
}

TEST_CASE("jumping from a Receive selects its Send, and back to every reader")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnAddNode", {{"args", {"G", gn::kSendType}}, {"position", {0, 300}}, {"name", "S"}});
    run("gnAddNode", {{"args", {"G", gn::kReceiveType}}, {"position", {400, 300}}, {"name", "R1"}});
    run("gnAddNode", {{"args", {"G", gn::kReceiveType}}, {"position", {400, 500}}, {"name", "R2"}});
    for (const char* n : {"S", "R1", "R2"})
        run("gnSetValue", {{"args", {"G", std::string(n) + ".name", "shared"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto selected = [&] {
        std::vector<QPointF> out;
        for (QGraphicsItem* i : view->scene()->selectedItems())
            out.push_back(i->pos());
        return out;
    };
    selectNode(view, "R1");
    editor->jumpToPortalSource();
    REQUIRE(selected().size() == 1);
    CHECK(selected()[0] == QPointF(0, 300));
    editor->jumpToPortalReaders();
    CHECK(selected().size() == 2);
}

namespace
{
QGraphicsItem* itemAt(QGraphicsView* view, QPointF pos)
{
    for (QGraphicsItem* i : view->scene()->items())
        if ((i->flags() & QGraphicsItem::ItemIsSelectable) && QLineF(i->pos(), pos).length() < 0.5)
            return i;
    return nullptr;
}
} // namespace

TEST_CASE("the flow highlight dims what the selection doesn't touch; double-clicking a wire end jumps to the other")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {800, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {0, 0}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {300, 0}}, {"name", "B"}});
    run("gnAddNode", {{"args", {"G", "MeshCube"}}, {"position", {0, 400}}, {"name", "Lone"}});
    run("gnConnect", {{"args", {"G", "GroupInput.geometry", "A.geometry"}}});
    run("gnConnect", {{"args", {"G", "A.geometry", "B.geometry"}}});
    run("gnConnect", {{"args", {"G", "B.geometry", "GroupOutput.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    selectNode(view, "A");
    editor->setFlowHighlight(true, false);
    pump();
    CHECK(itemAt(view, {0, 400})->opacity() < 0.5);
    CHECK(itemAt(view, {800, 0})->opacity() == 1.0);
    CHECK(itemAt(view, {-300, 0})->opacity() == 1.0);
    editor->setFlowHighlight(false, false);
    CHECK(itemAt(view, {0, 400})->opacity() == 1.0);

    // A double-click by the start of A -> B lands on B.
    view->scene()->clearSelection();
    QPointF nearA(170 + 12, 34);
    mouse(view, QEvent::MouseButtonPress, nearA, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, nearA, Qt::NoButton);
    mouse(view, QEvent::MouseButtonDblClick, nearA, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, nearA, Qt::NoButton);
    QList<QGraphicsItem*> sel = view->scene()->selectedItems();
    REQUIRE(sel.size() == 1);
    CHECK(sel[0]->pos() == QPointF(300, 0));
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).nodes.size() == 5);
}

TEST_CASE("frames gather the selection, carry their nodes, take dropped ones in and collapse")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {900, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {0, 0}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {250, 0}}, {"name", "B"}});
    run("gnAddNode", {{"args", {"G", "MeshCube"}}, {"position", {0, 500}}, {"name", "C"}});
    run("gnConnect", {{"args", {"G", "GroupInput.geometry", "A.geometry"}}});
    run("gnConnect", {{"args", {"G", "A.geometry", "B.geometry"}}});
    run("gnConnect", {{"args", {"G", "B.geometry", "GroupOutput.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    selectNode(view, "A");
    itemAt(view, {250, 0})->setSelected(true);
    press(view, Qt::Key_J, Qt::ControlModifier);
    gn::Group g = read();
    REQUIRE(g.frames.size() == 1);
    CHECK(g.frames[0].nodes.size() == 2);
    glm::dvec2 at = g.frames[0].position;

    // Dragging the title moves the frame and both nodes, as one undo step.
    QPointF title(at.x + 100, at.y + 10);
    view->scene()->clearSelection();
    dragInView(view, title, title + QPointF(0, -200));
    g = read();
    // Within a pixel: the drag goes through view coordinates.
    CHECK(g.frames[0].position.y == doctest::Approx(at.y - 200).epsilon(0.01));
    CHECK(g.node("A")->position.y == doctest::Approx(-200).epsilon(0.01));
    CHECK(g.node("B")->position.y == doctest::Approx(-200).epsilon(0.01));
    run("undo");
    pump();
    g = read();
    CHECK(g.node("A")->position.y == doctest::Approx(0));
    CHECK(g.frames[0].position.y == doctest::Approx(at.y));

    // A node dropped inside joins; dragged out, it leaves.
    dragInView(view, {85, 512}, {85 + 120, 12 + 60});
    CHECK(read().frames[0].nodes.size() == 3);
    run("undo");
    pump();
    CHECK(read().frames[0].nodes.size() == 2);

    // Collapsing hides the members; wires to the outside still show.
    QPointF arrow(at.x + 10, at.y + 10);
    mouse(view, QEvent::MouseButtonPress, arrow, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, arrow, Qt::NoButton);
    pump();
    CHECK(read().frames[0].collapsed);
    CHECK_FALSE(itemAt(view, {0, 0})->isVisible());
    CHECK(wires(view).size() == 2);
}

TEST_CASE("align, distribute and straighten move the selection undoably")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {900, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {0, 0}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {40, 300}}, {"name", "B"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {-30, 420}}, {"name", "C"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {400, 137}}, {"name", "D"}});
    run("gnConnect", {{"args", {"G", "A.geometry", "D.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    selectNode(view, "A");
    itemAt(view, {40, 300})->setSelected(true);
    itemAt(view, {-30, 420})->setSelected(true);
    editor->alignSelected(0);
    gn::Group g = read();
    CHECK(g.node("A")->position.x == doctest::Approx(-30));
    CHECK(g.node("B")->position.x == doctest::Approx(-30));
    pump();

    // Even gaps between A (top) and C (bottom): B sits midway between their facing edges.
    editor->distributeSelected(false);
    g = read();
    double nodeHeight = 0.0;
    for (QGraphicsItem* i : view->scene()->items())
        if (QLineF(i->pos(), QPointF(-30, 0)).length() < 0.5)
            nodeHeight = i->boundingRect().height() - 4.0;
    double gapAbove = g.node("B")->position.y - (g.node("A")->position.y + nodeHeight);
    double gapBelow = g.node("C")->position.y - (g.node("B")->position.y + nodeHeight);
    CHECK(gapAbove == doctest::Approx(gapBelow));
    run("undo");
    run("undo");
    pump();
    CHECK(read().node("A")->position.x == doctest::Approx(0));

    // D alone: its input lines up with A's output.
    view->scene()->clearSelection();
    itemAt(view, {400, 137})->setSelected(true);
    editor->straightenSelected();
    // A's output is its first row; D's geometry input sits one row lower, under its own output.
    CHECK(read().node("D")->position.y == doctest::Approx(read().node("A")->position.y - 20.0));
}

TEST_CASE("H collapses the selection and Ctrl+H hides its unused sockets")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {600, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {150, 0}}, {"name", "A"}});
    run("gnConnect", {{"args", {"G", "GroupInput.geometry", "A.geometry"}}});
    run("gnConnect", {{"args", {"G", "A.geometry", "GroupOutput.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    double open = itemAt(view, {150, 0})->boundingRect().height();
    selectNode(view, "A");
    press(view, Qt::Key_H, Qt::ControlModifier);
    double trimmed = itemAt(view, {150, 0})->boundingRect().height();
    CHECK(trimmed < open);
    press(view, Qt::Key_H);
    CHECK(itemAt(view, {150, 0})->boundingRect().height() < trimmed);
    gn::Group g = gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G"));
    CHECK(g.node("A")->collapsed);
    CHECK(g.node("A")->hideUnused);
    CHECK(wires(view).size() == 2);
    press(view, Qt::Key_H);
    CHECK(itemAt(view, {150, 0})->boundingRect().height() == doctest::Approx(trimmed));

    press(view, Qt::Key_M);
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).node("A")->muted);
    press(view, Qt::Key_M);
    CHECK_FALSE(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).node("A")->muted);
}

TEST_CASE("Alt+right-drag connects nodes; Ctrl moves and Shift copies a link; Alt+S swaps inputs")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {700, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnDisconnect", {{"args", {"G", "GroupOutput.geometry"}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {100, 0}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {100, 250}}, {"name", "B"}});
    run("gnAddNode", {{"args", {"G", "Math"}}, {"position", {400, 300}}, {"name", "M"}});
    run("gnSetValue", {{"args", {"G", "M.a", 1.0}}});
    run("gnSetValue", {{"args", {"G", "M.b", 7.0}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    auto linked = [&](gn::GLink l) {
        gn::Group g = read();
        return std::find(g.links.begin(), g.links.end(), l) != g.links.end();
    };
    auto rdrag = [&](QPointF from, QPointF to) {
        auto send = [&](QEvent::Type t, QPointF at, Qt::MouseButtons buttons) {
            QPoint p = view->mapFromScene(at);
            QMouseEvent e(t, QPointF(p), QPointF(view->viewport()->mapToGlobal(p)), t == QEvent::MouseMove ? Qt::NoButton : Qt::RightButton, buttons,
                          Qt::AltModifier);
            QApplication::sendEvent(view->viewport(), &e);
            pump();
        };
        send(QEvent::MouseButtonPress, from, Qt::RightButton);
        send(QEvent::MouseMove, (from + to) * 0.5, Qt::RightButton);
        send(QEvent::MouseMove, to, Qt::RightButton);
        send(QEvent::MouseButtonRelease, to, Qt::NoButton);
    };
    rdrag({-200, 12}, {180, 12});
    CHECK(linked({"GroupInput", "geometry", "A", "geometry"}));

    // A's geometry input is its second row: header 24, one output row, then the input at 24 + 20 + 10.
    QPointF aIn(100, 54), bIn(100, 304);
    dragInView(view, aIn, bIn, Qt::ShiftModifier);
    CHECK(linked({"GroupInput", "geometry", "A", "geometry"}));
    CHECK(linked({"GroupInput", "geometry", "B", "geometry"}));
    dragInView(view, bIn, aIn + QPointF(0, 0), Qt::ControlModifier);
    CHECK_FALSE(linked({"GroupInput", "geometry", "B", "geometry"}));
    CHECK(linked({"GroupInput", "geometry", "A", "geometry"}));

    view->scene()->clearSelection();
    itemAt(view, {400, 300})->setSelected(true);
    press(view, Qt::Key_S, Qt::AltModifier);
    gn::Group g = read();
    CHECK(g.node("M")->values["a"] == 7.0);
    CHECK(g.node("M")->values["b"] == 1.0);
}

TEST_CASE("auto layout puts the graph in data-flow columns, undoably")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {-500, 300}}, {"GroupInput", {400, 200}}}}});
    run("gnDisconnect", {{"args", {"G", "GroupOutput.geometry"}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {300, -200}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {-100, 0}}, {"name", "B"}});
    run("gnAddNode", {{"args", {"G", "MeshCube"}}, {"position", {0, 0}}, {"name", "C"}});
    run("gnConnect", {{"args", {"G", "GroupInput.geometry", "A.geometry"}}});
    run("gnConnect", {{"args", {"G", "A.geometry", "B.geometry"}}});
    run("gnConnect", {{"args", {"G", "B.geometry", "GroupOutput.geometry"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    editor->autoLayout();
    gn::Group g = read();
    auto x = [&](const char* n) { return g.node(n)->position.x; };
    CHECK(x("GroupInput") < x("A"));
    CHECK(x("A") < x("B"));
    CHECK(x("B") < x("GroupOutput"));
    // The unlinked cube shares the first column and doesn't overlap Group Input.
    CHECK(x("C") == doctest::Approx(x("GroupInput")));
    CHECK(std::abs(g.node("C")->position.y - g.node("GroupInput")->position.y) >= 50.0);
    run("undo");
    CHECK(read().node("A")->position.x == doctest::Approx(300));
}

TEST_CASE("bookmarks bring a view back; the minimap pans; find jumps to a node")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {2000, 1500}}, {"GroupInput", {-300, 0}}}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto centre = [&] { return view->mapToScene(view->viewport()->rect().center()); };
    view->centerOn(100, 100);
    QPointF saved = centre();
    press(view, Qt::Key_1, Qt::ControlModifier);
    CHECK(gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")).bookmarks.contains("1"));
    view->centerOn(1500, 1200);
    press(view, Qt::Key_1);
    CHECK(QLineF(centre(), saved).length() < 8.0); // centerOn rounds to whole view pixels

    editor->setMinimap(true);
    pump();
    QRect v = view->viewport()->rect();
    QPoint corner(v.right() - 20, v.bottom() - 20);
    QPointF before = centre();
    QMouseEvent down(QEvent::MouseButtonPress, QPointF(corner), QPointF(view->viewport()->mapToGlobal(corner)), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &down);
    QMouseEvent up(QEvent::MouseButtonRelease, QPointF(corner), QPointF(view->viewport()->mapToGlobal(corner)), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(view->viewport(), &up);
    pump();
    CHECK(QLineF(centre(), before).length() > 50.0);
    editor->setMinimap(false);

    editor->focusNode("GroupOutput");
    QList<QGraphicsItem*> sel = view->scene()->selectedItems();
    REQUIRE(sel.size() == 1);
    CHECK(sel[0]->pos() == QPointF(2000, 1500));
    CHECK(QLineF(centre(), sel[0]->sceneBoundingRect().center()).length() < 8.0);
}

TEST_CASE("wire styles, dimming unrelated wires, and wire summaries")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {600, 100}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "CombineXYZ"}}, {"position", {0, 300}}, {"name", "XYZ"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {300, 300}}, {"name", "S"}});
    run("gnSetValue", {{"args", {"G", "XYZ.z", 5.0}}});
    run("gnConnect", {{"args", {"G", "XYZ.vector", "S.offset"}}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto straightAndLevel = [&](QGraphicsPathItem* w) {
        const QPainterPath& p = w->path();
        bool ok = true;
        for (int i = 1; i < p.elementCount(); ++i)
            ok = ok && p.elementAt(i).type == QPainterPath::LineToElement &&
                 (std::abs(p.elementAt(i).x - p.elementAt(i - 1).x) < 1e-6 || std::abs(p.elementAt(i).y - p.elementAt(i - 1).y) < 1e-6);
        return ok;
    };
    PrefStore::get().set("general", "/nodeEditor/wireStyle", "straight");
    pump();
    for (QGraphicsPathItem* w : wires(view))
        CHECK(w->path().elementCount() == 2);
    PrefStore::get().set("general", "/nodeEditor/wireStyle", "rightAngle");
    pump();
    for (QGraphicsPathItem* w : wires(view))
        CHECK(straightAndLevel(w));
    PrefStore::get().set("general", "/nodeEditor/wireStyle", "curved");

    PrefStore::get().set("general", "/nodeEditor/dimUnrelated", true);
    pump();
    selectNode(view, "S");
    pump();
    int dim = 0, full = 0;
    for (QGraphicsPathItem* w : wires(view))
        (w->opacity() < 0.5 ? dim : full)++;
    CHECK(dim == 1);  // Group Input to Group Output
    CHECK(full == 1); // into S
    PrefStore::get().set("general", "/nodeEditor/dimUnrelated", false);
    pump();

    // Only the selected node's wires: S has one, so the other hides; with nothing selected, none show.
    editor->setSelectedWiresOnly(true);
    CHECK(wires(view).size() == 1);
    view->scene()->clearSelection();
    pump();
    CHECK(wires(view).empty());
    editor->setSelectedWiresOnly(false);
    CHECK(wires(view).size() == 2);

    // Wires take their socket type's colour; the pref draws them one grey in a grey theme (the Monotone toggle).
    std::set<QRgb> colours;
    for (QGraphicsPathItem* w : wires(view))
        colours.insert(w->pen().color().rgb());
    CHECK(colours.contains(editor->backend().socketStyle("geometry").color.rgb()));
    CHECK(colours.contains(editor->backend().socketStyle("vector").color.rgb()));
    // Geometry wires draw thicker than value wires.
    std::set<double> widths;
    for (QGraphicsPathItem* w : wires(view))
        widths.insert(w->pen().widthF());
    CHECK(widths.size() == 2);
    CHECK(*widths.rbegin() > *widths.begin());
    auto wireColours = [&] {
        std::set<QRgb> out;
        for (QGraphicsPathItem* w : wires(view))
            out.insert(w->pen().color().rgb());
        return out;
    };
    // A hued theme keeps its wire colours with the pref on.
    PrefStore::get().set("general", "/nodeEditor/greyWiresInMonotone", true);
    pump();
    CHECK(wireColours().size() == 2);
    PrefStore::get().set("general", ui::themes::kMonotonePref, true);
    ui::themes::invalidate();
    PrefStore::get().set("general", "/nodeEditor/greyWiresInMonotone", false);
    pump();
    PrefStore::get().set("general", "/nodeEditor/greyWiresInMonotone", true);
    pump();
    CHECK(wireColours().size() == 1);
    PrefStore::get().set("general", "/nodeEditor/greyWiresInMonotone", false);
    PrefStore::get().set("general", ui::themes::kMonotonePref, false);
    ui::themes::invalidate();
    pump();

    json info = run("gnGroupInfo", {{"args", {"G"}}});
    QString summary = editor->backend().wireSummary(info, "XYZ", "vector");
    CHECK(summary.contains("z 5"));
    CHECK(editor->backend().wireSummary(info, "S", "geometry").isEmpty());
}

TEST_CASE("merging the selection adds Join Geometry or a chain of Math nodes, wired")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {900, 0}}, {"GroupInput", {-300, 0}}}}});
    run("gnAddNode", {{"args", {"G", "MeshCube"}}, {"position", {0, 0}}, {"name", "C1"}});
    run("gnAddNode", {{"args", {"G", "MeshCube"}}, {"position", {0, 250}}, {"name", "C2"}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 500}}, {"name", "V1"}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 600}}, {"name", "V2"}});
    run("gnAddNode", {{"args", {"G", "Value"}}, {"position", {0, 700}}, {"name", "V3"}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    selectNode(view, "C1");
    itemAt(view, {0, 250})->setSelected(true);
    press(view, Qt::Key_Plus, Qt::ControlModifier | Qt::ShiftModifier);
    gn::Group g = read();
    const gn::GNode* join = g.firstOfType("JoinGeometry");
    REQUIRE(join);
    CHECK(g.linksInto(join->name, "geometry").size() == 2);

    view->scene()->clearSelection();
    for (QPointF p : {QPointF(0, 500), QPointF(0, 600), QPointF(0, 700)})
        itemAt(view, p)->setSelected(true);
    editor->mergeSelected("multiply");
    g = read();
    std::vector<const gn::GNode*> maths;
    for (const gn::GNode& n : g.nodes)
        if (n.type == "Math")
            maths.push_back(&n);
    REQUIRE(maths.size() == 2);
    CHECK(maths[0]->values.value("operation", "") == "multiply");
    CHECK(g.linksInto(maths[1]->name, "a").front().fromNode == maths[0]->name);
    run("undo");
    int left = 0;
    for (const gn::GNode& n : read().nodes)
        left += n.type == "Math" ? 1 : 0;
    CHECK(left == 0);

    ui::registerTexGraphBackend();
    std::optional<ui::GraphBackend::MergeNode> blend = ui::graphBackends::find(ui::kTextureGraphBackend)->mergeNode("color", "multiply");
    REQUIRE(blend);
    CHECK(blend->type == "blend");
    CHECK(blend->values["mode"] == "multiply");
}

TEST_CASE("graph editor Delete stays in the graph editor")
{
    ui::registerAnimationUi();
    run("newScene");
    run("spaceLocator", {{"name", "keepMe"}});
    run("select", {{"args", {"keepMe"}}});

    QMainWindow win;
    auto* editor = new ui::GraphEditor(&win);
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    win.activateWindow();
    pump();
    QWidget* curves = editor->findChild<QWidget*>("curveView");
    REQUIRE(curves);
    curves->setFocus();
    pump();
    REQUIRE(QApplication::focusWidget() == curves);
    press(curves, Qt::Key_Delete);
    press(curves, Qt::Key_Backspace);
    CHECK(scene::Graph::current().find("keepMe"));
}

TEST_CASE("W grabs the selection to the cursor and R scales its spacing; Esc puts it back, a click places it")
{
    run("newScene");
    run("gnCreateGroup", {{"args", {"G"}}});
    run("gnMoveNodes", {{"args", {"G"}}, {"positions", {{"GroupOutput", {900, 600}}, {"GroupInput", {-300, 600}}}}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {0, 0}}, {"name", "A"}});
    run("gnAddNode", {{"args", {"G", "SetPosition"}}, {"position", {400, 0}}, {"name", "B"}});
    QMainWindow win;
    ui::NodeEditor* editor = openOn(win, "G");
    auto* view = editor->findChild<QGraphicsView*>();
    view->setFocus();
    pump();
    auto read = [] { return gn::readGroup(*gn::findGroupNode(scene::Graph::current(), "G")); };
    auto live = [&](QPointF at) { return itemAt(view, at); };
    selectNode(view, "A");
    live({400, 0})->setSelected(true);
    QGraphicsItem* a = live({0, 0});
    QGraphicsItem* b = live({400, 0});

    // The selection follows the cursor's travel; X locks it to the horizontal and is not Delete.
    press(view, Qt::Key_W);
    mouse(view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    const QPointF a1 = a->pos();
    mouse(view, QEvent::MouseMove, {160, 130}, Qt::NoButton);
    CHECK(a->pos().x() - a1.x() == doctest::Approx(60).epsilon(0.02));
    CHECK(a->pos().y() - a1.y() == doctest::Approx(30).epsilon(0.02));
    CHECK(b->pos() - a->pos() == QPointF(400, 0));
    press(view, Qt::Key_X);
    CHECK(nodeCount("G") == 4);
    CHECK(a->pos().y() == doctest::Approx(0));
    press(view, Qt::Key_Escape);
    CHECK(a->pos() == QPointF(0, 0));
    CHECK(b->pos() == QPointF(400, 0));

    // A click places it as one undo step.
    press(view, Qt::Key_W);
    mouse(view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    mouse(view, QEvent::MouseMove, {150, 100}, Qt::NoButton);
    const double moved = a->pos().x();
    mouse(view, QEvent::MouseButtonPress, {150, 100}, Qt::LeftButton);
    mouse(view, QEvent::MouseButtonRelease, {150, 100}, Qt::NoButton);
    CHECK(read().node("A")->position.x == doctest::Approx(moved));
    CHECK(read().node("B")->position.x == doctest::Approx(moved + 400));
    run("undo");
    pump();
    CHECK(read().node("A")->position.x == doctest::Approx(0));

    // A tap of W toggles: W again places it. Held past a tap, its release places it.
    auto reselect = [&] {
        a = live({0, 0});
        b = live({400, 0});
        REQUIRE(a);
        REQUIRE(b);
        a->setSelected(true);
        b->setSelected(true);
    };
    reselect();
    press(view, Qt::Key_W);
    mouse(view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    mouse(view, QEvent::MouseMove, {130, 100}, Qt::NoButton);
    press(view, Qt::Key_W);
    CHECK(read().node("A")->position.x != doctest::Approx(0));
    CHECK(read().node("B")->position.x - read().node("A")->position.x == doctest::Approx(400));
    run("undo");
    pump();
    reselect();
    key(view, QEvent::KeyPress, Qt::Key_W);
    mouse(view, QEvent::MouseMove, {100, 100}, Qt::NoButton);
    QElapsedTimer held;
    held.start();
    while (held.elapsed() < 300)
        pump();
    mouse(view, QEvent::MouseMove, {120, 100}, Qt::NoButton);
    key(view, QEvent::KeyRelease, Qt::Key_W);
    CHECK(read().node("A")->position.x != doctest::Approx(0));
    run("undo");
    pump();
    reselect();

    // R: further from the centre spreads the pair apart, never pulls them into each other.
    press(view, Qt::Key_R);
    mouse(view, QEvent::MouseMove, {700, 0}, Qt::NoButton);
    const double wide = b->pos().x() - a->pos().x();
    mouse(view, QEvent::MouseMove, {1200, 0}, Qt::NoButton);
    CHECK(b->pos().x() - a->pos().x() > wide);
    mouse(view, QEvent::MouseMove, {285, 0}, Qt::NoButton);
    CHECK(b->pos().x() - a->pos().x() >= 170.0 + 8.0 - 0.01);
    press(view, Qt::Key_Escape);
    CHECK(a->pos() == QPointF(0, 0));
    CHECK(b->pos() == QPointF(400, 0));
}
