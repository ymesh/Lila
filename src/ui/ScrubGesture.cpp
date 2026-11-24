#include "ui/ScrubGesture.h"

#include "ui/Theme.h"
#include "ui/ValueReset.h"

#include <QApplication>
#include <QCursor>
#include <QDateTime>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
constexpr int kLadderRowHeight = 24;
constexpr int kLadderWidth = 64;

qint64 scrubNowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QString stepText(double step)
{
    return step >= 1.0 ? QString::number(step, 'f', 0) : QString::number(step, 'f', int(std::lround(-std::log10(step)))).mid(1);
}
} // namespace

/** The column of steps shown while the middle button picks one; it never takes the mouse. */
class StepLadder : public QWidget
{
public:
    StepLadder(std::vector<double> steps, int row) : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint), mSteps(std::move(steps)), mRow(row)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TranslucentBackground);
        setFixedSize(kLadderWidth, kLadderRowHeight * int(mSteps.size()));
    }

    void setRow(int row)
    {
        if (row != mRow)
        {
            mRow = row;
            update();
        }
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(themeColor("tooltip"));
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        QFont f = font();
        for (int i = 0; i < int(mSteps.size()); ++i)
        {
            const QRect cell(0, i * kLadderRowHeight, width(), kLadderRowHeight);
            const bool on = i == mRow;
            if (on)
            {
                p.setBrush(themeColor("checked"));
                p.drawRoundedRect(QRectF(cell).adjusted(2, 1, -2, -1), 3, 3);
            }
            f.setBold(on);
            p.setFont(f);
            p.setPen(on ? themeColor("checkedText") : themeColor("dimText"));
            p.drawText(cell.adjusted(0, 0, -8, 0), Qt::AlignRight | Qt::AlignVCenter, stepText(mSteps[size_t(i)]));
            p.setPen(Qt::NoPen);
        }
    }

private:
    std::vector<double> mSteps;
    int mRow = 0;
};

ScrubGesture::ScrubGesture(Target target) : mT(std::move(target)) {}

ScrubGesture::~ScrubGesture()
{
    if (mDragging)
        finish(false);
    closeLadder();
}

std::vector<double> ScrubGesture::ladderSteps(double span, int decimals, bool integer)
{
    const int smallest = integer ? 0 : -std::clamp(decimals, 0, 4);
    int largest = 2;
    if (span > 0.0)
        largest = std::clamp(int(std::floor(std::log10(span))), smallest, 2);
    std::vector<double> out;
    for (int e = largest; e >= smallest; --e)
        out.push_back(std::pow(10.0, e));
    return out;
}

double ScrubGesture::modifierScale(Qt::KeyboardModifiers mods) const
{
    return (mods & Qt::ControlModifier) ? 0.1 : (mods & Qt::ShiftModifier) ? 10.0 : 1.0;
}

bool ScrubGesture::press(QMouseEvent* e)
{
    // A release some other filter took leaves a press behind; this press starts afresh.
    if (mButton)
        finish(true);
    const bool left = e->button() == Qt::LeftButton, middle = e->button() == Qt::MiddleButton;
    if (!left && !middle)
        return false;
    // A field being typed in keeps its left clicks for the text.
    if (left && mT.editing && mT.editing())
        return false;
    mButton = int(e->button());
    mPress = mLast = e->globalPosition().toPoint();
    mPressMs = scrubNowMs();
    mDragging = mRefused = false;
    mTotal = mPixels = mRowBase = mRowPixels = 0.0;
    mRow = -1;
    if (middle)
    {
        if (!mHold)
        {
            mHold = std::make_unique<QTimer>();
            mHold->setSingleShot(true);
            QObject::connect(mHold.get(), &QTimer::timeout, [this] {
                if (mButton != Qt::MiddleButton || mDragging || mRow >= 0)
                    return;
                if (QGuiApplication::mouseButtons() & Qt::MiddleButton)
                    openLadder();
                else
                    finish(true);
            });
        }
        mHold->start(int(valueReset::kMaxMillis));
    }
    return true;
}

void ScrubGesture::openLadder()
{
    const double span = mT.span ? mT.span() : 0.0;
    mSteps = ladderSteps(span, mT.decimals, mT.integer);
    const double base = mT.step ? mT.step() : 1.0;
    const double want = std::log10(std::max(base, 1e-9));
    mStartRow = 0;
    for (int i = 0; i < int(mSteps.size()); ++i)
        if (std::abs(std::log10(mSteps[size_t(i)]) - want) < std::abs(std::log10(mSteps[size_t(mStartRow)]) - want))
            mStartRow = i;
    mRow = mStartRow;
    mRowBase = mTotal;
    mRowPixels = 0.0;
    auto* ladder = new StepLadder(mSteps, mRow);
    // The picked row sits under the pointer, so up and down read as moving along the column.
    ladder->move(mPress.x() - kLadderWidth / 2, mPress.y() - mRow * kLadderRowHeight - kLadderRowHeight / 2);
    ladder->show();
    mLadder = ladder;
    QApplication::setOverrideCursor(Qt::SizeAllCursor);
}

