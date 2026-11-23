#include "ui/AttributeEditor.h"
#include "ui/AttrAuthoring.h"
#include "ui/AttrView.h"
#include "ui/ArrayTable.h"
#include "ui/ChannelBox.h"
#include "ui/ChoiceTips.h"
#include "ui/Controls.h"
#include "ui/DrivePopover.h"
#include "ui/DocsUi.h"
#include "core/PrefStore.h"
#include "scene/AttrComponents.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Workspace.h"
#include "scene/Diagnostics.h"
#include "scene/Graph.h"
#include "scene/Light.h"
#include "ui/ColorPopup.h"
#include "ui/Deletion.h"
#include "ui/FlowLayout.h"
#include "ui/FontPicker.h"
#include "ui/Icons.h"
#include "ui/ImagePicker.h"
#include "ui/Issues.h"
#include "ui/CodeEdit.h"
#include "ui/MultilineEdit.h"
#include "ui/NumberExpr.h"
#include "ui/SceneLights.h"
#include "ui/ScrubGesture.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ValueReset.h"
#include "ui/FileDialog.h"

#include <QGuiApplication>
#include <QApplication>
#include <QDrag>
#include <QButtonGroup>
#include <QRadioButton>
#include <QMimeData>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QKeyEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <memory>
#include <set>
#include <cmath>
#include <filesystem>

namespace lila::ui
{
namespace
{
using scene::Graph;
using scene::PlugType;
namespace dx = scene::diagnostics;

std::vector<AttributeEditor*> gEditors;
/** An issue to scroll to once an editor shows its object. */
uint64_t gPendingReveal = 0;

/** Secondary text: dim while enabled, the theme's disabled colour when the row is switched off. */
void aeDim(QWidget* w, const char* token = "dimText")
{
    QPalette pal = w->palette();
    for (QPalette::ColorGroup group : {QPalette::Active, QPalette::Inactive})
        pal.setColor(group, QPalette::WindowText, themeColor(token));
    w->setPalette(pal);
}

void aeBold(QWidget* w)
{
    QFont f = w->font();
    f.setWeight(QFont::DemiBold);
    w->setFont(f);
}

QFont aeSmallFont(const QFont& base)
{
    QFont f = base;
    f.setPixelSize(11);
    return f;
}

/**
 * A row label that elides instead of holding its full width, so a narrow editor squeezes names before fields. An issue
 * on its attribute puts a badge before the name.
 */
class AttrRowLabel final : public QLabel
{
public:
    explicit AttrRowLabel(const QString& text) : QLabel(text)
    {
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        aeDim(this);
    }
    QSize minimumSizeHint() const override { return {fontMetrics().horizontalAdvance("Wwwww"), QLabel::minimumSizeHint().height()}; }
    void setMark(QPixmap mark)
    {
        mMark = std::move(mark);
        update();
    }
    /** A child pinned to the label's left end, after the ones already there; starts hidden. */
    void addLeading(QWidget* w)
    {
        w->hide();
        mLeading.push_back(w);
        placeLeading();
    }

protected:
    void resizeEvent(QResizeEvent* e) override
    {
        QLabel::resizeEvent(e);
        placeLeading();
    }
    void placeLeading()
    {
        int x = 0;
        for (QWidget* w : mLeading)
        {
            const QSize s = w->sizeHint();
            w->setGeometry(x, (height() - s.height()) / 2, s.width(), s.height());
            x += s.width();
        }
    }
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        QRect r = contentsRect();
        const int gap = mMark.isNull() ? 0 : mMark.width() + 4;
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, r.width() - gap);
        if (!mMark.isNull())
        {
            const int textWidth = fontMetrics().horizontalAdvance(shown);
            const int x = (alignment() & Qt::AlignRight) ? r.right() - textWidth - gap + 1 : r.left();
            p.drawPixmap(x, r.center().y() - mMark.height() / 2, mMark);
            if (!(alignment() & Qt::AlignRight))
                r.setLeft(r.left() + gap);
        }
        style()->drawItemText(&p, r, int(alignment()), palette(), isEnabled(), shown, foregroundRole());
    }

private:
    QPixmap mMark;
    std::vector<QWidget*> mLeading;
};

/** The right end of a row label: a drag there moves every label's edge, a double-click puts it back to auto. */
class LabelEdgeDrag final : public QObject
{
public:
    static constexpr int kGrab = 6;

    LabelEdgeDrag(QLabel* on, std::function<void(int x, bool commit)> dragged, std::function<void()> reset)
        : QObject(on), mOn(on), mDragged(std::move(dragged)), mReset(std::move(reset))
    {
        on->setMouseTracking(true);
        on->installEventFilter(this);
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseMove:
        {
            auto* me = static_cast<QMouseEvent*>(e);
            if (mDragging)
            {
                mDragged(edgeX(me) + mOffset, false);
                return true;
            }
            if (onEdge(me) != mHover)
            {
                mHover = !mHover;
                if (mHover)
                    mOn->setCursor(Qt::SplitHCursor);
                else
                    mOn->unsetCursor();
            }
            return false;
        }
        case QEvent::Leave:
            if (mHover && !mDragging)
            {
                mHover = false;
                mOn->unsetCursor();
            }
            return false;
        case QEvent::MouseButtonPress:
        {
            auto* me = static_cast<QMouseEvent*>(e);
            if (me->button() != Qt::LeftButton || !onEdge(me))
                return false;
            mDragging = true;
            mOffset = mOn->width() - int(me->position().x());
            return true;
        }
        case QEvent::MouseButtonRelease:
            if (!mDragging)
                return false;
            mDragging = false;
            mDragged(edgeX(static_cast<QMouseEvent*>(e)) + mOffset, true);
            return true;
        case QEvent::MouseButtonDblClick:
            if (!onEdge(static_cast<QMouseEvent*>(e)))
                return false;
            mReset();
            return true;
        default: return false;
        }
    }

private:
    bool onEdge(QMouseEvent* me) const { return me->position().x() >= mOn->width() - kGrab; }
    /** The pointer's x in the scrolled body: the label's parents up to it. */
    int edgeX(QMouseEvent* me) const
    {
        QWidget* body = mOn->window();
        for (QWidget* w = mOn; w; w = w->parentWidget())
            if (w->objectName() == "aeBody")
            {
                body = w;
                break;
            }
        return mOn->mapTo(body, me->position().toPoint()).x();
    }

    QLabel* mOn;
    std::function<void(int, bool)> mDragged;
    std::function<void()> mReset;
    bool mHover = false;
    bool mDragging = false;
    /** How far right of the pointer the edge sat when the drag began, so it moves without jumping. */
    int mOffset = 0;
};

constexpr const char* kAttrMime = "application/x-lila-attr";
constexpr const char* kHeadingMime = "application/x-lila-heading";
constexpr const char* kPlugMime = "application/x-lila-plug";

/** Drag, drop, double-click and hover hooks on one widget, for the editor's rows and headings. */
class RowGestures final : public QObject
{
public:
    explicit RowGestures(QWidget* on) : QObject(on) { on->installEventFilter(this); }

    std::function<void()> doubleClicked;
    /** A left press and release without a drag. */
    std::function<void()> clicked;
    /** Starts a drag of this mime type and text from a press and move; empty for none. */
    QString dragMime, dragText;
    std::function<bool(const QMimeData*)> accepts;
    std::function<void(const QMimeData*)> dropped;
    /** Shown only while the pointer is over the widget. */
    std::vector<QWidget*> reveal;

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        auto* w = static_cast<QWidget*>(o);
        switch (e->type())
        {
        case QEvent::Enter:
        case QEvent::Leave:
            for (QWidget* r : reveal)
                r->setVisible(e->type() == QEvent::Enter);
            break;
        case QEvent::MouseButtonDblClick:
            if (doubleClicked && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
            {
                doubleClicked();
                return true;
            }
            break;
        case QEvent::MouseButtonPress:
            mPress = static_cast<QMouseEvent*>(e)->pos();
            mDown = static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton;
            mPressed = mDown && !dragMime.isEmpty();
            break;
        case QEvent::MouseMove:
            if (mPressed && (static_cast<QMouseEvent*>(e)->pos() - mPress).manhattanLength() >= QApplication::startDragDistance())
            {
                mPressed = mDown = false;
                auto* data = new QMimeData;
                data->setData(dragMime, dragText.toUtf8());
                auto* drag = new QDrag(w);
                drag->setMimeData(data);
                drag->setPixmap(w->grab());
                drag->exec(Qt::MoveAction);
                return true;
            }
            break;
        case QEvent::MouseButtonRelease:
            if (mDown && clicked && static_cast<QMouseEvent*>(e)->button() == Qt::LeftButton)
            {
                mDown = mPressed = false;
                clicked();
                return true;
            }
            mPressed = mDown = false;
            break;
        case QEvent::DragEnter:
        case QEvent::DragMove:
        {
            auto* d = static_cast<QDropEvent*>(e);
            if (accepts && accepts(d->mimeData()))
            {
                d->acceptProposedAction();
                return true;
            }
            break;
        }
        case QEvent::Drop:
        {
            auto* d = static_cast<QDropEvent*>(e);
            if (accepts && dropped && accepts(d->mimeData()))
            {
                d->acceptProposedAction();
                dropped(d->mimeData());
                return true;
            }
            break;
        }
        default: break;
        }
        return false;
    }

private:
    QPoint mPress;
    bool mPressed = false;
    bool mDown = false;
};

std::string mimeText(const QMimeData* d, const char* format) { return QString::fromUtf8(d->data(format)).toStdString(); }

/** Marks a label or section header with the worst of the issues and lists them all in its tooltip. */
void markIssues(QWidget* w, const std::vector<const dx::Issue*>& list)
{
    std::optional<issues::Worst> worst = issues::worst(list);
    if (!worst)
        return;
    w->setToolTip(w->toolTip() + (w->toolTip().isEmpty() ? "" : "\n\n") + issues::lines(list));
    if (auto* label = dynamic_cast<AttrRowLabel*>(w))
        label->setMark(issues::badge(worst->severity, worst->blocking, label->fontMetrics().height() - 4));
    else
        aeDim(w, worst->severity == dx::Severity::Error ? "issueError" : worst->severity == dx::Severity::Warning ? "issueWarning" : "issueInfo");
}

/** A number a scrub drives: its plug, its component of a vector (-1 for a scalar) and the attr name setAttr takes. */
struct ScrubTarget
{
    scene::PlugRef ref;
    int component = -1;
    std::string attr;
    std::optional<double> minValue, maxValue;
    bool isInt = false;
};

double numberOf(const scene::PlugValue& v, int component)
{
    if (auto* v3 = std::get_if<glm::dvec3>(&v); v3 && component >= 0)
        return (*v3)[component];
    if (auto* f = std::get_if<double>(&v))
        return *f;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    return 0.0;
}

/**
 * The shared number-field drag (ScrubGesture) on an Attribute Editor field or row label: the drag previews its targets
 * live and the release commits them through setAttr as one undo step. A step is 1 (Shift 10x, Ctrl 0.1x); a ranged field
 * (span) covers its range across its width and stays inside it. On a field a left click starts typing with the value
 * selected; a label drags with either button.
 */
class NumberScrub final : public QObject
{
public:
    using Commit = std::function<void(const std::string& attr, const json& value)>;

    NumberScrub(QWidget* on, std::vector<ScrubTarget> targets, Commit commit, QLineEdit* field = nullptr,
                std::optional<std::pair<double, double>> span = std::nullopt)
        : QObject(on), mTargets(std::move(targets)), mCommit(std::move(commit)), mField(field), mSpan(span)
    {
        on->installEventFilter(this);
        ScrubGesture::Target t;
        t.field = field ? static_cast<QWidget*>(field) : on;
        t.step = [] { return 1.0; };
        t.span = [this] { return mSpan && mField ? mSpan->second - mSpan->first : 0.0; };
        t.integer = std::all_of(mTargets.begin(), mTargets.end(), [](const ScrubTarget& s) { return s.isInt; });
        t.decimals = 4;
        t.begin = [this] { return begin(); };
        t.drag = [this](double total, double) { drag(total); };
        t.end = [this](bool commit) { end(commit); };
        if (field)
        {
            QPointer<QLineEdit> f = field;
            t.click = [f] {
                if (!f || f->isReadOnly())
                    return;
                f->setFocus(Qt::MouseFocusReason);
                f->selectAll();
            };
            t.editing = [f] { return f && f->hasFocus(); };
        }
        mGesture = std::make_unique<ScrubGesture>(std::move(t));
    }

protected:
    bool eventFilter(QObject*, QEvent* e) override
    {
        switch (e->type())
        {
        case QEvent::MouseButtonPress: return mGesture->press(static_cast<QMouseEvent*>(e));
        case QEvent::MouseMove: return mGesture->move(static_cast<QMouseEvent*>(e));
        case QEvent::MouseButtonRelease: return mGesture->release(static_cast<QMouseEvent*>(e));
        case QEvent::KeyPress: return mGesture->key(static_cast<QKeyEvent*>(e));
        default: return false;
        }
    }

private:
    bool begin()
    {
        Graph& g = Graph::current();
        mSaved.clear();
        for (const ScrubTarget& t : mTargets)
            if (g.node(t.ref.node) && !g.isLocked(t.ref))
                mSaved.push_back({t, g.value(t.ref)});
        if (mSaved.empty())
            return false;
        mAccum = 0.0;
        UndoStack::get().openChunk("Drag Attribute");
        return true;
    }

