#include <doctest/doctest.h>

#include "core/MainQueue.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/LastOp.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/AdjustLastOp.h"
#include "ui/SpinScrub.h"

#include <QAbstractSpinBox>
#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QMouseEvent>

#include <utility>

using namespace lila;
using json = nlohmann::json;

namespace
{
void sendMouse(QWidget* w, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

/** Drags w sideways, letting the editor's queued re-runs happen between moves as they would live. */
void scrub(QWidget* w, Qt::MouseButton button, int dx)
{
    const QPoint at(4, 4);
    sendMouse(w, QEvent::MouseButtonPress, at, button, button);
    for (int x = 6; x <= dx; x += 6)
    {
        sendMouse(w, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, button);
        QApplication::processEvents();
    }
    sendMouse(w, QEvent::MouseButtonRelease, at + QPoint(dx, 0), button, Qt::NoButton);
    QApplication::processEvents();
}

QLabel* labelNamed(QWidget* root, const QString& text)
{
    for (QLabel* l : root->findChildren<QLabel*>())
        if (l->text() == text)
            return l;
    return nullptr;
}

bool showsText(QWidget* root, const QString& part)
{
    for (QLabel* l : root->findChildren<QLabel*>())
        if (l->text().contains(part))
            return true;
    return false;
}

double thickness()
{
    const scene::lastOp::Op* op = scene::lastOp::current();
    REQUIRE(op);
    return op->call.value("thickness", 0.0);
}
} // namespace

TEST_CASE("scrubbing an Adjust Last Op field re-runs the op instead of finding the undo stack busy")
{
    static bool installed = false;
    if (!std::exchange(installed, true))
    {
        mainq::init();
        ops::registerBuiltins();
        scene::types::registerBuiltins();
        scene::registerCommands();
        scene::lastOp::install();
        ui::installSpinScrub(qApp);
    }
    ops::execute("newScene", json::object());
    UndoStack::get().clear();
    const std::string cube = ops::execute("polyCube", {{"width", 100.0}, {"height", 100.0}, {"depth", 100.0}})[0];
    ops::execute("selectComponents", {{"args", {cube}}, {"faces", {0}}});
    ops::execute("polyExtrude", {{"thickness", 20.0}}, CallSource::UI);
    REQUIRE(scene::lastOp::live());

    ui::LastOpEditor editor;
    editor.resize(320, 400);
    editor.refresh();
    editor.show();
    QApplication::processEvents();

    QLabel* label = labelNamed(&editor, "Thickness");
    REQUIRE(label);
    scrub(label, Qt::LeftButton, 60);
    CHECK(thickness() != doctest::Approx(20.0));
    CHECK_FALSE(UndoStack::get().chunkOpen());
    CHECK_FALSE(showsText(&editor, "busy"));

    // A middle-drag on the field scrubs through the app-wide filter, which must leave the stack free too.
    const double before = thickness();
    QAbstractSpinBox* field = qobject_cast<QAbstractSpinBox*>(label->buddy());
    if (!field)
        for (QAbstractSpinBox* s : editor.findChildren<QAbstractSpinBox*>())
            if (s->isVisible())
            {
                field = s;
                break;
            }
    REQUIRE(field);
    scrub(field, Qt::MiddleButton, 60);
    CHECK(thickness() != doctest::Approx(before));
    CHECK_FALSE(UndoStack::get().chunkOpen());
    CHECK_FALSE(showsText(&editor, "busy"));
}
