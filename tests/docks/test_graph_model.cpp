#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "scene/Anim.h"
#include "scene/Graph.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Animation.h"
#include "ui/GraphEditor.h"
#include "ui/GraphModel.h"
#include "ui/Hotkeys.h"

#include <QApplication>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>

using namespace lila;
using namespace lila::ui::graphed;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
namespace anim = scene::anim;

void setupUi()
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    ui::actions::registerBuiltins();
    ui::registerAnimationUi();
    ui::Hotkeys::get().install();
}

/** A locator with translateX keyed 0 at frame 0, 10 at frame 10 and 4 at frame 20, and rotateY keyed 0 then 45. */
void keyedLocator()
{
    run("newScene");
    run("spaceLocator", {{"name", "loc"}});
    for (auto [frame, value] : {std::pair{0.0, 0.0}, {10.0, 10.0}, {20.0, 4.0}})
        run("setKeyframe", {{"args", {"loc.tx"}}, {"time", anim::secondsOf(frame)}, {"value", value}});
    run("setKeyframe", {{"args", {"loc.ry"}}, {"time", anim::secondsOf(0.0)}, {"value", 0.0}});
    run("setKeyframe", {{"args", {"loc.ry"}}, {"time", anim::secondsOf(10.0)}, {"value", 45.0}});
    run("select", {{"args", {"loc"}}});
}

double keyValue(const GraphModel& m, NodeId curve, double frame)
{
    const anim::Curve& c = m.curves.at(curve);
    const int i = c.find(anim::secondsOf(frame));
    REQUIRE(i >= 0);
    return c.keys[size_t(i)].value;
}

NodeId channelNamed(const GraphModel& m, const char* label)
{
    for (const Channel& ch : m.channels)
        if (ch.label == label)
            return ch.curve;
    FAIL("no channel " << label);
    return scene::kNoNode;
}

void mouseAt(QWidget* w, QEvent::Type type, QPointF at, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), button, buttons, mods);
    QApplication::sendEvent(w, &e);
    pump();
}

void typeKey(QWidget* w, Qt::Key key, const QString& text = {})
{
    QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier, text);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier, text);
    QApplication::sendEvent(w, &up);
    pump();
}
} // namespace

TEST_CASE("graph model: click and box share one set of selection modifiers")
{
    CHECK(selectOpFor(false, false) == SelectOp::Replace);
    CHECK(selectOpFor(true, false) == SelectOp::Toggle);
    CHECK(selectOpFor(false, true) == SelectOp::Remove);
    CHECK(selectOpFor(true, true) == SelectOp::Add);

    GraphModel m;
    const KeySel a{1, 0.0}, b{1, 1.0}, c{2, 0.0};
    const KeySel ab[] = {a, b};
    m.select(ab, SelectOp::Replace);
    CHECK(m.sel.size() == 2);
    const KeySel bc[] = {b, c};
    m.select(bc, SelectOp::Toggle);
    CHECK(m.isSelected(1, 0.0));
    CHECK_FALSE(m.isSelected(1, 1.0));
    CHECK(m.isSelected(2, 0.0));
    m.select(bc, SelectOp::Add);
    CHECK(m.sel.size() == 3);
    m.select(std::span(&a, 1), SelectOp::Remove);
    CHECK(m.sel.size() == 2);
    CHECK_FALSE(m.isSelected(1, 0.0));
    m.select({}, SelectOp::Replace);
    CHECK(m.sel.empty());
}

