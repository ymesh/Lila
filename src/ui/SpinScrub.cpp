#include "ui/SpinScrub.h"

#include "ops/Undo.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Theme.h"
#include "ui/ScrubGesture.h"
#include "ui/ValueReset.h"

#include <QAbstractScrollArea>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCursor>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QSpinBox>
#include <QTimer>
#include <QToolTip>
#include <QVariantAnimation>

#include <cmath>
#include <memory>
#include <utility>

namespace lila::ui
{
namespace
{
constexpr double kStepsPerPreset = 4.0;

int64_t steadyNowMs()
{
    static QElapsedTimer clock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
    return clock.elapsed();
}

constexpr const char* kUsageHintProperty = "lilaUsageHint";

/** Shows a widget's own tooltip, then its usage hint and the reset hint set apart. */
void showTipWithHint(QEvent* e, QWidget* w, const QString& own, const QString& defaultText)
{
    QString hints = w->property(kUsageHintProperty).toString();
    hints = hints.isEmpty() ? resetHint(defaultText) : hints + "\n" + resetHint(defaultText);
    QToolTip::showText(static_cast<QHelpEvent*>(e)->globalPos(), hintedTip(own, hints), w);
}

QString spinText(QAbstractSpinBox* box, double v)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        return d->textFromValue(v) + d->suffix();
    if (auto* i = qobject_cast<QSpinBox*>(box))
        return QString::number(std::lround(v)) + i->suffix();
    return QString::number(v);
}

/** Value of a spin box as a double, whichever kind it is. */
double spinValue(QAbstractSpinBox* box)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        return d->value();
    if (auto* i = qobject_cast<QSpinBox*>(box))
        return i->value();
    return 0.0;
}

void setSpinValue(QAbstractSpinBox* box, double v)
{
    if (auto* d = qobject_cast<QDoubleSpinBox*>(box))
        d->setValue(v);
    else if (auto* i = qobject_cast<QSpinBox*>(box))
        i->setValue(int(std::lround(v)));
}

/**
 * The app-wide filter that gives every spin box the shared number-field drag (ScrubGesture), on the box and on its form
 * label, and ends typing on Enter, Esc or a click anywhere else.
 */
class SpinScrub : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        // As an app-wide filter it also sees each event on its way through the QWindow; eating the release there leaves
        // Qt's implicit mouse grab on the field, so the cursor stays captured until focus moves.
        if (!obj->isWidgetType())
            return false;
        switch (e->type())
        {
        case QEvent::Polish:
            // Qt focuses on press before any filter runs, which would turn every drag into text selection.
            if (auto* box = qobject_cast<QAbstractSpinBox*>(obj))
            {
                box->setFocusPolicy(Qt::TabFocus);
                for (QLineEdit* edit : box->findChildren<QLineEdit*>(Qt::FindDirectChildrenOnly))
                    edit->setFocusPolicy(Qt::TabFocus);
            }
            return false;
        case QEvent::MouseButtonPress:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            endTypingOutside(static_cast<QWidget*>(obj));
            if (mGesture)
                return mGesture->press(m);
            if (m->button() != Qt::LeftButton && m->button() != Qt::MiddleButton)
                return false;
            if (QWidget* choice = choiceOf(obj); choice && m->button() == Qt::MiddleButton)
            {
                mChoice = choice;
                mClick.press(int(m->globalPosition().x()), int(m->globalPosition().y()), steadyNowMs());
                return true;
            }
            // A plain label only scrubs its buddy; one that set its own cursor handles itself.
            if (qobject_cast<QLabel*>(obj) && !scrubsBuddy(obj))
                return false;
            QAbstractSpinBox* box = spinBoxOf(obj);
            if (!box || !box->isEnabled() || box->isReadOnly())
                return false;
            mGesture = std::make_unique<ScrubGesture>(targetFor(box, qobject_cast<QLabel*>(obj) != nullptr));
            if (mGesture->press(m))
                return true;
            mGesture.reset();
            return false;
        }
        case QEvent::MouseMove:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (mChoice)
                mClick.move(int(m->globalPosition().x()), int(m->globalPosition().y()));
            if (!mGesture)
                return false;
            const bool used = mGesture->move(m);
            if (!mGesture->pressed())
                mGesture.reset();
            return used;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (mChoice && m->button() == Qt::MiddleButton)
            {
                QPointer<QWidget> choice = mChoice;
                mChoice = nullptr;
                if (mClick.release(int(m->globalPosition().x()), int(m->globalPosition().y()), steadyNowMs()))
                    resetChoice(choice);
                return true;
            }
            if (!mGesture)
                return false;
            // The gesture may focus the field or reset it; drop it first so nothing re-enters it.
            std::unique_ptr<ScrubGesture> g = std::move(mGesture);
            const bool used = g->release(m);
            if (g->pressed())
                mGesture = std::move(g);
            return used;
        }
        case QEvent::KeyPress:
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (mGesture && mGesture->key(k))
            {
                mGesture.reset();
                return true;
            }
            QAbstractSpinBox* box = spinBoxOf(obj);
            if (!box || qobject_cast<QLabel*>(obj) || !box->hasFocus())
                return false;
            if (k->key() == Qt::Key_Escape)
            {
                // Puts the typed text back to the value, then leaves typing.
                setSpinValue(box, spinValue(box));
                box->clearFocus();
                return true;
            }
            if (k->key() == Qt::Key_Return || k->key() == Qt::Key_Enter)
                // After the box commits and a dialog's default button has its turn.
                QTimer::singleShot(0, box, [box] { box->clearFocus(); });
            return false;
        }
        case QEvent::Enter:
            scrubsBuddy(obj);
            return false;
        case QEvent::ToolTip:
            if (QWidget* choice = choiceOf(obj))
            {
                showTipWithHint(e, static_cast<QWidget*>(obj), static_cast<QWidget*>(obj)->toolTip(), choiceText(choice));
                return true;
            }
            if (QAbstractSpinBox* box = spinBoxOf(obj))
            {
                QVariant def = box->property(kDefaultProperty);
                if (!def.isValid())
                    return false;
                auto* label = qobject_cast<QLabel*>(obj);
                showTipWithHint(e, static_cast<QWidget*>(obj), label ? label->toolTip() : box->toolTip(), spinText(box, def.toDouble()));
                return true;
            }
            return false;
        default:
            return false;
        }
    }