    void drag(double total)
    {
        mAccum = total;
        Graph& g = Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const auto& [t, start] : mSaved)
            if (g.node(t.ref.node))
                g.setValue(t.ref, withComponent(g.value(t.ref), t, valueFor(t, start)));
        u.setEnabled(was);
    }

    void end(bool commit)
    {
        Graph& g = Graph::current();
        UndoStack& u = UndoStack::get();
        const bool was = u.enabled();
        u.setEnabled(false);
        for (const auto& [t, start] : mSaved)
            if (g.node(t.ref.node))
                g.setValue(t.ref, start);
        u.setEnabled(was);
        // Inside the chunk opened when the drag started, so the drag undoes as one step.
        if (commit && mAccum != 0.0)
            for (const auto& [t, start] : mSaved)
            {
                const double v = valueFor(t, start);
                mCommit(t.attr, t.isInt ? json(int64_t(std::llround(v))) : json(std::round(v * 1e4) / 1e4));
            }
        UndoStack::get().closeChunk();
        mSaved.clear();
    }

    double valueFor(const ScrubTarget& t, const scene::PlugValue& start) const
    {
        const double from = numberOf(start, t.component);
        double v = from + mAccum;
        // A ranged field's drag stays in its range, unless the value already sat outside it.
        if (mSpan && mField && from >= mSpan->first && from <= mSpan->second)
            v = std::clamp(v, mSpan->first, mSpan->second);
        if (t.minValue)
            v = std::max(v, *t.minValue);
        if (t.maxValue)
            v = std::min(v, *t.maxValue);
        return t.isInt ? std::round(v) : v;
    }

    static scene::PlugValue withComponent(scene::PlugValue current, const ScrubTarget& t, double v)
    {
        if (auto* v3 = std::get_if<glm::dvec3>(&current); v3 && t.component >= 0)
            (*v3)[t.component] = v;
        else if (std::holds_alternative<int64_t>(current))
            current = int64_t(std::llround(v));
        else
            current = v;
        return current;
    }

    std::vector<ScrubTarget> mTargets;
    std::vector<std::pair<ScrubTarget, scene::PlugValue>> mSaved;
    Commit mCommit;
    QLineEdit* mField = nullptr;
    std::optional<std::pair<double, double>> mSpan;
    double mAccum = 0.0;
    std::unique_ptr<ScrubGesture> mGesture;
};

/** The Channel Box's channel-state hues, so a state reads the same in both panels. */
QColor aeStateColor(uint32_t state)
{
    using CB = ChannelBox;
    if (state & CB::kMuted)
        return QColor(154, 123, 82);
    if (state & CB::kBreakdown)
        return QColor(91, 180, 107);
    if (state & CB::kKeyed)
        return QColor(205, 91, 91);
    if (state & CB::kDrivenKey)
        return QColor(91, 143, 214);
    if (state & CB::kExpression)
        return QColor(167, 123, 214);
    if (state & CB::kConstraint)
        return QColor(107, 176, 200);
    if (state & CB::kConnected)
        return QColor(214, 194, 91);
    if (state & CB::kLocked)
        return QColor(138, 138, 138);
    return {};
}

/** The shape each driven or locked state carries besides its hue; keyed and muted use the edge bar instead. */
const char* aeStateGlyph(uint32_t state)
{
    using CB = ChannelBox;
    if (state & CB::kLocked)
        return "lock";
    if (state & CB::kConstraint)
        return "anchor";
    if (state & CB::kExpression)
        return "math-function";
    if (state & (CB::kConnected | CB::kDrivenKey))
        return "link";
    return nullptr;
}

/**
 * A number field: value right-aligned, unit dim at the right end, an axis letter at the left for a vector's component.
 * A ranged one shows its value as a fill across that range. Channel states show as a 4 px edge bar (keyed, muted) or a
 * glyph (locked, connected, constrained), each in the Channel Box's hue; muted also strikes the value.
 */
class NumberField final : public QLineEdit
{
public:
    NumberField(std::optional<std::pair<double, double>> span, QString unit = {}, char axis = 0)
        : mSpan(span), mUnit(std::move(unit)), mAxis(axis)
    {
        setCursor(Qt::SizeHorCursor);
        // Focus only from a click without a drag (NumberScrub) or Tab, so a press can start a drag.
        setFocusPolicy(Qt::TabFocus);
        setProperty(kScrubFieldProperty, true);
        setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        setAttribute(Qt::WA_Hover);
        setMinimumHeight(24);
        placeMarks();
    }

    void showValue(double v)
    {
        if (mSpan && mSpan->second > mSpan->first)
        {
            const double f = std::clamp((v - mSpan->first) / (mSpan->second - mSpan->first), 0.0, 1.0);
            if (f != mFill)
            {
                mFill = f;
                update();
            }
        }
    }

    void setState(uint32_t state)
    {
        if (state == mState)
            return;
        mState = state;
        placeMarks();
        update();
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        if (hasFocus())
            QLineEdit::paintEvent(e);
        else
            paintResting();
        paintMarks();
    }
    void focusInEvent(QFocusEvent* e) override
    {
        setCursor(Qt::IBeamCursor);
        mBefore = text();
        QLineEdit::focusInEvent(e);
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
        {
            setText(mBefore);
            setModified(false);
            clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(e);
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            clearFocus();
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        setCursor(Qt::SizeHorCursor);
        QLineEdit::focusOutEvent(e);
    }

private:
    static constexpr int kPad = 7;

    int glyphRoom() const { return aeStateGlyph(mState) ? 14 : 0; }
    int axisRoom() const { return mAxis ? fontMetrics().horizontalAdvance(QChar(mAxis)) + 5 : 0; }

    void placeMarks()
    {
        const int right = mUnit.isEmpty() ? 0 : QFontMetrics(aeSmallFont(font())).horizontalAdvance(mUnit) + 3;
        setTextMargins(axisRoom() + glyphRoom(), 0, right, 0);
    }

    /** The field at rest, drawn here so the range fill sits under the value rather than over it. */
    void paintResting()
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        p.setClipPath(clip);
        if (!isReadOnly())
            p.fillRect(rect(), underMouse() && isEnabled() ? themeColor("hover") : themeColor("input"));
        if (mSpan && mFill > 0.0)
        {
            QColor fill = themeColor("sliderFill");
            if (isReadOnly() || !isEnabled())
                fill.setAlphaF(0.5f);
            p.fillRect(QRectF(0, 0, width() * mFill, height()), fill);
        }
        p.setClipping(false);
        const QMargins m = textMargins();
        const QRect r = rect().adjusted(kPad + m.left(), 0, -kPad - m.right(), 0);
        QFont f = font();
        f.setStrikeOut((mState & ChannelBox::kMuted) != 0);
        p.setFont(f);
        p.setPen(isEnabled() ? palette().color(QPalette::Text) : themeColor("disabled"));
        p.drawText(r, Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(f).elidedText(text(), Qt::ElideLeft, r.width()));
    }

    void paintMarks()
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QColor state = aeStateColor(mState);
        if (state.isValid() && (mState & (ChannelBox::kKeyed | ChannelBox::kMuted)))
            p.fillRect(QRect(0, 2, 4, height() - 4), state);
        int x = kPad;
        if (mAxis)
        {
            QFont f = font();
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            const char* token = mAxis == 'X' ? "axisX" : mAxis == 'Y' ? "axisY" : "axisZ";
            p.setPen(isEnabled() ? themeColor(token) : themeColor("disabled"));
            p.drawText(QRect(x, 0, axisRoom(), height()), Qt::AlignLeft | Qt::AlignVCenter, QString(QChar(mAxis)));
            x += axisRoom();
        }
        if (const char* glyph = aeStateGlyph(mState))
        {
            const qreal dpr = devicePixelRatioF();
            QPixmap pm = icon(glyph).pixmap(QSize(12, 12), dpr);
            {
                QPainter tint(&pm);
                tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
                tint.fillRect(pm.rect(), state);
            }
            p.drawPixmap(x, (height() - 12) / 2, pm);
        }
        if (!mUnit.isEmpty())
        {
            p.setFont(aeSmallFont(font()));
            p.setPen(themeColor(isEnabled() ? "dimText" : "disabled"));
            p.drawText(rect().adjusted(0, 0, -kPad, 0), Qt::AlignRight | Qt::AlignVCenter, mUnit);
        }
    }

    std::optional<std::pair<double, double>> mSpan;
    QString mUnit;
    char mAxis = 0;
    double mFill = 0.0;
    uint32_t mState = 0;
    QString mBefore;
};

/** The range a field's fill spans: the soft range, else the hard one; none unless both ends are known. */
std::optional<std::pair<double, double>> fillSpan(const scene::AttrDef& def)
{
    const std::optional<double> lo = def.softMin ? def.softMin : def.minValue, hi = def.softMax ? def.softMax : def.maxValue;
    if (!lo || !hi || *hi <= *lo)
        return std::nullopt;
    return std::pair{*lo, *hi};
}

QString fmt(double v)
{
    if (std::abs(v) < 5e-7)
        v = 0.0;
    QString s = QString::number(v, 'f', 4);
    while (s.contains('.') && (s.endsWith('0') || s.endsWith('.')))
        s.chop(1);
    return s;
}

/** Folder a file browser opens in for a stored path. */
QString browseStart(const std::string& stored)
{
    namespace fs = std::filesystem;
    fs::path p = paths::resolvePath(stored);
    std::error_code ec;
    if (!stored.empty() && fs::is_directory(p.parent_path(), ec))
        return QString::fromStdU16String(p.parent_path().u16string());
    return Workspace::get().isOpen() ? QString::fromStdU16String(Workspace::get().root().u16string()) : QString();
}

/** The unit a number shows inside its field, read from the attr's doc ("in degrees", "object space, cm"). */
QString aeUnitOf(const scene::AttrDef& def)
{
    if (def.type != PlugType::Float && def.type != PlugType::Vec3)
        return {};
    static const QRegularExpression degrees(R"(\bdegrees?\b)", QRegularExpression::CaseInsensitiveOption), cm(R"(\bcm\b)");
    const QString doc = QString::fromStdString(def.doc);
    if (def.name == "rotate" || def.name == "rotateAxis" || def.name == "jointOrient" || doc.contains(degrees))
        return QString(QChar(0x00b0));
    if (def.name == "translate" || doc.contains(cm))
        return "cm";
    return {};
}

/** An enum value as users read it: "boundingBoxCenter" as "Bounding Box Center", axis names upper case. */
QString aeEnumLabel(const std::string& name)
{
    if (name.empty())
        return {};
    if (name.find_first_not_of("xyzXYZ") == std::string::npos)
        return QString::fromStdString(name).toUpper();
    bool acronym = name.find(' ') != std::string::npos;
    for (size_t i = 1; i < name.size() && !acronym; ++i)
        acronym = std::isupper(static_cast<unsigned char>(name[i])) && std::isupper(static_cast<unsigned char>(name[i - 1]));
    if (!acronym)
        return attrView::niceName(name);
    QString out = QString::fromStdString(name);
    out[0] = out[0].toUpper();
    return out;
}

std::vector<std::string> aeEnumLabels(const scene::AttrDef& def)
{
    std::vector<std::string> out;
    for (const std::string& e : def.enumNames)
        out.push_back(aeEnumLabel(e).toStdString());
    return out;
}

/** A USD API schema as a heading: "PhysicsRigidBodyAPI" as "Physics Rigid Body", "CollectionAPI:lights" as "Collection Lights". */
QString aeSchemaLabel(const std::string& schema)
{
    std::string base = schema, instance;
    if (size_t colon = schema.find(':'); colon != std::string::npos)
    {
        base = schema.substr(0, colon);
        instance = schema.substr(colon + 1);
    }
    if (base.size() > 3 && base.ends_with("API"))
        base.resize(base.size() - 3);
    QString out = attrView::niceName(base);
    if (!instance.empty())
        out += " " + attrView::niceName(instance);
    return out;
}

/** What a bool switches by name: enableFoo, useFoo and fooEnabled switch the foo attrs; a plain foo switches fooBar. */
std::string aeSwitchStem(const std::string& name)
{
    for (std::string_view pre : {std::string_view("enable"), std::string_view("use")})
        if (name.size() > pre.size() && name.starts_with(pre) && std::isupper(static_cast<unsigned char>(name[pre.size()])))
        {
            std::string stem = name.substr(pre.size());
            stem[0] = char(std::tolower(static_cast<unsigned char>(stem[0])));
            return stem;
        }
    if (name.size() > 7 && name.ends_with("Enabled"))
        return name.substr(0, name.size() - 7);
    return name;
}

/** True when the bool sw turns def on and off (def does nothing while sw is off). */
bool aeSwitches(const scene::AttrDef& sw, const scene::AttrDef& def)
{
    if (sw.type != PlugType::Bool || (sw.flags & (scene::AttrHidden | scene::AttrArray)) || !(sw.flags & scene::AttrInput) ||
        sw.name == def.name || def.name.find('.') != std::string::npos)
        return false;
    const std::string stem = aeSwitchStem(sw.name);
    return def.name.size() > stem.size() && def.name.starts_with(stem) && std::isupper(static_cast<unsigned char>(def.name[stem.size()]));
}

/** A section: its box (header row over content), the header that folds it, and the header row extras go on. */
struct AeSection
{
    QWidget* box = nullptr;
    QToolButton* header = nullptr;
    QHBoxLayout* head = nullptr;
};

constexpr const char* kHoverOnly = "aeHoverOnly";

/**
 * A collapsible section: a text header over content, split from the section above by the section rule. An issue on its
 * component puts the issue's badge (a shape as well as a hue) in the header.
 */
AeSection aeSection(const QString& title, QWidget* content, const std::vector<const dx::Issue*>& marks = {})
{
    AeSection s;
    s.box = new QWidget;
    s.box->setAttribute(Qt::WA_StyledBackground);
    controls::setRole(s.box, "section");
    auto* v = new QVBoxLayout(s.box);
    v->setContentsMargins(0, 2, 0, 8);
    v->setSpacing(2);
    auto* head = new QWidget(s.box);
    head->setObjectName("aeSectionHead");
    s.head = new QHBoxLayout(head);
    s.head->setContentsMargins(0, 0, 6, 0);
    s.head->setSpacing(4);
    s.header = new QToolButton(head);
    s.header->setObjectName("aeSectionHeader");
    s.header->setText(title);
    s.header->setCheckable(true);
    s.header->setChecked(true);
    s.header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    s.header->setIcon(icon("chevron-down"));
    s.header->setIconSize(QSize(14, 14));
    s.header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controls::setRole(s.header, "sectionHeader");
    s.head->addWidget(s.header, 1);
    if (std::optional<issues::Worst> worst = issues::worst(marks))
    {
        auto* badge = new QLabel(head);
        badge->setObjectName("aeSectionIssue");
        const int h = badge->fontMetrics().height() - 2;
        badge->setPixmap(issues::badge(worst->severity, worst->blocking, h));
        badge->setToolTip(issues::lines(marks));
        s.head->addWidget(badge);
        s.header->setToolTip(issues::lines(marks));
    }
    QObject::connect(s.header, &QToolButton::toggled, content, [header = s.header, content](bool on) {
        content->setVisible(on);
        header->setIcon(icon(on ? "chevron-down" : "chevron-right"));
    });
    v->addWidget(head);
    v->addWidget(content);
    return s;
}