TEST_CASE("graph model: gathers channels and edits keys, one undo step per edit")
{
    setupUi();
    keyedLocator();
    scene::Graph& g = scene::Graph::current();
    GraphModel m;
    m.channels = GraphModel::gather(g, g.selection(), RotationDisplay::Euler);
    m.reloadCurves();
    REQUIRE(m.channels.size() >= 2);
    const NodeId tx = channelNamed(m, "Translate X");
    const NodeId ry = channelNamed(m, "Rotate Y");
    CHECK(m.channelOf(tx)->unit == "cm");
    CHECK(m.channelOf(ry)->unit == QString::fromUtf8("°"));

    // A move: two frames later and 3 higher, one transformKeys.
    const KeySel key{tx, anim::secondsOf(10.0)};
    m.select(std::span(&key, 1), SelectOp::Replace);
    m.edit.kind = KeyEdit::Kind::Transform;
    m.edit.moved = true;
    m.edit.xform.timeOffset = anim::secondsOf(2.0) - anim::secondsOf(0.0);
    m.edit.displayOffset = 3.0;
    CHECK(m.preview(tx).find(anim::secondsOf(12.0)) >= 0);
    REQUIRE(m.commit());
    CHECK(m.edit.kind == KeyEdit::Kind::None);
    m.reloadCurves();
    CHECK(keyValue(m, tx, 12.0) == doctest::Approx(13.0));
    REQUIRE(m.sel.size() == 1);
    CHECK(m.sel[0].time == doctest::Approx(anim::secondsOf(12.0)));
    run("undo");
    m.reloadCurves();
    CHECK(keyValue(m, tx, 10.0) == doctest::Approx(10.0));

    // A tangent turn on the rotation's Euler channel.
    m.sel = {{ry, anim::secondsOf(10.0)}};
    m.edit = {};
    m.edit.kind = KeyEdit::Kind::Tangent;
    m.edit.moved = true;
    m.edit.tangent = m.sel[0];
    m.edit.tangentAngle = 30.0;
    REQUIRE(m.commit());
    m.reloadCurves();
    const anim::Curve& rc = m.curves.at(ry);
    CHECK(std::atan(rc.inSlope(size_t(rc.find(anim::secondsOf(10.0))))) * 180.0 / 3.14159265358979323846 == doctest::Approx(30.0).epsilon(0.01));
    run("undo");

    // Tangent types and deletion each undo in one step.
    m.reloadCurves();
    m.sel = {{tx, anim::secondsOf(0.0)}, {tx, anim::secondsOf(20.0)}};
    m.applyTangent("linear");
    m.reloadCurves();
    CHECK(m.curves.at(tx).keys.front().out == anim::Tangent::Linear);
    run("undo");
    m.reloadCurves();
    CHECK(m.curves.at(tx).keys.front().out != anim::Tangent::Linear);
    CHECK(m.deleteSelectedKeys() == 2);
    m.reloadCurves();
    CHECK(m.curves.at(tx).keys.size() == 1);
    run("undo");
    m.reloadCurves();
    CHECK(m.curves.at(tx).keys.size() == 3);

    // A cancelled edit changes nothing.
    m.sel = {{tx, anim::secondsOf(20.0)}};
    m.edit.kind = KeyEdit::Kind::Transform;
    m.edit.moved = true;
    m.edit.displayOffset = 50.0;
    m.pushPreviews();
    m.cancel();
    m.reloadCurves();
    CHECK(keyValue(m, tx, 20.0) == doctest::Approx(4.0));
}

TEST_CASE("graph editor: Shift box toggles, and a number typed mid-drag sets the dragged key's value")
{
    setupUi();
    keyedLocator();
    QMainWindow win;
    auto* editor = new ui::GraphEditor(&win);
    win.setCentralWidget(editor);
    win.resize(900, 600);
    win.show();
    win.activateWindow();
    pump();
    QWidget* view = editor->findChild<QWidget*>("curveView");
    REQUIRE(view);
    editor->frame(false);
    pump();
    GraphModel& m = editor->model();
    const NodeId tx = channelNamed(m, "Translate X");
    size_t total = 0;
    for (const Channel& ch : m.channels)
        total += m.curves.at(ch.curve).keys.size();

    // One key picked, then Shift and a box over the whole plot: the picked key drops out, the rest come in.
    const KeySel first{tx, anim::secondsOf(0.0)};
    m.select(std::span(&first, 1), SelectOp::Replace);
    const QPointF from(4, ui::graphed::kRulerHeight + 4), to(view->width() - 4, view->height() - 4);
    mouseAt(view, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
    mouseAt(view, QEvent::MouseMove, (from + to) / 2, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    mouseAt(view, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
    mouseAt(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
    CHECK(m.sel.size() == total - 1);
    CHECK_FALSE(m.isSelected(tx, anim::secondsOf(0.0)));

    // Ctrl+Shift box adds: everything is picked again.
    mouseAt(view, QEvent::MouseButtonPress, from, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier | Qt::ControlModifier);
    mouseAt(view, QEvent::MouseMove, to, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier | Qt::ControlModifier);
    mouseAt(view, QEvent::MouseButtonRelease, to, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier | Qt::ControlModifier);
    CHECK(m.sel.size() == total);

    // MMB drags the selection from anywhere; typing 7 and Enter puts the first key's value at 7, as one undo step.
    const KeySel middle{tx, anim::secondsOf(10.0)};
    m.select(std::span(&middle, 1), SelectOp::Replace);
    view->setFocus();
    const QPointF c(view->width() / 2.0, view->height() / 2.0);
    mouseAt(view, QEvent::MouseButtonPress, c, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    mouseAt(view, QEvent::MouseMove, c + QPointF(0, -30), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    typeKey(view, Qt::Key_7, "7");
    typeKey(view, Qt::Key_Return);
    mouseAt(view, QEvent::MouseButtonRelease, c + QPointF(0, -30), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    m.reloadCurves();
    CHECK(keyValue(m, tx, 10.0) == doctest::Approx(7.0));
    run("undo");
    m.reloadCurves();
    CHECK(keyValue(m, tx, 10.0) == doctest::Approx(10.0));

    // Esc mid-drag puts the keys back.
    m.select(std::span(&middle, 1), SelectOp::Replace);
    mouseAt(view, QEvent::MouseButtonPress, c, Qt::MiddleButton, Qt::MiddleButton, Qt::NoModifier);
    mouseAt(view, QEvent::MouseMove, c + QPointF(0, -60), Qt::NoButton, Qt::MiddleButton, Qt::NoModifier);
    typeKey(view, Qt::Key_Escape);
    mouseAt(view, QEvent::MouseButtonRelease, c + QPointF(0, -60), Qt::MiddleButton, Qt::NoButton, Qt::NoModifier);
    m.reloadCurves();
    CHECK(keyValue(m, tx, 10.0) == doctest::Approx(10.0));
}
