#pragma once

#include "core/Export.h"

#include <QPoint>
#include <QPointer>

#include <functional>
#include <memory>
#include <vector>

class QKeyEvent;
class QMouseEvent;
class QTimer;
class QWidget;

namespace lila::ui
{
class StepLadder;

/** Pixels of sideways travel per step, on every number field and in the Step Ladder. */
inline constexpr double kScrubPixelsPerStep = 10.0;

/**
 * The drag every number field shares, so they all behave alike:
 * - Left or middle drag sideways changes the value a step per kScrubPixelsPerStep pixels (Ctrl 0.1x, Shift 10x), and
 *   carries on from the other side when the pointer reaches a screen edge. Esc during the drag puts the value back.
 * - A left click without a drag starts typing (Target::click); a middle click without a drag resets (Target::reset).
 * - Moving the middle button up or down first, or holding it still, opens the Step Ladder: up and down pick a step
 *   (100 down to 0.0001, bounded by the field), sideways then changes the value by that step.
 * Feed it the field's mouse and key events; each call returns whether it used the event.
 */
class LILA_UI_API ScrubGesture
{
public:
    struct Target
    {
        /** The field the drag belongs to; the ladder opens beside it. */
        QWidget* field = nullptr;
        /** Value per step at 1x (a field's single step), and whether a drag snaps to whole steps. */
        std::function<double()> step;
        bool snap = false;
        /** Range covered by the field's width, for fields that show a fill; 0 for none. */
        std::function<double()> span;
        /** Shown decimals and whole-number fields bound the ladder's smallest step. */
        int decimals = 3;
        bool integer = false;
        /** False refuses the drag (read-only, locked). Called when sideways travel starts one. */
        std::function<bool()> begin;
        /** The value change since begin. snap is the step to round the result to, 0 to leave it as is. */
        std::function<void(double total, double snap)> drag;
        /** Ends a drag: commit, or put the value back (Esc). */
        std::function<void(bool commit)> end;
        /** A left click without a drag; empty leaves the left button to the field. */
        std::function<void()> click;
        /** A middle click without a drag; empty for no reset. */
        std::function<void()> reset;
        /** True while the field is being typed in: the left button then edits text. */
        std::function<bool()> editing;
    };

    explicit ScrubGesture(Target target);
    ~ScrubGesture();
    ScrubGesture(const ScrubGesture&) = delete;
    ScrubGesture& operator=(const ScrubGesture&) = delete;

    bool press(QMouseEvent* e);
    bool move(QMouseEvent* e);
    bool release(QMouseEvent* e);
    bool key(QKeyEvent* e);

    /** A button is down on the field. */
    bool pressed() const { return mButton != 0; }
    /** A drag is changing the value. */
    bool dragging() const { return mDragging; }

    /** The ladder's steps for a field: powers of ten from 100 (or the span) down to its smallest shown step. */
    static std::vector<double> ladderSteps(double span, int decimals, bool integer);

private:
    void openLadder();
    void closeLadder();
    void startDrag();
    void finish(bool commit);
    double modifierScale(Qt::KeyboardModifiers mods) const;
    void wrapAtScreenEdge(const QPoint& at);

    Target mT;
    int mButton = 0;
    QPoint mPress;
    QPoint mLast;
    qint64 mPressMs = 0;
    bool mDragging = false;
    bool mRefused = false;
    double mTotal = 0.0;
    double mPixels = 0.0;
    double mSnap = 0.0;
    // Ladder: the step picked, the total when it was picked, and the sideways pixels since.
    std::vector<double> mSteps;
    int mRow = -1;
    int mStartRow = 0;
    double mRowBase = 0.0;
    double mRowPixels = 0.0;
    QPointer<StepLadder> mLadder;
    std::unique_ptr<QTimer> mHold;
};
} // namespace lila::ui