/** Puts the title on title (a header checkbox, a draggable component name) beside the chevron; header->text() keeps it for search. */
void aeTitleOn(const AeSection& s, QWidget* title, QWidget* tag = nullptr)
{
    s.header->setToolButtonStyle(Qt::ToolButtonIconOnly);
    s.header->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    s.head->setStretch(0, 0);
    s.head->insertWidget(1, title);
    int at = 2;
    if (tag)
        s.head->insertWidget(at++, tag);
    s.head->insertStretch(at, 1);
}

QToolButton* aeHeaderOf(QWidget* box)
{
    QWidget* head = box ? box->findChild<QWidget*>("aeSectionHead", Qt::FindDirectChildrenOnly) : nullptr;
    return head ? head->findChild<QToolButton*>("aeSectionHeader", Qt::FindDirectChildrenOnly) : nullptr;
}

std::vector<attributeEditor::Section*> extraSections()
{
    return attributeEditor::registry().allInOrder();
}
} // namespace

Registry<attributeEditor::AttrFilter>& attributeEditor::filters()
{
    static Registry<AttrFilter>& r = []() -> Registry<AttrFilter>& {
        static Registry<AttrFilter> made;
        using scene::lights::Kind;
        made.put("lila.light",
                 {.typeName = "light",
                  .triggers = {"lightType"},
                  .shows = [](const scene::Node& n, const std::string& attr) {
                      const Kind k = scene::lights::settings(n).kind;
                      const bool spot = k == Kind::Spot, sized = k == Kind::Area || k == Kind::Disc;
                      if (attr == "innerConeAngle" || attr == "outerConeAngle")
                          return spot;
                      if (attr == "width")
                          return sized;
                      if (attr == "height")
                          return k == Kind::Area;
                      if (attr == "sourceRadius")
                          return k == Kind::Point || spot || k == Kind::Directional;
                      if (attr == "range")
                          return k != Kind::Directional && k != Kind::Dome;
                      if (attr == "texture")
                          return k == Kind::Dome || k == Kind::Point || spot;
                      return true;
                  }},
                 kLilaOwner);
        made.put("lila.meshSymmetry",
                 {.typeName = "mesh",
                  .enabled = [](const scene::Node& n, const std::string& attr) {
                      // Enum attrs compared by their value's name, so reordered enum names keep working.
                      auto is = [&n](const char* a, std::string_view name) {
                          const int i = n.type().attrIndex(a);
                          if (i < 0)
                              return false;
                          const scene::AttrDef& d = n.type().attrs[size_t(i)];
                          const int64_t v = n.as<int64_t>(i);
                          return v >= 0 && size_t(v) < d.enumNames.size() && d.enumNames[size_t(v)] == name;
                      };
                      const bool on = !is("symmetry", "off"), radial = is("symmetry", "radial"), custom = is("symmetryPlane", "custom");
                      if (attr == "symmetryPlane")
                          return on;
                      if (attr == "symmetryOffset")
                          return on && custom;
                      if (attr == "radialCenter")
                          return radial && custom;
                      if (attr == "radialDirection")
                          return radial && is("radialAxis", "custom");
                      if (attr.starts_with("radial"))
                          return radial;
                      return true;
                  }},
                 kLilaOwner);
        return made;
    }();
    return r;
}

Registry<attributeEditor::Section>& attributeEditor::registry()
{
    static Registry<Section> r;
    return r;
}

void attributeEditor::addSection(std::string typeName, std::string title, SectionFactory fn, std::string owner)
{
    std::string id = typeName + "/" + title;
    registry().put(std::move(id), {std::move(typeName), std::move(title), std::move(fn)}, std::move(owner));
}

uint64_t attributeEditor::addSection(ext::Context& ctx, std::string typeName, std::string title, SectionFactory fn)
{
    std::string id = typeName + "/" + title;
    if (const std::string* owner = registry().ownerOf(id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "Attribute Editor section '" + id + "' already belongs to " + *owner);
        return 0;
    }
    addSection(std::move(typeName), std::move(title), std::move(fn), ctx.owner());
    return ctx.track("attributeEditorSection:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
    });
}

void attributeEditor::revealIssue(uint64_t id)
{
    gPendingReveal = id;
    for (AttributeEditor* e : gEditors)
        if (e->reveal(id))
            gPendingReveal = 0;
}

bool attributeEditor::removeSection(const std::string& typeName, const std::string& title)
{
    return registry().remove(typeName + "/" + title);
}

std::vector<std::pair<std::string, QWidget*>> attributeEditor::buildSections(scene::NodeId id)
{
    std::vector<std::pair<std::string, QWidget*>> out;
    scene::Node* n = Graph::current().node(id);
    if (!n)
        return out;
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
                out.emplace_back(extra->title, w);
    return out;
}

AttributeEditor::AttributeEditor(QWidget* parent) : QWidget(parent)
{
    deletion::claimScene(this);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(8, 6, 8, 4);
    outer->setSpacing(4);
    // A label column plus a row of three float fields.
    setMinimumWidth(fontMetrics().averageCharWidth() * 42);

    setProperty("lilaMultiEditPanel", true);
    attrView::installKeyWatch();
    auto* top = new QHBoxLayout;
    mType = new QLabel(this);
    aeDim(mType);
    mName = new QLineEdit(this);
    mName->setPlaceholderText("Name");
    mName->setAccessibleName("Name");
    top->addWidget(mName, 1);
    top->addWidget(mType);
    outer->addLayout(top);
    auto* find = new QHBoxLayout;
    mSearch = new QLineEdit(this);
    mSearch->setObjectName("attrSearch");
    mSearch->setPlaceholderText("Search attributes");
    mSearch->setClearButtonEnabled(true);
    mSearch->addAction(icon("search"), QLineEdit::LeadingPosition);
    connect(mSearch, &QLineEdit::textChanged, this, [this] { applySearch(); });
    mAdd = new QToolButton(this);
    mAdd->setObjectName("attrAdd");
    mAdd->setIcon(icon("plus"));
    mAdd->setAutoRaise(true);
    mAdd->setToolTip("Add an attribute or component");
    connect(mAdd, &QToolButton::clicked, this, [this] { openAdd(mAdd); });
    find->addWidget(mSearch, 1);
    find->addWidget(mAdd);
    outer->addLayout(find);
    mChips = new QWidget(this);
    mChips->setObjectName("aeChips");
    new QHBoxLayout(mChips);
    mChips->layout()->setContentsMargins(0, 0, 0, 0);
    mChips->layout()->setSpacing(3);
    outer->addWidget(mChips);
    mHint = new QFrame(this);
    mHint->setObjectName("multiEditHint");
    mHint->setStyleSheet(QString("#multiEditHint { background: %1; border-radius: 3px; }").arg(themeColor("altBase").name()));
    auto* hintRow = new QHBoxLayout(mHint);
    hintRow->setContentsMargins(6, 3, 6, 3);
    mHintText = new QLabel(mHint);
    mHintText->setObjectName("multiEditHintText");
    mHintText->setWordWrap(true);
    auto* hintOff = new QLabel("<a href=\"off\">Turn off this hint</a>", mHint);
    hintOff->setObjectName("multiEditHintOff");
    connect(hintOff, &QLabel::linkActivated, this, [this] {
        attrView::setHintEnabled(false);
        mHint->hide();
    });
    hintRow->addWidget(mHintText, 1);
    hintRow->addWidget(hintOff);
    mHint->hide();
    outer->addWidget(mHint);
    mViewSig = viewSignature();
    mViewListener = attrView::addListener([this] {
        const std::string sig = viewSignature();
        if (sig != mViewSig)
        {
            mViewSig = sig;
            mRebuild = true;
        }
        refreshMultiState();
        scheduleRefresh();
    });
    connect(mName, &QLineEdit::editingFinished, this, [this] {
        scene::Node* n = Graph::current().node(mNode);
        if (!n || mName->text().isEmpty() || mName->text().toStdString() == n->name())
            return;
        try
        {
            ops::execute("rename", {{"args", {n->path(), mName->text().toStdString()}}}, CallSource::UI);
        }
        catch (const CommandError& e)
        {
            log::error("{}", e.what());
        }
    });

    auto* scroll = mScroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->installEventFilter(this);
    mBody = new QWidget;
    mBody->setObjectName("aeBody");
    mBodyLayout = new QVBoxLayout(mBody);
    mBodyLayout->setContentsMargins(0, 4, 0, 0);
    mBodyLayout->setSpacing(0);
    scroll->setWidget(mBody);
    outer->addWidget(scroll, 1);

    mSectionListener = attributeEditor::registry().addListener([this](RegistryChange, const std::string&, const std::string&) {
        mRebuild = true;
        scheduleRefresh();
    });
    mListener = Graph::current().addListener([this](scene::Change c, scene::NodeId) {
        // An array element's change may add or drop a row, so element edits rebuild too.
        const scene::PlugRef* plug = Graph::current().changedPlug();
        if (c != scene::Change::Value || (plug && plug->element >= 0 && !tableKeepsUp(*plug)) ||
            (plug && plug->node == mNode && mTriggers.contains(plug->attr)))
            mRebuild = true;
        scheduleRefresh();
    });
    mIssueListener = dx::addListener([this] {
        if (mHadIssues || !dx::issuesOf(mNode).empty())
        {
            mRebuild = true;
            scheduleRefresh();
        }
    });
    gEditors.push_back(this);
    rebuild();
}

AttributeEditor::~AttributeEditor()
{
    std::erase(gEditors, this);
    attrView::removeListener(mViewListener);
    dx::removeListener(mIssueListener);
    Graph::current().removeListener(mListener);
    attributeEditor::registry().removeListener(mSectionListener);
}

bool AttributeEditor::reveal(uint64_t issue)
{
    const dx::Issue* i = dx::find(issue);
    if (!i || i->node != mNode || mNode == scene::kNoNode)
        return false;
    QWidget* to = nullptr;
    if (!i->attr.empty())
        if (auto it = mAttrWidgets.find(i->attr); it != mAttrWidgets.end())
            to = it->second;
    if (!to && !i->component.empty())
        if (auto it = mComponentWidgets.find(QString::fromStdString(i->component).toLower().toStdString()); it != mComponentWidgets.end())
            to = it->second;
    if (!to)
        to = mBody->findChild<QWidget*>("issueList");
    if (!to)
        return false;
    // After the layout settles: a fresh rebuild's widgets have no geometry yet.
    QTimer::singleShot(0, this, [this, to = QPointer<QWidget>(to)] {
        if (to)
            mScroll->ensureWidgetVisible(to, 0, 40);
    });
    return true;
}

QWidget* AttributeEditor::issueList()
{
    const std::vector<const dx::Issue*> list = dx::issuesOf(mNode);
    if (list.empty())
        return nullptr;
    auto* box = new QWidget;
    box->setObjectName("issueList");
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(4, 2, 4, 4);
    v->setSpacing(2);
    for (const dx::Issue* i : list)
    {
        auto* row = new QHBoxLayout;
        row->setSpacing(6);
        auto* badge = new QLabel;
        const int h = fontMetrics().height() - 2;
        badge->setPixmap(issues::badge(i->severity, i->blocking(), h));
        badge->setFixedSize(h, h);
        badge->setToolTip(QString(dx::severityName(i->severity)) + (i->blocking() ? " (blocking)" : ""));
        row->addWidget(badge, 0, Qt::AlignTop);
        const QString at = issues::where(*i);
        const QString message = QString::fromStdString(i->message).toHtmlEscaped();
        auto* text = new QLabel(at.isEmpty() ? message
                                             : QString("<a href=\"#\" style=\"color: %1;\">%2</a>: %3")
                                                   .arg(issues::color(i->severity).name(), at.toHtmlEscaped(), message));
        text->setWordWrap(true);
        text->setTextFormat(Qt::RichText);
        std::string tip = "From " + i->source;
        if (i->blocking())
        {
            tip += "\nBlocks";
            for (const std::string& c : i->blocks)
                tip += " " + c;
        }
        text->setToolTip(QString::fromStdString(tip));
        const uint64_t id = i->id;
        connect(text, &QLabel::linkActivated, this, [this, id](const QString&) { reveal(id); });
        row->addWidget(text, 1);
        if (!i->fix.empty())
        {
            auto* fix = new QToolButton;
            fix->setText("Fix");
            fix->setAutoRaise(true);
            fix->setToolTip(QString::fromStdString(i->fix));
            connect(fix, &QToolButton::clicked, this, [id] {
                try
                {
                    ops::execute("fixIssue", {{"args", {id}}}, CallSource::UI);
                }
                catch (const CommandError& e)
                {
                    log::error("{}", e.what());
                }
            });
            row->addWidget(fix, 0, Qt::AlignTop);
        }
        v->addLayout(row);
    }
    return box;
}

void AttributeEditor::scheduleRefresh()
{
    if (mQueued)
        return;
    mQueued = true;
    // While a drag edits the scene every frame, the fields catch up ten times a second instead of every frame.
    QTimer::singleShot(QGuiApplication::mouseButtons() != Qt::NoButton ? 100 : 0, this, [this] {
        mQueued = false;
        if (mRebuild || shownNode() != mNode || Graph::current().selection() != mLastSelection)
            rebuild();
        else
            updateValues();
        mRebuild = false;
    });
}

