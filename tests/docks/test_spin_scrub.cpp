#include <doctest/doctest.h>

#include "ui/SpinScrub.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

#include <utility>

namespace
{
void widgetMouse(QWidget* w, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, at, w->mapToGlobal(at), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

/** Sends to the window, as real input arrives: Qt then routes it to the widget under the cursor. */
void windowMouse(QWindow* w, QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons buttons)
{
    QMouseEvent e(type, QPointF(at), QPointF(w->mapToGlobal(at)), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(w, &e);
}

void dragScrub(QWidget* w, Qt::MouseButton button, int dx)
{
    const QPoint at(4, 4);
    widgetMouse(w, QEvent::MouseButtonPress, at, button, button);
    for (int x = 6; x <= dx; x += 6)
        widgetMouse(w, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, button);
    widgetMouse(w, QEvent::MouseButtonRelease, at + QPoint(dx, 0), button, Qt::NoButton);
}
} // namespace

TEST_CASE("a spin box's form label scrubs it with the left button")
{
    static bool installed = false;
    if (!std::exchange(installed, true))
        lila::ui::installSpinScrub(qApp);

    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(0.0, 10.0);
    box->setSingleStep(0.5);
    form->addRow("Strength", box);
    auto* label = qobject_cast<QLabel*>(form->labelForField(box));
    REQUIRE(label);

    // Six pixels a step.
    dragScrub(label, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(3.0));
    CHECK(label->cursor().shape() == Qt::SizeHorCursor);
    dragScrub(label, Qt::MiddleButton, 12);
    CHECK(box->value() == doctest::Approx(4.0));

    // The field itself keeps its left button for editing.
    dragScrub(box, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(4.0));

    // A label that scrubs by itself is left to do so.
    QLabel own("Own", &panel);
    own.setCursor(Qt::SizeHorCursor);
    own.setBuddy(box);
    dragScrub(&own, Qt::LeftButton, 36);
    CHECK(box->value() == doctest::Approx(4.0));
}

TEST_CASE("a middle-drag scrub through the window lets the mouse go on release")
{
    static bool installed = false;
    if (!std::exchange(installed, true))
        lila::ui::installSpinScrub(qApp);

    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(0.0, 10.0);
    box->setSingleStep(0.5);
    form->addRow("Depth", box);
    auto* other = new QLabel("Other", &panel);
    form->addRow(other);
    panel.resize(300, 120);
    panel.show();
    QApplication::processEvents();
    QWindow* window = panel.windowHandle();
    REQUIRE(window);

    // Through the window, so the app-wide filter sees each event twice.
    const QPoint at = box->mapTo(&panel, QPoint(4, 4));
    windowMouse(window, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
    for (int x = 6; x <= 24; x += 6)
        windowMouse(window, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, Qt::MiddleButton);
    windowMouse(window, QEvent::MouseButtonRelease, at + QPoint(24, 0), Qt::MiddleButton, Qt::NoButton);
    CHECK(box->value() == doctest::Approx(2.0));
    CHECK(QApplication::overrideCursor() == nullptr);

    // After the release, moving over another widget reaches it, not the field the drag began on.
    struct Seen : QObject
    {
        bool moved = false;
        bool eventFilter(QObject*, QEvent* e) override
        {
            moved = moved || e->type() == QEvent::MouseMove;
            return false;
        }
    } seen;
    other->setMouseTracking(true);
    other->installEventFilter(&seen);
    windowMouse(window, QEvent::MouseMove, other->mapTo(&panel, QPoint(4, 4)), Qt::NoButton, Qt::NoButton);
    CHECK(seen.moved);
}
