#include "ui/ProgressOverlay.h"

#include "core/MainQueue.h"
#include "core/Progress.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMainWindow>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QWindow>

#include <cmath>
#include <optional>

namespace lila::ui::progressui
{
namespace
{
constexpr double kShowAfter = 0.3; // seconds; quicker operations never flash a card
constexpr int kCardWidth = 440;
constexpr int kCardHeight = 132;

const QColor kVeil(0, 0, 0, 110);
const QColor kCard(30, 30, 30, 242);
const QColor kCardEdge(72, 72, 72);
const QColor kTrack(58, 58, 58);
const QColor kFill(205, 205, 205);
const QColor kTitle(232, 232, 232);
const QColor kStatus(168, 168, 168);
const QColor kFaint(120, 120, 120);

/** Determinate fill, or a segment sliding back and forth when the total is unknown. */
void paintBar(QPainter& p, const QRectF& r, const progress::Snapshot& s, double clock)
{
    const double radius = r.height() / 2.0;
    p.setPen(Qt::NoPen);
    p.setBrush(kTrack);
    p.drawRoundedRect(r, radius, radius);
    QRectF fill = r;
    if (s.determinate)
        fill.setWidth(std::max(r.height(), r.width() * s.fraction));
    else
    {
        const double seg = r.width() * 0.28;
        const double t = 0.5 - 0.5 * std::cos(clock * 2.4);
        fill = QRectF(r.left() + (r.width() - seg) * t, r.top(), seg, r.height());
    }
    p.setBrush(kFill);
    p.drawRoundedRect(fill, radius, radius);
}

/** A dark card centred over the main window, on a veil that dims the rest. */
class Card : public QWidget
{
public:
    explicit Card(QWidget* parent) : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setCursor(Qt::BusyCursor);
        mCancel = makeButton("Abort");
        mKeepGoing = makeButton("Continue");
        connect(mCancel, &QPushButton::clicked, this, [this] {
            if (mConfirming)
                abort();
            else
                setConfirming(true);
        });
        connect(mKeepGoing, &QPushButton::clicked, this, [this] { setConfirming(false); });
        mClock.start();
    }

    void set(const progress::Snapshot& s)
    {
        if (s.id != mState.id)
        {
            mConfirming = false;
            mCancel->setEnabled(true);
            mCancel->setText("Abort");
        }
        mState = s;
        mCancel->setVisible(s.cancellable && !s.cancelled);
        mKeepGoing->setVisible(mConfirming && cancellable());
        placeButtons();
        update();
    }

    uint64_t id() const { return mState.id; }
    bool cancellable() const { return mState.cancellable && !mState.cancelled; }
    bool confirming() const { return mConfirming; }

    /** The first click (or Esc) asks; the Abort button then confirms. */
    void setConfirming(bool on)
    {
        mConfirming = on && cancellable();
        mKeepGoing->setVisible(mConfirming);
        placeButtons();
        update();
    }