void AttributeEditor::set(const std::string& attr, const json& value)
{
    Graph& g = Graph::current();
    scene::Node* n = g.node(mNode);
    if (!n)
        return;
    const std::vector<scene::NodeId> all = holders(attr);
    const bool many = all.size() > 1 && attrView::multiEditHeld();
    try
    {
        UndoChunk chunk("Set " + attr);
        for (scene::NodeId id : many ? all : std::vector<scene::NodeId>{mNode})
            ops::execute("setAttr", {{"args", {g.node(id)->path() + "." + attr, value}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        updateValues();
    }
    const std::string name = g.attr(g.plug(n->path() + "." + attr)).name;
    if (many)
    {
        attrView::noteMultiEditUsed();
        mBadgeCounts[name] = int(all.size());
        if (auto it = mBadges.find(name); it != mBadges.end())
        {
            it->second->setText(QString("\u00d7%1").arg(all.size()));
            it->second->show();
        }
        refreshMultiState();
    }
    else if (all.size() > 1 && attrView::hintEnabled())
        showHint(QString("Changed %1 only. Hold Alt to change all %2.").arg(QString::fromStdString(n->name())).arg(all.size()));
}

std::vector<scene::NodeId> AttributeEditor::holders(const std::string& attr) const
{
    Graph& g = Graph::current();
    std::vector<scene::NodeId> out;
    if (mNode != attrView::lead())
        return {mNode};
    for (scene::NodeId id : attrView::selectionLeadFirst())
        if (scene::Node* n = g.node(id))
            try
            {
                g.plug(n->path() + "." + attr);
                out.push_back(id);
            }
            catch (const std::exception&)
            {
            }
    return out;
}

bool AttributeEditor::mixed(const scene::PlugRef& ref) const
{
    if (!attrView::multiEditHeld() || Graph::current().selection().size() < 2)
        return false;
    Graph& g = Graph::current();
    const std::string plug = g.plugName(ref);
    const std::string attr = plug.substr(plug.find('.') + 1);
    const std::vector<scene::NodeId> all = holders(attr);
    if (all.size() < 2)
        return false;
    const scene::PlugValue first = g.value(g.plug(g.node(all[0])->path() + "." + attr));
    for (size_t i = 1; i < all.size(); ++i)
        if (!(g.value(g.plug(g.node(all[i])->path() + "." + attr)) == first))
            return true;
    return false;
}

void AttributeEditor::addElement(const std::vector<int>& attrs, int element)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    try
    {
        for (int a : attrs)
        {
            const scene::AttrDef& def = n->type().attrs[size_t(a)];
            std::string plug = Graph::current().plugName({mNode, a, -1, element});
            ops::execute("setAttr", {{"args", {plug, scene::toJson(def.defaultValue)}}}, CallSource::UI);
        }
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void AttributeEditor::runOnNode(const char* command, json call)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    call["args"] = {n->path()};
    try
    {
        ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void AttributeEditor::removeCategory(const std::string& category)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    UndoChunk chunk("Remove " + category);
    // Custom also holds the attributes added without a category.
    if (category == attrAuthoring::kCustomCategory)
    {
        std::vector<std::string> loose;
        bool named = false;
        for (const scene::AttrDef& d : n->type().attrs)
            if ((d.flags & scene::AttrUser) && d.name != "usdApiSchemas")
            {
                if (d.category.empty())
                    loose.push_back(d.name);
                named |= d.category == category;
            }
        for (const std::string& attr : loose)
            runOnNode("deleteAttr", {{"attribute", attr}});
        if (!named)
            return;
    }
    runOnNode("deleteAttr", {{"category", category}});
}

void AttributeEditor::removeElement(const std::string& attr)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    // A grouped child's element goes as a whole: group[3], every child at index 3.
    std::string plug = attr;
    if (size_t close = plug.find("]."); close != std::string::npos)
        plug = plug.substr(0, close + 1);
    try
    {
        ops::execute("removeMultiInstance", {{"args", {n->path() + "." + plug}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

void AttributeEditor::removeElements(const std::vector<int>& attrs, const std::vector<int>& elements)
{
    Graph& g = Graph::current();
    if (!g.node(mNode) || elements.empty())
        return;
    try
    {
        UndoChunk chunk(elements.size() == 1 ? "Remove Element" : "Remove Elements");
        for (auto e = elements.rbegin(); e != elements.rend(); ++e)
            for (int a : attrs)
                ops::execute("removeMultiInstance", {{"args", {g.plugName({mNode, a, -1, *e})}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
    }
}

bool AttributeEditor::tableKeepsUp(const scene::PlugRef& plug) const
{
    const auto it = mTableCounts.find(plug.attr);
    const scene::Node* n = plug.node == mNode ? Graph::current().node(mNode) : nullptr;
    return n && it != mTableCounts.end() && n->elements(plug.attr).size() == it->second;
}

QWidget* AttributeEditor::colorSwatch(const scene::AttrDef& def, scene::NodeId id, int index, bool readOnly)
{
    // A data display: the swatch shows the stored linear colour in sRGB.
    auto* b = new QToolButton;
    b->setProperty("lilaRawColors", true);
    b->setMinimumWidth(36);
    b->setEnabled(!readOnly);
    std::string attr = def.name;
    const QString title = def.niceName.empty() ? niceName(def.name) : QString::fromStdString(def.niceName);
    auto current = [id, index] {
        const scene::PlugValue& v = Graph::current().value({id, index});
        auto* c = std::get_if<glm::dvec3>(&v);
        return c ? *c : glm::dvec3(0.0);
    };
    mUpdaters.push_back([b, current] {
        glm::dvec3 c = current();
        QColor q = QColor::fromRgbF(float(linearToSrgb(c.x)), float(linearToSrgb(c.y)), float(linearToSrgb(c.z)));
        b->setStyleSheet(QString("QToolButton { background: %1; border-radius: 4px; }").arg(q.name()));
    });
    connect(b, &QToolButton::clicked, this, [b, attr, title, id, index, current] {
        const scene::Node* n = Graph::current().node(id);
        if (!n)
            return;
        const glm::dvec3 start = current();
        const std::string plug = n->path() + "." + attr;
        // Previews with undo off; closing restores the start and sets the result once, as one undo step.
        auto preview = [id, index](const glm::dvec3& c) {
            Graph& g = Graph::current();
            if (!g.node(id))
                return;
            UndoStack& u = UndoStack::get();
            const bool was = u.enabled();
            u.setEnabled(false);
            g.setValue({id, index}, c);
            u.setEnabled(was);
        };
        colorPopup::Options o;
        o.value.color = start;
        o.title = title;
        o.changed = [preview](const colorPopup::Value& v) { preview(v.color); };
        o.closed = [preview, start, plug](const colorPopup::Value& v, bool accepted) {
            preview(start);
            if (!accepted || v.color == start)
                return;
            try
            {
                ops::execute("setAttr", {{"args", {plug, json::array({v.color.x, v.color.y, v.color.z})}}}, CallSource::UI);
            }
            catch (const CommandError& e)
            {
                log::error("{}", e.what());
            }
        };
        colorPopup::show(colorPopup::placeFor(b), std::move(o));
    });
    b->setMaximumWidth(48);
    if (readOnly)
        return b;
    return colorPopup::withEyedropper(b, [this, attr](const glm::dvec3& c) { set(attr, json::array({c.x, c.y, c.z})); });
}

void AttributeEditor::rebuild()
{
    LILA_ZONE();

    mUpdaters.clear();
    while (QLayoutItem* item = mBodyLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }

    Graph& g = Graph::current();
    if (g.selection() != mLastSelection)
    {
        mLastSelection = g.selection();
        mBadgeCounts.clear();
        if (!mPinned)
            mFocus = scene::kNoNode;
    }
    mBadges.clear();
    if (shownNode() != mNode)
        mArrayScroll.clear();
    mTableCounts.clear();
    mNode = shownNode();
    if (!mNavigating && mNode != scene::kNoNode && (mHistoryAt < 0 || mHistory[size_t(mHistoryAt)] != mNode))
    {
        mHistory.resize(size_t(mHistoryAt + 1));
        mHistory.push_back(mNode);
        mHistoryAt = int(mHistory.size()) - 1;
    }
    scene::Node* n = g.node(mNode);
    rebuildHeader();
    mName->setText(n ? QString::fromStdString(n->name()) : QString());
    mName->setToolTip(n ? QString::fromStdString(n->fullPath()) : QString());
    mName->setEnabled(n != nullptr);
    mType->setText(n ? QString::fromStdString(n->type().name) : QString());
    mType->setToolTip(n ? QString::fromStdString(n->type().doc) : QString());
    mAttrWidgets.clear();
    mComponentWidgets.clear();
    mSearchRows.clear();
    mSections.clear();
    mAdd->setEnabled(n != nullptr);
    const std::vector<const dx::Issue*> nodeIssues = dx::issuesOf(mNode);
    mHadIssues = !nodeIssues.empty();
    if (!n)
    {
        mBodyLayout->addStretch(1);
        mBodyLayout->addWidget(emptyStateLabel("Select an object to see its attributes.", this));
        mBodyLayout->addStretch(2);
        return;
    }
    if (QWidget* list = issueList())
        mBodyLayout->addWidget(aeSection("Issues", list).box);
    if (n->type().isA("light"))
        mBodyLayout->addWidget(sceneLights::attributeNote(mBody));
    /** The issues on a component as a whole: a type, a category or a section by its title. */
    auto componentIssues = [&](std::string_view typeName, std::string_view category, const QString& title) {
        std::vector<const dx::Issue*> out;
        for (const dx::Issue* i : nodeIssues)
            if (i->attr.empty() && (dx::componentMatches(i->component, typeName, category, {}) ||
                                    (!i->component.empty() && QString::fromStdString(i->component).compare(title, Qt::CaseInsensitive) == 0)))
                out.push_back(i);
        return out;
    };
    auto noteComponent = [&](const std::string& name, QWidget* w) {
        if (!name.empty())
            mComponentWidgets.emplace(QString::fromStdString(name).toLower().toStdString(), w);
    };

    // One section per type in the inheritance chain, base first.
    std::vector<const scene::NodeType*> chain;
    for (const scene::NodeType* t = &n->type(); t; t = t->base)
        chain.insert(chain.begin(), t);

    scene::NodeId id = mNode;
    std::vector<const attributeEditor::AttrFilter*> filters;
    mTriggers.clear();
    for (const attributeEditor::AttrFilter* f : attributeEditor::filters().all())
        if (n->type().isA(f->typeName))
        {
            filters.push_back(f);
            for (const std::string& t : f->triggers)
                if (int index = n->type().attrIndex(t); index >= 0)
                    mTriggers.insert(index);
        }
    /** Greys out a row while its switch is off: a bool its name extends, or a filter's enabled test. */
    auto switchWith = [&](const scene::AttrDef& def, QWidget* label, QWidget* field) {
        const scene::AttrDef* sw = nullptr;
        for (const scene::AttrDef& s : n->type().attrs)
            if (aeSwitches(s, def) && (!sw || s.name.size() > sw->name.size()))
                sw = &s;
        std::vector<std::function<bool(const scene::Node&, const std::string&)>> tests;
        for (const attributeEditor::AttrFilter* f : filters)
            if (f->enabled)
                tests.push_back(f->enabled);
        if (!sw && tests.empty())
            return;
        const int swIndex = sw ? n->type().attrIndex(sw->name) : -1;
        mUpdaters.push_back([label, field, id, swIndex, tests, name = def.name] {
            Graph& graph = Graph::current();
            const scene::Node* node = graph.node(id);
            if (!node)
                return;
            bool on = true;
            if (swIndex >= 0)
            {
                const scene::PlugValue& v = graph.value({id, swIndex});
                if (auto* b = std::get_if<bool>(&v))
                    on = *b;
            }
            for (const auto& test : tests)
                on = on && test(*node, name);
            label->setEnabled(on);
            field->setEnabled(on);
        });
    };
    /** Adds one attr's row to form; false when the attr shows no editor. */
    auto addRow = [&](QFormLayout* form, const scene::AttrDef& def, int element, QString labelText) -> bool {
        if (def.flags & scene::AttrHidden)
            return false;
        for (const attributeEditor::AttrFilter* f : filters)
            if (f->shows && !f->shows(*n, def.name))
                return false;
        int index = n->type().attrIndex(def.name);
        const scene::PlugRef ref{id, index, -1, element};
        bool readOnly = !(def.flags & scene::AttrInput) || (element >= 0 && g.source(ref));
        std::string attr = def.name;
        if (element >= 0)
        {
            std::string plug = g.plugName(ref);
            attr = plug.substr(plug.find('.') + 1);
        }
        QWidget* field = nullptr;
        std::vector<ScrubTarget> scrubbed;
        const NumberScrub::Commit commit = [this](const std::string& a, const json& v) { set(a, v); };

        const QString unit = aeUnitOf(def);
        auto numberEdit = [&](int component) {
            const std::optional<std::pair<double, double>> span = fillSpan(def);
            auto* e = new NumberField(span, unit, component >= 0 && !(def.flags & scene::AttrColor) ? char('X' + component) : 0);
            e->setReadOnly(readOnly);
            if (readOnly)
                e->unsetCursor();
            e->setMinimumWidth(e->fontMetrics().horizontalAdvance("-0.0") + 8 + e->textMargins().left() + e->textMargins().right());
            e->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            std::string target = component >= 0 ? def.childNames[component] : attr;
            if (!readOnly)
            {
                scrubbed.push_back({ref, component, target, def.minValue, def.maxValue, def.type == PlugType::Int});
                new NumberScrub(e, {scrubbed.back()}, commit, e, span);
                setUsageHint(e, "Drag sideways to scrub; Ctrl for fine steps, Shift for coarse; middle-drag up or down to pick the step");
                if (std::optional<json> d = valueReset::plugDefault(def, component))
                    onMiddleClickReset(e, [this, target, v = *d] { set(target, v); }, QString::fromStdString(valueReset::describe(*d)));
            }
            connect(e, &QLineEdit::editingFinished, this, [this, e, target, def, ref, component] {
                if (!e->isModified())
                    return;
                e->setModified(false);
                double current = 0.0;
                if (Graph::current().node(ref.node))
                {
                    const scene::PlugValue& pv = Graph::current().value(ref);
                    if (auto* v3 = std::get_if<glm::dvec3>(&pv); v3 && component >= 0)
                        current = (*v3)[component];
                    else if (auto* f = std::get_if<double>(&pv))
                        current = *f;
                    else if (auto* i = std::get_if<int64_t>(&pv))
                        current = double(*i);
                }
                std::optional<double> v = evalExpression(e->text(), current);
                if (!v)
                {
                    updateValues();
                    return;
                }
                if (def.minValue)
                    v = std::max(*v, *def.minValue);
                if (def.maxValue)
                    v = std::min(*v, *def.maxValue);
                set(target, def.type == PlugType::Int ? json(int64_t(std::llround(*v))) : json(*v));
            });
            mUpdaters.push_back([e, ref, component, owner = this] {
                // Only typing holds a value back; a field focused by a middle-drag still follows it.
                if (e->hasFocus() && e->isModified())
                    return;
                scene::Node* node = Graph::current().node(ref.node);
                if (!node)
                    return;
                if (owner->mixed(ref))
                {
                    e->setText("--");
                    return;
                }
                const scene::PlugValue& v = Graph::current().value(ref);
                double d = 0.0;
                if (auto* v3 = std::get_if<glm::dvec3>(&v))
                    d = (*v3)[component];
                else if (auto* f = std::get_if<double>(&v))
                    d = *f;
                else if (auto* i = std::get_if<int64_t>(&v))
                    d = double(*i);
                e->setText(attrView::formatNumber(d));
                e->showValue(d);
                e->setState(ChannelBox::stateOf({ref.node, ref.attr, component, ref.element}));
            });
            return e;
        };

        switch (def.type)
        {
        case PlugType::Float:
        case PlugType::Int: field = numberEdit(-1); break;
        case PlugType::Vec3:
        {
            field = new QWidget;
            auto* h = new QHBoxLayout(field);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(2);
            for (int c = 0; c < 3; ++c)
                h->addWidget(numberEdit(c), 1);
            if ((def.flags & scene::AttrColor) && element < 0)
                h->addWidget(colorSwatch(def, id, index, readOnly));
            break;
        }
        case PlugType::Bool:
        {
            auto* cb = new QCheckBox;
            cb->setEnabled(!readOnly);
            connect(cb, &QCheckBox::clicked, this, [this, attr](bool on) { set(attr, on); });
            mUpdaters.push_back([cb, ref, owner = this] {
                QSignalBlocker block(cb);
                if (owner->mixed(ref))
                    cb->setCheckState(Qt::PartiallyChecked);
                else
                    cb->setChecked(std::get<bool>(Graph::current().value(ref)));
            });
            field = cb;
            break;
        }
        case PlugType::Enum:
        {
            std::vector<QString> shown;
            int chars = 0;
            for (const std::string& e : def.enumNames)
            {
                shown.push_back(aeEnumLabel(e));
                chars += int(shown.back().size());
            }
            // A short exclusive choice (an axis, a plane) reads at a glance as one strip.
            if (shown.size() >= 2 && shown.size() <= 4 && chars <= 18)
            {
                auto* strip = new controls::Segmented;
                const std::vector<std::string> docs = choiceTips::forAttr(def);
                for (size_t i = 0; i < shown.size(); ++i)
                    strip->add(shown[i], {}, i < docs.size() ? QString::fromStdString(docs[i]) : QString(), int(i));
                strip->setEnabled(!readOnly);
                connect(strip, &controls::Segmented::picked, this, [this, attr](int i) { set(attr, int64_t(i)); });
                mUpdaters.push_back([strip, ref] {
                    QSignalBlocker block(strip);
                    strip->setCurrent(int(std::get<int64_t>(Graph::current().value(ref))));
                });
                field = strip;
                break;
            }
            auto* combo = new QComboBox;
            for (size_t i = 0; i < shown.size(); ++i)
                combo->addItem(shown[i], qlonglong(i));
            combo->setEnabled(!readOnly);
            connect(combo, &QComboBox::activated, this, [this, attr, combo](int i) { set(attr, int64_t(combo->itemData(i).toLongLong())); });
            mUpdaters.push_back([combo, ref, owner = this] {
                QSignalBlocker block(combo);
                combo->setPlaceholderText("--");
                combo->setCurrentIndex(owner->mixed(ref) ? -1 : int(std::get<int64_t>(Graph::current().value(ref))));
                choiceTips::refresh(combo);
            });
            field = combo;
            break;
        }
        case PlugType::String:
        {
            if ((def.flags & scene::AttrFont) && !readOnly)
            {
                auto* picker = new FontPicker;
                connect(picker, &FontPicker::chosen, this, [this, attr](const QString& font) { set(attr, font.toStdString()); });
                mUpdaters.push_back([picker, ref] { picker->setValue(std::get<std::string>(Graph::current().value(ref))); });
                field = picker;
                break;
            }
            if ((def.flags & scene::AttrImage) && !readOnly)
            {
                auto* picker = new ImagePicker(imagePicker::libraryFor(n->type(), def.name), niceName(def.name));
                connect(picker, &ImagePicker::chosen, this, [this, attr](const QString& v) { set(attr, v.toStdString()); });
                mUpdaters.push_back([picker, ref] { picker->setValue(std::get<std::string>(Graph::current().value(ref))); });
                field = picker;
                break;
            }
            if (def.flags & scene::AttrCode)
            {
                auto* box = new CodeEdit;
                box->setReadOnly(readOnly);
                connect(box, &CodeEdit::applied, this, [this, attr](const QString& text) { set(attr, text.toStdString()); });
                mUpdaters.push_back([box, ref] { box->showText(std::get<std::string>(Graph::current().value(ref))); });
                field = box;
                break;
            }
            if (def.flags & scene::AttrMultiline)
            {
                auto* box = new MultilineEdit;
                box->setReadOnly(readOnly);
                connect(box, &MultilineEdit::committed, this, [this, attr](const QString& text) { set(attr, text.toStdString()); });
                mUpdaters.push_back([box, ref] { box->showText(std::get<std::string>(Graph::current().value(ref))); });
                field = box;
                break;
            }
            auto* e = new QLineEdit;
            e->setReadOnly(readOnly);
            connect(e, &QLineEdit::editingFinished, this, [this, e, attr] {
                if (e->isModified())
                {
                    e->setModified(false);
                    set(attr, e->text().toStdString());
                }
            });
            mUpdaters.push_back([e, ref] {
                if (!e->hasFocus())
                    e->setText(QString::fromStdString(std::get<std::string>(Graph::current().value(ref))));
            });
            field = e;
            if ((def.flags & scene::AttrFile) && !readOnly)
            {
                field = new QWidget;
                auto* h = new QHBoxLayout(field);
                h->setContentsMargins(0, 0, 0, 0);
                h->setSpacing(3);
                auto* browse = new QToolButton;
                browse->setIcon(icon("folder-open"));
                browse->setToolTip("Browse...");
                connect(browse, &QToolButton::clicked, this, [this, e, attr, label = niceName(def.name)] {
                    QString f = ui::fileDialog::getOpenFileName(this, label, browseStart(e->text().toStdString()),
                                                             "Images (*.png *.jpg *.jpeg *.tga *.hdr *.exr *.bmp *.tif *.tiff *.webp);;All Files (*)");
                    if (!f.isEmpty())
                        set(attr, paths::storedPath(std::filesystem::path(f.toStdU16String())));
                });
                h->addWidget(e, 1);
                h->addWidget(browse);
            }
            break;
        }
        case PlugType::Matrix:
        {
            // Collapsed to a one-line summary: the full 4x4 is rarely read and would push the editable rows apart.
            auto* box = new QWidget;
            auto* v = new QVBoxLayout(box);
            v->setContentsMargins(0, 0, 0, 0);
            v->setSpacing(2);
            auto* toggle = new QToolButton(box);
            toggle->setCheckable(true);
            toggle->setAutoRaise(true);
            toggle->setArrowType(Qt::RightArrow);
            toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            auto* label = new QLabel(box);
            label->setFont(monoFont());
            label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->hide();
            QObject::connect(toggle, &QToolButton::toggled, label, [toggle, label](bool on) {
                label->setVisible(on);
                toggle->setArrowType(on ? Qt::DownArrow : Qt::RightArrow);
            });
            v->addWidget(toggle, 0, Qt::AlignLeft);
            v->addWidget(label);
            mUpdaters.push_back([label, toggle, ref] {
                const glm::dmat4 m = std::get<glm::dmat4>(Graph::current().value(ref));
                toggle->setText(m == glm::dmat4(1.0) ? "Identity" : "4 x 4");
                QString cells[4][4];
                qsizetype width = 1;
                // Rows of the matrix as written on paper (column-major storage).
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c)
                        width = std::max(width, (cells[r][c] = fmt(m[c][r])).size());
                QString text;
                for (int r = 0; r < 4; ++r)
                {
                    for (int c = 0; c < 4; ++c)
                        text += QString("%1").arg(cells[r][c], int(width) + (c ? 2 : 0));
                    if (r < 3)
                        text += "\n";
                }
                label->setText(text);
            });
            field = box;
            break;
        }
        }
        if (!field)
            return false;
        field->setToolTip(QString::fromStdString(def.doc));
        if (def.type == PlugType::Enum)
            if (auto* combo = qobject_cast<QComboBox*>(field))
                choiceTips::apply(combo, choiceTips::forAttr(def));
        if (element >= 0)
        {
            // An element's row ends in a button that removes it.
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            h->addWidget(field, 1);
            auto* remove = new QToolButton;
            remove->setIcon(icon("x"));
            remove->setToolTip("Remove this element");
            remove->setAutoRaise(true);
            connect(remove, &QToolButton::clicked, this, [this, attr] { removeElement(attr); });
            h->addWidget(remove);
            field->setToolTip(QString::fromStdString(def.doc));
            field = row;
        }
        auto* label = new AttrRowLabel(!labelText.isEmpty() ? labelText : attrView::label(def));
        label->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(label, &QWidget::customContextMenuRequested, this,
                [this, label, attr, ref, def, element, readOnly, keyable = bool(def.flags & scene::AttrKeyable), user = bool(def.flags & scene::AttrUser)](const QPoint& at) {
            QMenu menu;
            addCopyActions(&menu, QString::fromStdString(attr), [this, ref] {
                return Graph::current().node(mNode) ? scene::toJson(Graph::current().value(ref)) : json();
            });
            if (user)
            {
                menu.addSeparator();
                menu.addAction(icon("pencil"), "Edit Attribute...", [this, attr, label] { openEdit(attr, label); });
                menu.addAction("Delete Attribute", [this, attr] { runOnNode("deleteAttr", {{"attribute", attr}}); });
            }
            if (!readOnly && element < 0 && def.type != PlugType::String && def.type != PlugType::Mesh && def.type != PlugType::Data)
                menu.addAction(icon("link"), "Drive...", [this, attr, label] {
                    if (scene::Node* node = Graph::current().node(mNode))
                        drivePopover::show(node->path() + "." + attr, label);
                });
            if (keyable)
            {
                menu.addSeparator();
                menu.addAction("Set Key", [this, attr] {
                    scene::Node* node = Graph::current().node(mNode);
                    if (!node)
                        return;
                    try
                    {
                        ops::execute("setKeyframe", {{"args", {node->path() + "." + attr}}}, CallSource::UI);
                    }
                    catch (const CommandError& e)
                    {
                        log::error("{}", e.what());
                    }
                });
            }
            menu.exec(label->mapToGlobal(at));
        });
        label->setToolTip(QString::fromStdString(def.name + (def.shortName.empty() ? "" : " (" + def.shortName + ")") +
                                                 (def.doc.empty() ? "" : "\n" + def.doc)));
        // The scrub goes on first: the reset filter installed after it runs first, so it sees the middle press the
        // scrub takes, as on the fields.
        if (!scrubbed.empty())
            new NumberScrub(label, std::move(scrubbed), commit);
        if (readOnly)
            aeDim(label, "disabled");
        else if (std::optional<json> d = valueReset::plugDefault(def))
            onMiddleClickReset(label, [this, attr, v = *d] { set(attr, v); },
                               QString::fromStdString(valueReset::describe(*d, aeEnumLabels(def))), field);
        std::vector<const dx::Issue*> rowIssues;
        for (const dx::Issue* i : nodeIssues)
            if (dx::attrMatches(*i, attr) || (i->attr.empty() && dx::componentMatches(i->component, {}, {}, def.name)))
                rowIssues.push_back(i);
        markIssues(label, rowIssues);
        mAttrWidgets.emplace(attr, label);
        mAttrWidgets.emplace(def.name, label);
        if ((def.flags & scene::AttrUser) && element < 0)
        {
            // Added attributes: a grip to drag and a pencil to edit, in the label's free left end while the row is hovered.
            auto* grip = new QLabel(label);
            grip->setPixmap(icon("grip-vertical").pixmap(14, 14));
            grip->setToolTip("Drag onto another attribute or heading to move it");
            grip->setCursor(Qt::OpenHandCursor);
            auto* pencil = new QToolButton(label);
            pencil->setObjectName("editAttr");
            pencil->setIcon(icon("pencil"));
            pencil->setIconSize(QSize(12, 12));
            pencil->setAutoRaise(true);
            pencil->setToolTip("Edit this attribute");
            label->addLeading(grip);
            label->addLeading(pencil);
            connect(pencil, &QToolButton::clicked, this, [this, attr, pencil] { openEdit(attr, pencil); });
            for (QWidget* w : {static_cast<QWidget*>(label), field})
                (new RowGestures(w))->reveal = {grip, pencil};
            auto* drag = new RowGestures(grip);
            drag->dragMime = kAttrMime;
            drag->dragText = QString::fromStdString(attr);
            auto* drop = new RowGestures(label);
            label->setAcceptDrops(true);
            drop->doubleClicked = [this, attr, label] { openEdit(attr, label); };
            drop->accepts = [attr](const QMimeData* d) {
                return (d->hasFormat(kAttrMime) && mimeText(d, kAttrMime) != attr) || d->hasFormat(kHeadingMime);
            };
            drop->dropped = [this, attr, category = def.category](const QMimeData* d) {
                if (d->hasFormat(kAttrMime))
                    dropAttr(mimeText(d, kAttrMime), attr, category);
                else
                    dropHeading(mimeText(d, kHeadingMime), category);
            };
        }
        if (element < 0 && !readOnly && (def.flags & scene::AttrInput))
        {
            // A plug dragged from the Channel Box connects here; with Alt it becomes a driven key.
            auto* plugDrop = new RowGestures(label);
            label->setAcceptDrops(true);
            plugDrop->accepts = [](const QMimeData* d) { return d->hasFormat(kPlugMime); };
            plugDrop->dropped = [this, attr](const QMimeData* d) {
                if (scene::Node* node = Graph::current().node(mNode))
                    dropPlug(mimeText(d, kPlugMime), node->path() + "." + attr);
            };
        }
        if (element < 0)
        {
            auto* row = new QWidget;
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            h->setSpacing(3);
            h->addWidget(field, 1);
            // What drives it, inline: a link that shows the source here.
            std::optional<scene::PlugRef> src = g.source(ref);
            for (int c = 0; !src && def.type == PlugType::Vec3 && c < 3; ++c)
                src = g.source({ref.node, ref.attr, c, -1});
            if (const scene::Node* from = src ? g.node(src->node) : nullptr; from && !from->type().isA("animCurve") && !from->type().isA("animCurveRQ"))
            {
                auto* link = new QLabel(QString("<a href=\"x\">%1</a>").arg(QString::fromStdString(g.plugName(*src)).toHtmlEscaped()), row);
                link->setObjectName("aeSourceLink");
                link->setToolTip("Driven by this plug: click to show its node here");
                connect(link, &QLabel::linkActivated, this, [this, id = src->node] { focusNode(id); });
                h->addWidget(link);
            }
            auto* badge = new QLabel(row);
            badge->setObjectName("multiBadge");
            badge->setFont(aeSmallFont(badge->font()));
            aeDim(badge);
            if (auto it = mBadgeCounts.find(def.name); it != mBadgeCounts.end())
                badge->setText(QString("\u00d7%1").arg(it->second));
            else
                badge->hide();
            badge->setToolTip("Changed on this many objects");
            badge->setAccessibleName("Changed on several objects");
            mBadges[def.name] = badge;
            h->addWidget(badge);
            field = row;
        }
        if (element < 0)
            switchWith(def, label, field);
        new LabelEdgeDrag(label, [this](int x, bool done) { dragLabelEdge(x, done); }, [this] { resetLabelEdge(); });
        mSearchRows.push_back({form, label, def});
        form->addRow(label, field);
        return true;
    };
    /** Rows for an array: one per element and an Add button; a grouped array shows each element's children together. */
    std::set<std::string> groupsShown;
    auto addAttrRows = [&](QFormLayout* form, const scene::AttrDef& def) -> int {
        if (!(def.flags & scene::AttrArray))
            return addRow(form, def, -1, {}) ? 1 : 0;
        if (def.flags & scene::AttrHidden)
            return 0;
        const size_t dot = def.name.find('.');
        const int index = n->type().attrIndex(def.name);
        std::vector<int> children = {index};
        std::vector<int> indices;
        std::string addPlug = def.name;
        if (dot != std::string::npos)
        {
            const std::string group = def.name.substr(0, dot);
            if (!groupsShown.insert(group).second)
                return 0;
            children = n->type().groupChildren(group);
            indices = g.groupIndices(id, group);
            addPlug = group;
        }
        else
            for (const scene::PlugRef& e : g.elements({id, index}))
                indices.push_back(e.element);
        auto* heading = new QLabel(niceName(addPlug));
        heading->setContentsMargins(0, 4, 0, 0);
        heading->setToolTip(QString::fromStdString(def.doc));
        form->addRow(heading);
        int added = 1;
        const int last = indices.empty() ? -1 : indices.back();
        // Past a few dozen elements, rows of fields get unreadable and a 70k-element USD primvar would freeze the editor.
        constexpr size_t kTableAt = 32;
        if (indices.size() > kTableAt)
        {
            ArrayTable::Spec spec;
            spec.node = id;
            for (int c : children)
            {
                const scene::AttrDef& cd = n->type().attrs[size_t(c)];
                if (cd.flags & scene::AttrHidden)
                    continue;
                spec.attrs.push_back(c);
                spec.headers << (dot == std::string::npos ? niceName(cd.name) : niceName(cd.name.substr(dot + 1)));
                mTableCounts[c] = n->elements(c).size();
            }
            spec.elements = indices;
            spec.set = [this](const std::string& plug, const json& v) { set(plug, v); };
            spec.remove = [this, children](const std::vector<int>& elements) { removeElements(children, elements); };
            heading->setText(QString("%1 (%L2)").arg(niceName(addPlug)).arg(indices.size()));
            auto* table = new ArrayTable(std::move(spec), 12);
            table->setToolTip(QString::fromStdString(def.doc));
            mUpdaters.push_back([table] { table->refresh(); });
            QScrollBar* bar = table->verticalScrollBar();
            if (auto it = mArrayScroll.find(addPlug); it != mArrayScroll.end())
                QTimer::singleShot(0, table, [bar, at = it->second] { bar->setValue(at); });
            connect(bar, &QScrollBar::valueChanged, this, [this, addPlug](int at) { mArrayScroll[addPlug] = at; });
            form->addRow(table);
            ++added;
        }
        else
            for (int i : indices)
            {
                if (dot == std::string::npos)
                    added += addRow(form, def, i, QString("[%1]").arg(i)) ? 1 : 0;
                else
                    for (int c : children)
                    {
                        const scene::AttrDef& cd = n->type().attrs[size_t(c)];
                        QString child = niceName(cd.name.substr(dot + 1));
                        added += addRow(form, cd, i, QString("[%1] %2").arg(i).arg(child)) ? 1 : 0;
                    }
            }
        // Only arrays that hold values can grow here; connection-only ones grow by connecting.
        bool addable = true;
        for (int c : children)
            addable = addable && (n->type().attrs[size_t(c)].flags & scene::AttrStorable) && (n->type().attrs[size_t(c)].flags & scene::AttrInput);
        if (addable)
        {
            auto* add = new QToolButton;
            add->setIcon(icon("plus"));
            add->setText("Add");
            add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            add->setToolTip("Add an element after the last one");
            const int next = last + 1;
            connect(add, &QToolButton::clicked, this, [this, children, next] { addElement(children, next); });
            form->addRow(QString(), add);
        }
        return added;
    };
    auto newForm = [](QWidget* content) {
        auto* form = new QFormLayout(content);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::DontWrapRows);
        form->setContentsMargins(4, 0, 6, 0);
        form->setHorizontalSpacing(8);
        form->setVerticalSpacing(4);
        return form;
    };
    auto visible = [&](const scene::AttrDef& def) {
        if (def.flags & scene::AttrHidden)
            return false;
        for (const attributeEditor::AttrFilter* f : filters)
            if (f->shows && !f->shows(*n, def.name))
                return false;
        return true;
    };
    /** The bool every other shown attr of a section depends on, which then sits in the section's header. */
    auto headerSwitch = [&](const std::vector<const scene::AttrDef*>& defs) -> const scene::AttrDef* {
        std::vector<const scene::AttrDef*> shown;
        for (const scene::AttrDef* d : defs)
            if (visible(*d))
                shown.push_back(d);
        if (shown.size() < 2)
            return nullptr;
        for (const scene::AttrDef* sw : shown)
            if (std::all_of(shown.begin(), shown.end(), [sw](const scene::AttrDef* d) { return d == sw || aeSwitches(*sw, *d); }))
                return sw;
        return nullptr;
    };
    /** A section of attr rows; null box when no row shows, unless keepEmpty (a component without attributes yet). */
    auto attrSection = [&](const QString& title, const std::vector<const scene::AttrDef*>& defs, const std::vector<const dx::Issue*>& marks,
                           const QString& about, bool allowSwitch, bool keepEmpty) -> AeSection {
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        const scene::AttrDef* sw = allowSwitch ? headerSwitch(defs) : nullptr;
        int added = 0;
        for (const scene::AttrDef* d : defs)
            if (d != sw)
                added += addAttrRows(form, *d);
        if (!added && !keepEmpty)
        {
            delete content;
            return {};
        }
        if (added && !about.isEmpty())
        {
            auto* note = new QLabel(about);
            note->setWordWrap(true);
            aeDim(note);
            form->insertRow(0, note);
        }
        AeSection s = aeSection(title, content, marks);
        if (sw)
        {
            const scene::PlugRef ref{id, n->type().attrIndex(sw->name)};
            auto* check = new QCheckBox(title);
            check->setObjectName("aeSectionSwitch");
            aeBold(check);
            check->setToolTip(QString::fromStdString(sw->doc));
            check->setEnabled((sw->flags & scene::AttrInput) && !g.source(ref));
            connect(check, &QCheckBox::clicked, this, [this, attr = sw->name](bool on) { set(attr, on); });
            mUpdaters.push_back([check, ref, owner = this] {
                QSignalBlocker block(check);
                const scene::PlugValue v = Graph::current().value(ref);
                if (owner->mixed(ref))
                    check->setCheckState(Qt::PartiallyChecked);
                else if (auto* b = std::get_if<bool>(&v))
                    check->setChecked(*b);
            });
            mAttrWidgets.emplace(sw->name, check);
            aeTitleOn(s, check);
        }
        return s;
    };

    // Attrs with a category show as that category's section, in attr order; those with only a nice name as Parameters.
    std::vector<const scene::AttrDef*> parameters;
    for (const scene::NodeType* t : chain)
        for (const scene::AttrDef& def : t->ownAttrs)
            if (!def.category.empty() || (!def.niceName.empty() && !(def.flags & scene::AttrHidden)))
                parameters.push_back(&def);
    {
        std::vector<std::string> categories{""};
        for (const scene::AttrDef* d : parameters)
            if (std::find(categories.begin(), categories.end(), d->category) == categories.end())
                categories.push_back(d->category);
        for (const std::string& c : categories)
        {
            std::vector<const scene::AttrDef*> defs;
            for (const scene::AttrDef* d : parameters)
                if (d->category == c)
                    defs.push_back(d);
            const QString title = c.empty() ? QString("Parameters") : QString::fromStdString(c);
            AeSection s = attrSection(title, defs, c.empty() ? std::vector<const dx::Issue*>{} : componentIssues({}, c, title), {}, true, false);
            if (!s.box)
                continue;
            noteComponent(c, s.box);
            mBodyLayout->addWidget(s.box);
        }
    }

    for (const scene::NodeType* t : chain)
    {
        std::vector<const scene::AttrDef*> defs;
        for (const scene::AttrDef& def : t->ownAttrs)
            if (std::find(parameters.begin(), parameters.end(), &def) == parameters.end())
                defs.push_back(&def);
        const QString title = niceName(t->name);
        AeSection s = attrSection(title, defs, componentIssues(t->name, {}, title), QString::fromStdString(t->doc), true, false);
        if (!s.box)
            continue;
        noteComponent(t->name, s.box);
        mBodyLayout->addWidget(s.box);
    }
    {
        // Drives: what this object's plugs feed, each a link to the node it drives.
        auto* content = new QWidget;
        QFormLayout* form = newForm(content);
        int added = 0;
        for (const auto& [dst, src] : g.connections())
        {
            if (src.node != id || dst.node == id)
                continue;
            const scene::Node* to = g.node(dst.node);
            if (!to || to->type().isA("animCurve") || to->type().isA("animCurveRQ"))
                continue;
            const std::string from = g.plugName(src);
            auto* link = new QLabel(QString("<a href=\"x\">%1</a>").arg(QString::fromStdString(g.plugName(dst)).toHtmlEscaped()));
            link->setObjectName("aeDrivesLink");
            link->setToolTip("Show the driven node here");
            connect(link, &QLabel::linkActivated, this, [this, nodeId = dst.node] { focusNode(nodeId); });
            auto* label = new QLabel(QString::fromStdString(from.substr(from.find('.') + 1)));
            label->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
            aeDim(label);
            form->addRow(label, link);
            ++added;
        }
        if (added)
            mBodyLayout->addWidget(aeSection("Drives", content).box);
        else
            delete content;
    }
    for (const attributeEditor::Section* extra : extraSections())
        if (n->type().isA(extra->typeName))
            if (QWidget* w = extra->make(id))
            {
                const QString title = QString::fromStdString(extra->title);
                QWidget* box = aeSection(title, w, componentIssues({}, {}, title)).box;
                noteComponent(extra->title, box);
                mBodyLayout->addWidget(box);
            }

    // Added attributes: a divider carrying Add Attribute, then one section per category; uncategorised ones are Custom.
    std::set<QWidget*> addedBoxes;
    {
        auto* divider = new QWidget;
        divider->setObjectName("aeAddedDivider");
        auto* dh = new QHBoxLayout(divider);
        dh->setContentsMargins(4, 10, 6, 4);
        dh->setSpacing(8);
        auto* word = new QLabel("Added", divider);
        word->setFont(aeSmallFont(word->font()));
        aeDim(word);
        auto* rule = new QFrame(divider);
        rule->setFrameShape(QFrame::NoFrame);
        rule->setFixedHeight(1);
        rule->setAutoFillBackground(true);
        QPalette rulePal = rule->palette();
        rulePal.setColor(QPalette::Window, themeColor("border"));
        rule->setPalette(rulePal);
        auto* add = new QToolButton(divider);
        add->setObjectName("extraAdd");
        add->setIcon(icon("plus"));
        add->setText("Add Attribute");
        add->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        add->setAutoRaise(true);
        add->setToolTip("Add an attribute or component to this object");
        connect(add, &QToolButton::clicked, this, [this, add] { openAdd(add); });
        dh->addWidget(word);
        dh->addWidget(rule, 1, Qt::AlignVCenter);
        dh->addWidget(add);
        mBodyLayout->addWidget(divider);

        auto keyOf = [](const std::string& c) { return c.empty() ? std::string(attrAuthoring::kCustomCategory) : c; };
        std::vector<std::string> categories;
        auto addCategory = [&](const std::string& c) {
            if (std::find(categories.begin(), categories.end(), c) == categories.end())
                categories.push_back(c);
        };
        // The schema list itself stays out: each schema is its section, and removing the section removes it.
        const int schemaList = n->type().attrIndex("usdApiSchemas");
        auto shownExtra = [&](const scene::AttrDef& def) { return (def.flags & scene::AttrUser) && def.name != "usdApiSchemas"; };
        for (const scene::AttrDef& def : n->type().attrs)
            if (shownExtra(def))
                addCategory(keyOf(def.category));
        std::set<std::string> schemas;
        if (schemaList >= 0 && n->type().attrs[size_t(schemaList)].type == scene::PlugType::String)
            for (const auto& [index, v] : n->elements(schemaList))
                if (const std::string* s = std::get_if<std::string>(&v); s && !s->empty())
                {
                    addCategory(*s);
                    schemas.insert(*s);
                }
        for (const std::string& c : categories)
        {
            std::vector<const scene::AttrDef*> defs;
            for (const scene::AttrDef& def : n->type().attrs)
                if (shownExtra(def) && keyOf(def.category) == c)
                    defs.push_back(&def);
            const bool usd = schemas.contains(c);
            QString label = QString::fromStdString(c);
            // Only USD objects look up labels: listing USD's schemas loads OpenUSDCore.
            if (schemaList >= 0)
                label = QString::fromStdString(scene::attrComponents::labelOf(c));
            if (usd && label == QString::fromStdString(c))
                label = aeSchemaLabel(c);
            AeSection s = attrSection(label, defs, componentIssues({}, c, QString::fromStdString(c)), {}, false, true);
            auto* heading = new QLabel(label);
            heading->setObjectName("componentHeading");
            aeBold(heading);
            heading->setToolTip(QString::fromStdString(c) + "\nDrag onto another heading to move its attributes; right-click for more");
            QLabel* tag = nullptr;
            if (usd)
            {
                tag = new QLabel("USD");
                tag->setFont(aeSmallFont(tag->font()));
                aeDim(tag);
                tag->setToolTip("A USD API schema applied to this prim");
            }
            aeTitleOn(s, heading, tag);
            heading->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(heading, &QWidget::customContextMenuRequested, this, [this, heading, c](const QPoint& at) { headingMenu(c, heading, at); });
            heading->setAcceptDrops(true);
            heading->setCursor(Qt::OpenHandCursor);
            auto* gestures = new RowGestures(heading);
            gestures->dragMime = kHeadingMime;
            gestures->dragText = QString::fromStdString(c);
            gestures->clicked = [header = s.header] { header->toggle(); };
            gestures->accepts = [c](const QMimeData* d) {
                return d->hasFormat(kAttrMime) || (d->hasFormat(kHeadingMime) && mimeText(d, kHeadingMime) != c);
            };
            gestures->dropped = [this, c](const QMimeData* d) {
                if (d->hasFormat(kAttrMime))
                    dropAttr(mimeText(d, kAttrMime), {}, c);
                else
                    dropHeading(mimeText(d, kHeadingMime), c);
            };
            auto* plus = new QToolButton;
            plus->setObjectName("headingAdd");
            plus->setIcon(icon("plus"));
            plus->setAutoRaise(true);
            plus->setToolTip("Add an attribute under " + label);
            plus->setProperty(kHoverOnly, true);
            connect(plus, &QToolButton::clicked, this, [this, plus, c] { openAdd(plus, c); });
            s.head->addWidget(plus);
            auto* x = new QToolButton;
            x->setIcon(icon("x"));
            x->setAutoRaise(true);
            x->setToolTip("Remove " + label + " and its attributes");
            x->setProperty(kHoverOnly, true);
            connect(x, &QToolButton::clicked, this, [this, c] { removeCategory(c); });
            s.head->addWidget(x);
            noteComponent(c, s.box);
            addedBoxes.insert(s.box);
            mBodyLayout->addWidget(s.box);
        }
    }
    for (int i = 0; i < mBodyLayout->count(); ++i)
        if (QWidget* box = mBodyLayout->itemAt(i)->widget())
            if (QToolButton* header = aeHeaderOf(box))
            {
                mSections.push_back({box, header->text(), addedBoxes.contains(box)});
                rememberCollapse(box, n->type().registered().name + ":" + header->text().toStdString());
                QWidget* head = header->parentWidget();
                auto* help = new QToolButton(head);
                help->setObjectName("sectionHelp");
                help->setIcon(icon("help-circle"));
                help->setAutoRaise(true);
                help->setToolTip("Open the docs for " + QString::fromStdString(n->type().registered().name));
                help->setProperty(kHoverOnly, true);
                static_cast<QHBoxLayout*>(head->layout())->addWidget(help);
                connect(help, &QToolButton::clicked, this, [page = attrView::helpPageFor(n->type())] { docs::open(page); });
                // Header tools show while the pointer is over the header, so resting headers stay plain text.
                std::vector<QWidget*> tools;
                for (QWidget* w : head->findChildren<QWidget*>(QString(), Qt::FindDirectChildrenOnly))
                    if (w->property(kHoverOnly).toBool())
                    {
                        w->hide();
                        tools.push_back(w);
                    }
                (new RowGestures(head))->reveal = std::move(tools);
            }
    mBodyLayout->addStretch(1);
    updateValues();
    fitLabels();
    // Label edges come from where forms sit, which settles once the layout runs and when folded rows first show.
    QTimer::singleShot(0, this, [this] { fitLabels(); });
    for (QToolButton* fold : mBody->findChildren<QToolButton*>())
        if (fold->isCheckable())
            connect(fold, &QToolButton::toggled, this, [this] { QTimer::singleShot(0, this, [this] { fitLabels(); }); });
    applySearch();
    if (gPendingReveal && reveal(gPendingReveal))
        gPendingReveal = 0;
}

std::vector<scene::NodeId> AttributeEditor::targets() const
{
    if (mNode == scene::kNoNode)
        return {};
    if (attrView::multiEditHeld() && mNode == attrView::lead() && Graph::current().selection().size() > 1)
        return attrView::selectionLeadFirst();
    return {mNode};
}

scene::NodeId AttributeEditor::shownNode() const
{
    Graph& g = Graph::current();
    if (mPinned && g.node(mNode))
        return mNode;
    if (mFocus != scene::kNoNode && g.node(mFocus))
        return mFocus;
    return attrView::lead();
}

std::string AttributeEditor::viewSignature() const
{
    std::string sig = std::string(attrView::nameModeName(attrView::nameMode())) + "|" + attrView::currentView() + "|" +
                      std::to_string(attrView::precision()) + "|" + (attrView::showNamespaces() ? "ns" : "");
    for (const attrView::View& v : attrView::views())
        if (v.name == attrView::currentView())
            for (const auto& [type, titles] : v.hidden)
                for (const std::string& t : titles)
                    sig += "|" + type + "/" + t;
    return sig;
}

void AttributeEditor::focusNode(scene::NodeId id)
{
    if (!Graph::current().node(id))
        return;
    mFocus = id;
    mPinned = false;
    mRebuild = true;
    scheduleRefresh();
}

void AttributeEditor::rebuildHeader()
{
    auto* row = static_cast<QHBoxLayout*>(mChips->layout());
    while (QLayoutItem* item = row->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    Graph& g = Graph::current();
    auto tool = [&](const char* name, const char* iconName, const QString& tip) {
        auto* b = new QToolButton(mChips);
        b->setObjectName(name);
        b->setIcon(icon(iconName));
        b->setAutoRaise(true);
        b->setToolTip(tip);
        row->addWidget(b);
        return b;
    };
    QToolButton* back = tool("focusBack", "arrow-left", "Back to the previous object shown");
    QToolButton* forward = tool("focusForward", "arrow-right", "Forward");
    // Drop history entries whose nodes are gone.
    back->setEnabled(mHistoryAt > 0);
    forward->setEnabled(mHistoryAt >= 0 && mHistoryAt + 1 < int(mHistory.size()));
    auto step = [this](int delta) {
        int at = mHistoryAt + delta;
        while (at >= 0 && at < int(mHistory.size()) && !Graph::current().node(mHistory[size_t(at)]))
            at += delta;
        if (at < 0 || at >= int(mHistory.size()))
            return;
        mHistoryAt = at;
        mFocus = mHistory[size_t(at)];
        mNavigating = true;
        rebuild();
        mNavigating = false;
    };
    connect(back, &QToolButton::clicked, this, [step] { step(-1); });
    connect(forward, &QToolButton::clicked, this, [step] { step(1); });

    // Chips wrap rather than widen: a tabbed dock can't shrink below its widest tab, so this row would pin its neighbours.
    auto* chipArea = new QWidget(mChips);
    auto* flow = new FlowLayout(3);
    flow->setContentsMargins(0, 0, 0, 0);
    chipArea->setLayout(flow);
    row->addWidget(chipArea, 1);

    // The selected objects, lead first; a click makes one the lead without touching the selection.
    const std::vector<scene::NodeId> sel = attrView::selectionLeadFirst();
    for (size_t i = 0; i < sel.size() && i < 4; ++i)
    {
        const scene::Node* n = g.node(sel[i]);
        auto* chip = new QToolButton(chipArea);
        chip->setObjectName("selectionChip");
        chip->setText(QString::fromStdString(n->name()));
        chip->setToolTip(i == 0 ? "The lead object: edits go here" : "Click to make this the lead object");
        chip->setCheckable(true);
        chip->setChecked(sel[i] == mNode);
        chip->setAutoRaise(true);
        chip->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        if (i == 0)
            aeBold(chip);
        connect(chip, &QToolButton::clicked, this, [this, id = sel[i]] {
            mFocus = scene::kNoNode;
            mPinned = false;
            attrView::setLead(id);
            mRebuild = true;
            scheduleRefresh();
        });
        flow->addWidget(chip);
    }
    if (sel.size() > 4)
    {
        auto* more = new QLabel(QString("+%1").arg(sel.size() - 4), chipArea);
        aeDim(more);
        flow->addWidget(more);
    }
    mMultiPill = new QLabel(chipArea);
    mMultiPill->setObjectName("multiEditPill");
    aeBold(mMultiPill);
    mMultiPill->hide();
    flow->addWidget(mMultiPill);

    // Related nodes: what feeds the shown one, and the deformers and modifiers it feeds.
    if (const scene::Node* shown = g.node(mNode))
    {
        std::vector<scene::NodeId> related;
        for (const auto& [dst, src] : g.connections())
        {
            scene::NodeId other = scene::kNoNode;
            if (dst.node == mNode)
                other = src.node;
            else if (src.node == mNode)
                if (const scene::Node* d = g.node(dst.node); d && (d->traits() & (scene::Node::kDeformer | scene::Node::kModifier)))
                    other = dst.node;
            const scene::Node* o = g.node(other);
            if (!o || other == mNode || o->type().isA("animCurve") || o->type().isA("animCurveRQ") ||
                std::find(related.begin(), related.end(), other) != related.end())
                continue;
            related.push_back(other);
        }
        (void)shown;
        if (!related.empty())
        {
            auto* sep = new QLabel(QString::fromUtf8("·"), chipArea);
            aeDim(sep);
            flow->addWidget(sep);
        }
        for (size_t i = 0; i < related.size() && i < 4; ++i)
        {
            const scene::Node* o = g.node(related[i]);
            auto* chip = new QToolButton(chipArea);
            chip->setObjectName("relatedChip");
            chip->setText(QString::fromStdString(o->name()));
            chip->setIcon(icon(QString::fromStdString(scene::types::iconFor(o->type(), "box"))));
            chip->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            chip->setToolTip(QString::fromStdString(o->type().registered().name) + ": show it here");
            chip->setAutoRaise(true);
            connect(chip, &QToolButton::clicked, this, [this, id = related[i]] { focusNode(id); });
            flow->addWidget(chip);
        }
    }
    QToolButton* pin = tool("aePin", mPinned ? "pinned" : "pin", mPinned ? "Unpin: follow the selection again" : "Pin: keep showing this object");
    pin->setCheckable(true);
    pin->setChecked(mPinned);
    connect(pin, &QToolButton::toggled, this, [this](bool on) {
        mPinned = on;
        if (!on)
            mFocus = scene::kNoNode;
        mRebuild = true;
        scheduleRefresh();
    });
    QToolButton* show = tool("aeShow", "adjustments", "Show: sections, name mode and saved views");
    connect(show, &QToolButton::clicked, this, [this, show] { showViewPopover(show); });
    mChips->setVisible(true);
    refreshMultiState();
}

void AttributeEditor::refreshMultiState()
{
    if (!mMultiPill)
        return;
    const size_t count = Graph::current().selection().size();
    const bool armed = count > 1 && mNode == attrView::lead() && (attrView::multiEditHeld() || !mBadgeCounts.empty());
    mMultiPill->setText(QString("Editing %1 objects").arg(count));
    mMultiPill->setVisible(armed);
    if (armed)
        mHint->hide();
}

void AttributeEditor::showHint(const QString& text)
{
    mHintText->setText(text);
    mHint->show();
    QTimer::singleShot(5000, mHint, [hint = QPointer<QWidget>(mHint), text, label = QPointer<QLabel>(mHintText)] {
        if (hint && label && label->text() == text)
            hint->hide();
    });
}

void AttributeEditor::showViewPopover(QWidget* anchor)
{
    scene::Node* n = Graph::current().node(mNode);
    auto* pop = new QFrame(window(), Qt::Popup);
    pop->setObjectName("aeViewPopover");
    pop->setAttribute(Qt::WA_DeleteOnClose);
    pop->setFrameShape(QFrame::StyledPanel);
    auto* v = new QVBoxLayout(pop);
    v->setContentsMargins(8, 8, 8, 8);
    v->addWidget(new QLabel("<b>Names</b>", pop));
    auto* modes = new QHBoxLayout;
    auto* group = new QButtonGroup(pop);
    for (attrView::NameMode m : {attrView::NameMode::Nice, attrView::NameMode::Long, attrView::NameMode::Short})
    {
        auto* r = new QRadioButton(m == attrView::NameMode::Nice ? "Nice" : m == attrView::NameMode::Long ? "Long" : "Short", pop);
        r->setObjectName(QString("nameMode_") + attrView::nameModeName(m));
        r->setChecked(attrView::nameMode() == m);
        group->addButton(r);
        connect(r, &QRadioButton::toggled, pop, [m](bool on) {
            if (on)
                attrView::setNameMode(m);
        });
        modes->addWidget(r);
    }
    v->addLayout(modes);
    v->addWidget(new QLabel("<b>View</b>", pop));
    auto* views = new QComboBox(pop);
    views->setObjectName("aeViews");
    for (const attrView::View& view : attrView::views())
        views->addItem(QString::fromStdString(view.name) + (view.source == "studio" ? "  (studio)" : ""), QString::fromStdString(view.name));
    views->setCurrentIndex(std::max(0, views->findData(QString::fromStdString(attrView::currentView()))));
    connect(views, &QComboBox::activated, pop, [views](int i) { attrView::setCurrentView(views->itemData(i).toString().toStdString()); });
    v->addWidget(views);
    auto* saveRow = new QHBoxLayout;
    auto* name = new QLineEdit(pop);
    name->setObjectName("aeViewName");
    name->setPlaceholderText("Save view as...");
    auto* studio = new QCheckBox("Studio", pop);
    studio->setEnabled(!PrefStore::get().studioDirs().empty());
    connect(name, &QLineEdit::returnPressed, pop, [name, studio, pop] {
        if (attrView::saveView(name->text().trimmed().toStdString(), studio->isChecked()))
            pop->close();
    });
    saveRow->addWidget(name, 1);
    saveRow->addWidget(studio);
    v->addLayout(saveRow);
    if (n)
    {
        v->addWidget(new QLabel("<b>Sections</b>", pop));
        const std::string type = n->type().registered().name;
        for (const SectionEntry& s : mSections)
        {
            const QString title = s.title;
            if (title == "Issues")
                continue;
            auto* cb = new QCheckBox(title, pop);
            cb->setObjectName("sectionToggle");
            cb->setChecked(attrView::sectionShown(type, title, s.added));
            connect(cb, &QCheckBox::toggled, pop, [type, title](bool on) { attrView::setSectionShown(type, title, on); });
            v->addWidget(cb);
        }
    }
    pop->adjustSize();
    pop->move(anchor->mapToGlobal(QPoint(anchor->width() - pop->width(), anchor->height())));
    pop->show();
}

void AttributeEditor::openAdd(QWidget* anchor, std::string category)
{
    if (mNode != scene::kNoNode)
        attrAuthoring::showAdd(targets(), anchor, std::move(category));
}

void AttributeEditor::openEdit(const std::string& attr, QWidget* anchor)
{
    if (mNode != scene::kNoNode)
        attrAuthoring::showEdit(targets(), attr, anchor);
}

namespace
{
double valueOfPlug(Graph& g, const std::string& plug)
{
    const scene::PlugRef p = g.plug(plug);
    const scene::PlugValue v = g.value(p);
    if (auto* d = std::get_if<double>(&v))
        return *d;
    if (auto* i = std::get_if<int64_t>(&v))
        return double(*i);
    if (auto* b = std::get_if<bool>(&v))
        return *b ? 1.0 : 0.0;
    if (auto* v3 = std::get_if<glm::dvec3>(&v))
        return (*v3)[std::max(0, p.component)];
    return 0.0;
}

json aeRunLogged(const char* command, const json& call)
{
    try
    {
        return ops::execute(command, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return json();
    }
}
} // namespace

void AttributeEditor::headingMenu(const std::string& category, QWidget* on, QPoint at)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n)
        return;
    const std::string path = n->path();
    QMenu menu;
    menu.addAction(icon("plus"), "Add Attribute Here...", [this, on, category] { openAdd(on, category); });
    menu.addSeparator();
    auto save = [path, category](bool studio) {
        json file = aeRunLogged("saveComponent", {{"args", {path}}, {"category", category}, {"studio", studio}});
        if (file.is_string())
            log::info("Saved component {} to {}", category, file.get<std::string>());
    };
    menu.addAction(icon("device-floppy"), "Save as Component", [save] { save(false); });
    QAction* studio = menu.addAction("Save to Studio Library", [save] { save(true); });
    studio->setEnabled(!PrefStore::get().studioDirs().empty());
    QMenu* unreal = menu.addMenu("Unreal Export");
    const json current = aeRunLogged("componentExport", {{"args", {path}}, {"component", category}, {"query", true}});
    const std::string mode = current.is_object() ? current.value("mode", "assetUserData") : "assetUserData";
    for (auto [id, label] : {std::pair{"assetUserData", "Asset User Data"}, {"actorTags", "Actor Tags"}, {"component", "Actor Component..."},
                             {"dataAsset", "Data Asset..."}, {"custom", "Custom Exporter..."}, {"none", "Don't Export"}})
    {
        QAction* a = unreal->addAction(label);
        a->setCheckable(true);
        a->setChecked(mode == id);
        const std::string modeId = id;
        connect(a, &QAction::triggered, this, [path, category, modeId, on, current] {
            json call = {{"args", {path}}, {"component", category}, {"mode", modeId}};
            if (modeId != "component" && modeId != "dataAsset" && modeId != "custom")
            {
                aeRunLogged("componentExport", call);
                return;
            }
            // The class or exporter name, typed in place.
            auto* box = new QLineEdit(on->window());
            box->setWindowFlags(Qt::Popup);
            box->setAttribute(Qt::WA_DeleteOnClose);
            box->setMinimumWidth(320);
            box->setPlaceholderText(modeId == "custom" ? "Exporter name" : "/Script/Module.ClassName or /Game/Path/Class");
            box->setText(QString::fromStdString(current.is_object() ? current.value("class", "") : ""));
            box->move(on->mapToGlobal(QPoint(0, on->height())));
            QObject::connect(box, &QLineEdit::returnPressed, box, [box, call]() mutable {
                call["unrealClass"] = box->text().trimmed().toStdString();
                aeRunLogged("componentExport", call);
                box->close();
            });
            box->show();
            box->setFocus();
        });
    }
    menu.addSeparator();
    menu.addAction(icon("x"), "Remove Component", [this, category] { removeCategory(category); });
    menu.exec(on->mapToGlobal(at));
}

void AttributeEditor::dropAttr(const std::string& attr, const std::string& before, const std::string& category)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n || attr == before)
        return;
    const int from = n->type().attrIndex(attr);
    if (from < 0)
        return;
    UndoChunk chunk("Move Attribute");
    const std::string plug = n->path() + "." + attr;
    if (n->type().attrs[size_t(from)].category != category)
        aeRunLogged("editAttr", {{"args", {plug}}, {"category", category}});
    if (!before.empty())
    {
        aeRunLogged("reorderAttr", {{"args", {plug}}, {"before", before}});
        return;
    }
    // Under a heading: after its last attribute.
    std::string last;
    for (const scene::AttrDef& d : n->type().attrs)
        if ((d.flags & scene::AttrUser) && d.category == category && d.name != attr)
            last = d.name;
    if (!last.empty())
        aeRunLogged("reorderAttr", {{"args", {plug}}, {"after", last}});
}

void AttributeEditor::dropHeading(const std::string& category, const std::string& before)
{
    scene::Node* n = Graph::current().node(mNode);
    if (!n || category == before)
        return;
    std::vector<std::string> moving;
    std::string first;
    for (const scene::AttrDef& d : n->type().attrs)
        if (d.flags & scene::AttrUser)
        {
            if (d.category == category)
                moving.push_back(d.name);
            else if (d.category == before && first.empty())
                first = d.name;
        }
    if (moving.empty() || first.empty())
        return;
    UndoChunk chunk("Move Component");
    for (const std::string& a : moving)
        aeRunLogged("reorderAttr", {{"args", {n->path() + "." + a}}, {"before", first}});
}

void AttributeEditor::dropPlug(const std::string& source, const std::string& target)
{
    if (source.empty() || source == target)
        return;
    if (QGuiApplication::keyboardModifiers() & Qt::AltModifier)
    {
        attrView::noteMultiEditUsed();
        Graph& g = Graph::current();
        try
        {
            const double d = valueOfPlug(g, source), v = valueOfPlug(g, target);
            aeRunLogged("driveChannel", {{"args", {target}}, {"driver", source}, {"keys", json::array({json::array({d, v}), json::array({d + 1.0, v + 1.0})})},
                                        {"force", true}});
        }
        catch (const std::exception& e)
        {
            log::error("{}", e.what());
        }
        return;
    }
    aeRunLogged("connectAttr", {{"args", {source, target}}, {"force", true}});
}

void AttributeEditor::applySearch()
{
    const QString q = mSearch->text().trimmed();
    std::set<QWidget*> hit;
    for (SearchRow& r : mSearchRows)
    {
        const bool show = attrAuthoring::matches(r.def, q);
        if (show != r.shown)
        {
            r.form->setRowVisible(r.label, show);
            r.shown = show;
        }
        if (show)
            for (QWidget* w = r.form->parentWidget(); w && w != mBody; w = w->parentWidget())
                hit.insert(w);
    }
    for (const auto& [box, title, added] : mSections)
    {
        const scene::Node* shown = Graph::current().node(mNode);
        const bool inView = !shown || title == "Issues" || attrView::sectionShown(shown->type().registered().name, title, added);
        const bool show = inView && (q.isEmpty() || hit.contains(box) || title.contains(q, Qt::CaseInsensitive));
        box->setVisible(show);
        if (q.isEmpty() || !show)
            continue;
        // Searching opens the sections it finds rows in, without remembering them open.
        if (QToolButton* header = aeHeaderOf(box))
        {
            QSignalBlocker block(header);
            header->setChecked(true);
            header->setIcon(icon("chevron-down"));
            if (QLayoutItem* item = box->layout()->itemAt(1); item && item->widget())
                item->widget()->setVisible(true);
        }
    }
}

void AttributeEditor::rememberCollapse(QWidget* section, const std::string& key)
{
    QToolButton* header = aeHeaderOf(section);
    if (!header)
        return;
    std::string safe = key;
    for (char& c : safe)
        if (c == '/' || c == '~')
            c = '_';
    const std::string pointer = "/collapsed/" + safe;
    if (PrefStore::get().value("attributeEditor", pointer, false))
        header->setChecked(false);
    connect(header, &QToolButton::toggled, this, [this, pointer](bool on) {
        if (mSearch->text().trimmed().isEmpty())
            PrefStore::get().set("attributeEditor", pointer, !on);
    });
}

void AttributeEditor::fitLabels()
{
    struct Row
    {
        AttrRowLabel* label;
        int x;
    };
    std::vector<Row> rows;
    int longest = 0, shortest = 0;
    for (QFormLayout* form : mBody->findChildren<QFormLayout*>())
    {
        if (!form->parentWidget())
            continue;
        const int x = form->parentWidget()->mapTo(mBody, QPoint()).x() + form->contentsMargins().left();
        for (int r = 0; r < form->rowCount(); ++r)
        {
            QLayoutItem* l = form->itemAt(r, QFormLayout::LabelRole);
            auto* label = l ? dynamic_cast<AttrRowLabel*>(l->widget()) : nullptr;
            if (!label || !form->itemAt(r, QFormLayout::FieldRole))
                continue;
            rows.push_back({label, x});
            longest = std::max(longest, x + label->QLabel::sizeHint().width());
            shortest = std::max(shortest, x + label->minimumSizeHint().width());
        }
    }
    if (rows.empty())
        return;
    // Fields keep at least this much of the width, so a long label or a wide drag never squeezes them away.
    constexpr int kFieldRoom = 90;
    const int width = mScroll->viewport()->width();
    const double fraction = mDragFraction >= 0.0 ? mDragFraction : PrefStore::get().value("attributeEditor", "/labelFraction", 0.0);
    const int wanted = fraction > 0.0 ? int(std::lround(fraction * width)) : std::min(longest, int(width * 0.34));
    mLabelEdge = std::clamp(wanted, shortest, std::max(shortest, width - kFieldRoom));
    for (const Row& r : rows)
    {
        const int w = mLabelEdge - r.x;
        if (r.label->minimumWidth() != w || r.label->maximumWidth() != w)
            r.label->setFixedWidth(w);
    }
}

void AttributeEditor::dragLabelEdge(int x, bool commit)
{
    const int width = std::max(1, mScroll->viewport()->width());
    mDragFraction = double(x) / width;
    fitLabels();
    if (!commit)
        return;
    PrefStore::get().set("attributeEditor", "/labelFraction", double(mLabelEdge) / width);
    mDragFraction = -1.0;
}

void AttributeEditor::resetLabelEdge()
{
    mDragFraction = -1.0;
    PrefStore::get().remove("attributeEditor", "/labelFraction");
    fitLabels();
}

bool AttributeEditor::eventFilter(QObject* watched, QEvent* event)
{
    if (watched == mScroll->viewport() && event->type() == QEvent::Resize)
        fitLabels();
    return QWidget::eventFilter(watched, event);
}

void AttributeEditor::updateValues()
{
    LILA_ZONE();

    Graph::current().evaluate();
    if (!Graph::current().node(mNode))
        return;
    for (auto& fn : mUpdaters)
        fn();
}
} // namespace lila::ui