private:
    ScrubGesture::Target targetFor(QAbstractSpinBox* box, bool fromLabel)
    {
        QPointer<QAbstractSpinBox> b = box;
        auto* d = qobject_cast<QDoubleSpinBox*>(box);
        ScrubGesture::Target t;
        t.field = box;
        t.snap = true;
        t.decimals = d ? d->decimals() : 0;
        t.integer = !d || d->decimals() == 0;
        t.step = [b] {
            if (auto* d = qobject_cast<QDoubleSpinBox*>(b.data()))
                return d->singleStep();
            if (auto* i = qobject_cast<QSpinBox*>(b.data()))
                return double(i->singleStep());
            return 1.0;
        };
        // A slider field's width covers its range, as its fill shows.
        t.span = [b] {
            auto* d = qobject_cast<QDoubleSpinBox*>(b.data());
            return d && d->property("role").toString() == "slider" ? d->maximum() - d->minimum() : 0.0;
        };
        t.begin = [this, b] {
            if (!b)
                return false;
            mStart = spinValue(b);
            mPresetsApplied = 0;
            mChunk = !scrubsWithoutUndoStep(b);
            if (mChunk)
                UndoStack::get().openChunk("Drag Value");
            return true;
        };
        t.drag = [this, b](double total, double snap) {
            if (!b)
                return;
            // Presets step like a choice: one per few steps of travel, so they can be picked one at a time.
            if (controls::stepPresets(b, 0))
            {
                const double step = std::max(1e-9, snap > 0.0 ? snap : 1.0);
                const int want = int(total / step / kStepsPerPreset);
                if (want != mPresetsApplied)
                {
                    controls::stepPresets(b, want - mPresetsApplied);
                    mPresetsApplied = want;
                }
                return;
            }
            double v = mStart + total;
            // Land on whole multiples of the step so values stay tidy.
            if (snap > 0.0)
                v = std::round(v / snap) * snap;
            setSpinValue(b, v);
        };
        t.end = [this, b](bool commit) {
            if (!commit && b && !controls::stepPresets(b, 0))
                setSpinValue(b, mStart);
            if (std::exchange(mChunk, false))
                commit ? UndoStack::get().closeChunk() : UndoStack::get().cancelChunk();
        };
        if (!fromLabel)
        {
            t.click = [b] {
                if (!b)
                    return;
                b->setFocus(Qt::MouseFocusReason);
                b->selectAll();
            };
            t.editing = [b] { return b && b->hasFocus(); };
        }
        if (box->property(kDefaultProperty).isValid())
            t.reset = [b] { resetToDefault(b); };
        return t;
    }

    /** A press outside the field being typed in ends the typing, as Enter would. */
    static void endTypingOutside(QWidget* pressed)
    {
        QWidget* focus = QApplication::focusWidget();
        if (!focus || !isScrubField(focus))
            return;
        QWidget* field = spinBoxOf(focus) ? spinBoxOf(focus) : focus;
        // Popups opened from the field (its context menu) and other windows leave it be.
        if (pressed == field || field->isAncestorOf(pressed) || pressed->window() != field->window())
            return;
        field->clearFocus();
    }

    static bool isScrubField(QWidget* w)
    {
        return spinBoxOf(w) || w->property(kScrubFieldProperty).toBool();
    }

    static QAbstractSpinBox* spinBoxOf(QObject* obj)
    {
        // A form row's label (its buddy is the field) scrubs and resets the field too.
        if (auto* label = qobject_cast<QLabel*>(obj))
            return qobject_cast<QAbstractSpinBox*>(label->buddy());
        // The press lands on the spin box's inner line edit as often as on the box itself.
        for (QObject* o = obj; o; o = o->parent())
            if (auto* box = qobject_cast<QAbstractSpinBox*>(o))
                return box;
            else if (o->isWidgetType() && static_cast<QWidget*>(o)->isWindow())
                break;
        return nullptr;
    }

    /**
     * A plain form label whose buddy is a spin box, which left-drags scrub; it gets the scrub cursor. Labels that set
     * their own cursor scrub by themselves.
     */
    static bool scrubsBuddy(QObject* obj)
    {
        auto* label = qobject_cast<QLabel*>(obj);
        if (!label || !qobject_cast<QAbstractSpinBox*>(label->buddy()))
            return false;
        if (label->property("lilaScrubsBuddy").toBool())
            return true;
        if (label->testAttribute(Qt::WA_SetCursor))
            return false;
        label->setProperty("lilaScrubsBuddy", true);
        label->setCursor(Qt::SizeHorCursor);
        return true;
    }

    /** A check box or combo box with a default, or the form label naming one as its buddy. */
    static QWidget* choiceOf(QObject* obj)
    {
        QWidget* w = nullptr;
        if (auto* label = qobject_cast<QLabel*>(obj))
            w = label->buddy();
        else if (obj->isWidgetType())
            w = static_cast<QWidget*>(obj);
        if (!w || !w->isEnabled() || !w->property(kDefaultProperty).isValid())
            return nullptr;
        return qobject_cast<QCheckBox*>(w) || qobject_cast<QComboBox*>(w) ? w : nullptr;
    }

    static QString choiceText(QWidget* w)
    {
        QVariant def = w->property(kDefaultProperty);
        if (auto* combo = qobject_cast<QComboBox*>(w))
            return combo->itemText(def.toInt());
        return def.toBool() ? "on" : "off";
    }

    static void resetChoice(QWidget* w)
    {
        if (!w)
            return;
        QVariant def = w->property(kDefaultProperty);
        if (auto* combo = qobject_cast<QComboBox*>(w))
            combo->setCurrentIndex(def.toInt());
        else if (auto* check = qobject_cast<QCheckBox*>(w))
            check->setChecked(def.toBool());
        flashReset(w);
    }

    static void resetToDefault(QAbstractSpinBox* box)
    {
        if (!box)
            return;
        QVariant def = box->property(kDefaultProperty);
        if (!def.isValid())
            return;
        setSpinValue(box, def.toDouble());
        flashReset(box);
    }

    std::unique_ptr<ScrubGesture> mGesture;
    double mStart = 0.0;
    int mPresetsApplied = 0;
    bool mChunk = false;
    QPointer<QWidget> mChoice;
    valueReset::ClickTracker mClick;
};