    void abort()
    {
        progress::cancel(mState.id);
        mConfirming = false;
        mKeepGoing->hide();
        mCancel->setEnabled(false);
        mCancel->setText("Aborting...");
        placeButtons();
        update();
    }

protected:
    void resizeEvent(QResizeEvent*) override { placeButtons(); }

    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), kVeil);

        const QRect c = cardRect();
        p.setPen(QPen(kCardEdge, 1.0));
        p.setBrush(kCard);
        p.drawRoundedRect(QRectF(c).adjusted(0.5, 0.5, -0.5, -0.5), 8, 8);

        const int pad = 20;
        QFont title = font();
        title.setPointSizeF(title.pointSizeF() * 1.25);
        title.setBold(true);
        p.setFont(title);
        p.setPen(kTitle);
        const QRect titleRect(c.left() + pad, c.top() + 16, c.width() - 2 * pad, QFontMetrics(title).height());
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(title).elidedText(QString::fromStdString(mState.title), Qt::ElideRight, titleRect.width()));

        const QRectF bar(c.left() + pad, titleRect.bottom() + 14, c.width() - 2 * pad, 6);
        paintBar(p, bar, mState, mClock.elapsed() / 1000.0);

        QFont smallFont = font();
        p.setFont(smallFont);
        const QFontMetrics fm(smallFont);
        const QString percent = mState.determinate ? QString("%1%").arg(int(std::floor(mState.fraction * 100.0))) : QString();
        const int percentWidth = percent.isEmpty() ? 0 : fm.horizontalAdvance("100%") + 8;
        const QRect statusRect(int(bar.left()), int(bar.bottom()) + 10, int(bar.width()) - percentWidth, fm.height());
        p.setPen(kStatus);
        p.drawText(statusRect, Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(QString::fromStdString(mState.status), Qt::ElideMiddle, statusRect.width()));
        if (!percent.isEmpty())
            p.drawText(QRect(statusRect.right(), statusRect.top(), percentWidth, statusRect.height()), Qt::AlignRight | Qt::AlignVCenter, percent);

        const QRect elapsedRect(int(bar.left()), c.bottom() - 12 - mCancel->sizeHint().height(), 200, mCancel->sizeHint().height());
        p.setPen(mConfirming ? kTitle : kFaint);
        p.drawText(elapsedRect, Qt::AlignLeft | Qt::AlignVCenter,
                   mConfirming ? QStringLiteral("Abort? Nothing is kept.") : QString::fromStdString(progress::formatElapsed(mState.elapsed)));
    }

private:
    /** Never takes focus, so Space or Enter meant for the app can't press it. */
    QPushButton* makeButton(const QString& text)
    {
        auto* b = new QPushButton(text, this);
        b->setCursor(Qt::ArrowCursor);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoDefault(false);
        b->hide();
        return b;
    }

    void placeButtons()
    {
        const QRect c = cardRect();
        mCancel->adjustSize();
        mCancel->move(c.right() - 16 - mCancel->width(), c.bottom() - 12 - mCancel->height());
        mKeepGoing->adjustSize();
        mKeepGoing->move(mCancel->x() - 8 - mKeepGoing->width(), mCancel->y());
    }

    QRect cardRect() const
    {
        return QRect((width() - kCardWidth) / 2, (height() - kCardHeight) / 2, kCardWidth, kCardHeight);
    }

    progress::Snapshot mState;
    QPushButton* mCancel = nullptr;
    QPushButton* mKeepGoing = nullptr;
    bool mConfirming = false;
    QElapsedTimer mClock;
};

/** Background operations: a label and a thin bar in the status bar, hidden when nothing runs. */
class Indicator : public QWidget
{
public:
    explicit Indicator(QWidget* parent) : QWidget(parent)
    {
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(4, 0, 4, 0);
        row->setSpacing(6);
        mLabel = new QLabel(this);
        mLabel->setStyleSheet("color: #9a9a9a;");
        row->addWidget(mLabel);
        row->addSpacing(64);
        mClock.start();
        hide();
    }

    void set(const std::vector<progress::Snapshot>& background)
    {
        if (background.empty())
        {
            hide();
            return;
        }
        mState = background.front();
        QString text = QString::fromStdString(mState.title);
        if (background.size() > 1)
            text += QString(" (+%1)").arg(background.size() - 1);
        mLabel->setText(text);
        setToolTip(QString::fromStdString(mState.status));
        show();
        update();
    }

protected:
    void paintEvent(QPaintEvent* e) override
    {
        QWidget::paintEvent(e);
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        paintBar(p, QRectF(width() - 64 - 4, height() / 2.0 - 2, 60, 4), mState, mClock.elapsed() / 1000.0);
    }

private:
    QLabel* mLabel = nullptr;
    progress::Snapshot mState;
    QElapsedTimer mClock;
};

/** While the card is up: input anywhere else is dropped and closing the window is refused. */
class InputBlocker : public QObject
{
public:
    InputBlocker(QWidget* window, Card* card) : QObject(card), mWindow(window), mCard(card) {}

