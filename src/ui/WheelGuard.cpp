#include "ui/WheelGuard.h"

#include "ops/Command.h"

#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QScrollBar>
#include <QTimer>
#include <QWidget>

namespace lila::ui
{
namespace
{
bool guarded(QObject* obj)
{
    if (qobject_cast<QAbstractSpinBox*>(obj) || qobject_cast<QComboBox*>(obj))
        return true;
    return qobject_cast<QAbstractSlider*>(obj) && !qobject_cast<QScrollBar*>(obj);
}

/** Qt focuses a WheelFocus widget the wheel lands on before any filter sees the event, so such fields lose WheelFocus. */
void dropWheelFocus(QWidget* w)
{
    if ((w->focusPolicy() & Qt::WheelFocus) == Qt::WheelFocus)
        w->setFocusPolicy(Qt::StrongFocus);
}

/** How long after the last wheel notch a scroll still counts as the same edit. */
constexpr int kScrollIdleMs = 800;

class WheelGuard : public QObject
{
public:
    explicit WheelGuard(QObject* parent) : QObject(parent)
    {
        mIdle.setSingleShot(true);
        mIdle.setInterval(kScrollIdleMs);
        connect(&mIdle, &QTimer::timeout, this, [] { ops::setCoalesceKey({}); });
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        // Show too: a field polished before the guard was installed never sends Polish again.
        if ((e->type() == QEvent::Polish || e->type() == QEvent::Show) && guarded(obj))
            dropWheelFocus(static_cast<QWidget*>(obj));
        else if (e->type() == QEvent::Wheel)
        {
            // Scrolling anywhere else ends a field's hold on the wheel, so the page scrolling past it leaves it alone.
            QWidget* focused = QApplication::focusWidget();
            auto* w = qobject_cast<QWidget*>(obj);
            if (focused && guarded(focused) && w && focused != w && !focused->isAncestorOf(w))
                focused->clearFocus();
            if (guarded(obj) && !static_cast<QWidget*>(obj)->hasFocus())
            {
                // Ignored and filtered, the event goes on up to the parent, the panel's scroll area.
                e->ignore();
                return true;
            }
            if (guarded(obj))
                markScrolled(static_cast<QWidget*>(obj));
        }
        return false;
    }

private:
    /**
     * The field takes the wheel: its edits fold into one undo step until the wheel rests (ops::setCoalesceKey). The key is
     * where the field sits on screen, since panels often rebuild their fields after each edit and the same place is the
     * same field.
     */
    void markScrolled(QWidget* field)
    {
        const QRect r(field->mapToGlobal(QPoint(0, 0)), field->size());
        ops::setCoalesceKey(QString("wheel:%1,%2,%3,%4").arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()).toStdString());
        mIdle.start();
    }

    QTimer mIdle;
};
} // namespace

void installWheelGuard(QObject* parent)
{
    qApp->installEventFilter(new WheelGuard(parent));
    for (QWidget* w : QApplication::allWidgets())
        if (guarded(w))
            dropWheelFocus(w);
}
} // namespace lila::ui
