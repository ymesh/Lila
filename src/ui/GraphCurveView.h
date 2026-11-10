#pragma once

#include "ui/GraphEditorCommon.h"
#include "ui/GraphModel.h"
#include "ui/GraphMotion.h"

#include <QWidget>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace lila::ui
{
class GraphEditor;

namespace graphed
{
enum class ViewMode : uint8_t
{
    Absolute,
    Stacked,
    Normalized,
};

enum class Tool : uint8_t
{
    Move,
    Rotate, // turns each selected key's tangents about the key
    Scale,
};

enum class Pivot : uint8_t
{
    Selection,
    Click,
    CurrentFrame,
    FirstKey,
    LastKey,
};

enum class Quantity : uint8_t
{
    Channels, // each shown curve's rate of change
    WorldLinear, // cm/s of the selected transforms
    WorldAngular, // degrees/s of the selected transforms
};

/** A curve as it was when snapshotted, drawn behind the live one and swapped back in on request. */
struct Buffer
{
    bool rotation = false;
    anim::Curve curve;
    anim::RotationCurve rotationCurve;
};

/** What the view shows and how its tools behave; the editor's toolbar edits it and it persists in prefs. */
struct ViewSettings
{
    ViewMode view = ViewMode::Absolute;
    Tool tool = Tool::Move;
    Pivot pivot = Pivot::Selection;
    bool falloff = false;
    double falloffFrames = 6.0;
    bool speedStrip = false;
    bool accelStrip = false;
    Quantity quantity = Quantity::Channels;
    double targetSpeed = 0.0; // 0 = none
    RotationDisplay rotation = RotationDisplay::Euler;
    bool showBuffer = true;
};

/** The Graph Editor's plot: draws a GraphModel and edits it with the mouse. */
class CurveView : public QWidget, public GraphModel
{
public:
    explicit CurveView(GraphEditor* editor);
    ~CurveView() override;

    ViewSettings settings;
    /** By the curve node's own id. */
    std::map<NodeId, Buffer> buffers;

    void frame(bool selectedOnly);
    /** Recomputes each channel's display map for the view mode. */
    void updateMaps();
    void loopKeys(bool on);
    void invalidateMotion() { mWorld.invalidate(); }

    /** The tween slider: a live preview while dragged, one blendKeys on release. */
    void beginTween(anim::BlendMode mode);
    void setTween(double amount);
    void endTween();

    /**
     * W, E or R pressed: the tool turns on, and while the key stays down, moving the mouse over the view drags the
     * selected keys with it. A tap (released quickly, or before the mouse travels) only switches the tool.
     */
    void toolKeyDown(Tool tool, bool holdToDrag);
    void toolKeyUp();
    /** B held: LMB drag sets the falloff radius, MMB drag sets it from zero. Returns whether the radius was dragged. */
    void falloffKeyDown();
    bool falloffKeyUp();

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    enum class Drag : uint8_t
    {
        None,
        Pan,
        Zoom,
        Box,
        Move,
        Scale,
        Region,
        Tangent,
        TangentRotate,
        Time,
    };

    double stripHeight() const;
    double plotBottom() const { return height() - stripHeight(); }
    double xOf(double t) const { return (t - mT0) * mSx; }
    double yOf(double display) const { return plotBottom() - (display - mV0) * mSy; }
    double timeAt(double x) const { return mT0 + x / mSx; }
    double displayAt(double y) const { return mV0 + (plotBottom() - y) / mSy; }
    double yOfKey(const Channel& ch, double v) const { return yOf(ch.display(v)); }

    QPointF handlePoint(const anim::Curve& c, size_t i, bool out, const Channel& ch) const;
    void drawCurve(QPainter& p, const anim::Curve& c, const Channel& ch, bool active, const std::function<double(double)>& eval,
                   const QColor* ghost = nullptr);
    void drawGrid(QPainter& p);
    void drawStrips(QPainter& p, const std::map<NodeId, anim::Curve>& shown);
    /** The selected keys' bounds on screen; empty when fewer than two keys are selected. */
    QRectF regionRect() const;
    /** 0..7 (corners then edges, clockwise from top left) when pos is on a region handle, else -1. */
    int hitRegionHandle(QPointF pos) const;

    std::optional<KeySel> hitKey(QPointF pos) const;
    bool hitHandle(QPointF pos);
    NodeId hitCurve(QPointF pos) const;
    double angleFor(QPointF pos, double* weight) const;
    void scrubTo(double x);

    void beginTransform(Drag kind);
    /** Makes key the one the readout follows and typed numbers set. */
    void setAnchor(const KeySel& key);
    /** Pivots for a scale: the time pivot and each channel's value pivot, from the pivot setting. */
    void setPivots(QPointF press);
    void finishBox();
    /** Ends the drag in progress: commits it, or with cancel puts back what it changed. */
    void endDrag(bool cancel);
    void showMenu(QPoint at);
    /** The drag's readout at the cursor: the dragged key's frame and value, or the tangent's angle, and any axis lock. */
    QString readout() const;
    /** A number typed during a key drag: the dragged key's frame (when the value is locked) or value, else its tangent angle. */
    void applyTyped();

    GraphEditor* mEditor;
    double mT0 = -0.2; // time at the left edge
    double mV0 = -5.0; // display value at the bottom of the plot
    double mSx = 200.0; // pixels per second
    double mSy = 10.0; // pixels per display unit
    bool mFramed = false;
    bool mAxisColors = true;
    int mPrefListener = 0;

    Drag mDrag = Drag::None;
    QPointF mPress;
    QPointF mLast;
    bool mMoved = false;
    SelectOp mBoxOp = SelectOp::Replace;
    NodeId mPressCurve = scene::kNoNode;
    double mZoomAnchorT = 0.0, mZoomAnchorV = 0.0, mZoomSx = 1.0, mZoomSy = 1.0;

    int mRegionHandle = -1;
    QRectF mRegionStart;
    /** The key a move drag started on (or the first selected key), whose frame and value the readout shows. */
    std::optional<KeySel> mAnchor;
    double mAnchorValue = 0.0;
    /** Axis lock of the drag in progress: 1 time locked (value moves), 2 value locked, 0 free. */
    int mLock = 0;
    QString mTyped;
    /** A right-button press in the plot: a click opens the context menu, a drag the tangent marking menu. */
    bool mRightPending = false;

    WorldMotion mWorld;

    void endHold();

    struct Hold
    {
        bool active = false;
        bool dragging = false;
        Drag kind = Drag::Move;
        QPointF anchor;
        qint64 since = 0; // ms
    } mHold;
    bool mRadiusKey = false;
    bool mRadiusUsed = false;
    bool mRadiusDrag = false;
    double mRadiusStart = 0.0;
    QPointF mRadiusPress;
    QPointF mCursor;
    bool mHoldPose = false; // the ruler's MMB scrub

    friend class lila::ui::GraphEditor;
};
} // namespace graphed
} // namespace lila::ui