/** A fading light laid over a field. */
class Flash : public QWidget
{
public:
    Flash(QWidget* parent, QRect rect) : QWidget(parent)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(rect);
        auto* fade = new QVariantAnimation(this);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setDuration(350);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        connect(fade, &QVariantAnimation::valueChanged, this, [this](const QVariant& v) {
            mAlpha = v.toDouble();
            update();
        });
        connect(fade, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show();
        raise();
        fade->start();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.fillRect(rect(), themeShade(255, int(120 * mAlpha)));
    }

private:
    double mAlpha = 1.0;
};

class ResetClick : public QObject
{
public:
    ResetClick(QWidget* target, std::function<void()> reset, QString defaultText, QWidget* flash)
        : QObject(target), mReset(std::move(reset)), mDefault(std::move(defaultText)), mFlash(flash ? flash : target)
    {
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress:
            if (auto* m = static_cast<QMouseEvent*>(e); m->button() == Qt::MiddleButton)
                mClick.press(int(m->globalPosition().x()), int(m->globalPosition().y()), steadyNowMs());
            return false;
        case QEvent::MouseMove:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            mClick.move(int(m->globalPosition().x()), int(m->globalPosition().y()));
            return false;
        }
        case QEvent::MouseButtonRelease:
        {
            auto* m = static_cast<QMouseEvent*>(e);
            if (m->button() != Qt::MiddleButton || !mClick.down())
                return false;
            if (!mClick.release(int(m->globalPosition().x()), int(m->globalPosition().y()), steadyNowMs()))
                return false;
            QPointer<QWidget> flash = mFlash;
            mReset();
            if (flash)
                flashReset(flash);
            return true;
        }
        case QEvent::ToolTip:
            showTipWithHint(e, static_cast<QWidget*>(obj), static_cast<QWidget*>(obj)->toolTip(), mDefault);
            return true;
        default:
            return false;
        }
    }

