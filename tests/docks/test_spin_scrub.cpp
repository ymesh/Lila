#include <doctest/doctest.h>

#include "ui/NumberExpr.h"
#include "ui/ScrubGesture.h"
#include "ui/SpinScrub.h"

#include <QApplication>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLabel>
#include <QMouseEvent>
#include <QWidget>
#include <QWindow>

#include <utility>
#include <vector>

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
    for (int x = 5; x <= dx; x += 5)
        widgetMouse(w, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, button);
    widgetMouse(w, QEvent::MouseButtonRelease, at + QPoint(dx, 0), button, Qt::NoButton);
}

void clickField(QWidget* w, Qt::MouseButton button)
{
    widgetMouse(w, QEvent::MouseButtonPress, QPoint(4, 4), button, button);
    widgetMouse(w, QEvent::MouseButtonRelease, QPoint(4, 4), button, Qt::NoButton);
}

void installOnce()
{
    static bool installed = false;
    if (!std::exchange(installed, true))
        lila::ui::installSpinScrub(qApp);
}
} // namespace

TEST_CASE("a spin box and its form label drag it with either button, a step per ten pixels")
{
    installOnce();
    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(0.0, 10.0);
    box->setSingleStep(0.5);
    form->addRow("Strength", box);
    auto* label = qobject_cast<QLabel*>(form->labelForField(box));
    REQUIRE(label);

    dragScrub(label, Qt::LeftButton, 40);
    CHECK(box->value() == doctest::Approx(2.0));
    CHECK(label->cursor().shape() == Qt::SizeHorCursor);
    dragScrub(label, Qt::MiddleButton, 20);
    CHECK(box->value() == doctest::Approx(3.0));
    // The field drags with the left button like every number field.
    dragScrub(box, Qt::LeftButton, 40);
    CHECK(box->value() == doctest::Approx(5.0));
    CHECK(QApplication::overrideCursor() == nullptr);

    // A label that scrubs by itself is left to do so.
    QLabel own("Own", &panel);
    own.setCursor(Qt::SizeHorCursor);
    own.setBuddy(box);
    dragScrub(&own, Qt::LeftButton, 40);
    CHECK(box->value() == doctest::Approx(5.0));
}

TEST_CASE("a click on a spin box types; Esc, Enter or a click elsewhere ends typing")
{
    installOnce();
    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(0.0, 10.0);
    box->setValue(4.0);
    form->addRow("Size", box);
    auto* other = new QLabel("Other", &panel);
    form->addRow(other);
    panel.show();
    panel.activateWindow();
    for (int i = 0; i < 50 && !panel.isActiveWindow(); ++i)
        QApplication::processEvents();
    REQUIRE(panel.isActiveWindow());
    // Qt focuses on a real press before filters run, so a press must not focus it.
    CHECK(box->focusPolicy() == Qt::TabFocus);

    // Activating the window focuses its first field; start from none, as a panel in the main window does.
    box->clearFocus();
    clickField(box, Qt::LeftButton);
    REQUIRE(box->hasFocus());
    // While typing, a left drag edits text instead of the value.
    dragScrub(box, Qt::LeftButton, 40);
    CHECK(box->value() == doctest::Approx(4.0));

    QKeyEvent esc(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(box, &esc);
    CHECK_FALSE(box->hasFocus());

    clickField(box, Qt::LeftButton);
    REQUIRE(box->hasFocus());
    clickField(other, Qt::LeftButton);
    CHECK_FALSE(box->hasFocus());
}

TEST_CASE("a middle click resets a spin box, and moving the middle button up or down opens the Step Ladder")
{
    installOnce();
    QWidget panel;
    auto* form = new QFormLayout(&panel);
    auto* box = new QDoubleSpinBox(&panel);
    box->setRange(-1000.0, 1000.0);
    box->setDecimals(2);
    box->setSingleStep(0.5);
    box->setValue(7.0);
    lila::ui::setSpinDefault(box, 1.0);
    form->addRow("Offset", box);
    panel.show();
    QApplication::processEvents();

    clickField(box, Qt::MiddleButton);
    CHECK(box->value() == doctest::Approx(1.0));

    // The ladder starts on the step nearest the field's (1 for 0.5); one row up is 10.
    const QPoint at(4, 4);
    widgetMouse(box, QEvent::MouseButtonPress, at, Qt::MiddleButton, Qt::MiddleButton);
    widgetMouse(box, QEvent::MouseMove, at + QPoint(0, -12), Qt::NoButton, Qt::MiddleButton);
    widgetMouse(box, QEvent::MouseMove, at + QPoint(0, -24), Qt::NoButton, Qt::MiddleButton);
    bool shown = false;
    for (QWidget* w : QApplication::topLevelWidgets())
        shown |= w->isVisible() && w->windowFlags().testFlag(Qt::ToolTip) && w->testAttribute(Qt::WA_TransparentForMouseEvents);
    CHECK(shown);
    for (int x = 5; x <= 20; x += 5)
        widgetMouse(box, QEvent::MouseMove, at + QPoint(x, -24), Qt::NoButton, Qt::MiddleButton);
    widgetMouse(box, QEvent::MouseButtonRelease, at + QPoint(20, -24), Qt::MiddleButton, Qt::NoButton);
    CHECK(box->value() == doctest::Approx(21.0));
    CHECK(QApplication::overrideCursor() == nullptr);
}

TEST_CASE("a middle-drag scrub through the window lets the mouse go on release")
{
    installOnce();
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
    for (int x = 5; x <= 40; x += 5)
        windowMouse(window, QEvent::MouseMove, at + QPoint(x, 0), Qt::NoButton, Qt::MiddleButton);
    windowMouse(window, QEvent::MouseButtonRelease, at + QPoint(40, 0), Qt::MiddleButton, Qt::NoButton);
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

TEST_CASE("the Step Ladder runs in powers of ten, bounded by the field")
{
    using lila::ui::ScrubGesture;
    CHECK(ScrubGesture::ladderSteps(0.0, 4, false) == std::vector<double>{100, 10, 1, 0.1, 0.01, 0.001, 0.0001});
    CHECK(ScrubGesture::ladderSteps(0.0, 2, false) == std::vector<double>{100, 10, 1, 0.1, 0.01});
    CHECK(ScrubGesture::ladderSteps(0.0, 3, true) == std::vector<double>{100, 10, 1});
    CHECK(ScrubGesture::ladderSteps(5.0, 3, false) == std::vector<double>{1, 0.1, 0.01, 0.001});
}

TEST_CASE("a spin box takes math typed into it")
{
    QWidget host;
    auto* box = new QDoubleSpinBox(&host);
    box->setRange(0.0, 100000.0);
    box->setValue(2052.0);
    host.show();
    QObject owner;
    lila::ui::installSpinMath(&owner);
    QFocusEvent in(QEvent::FocusIn);
    QApplication::sendEvent(box, &in);
    auto* edit = box->findChild<QLineEdit*>();
    REQUIRE(edit);
    edit->setText("*=3");
    CHECK(edit->text() == "*=3");
    QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
    QApplication::sendEvent(box, &enter);
    CHECK(box->value() == doctest::Approx(6156.0));
}