void ScrubGesture::closeLadder()
{
    if (mLadder)
    {
        delete mLadder.data();
        QApplication::restoreOverrideCursor();
    }
    mRow = -1;
}

void ScrubGesture::startDrag()
{
    if (mHold)
        mHold->stop();
    if (mT.begin && !mT.begin())
    {
        mRefused = true;
        return;
    }
    mDragging = true;
    if (!mLadder)
        QApplication::setOverrideCursor(Qt::SizeHorCursor);
}

bool ScrubGesture::move(QMouseEvent* e)
{
    if (!mButton)
        return false;
    // The release went missing (focus moved mid-drag).
    if (!(e->buttons() & Qt::MouseButton(mButton)))
    {
        finish(true);
        return false;
    }
    const QPoint at = e->globalPosition().toPoint();
    const int dx = at.x() - mLast.x();
    mLast = at;
    if (mRefused)
        return true;
    const QPoint travel = at - mPress;
    if (!mDragging && mRow < 0)
    {
        if (std::max(std::abs(travel.x()), std::abs(travel.y())) < valueReset::kMaxTravel)
            return true;
        // Up or down first with the middle button asks for the ladder; sideways is a plain drag.
        if (mButton == Qt::MiddleButton && std::abs(travel.y()) > std::abs(travel.x()))
        {
            openLadder();
            return true;
        }
        startDrag();
        if (mRefused)
            return true;
    }
    if (mRow >= 0)
    {
        const int row = std::clamp(mStartRow + int(std::lround(double(travel.y()) / kLadderRowHeight)), 0, int(mSteps.size()) - 1);
        if (row != mRow)
        {
            mRow = row;
            mRowBase = mTotal;
            mRowPixels = 0.0;
            mLadder->setRow(row);
        }
        if (dx != 0 && !mDragging)
        {
            startDrag();
            if (mRefused)
                return true;
        }
        mRowPixels += dx;
        mTotal = mRowBase + std::trunc(mRowPixels / kScrubPixelsPerStep) * mSteps[size_t(mRow)];
        if (mDragging)
            mT.drag(mTotal, 0.0);
        wrapAtScreenEdge(at);
        return true;
    }
    const double scale = modifierScale(e->modifiers());
    const double span = mT.span ? mT.span() : 0.0;
    const double perStep = (mT.step ? mT.step() : 1.0) / kScrubPixelsPerStep;
    double perPixel = span > 0.0 && mT.field ? span / std::max(20, mT.field->width()) : perStep;
    // Counts (subdivisions, copies) often have a huge safety range; one step per kScrubPixelsPerStep keeps a drag from
    // building a mesh that freezes the machine.
    if (mT.integer)
        perPixel = std::min(perPixel, perStep);
    mTotal += dx * perPixel * scale;
    mSnap = mT.snap && span <= 0.0 ? (mT.step ? mT.step() : 1.0) * scale : 0.0;
    mT.drag(mTotal, mSnap);
    wrapAtScreenEdge(at);
    return true;
}

bool ScrubGesture::release(QMouseEvent* e)
{
    if (!mButton || e->button() != Qt::MouseButton(mButton))
        return mButton != 0;
    const bool middle = mButton == Qt::MiddleButton;
    const QPoint travel = e->globalPosition().toPoint() - mPress;
    const bool still = std::max(std::abs(travel.x()), std::abs(travel.y())) < valueReset::kMaxTravel;
    const bool quick = scrubNowMs() - mPressMs < valueReset::kMaxMillis;
    const bool ladder = mRow >= 0;
    const bool dragged = mDragging;
    finish(true);
    if (dragged || ladder || mRefused)
        return true;
    if (middle && still && quick && mT.reset)
        mT.reset();
    else if (!middle && still && mT.click)
        mT.click();
    return true;
}

bool ScrubGesture::key(QKeyEvent* e)
{
    if (!mDragging || e->key() != Qt::Key_Escape)
        return false;
    finish(false);
    mButton = 0;
    return true;
}

void ScrubGesture::finish(bool commit)
{
    if (mHold)
        mHold->stop();
    const bool dragged = std::exchange(mDragging, false);
    const bool ladder = mLadder;
    closeLadder();
    if (dragged)
    {
        if (!ladder)
            QApplication::restoreOverrideCursor();
        if (mT.end)
            mT.end(commit);
    }
    mButton = 0;
}

void ScrubGesture::wrapAtScreenEdge(const QPoint& at)
{
    QScreen* screen = QGuiApplication::screenAt(at);
    if (!screen)
        return;
    const QRect g = screen->geometry();
    int x = at.x();
    if (x <= g.left() + 1)
        x = g.right() - 2;
    else if (x >= g.right() - 1)
        x = g.left() + 2;
    else
        return;
    // The press point moves with the pointer so travel and the ladder row stay true.
    mPress.rx() += x - at.x();
    mLast.setX(x);
    QCursor::setPos(screen, x, at.y());
}
} // namespace lila::ui