    bool active = false;

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (!active)
            return false;
        switch (e->type())
        {
        case QEvent::Close:
            if (o == mWindow || o == mWindow->windowHandle())
            {
                e->ignore();
                return true;
            }
            return false;
        case QEvent::KeyPress:
            if (auto* k = static_cast<QKeyEvent*>(e); k->key() == Qt::Key_Escape && !k->isAutoRepeat() && mCard->cancellable())
                mCard->setConfirming(!mCard->confirming());
            [[fallthrough]];
        case QEvent::KeyRelease:
        case QEvent::ShortcutOverride:
        case QEvent::Shortcut:
        case QEvent::MouseButtonPress:
        case QEvent::MouseButtonRelease:
        case QEvent::MouseButtonDblClick:
        case QEvent::MouseMove:
        case QEvent::Wheel:
        case QEvent::ContextMenu:
        case QEvent::TabletPress:
        case QEvent::TabletRelease:
        case QEvent::TabletMove:
        case QEvent::TouchBegin:
        case QEvent::TouchUpdate:
        case QEvent::TouchEnd:
        case QEvent::DragEnter:
        case QEvent::DragMove:
        case QEvent::Drop:
        case QEvent::HoverEnter:
        case QEvent::HoverMove:
            return !onCard(o);
        default:
            return false;
        }
    }

private:
    bool onCard(QObject* o) const
    {
        if (auto* w = qobject_cast<QWidget*>(o))
            return w->window() == mCard;
        if (auto* win = qobject_cast<QWindow*>(o))
            return win == mCard->windowHandle();
        return false;
    }

    QWidget* mWindow;
    Card* mCard;
};

struct State
{
    QPointer<QMainWindow> window;
    Card* card = nullptr;
    Indicator* indicator = nullptr;
    InputBlocker* blocker = nullptr;
    bool pumping = false;
};

State& state()
{
    static State s;
    return s;
}

std::optional<progress::Snapshot> modalRoot(const std::vector<progress::Snapshot>& roots)
{
    for (const progress::Snapshot& s : roots)
        if (s.mainThread && !s.silent)
            return s;
    return std::nullopt;
}

void hideCard()
{
    State& s = state();
    if (s.blocker)
        s.blocker->active = false;
    if (s.card && s.card->isVisible())
        s.card->hide();
}

void showCard(const progress::Snapshot& snap)
{
    State& s = state();
    s.card->setGeometry(s.window->geometry());
    s.card->set(snap);
    s.blocker->active = true;
    if (!s.card->isVisible())
    {
        s.card->show();
        s.card->raise();
    }
}

void refreshIndicator(const std::vector<progress::Snapshot>& roots)
{
    std::vector<progress::Snapshot> background;
    for (const progress::Snapshot& r : roots)
        if (!r.mainThread || r.silent)
            background.push_back(r);
    state().indicator->set(background);
}

/** Called from progress steps on the main thread while a long operation runs there. */
void pump()
{
    State& s = state();
    if (!s.window || !s.window->isVisible() || s.pumping)
        return;
    const std::vector<progress::Snapshot> roots = progress::snapshot();
    const std::optional<progress::Snapshot> modal = modalRoot(roots);
    if (!modal)
    {
        hideCard();
        return;
    }
    if (modal->elapsed < kShowAfter && !modal->immediate)
        return;
    showCard(*modal);
    refreshIndicator(roots);
    s.pumping = true;
    {
        // Work queued for the main thread (MCP calls, file watchers) waits for the operation to finish.
        mainq::Hold hold;
        QApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    s.pumping = false;
}
} // namespace

void install(QMainWindow* window)
{
    State& s = state();
    s.window = window;
    s.card = new Card(window);
    s.blocker = new InputBlocker(window, s.card);
    qApp->installEventFilter(s.blocker);
    s.indicator = new Indicator(window);
    window->statusBar()->addPermanentWidget(s.indicator);

    progress::setPump(pump, 30);
    // Between operations, and for ones on other threads: the indicator, and the card going away when a load ends.
    auto* poll = new QTimer(window);
    QObject::connect(poll, &QTimer::timeout, window, [] {
        const std::vector<progress::Snapshot> roots = progress::snapshot();
        refreshIndicator(roots);
        if (!modalRoot(roots))
            hideCard();
    });
    poll->start(100);
    QObject::connect(window, &QObject::destroyed, [] {
        progress::setPump(nullptr);
        State& st = state();
        st.card = nullptr;
        st.indicator = nullptr;
        st.blocker = nullptr;
    });
}
} // namespace lila::ui::progressui