private:
    std::function<void()> mReset;
    QString mDefault;
    QPointer<QWidget> mFlash;
    valueReset::ClickTracker mClick;
};
constexpr const char* kNoUndoStepProperty = "lilaScrubNoUndoStep";
} // namespace

void setScrubsWithoutUndoStep(QWidget* w)
{
    w->setProperty(kNoUndoStepProperty, true);
}

bool scrubsWithoutUndoStep(const QWidget* w)
{
    for (; w; w = w->parentWidget())
        if (w->property(kNoUndoStepProperty).toBool())
            return true;
    return false;
}

void installSpinScrub(QObject* parent)
{
    qApp->installEventFilter(new SpinScrub(parent));
}

void setSpinDefault(QAbstractSpinBox* box, double value)
{
    box->setProperty(kDefaultProperty, value);
}

void setCheckDefault(QCheckBox* box, bool on)
{
    box->setProperty(kDefaultProperty, on);
}

void setComboDefault(QComboBox* box, int index)
{
    if (index >= 0)
        box->setProperty(kDefaultProperty, index);
}

QString resetHint(const QString& defaultText)
{
    return QString("Middle-click to reset to default (%1)").arg(defaultText);
}

QString hintedTip(const QString& own, const QString& hints)
{
    auto html = [](const QString& plain) { return plain.toHtmlEscaped().replace('\n', "<br>"); };
    QPalette pal = QToolTip::palette();
    QColor text = pal.color(QPalette::ToolTipText), base = pal.color(QPalette::ToolTipBase);
    auto mix = [](float a, float b) { return a + (b - a) * 0.6f; };
    QColor dim = QColor::fromRgbF(mix(base.redF(), text.redF()), mix(base.greenF(), text.greenF()), mix(base.blueF(), text.blueF()));
    QString tail = QString("<span style=\"color:%1; font-style:italic\">%2</span>").arg(dim.name(), html(hints));
    // No raw newline anywhere: Qt only reads a tooltip as rich text when a tag sits on its first line.
    return own.isEmpty() ? tail : html(own) + "<br>" + tail;
}

void setUsageHint(QWidget* w, const QString& hint)
{
    w->setProperty(kUsageHintProperty, hint);
}

void flashReset(QWidget* w, QRect rect)
{
    if (!w)
        return;
    // A scroll area paints its content in the viewport, which would cover a flash laid on the frame.
    if (auto* area = qobject_cast<QAbstractScrollArea*>(w); area && rect.isNull())
        w = area->viewport();
    new Flash(w, rect.isNull() ? w->rect() : rect);
}

void onMiddleClickReset(QWidget* target, std::function<void()> reset, const QString& defaultText, QWidget* flash)
{
    target->installEventFilter(new ResetClick(target, std::move(reset), defaultText, flash));
}

void addCopyActions(QMenu* menu, const QString& name, std::function<nlohmann::json()> value)
{
    // Strings copy bare so a path or enum name pastes as itself.
    auto text = [value] {
        nlohmann::json v = value();
        return QString::fromStdString(v.is_string() ? v.get<std::string>() : v.dump());
    };
    menu->addAction(icon("copy"), "Copy Name", menu, [name] { QApplication::clipboard()->setText(name); });
    menu->addAction("Copy Value", menu, [text] { QApplication::clipboard()->setText(text()); });
    menu->addAction("Copy Name and Value", menu, [name, text] { QApplication::clipboard()->setText(name + " = " + text()); });
}

void onRightClickCopy(QWidget* target, const QString& name, std::function<nlohmann::json()> value)
{
    target->setContextMenuPolicy(Qt::CustomContextMenu);
    QObject::connect(target, &QWidget::customContextMenuRequested, target, [target, name, value](const QPoint& at) {
        QMenu menu;
        addCopyActions(&menu, name, value);
        menu.exec(target->mapToGlobal(at));
    });
}
} // namespace lila::ui
