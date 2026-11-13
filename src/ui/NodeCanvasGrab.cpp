#include "ui/NodeCanvas.h"

#include "ui/Hotkeys.h"

#include <QKeyEvent>

#include <cmath>
#include <limits>

namespace lila::ui
{
namespace
{
constexpr double kGrabGrid = 25.0;   // the background's dot spacing
constexpr double kSpacingGap = 8.0;  // the closest Spacing packs two nodes that started apart
constexpr double kMaxSpacing = 20.0;

QRectF grabBox(const NodeItem* n, QPointF pos)
{
    if (n->reroute)
        return QRectF(pos - QPointF(NodeItem::kRerouteR, NodeItem::kRerouteR), QSizeF(2 * NodeItem::kRerouteR, 2 * NodeItem::kRerouteR));
    return QRectF(pos, QSizeF(kNodeWidth, n->height()));
}
} // namespace

void NodeCanvas::beginGrab(Grab mode, const std::string& action)
{
    if (mGrab != Grab::None || mode == Grab::None)
        return;
    mGrabAction = action;
    mGrabHeld.start();
    if (mode == Grab::TieTurn)
    {
        if (mTies.empty())
            return;
        const QPointF at = mHoverScene ? *mHoverScene : cursorScenePos();
        setFocus(Qt::OtherFocusReason);
        setProperty("keysCaptured", true);
        setCursor(Qt::SizeAllCursor);
        mGrabDragging = false;
        if (const int t = tieAt(at); t >= 0)
            turnTieAt(t, at);
        else
            mGrab = Grab::TieArmed;
        return;
    }
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.empty() || (mode == Grab::Spacing && nodes.size() < 2))
        return;
    mGrab = mode;
    mGrabAxis = 0;
    mGrabFactor = 1.0;
    mGrabStart.clear();
    QPointF centre;
    for (NodeItem* n : nodes)
    {
        mGrabStart[n] = n->pos();
        centre += grabBox(n, n->pos()).center();
    }
    mGrabCentre = centre / double(nodes.size());
    mGrabAnchor = cursorScenePos();
    mGrabCursor = mapFromScene(mGrabAnchor);
    setFocus(Qt::OtherFocusReason);
    // Keys belong to the grab until it ends: X locks an axis here rather than deleting.
    setProperty("keysCaptured", true);
    setCursor(mode == Grab::Move ? Qt::SizeAllCursor : Qt::SizeFDiagCursor);
    viewport()->update();
}

double NodeCanvas::minSpacing(int axis) const
{
    double lowest = 0.0;
    for (auto a = mGrabStart.begin(); a != mGrabStart.end(); ++a)
        for (auto b = std::next(a); b != mGrabStart.end(); ++b)
        {
            const QRectF ra = grabBox(a->first, a->second), rb = grabBox(b->first, b->second);
            const QPointF d = rb.center() - ra.center();
            const double needX = (ra.width() + rb.width()) * 0.5 + kSpacingGap, needY = (ra.height() + rb.height()) * 0.5 + kSpacingGap;
            // A pair stays apart while either axis keeps its boxes clear; its floor is the axis that gets there first.
            auto floorOn = [](bool scaled, double dist, double need) {
                if (dist >= need && !scaled)
                    return 0.0;
                return scaled && dist > 1e-6 ? need / dist : std::numeric_limits<double>::infinity();
            };
            const double pair = std::min(floorOn(axis != 2, std::abs(d.x()), needX), floorOn(axis != 1, std::abs(d.y()), needY));
            if (std::isfinite(pair))
                lowest = std::max(lowest, pair);
        }
    // Nodes already overlapping at the start never hold the selection open wider than it was.
    return std::min(lowest, 1.0);
}

void NodeCanvas::updateGrab(QPointF sp, Qt::KeyboardModifiers mods)
{
    mGrabCursor = mapFromScene(sp);
    if (mGrab == Grab::TieArmed)
    {
        hoverTie(tieAt(sp));
        return;
    }
    if (mGrab == Grab::TieTurn)
    {
        TieView& tie = mTies[size_t(mGrabTie)];
        const QPointF v = sp - tie.pos;
        // The turn measures from where the cursor first clears the tie's middle, where its direction means something.
        if (std::hypot(v.x(), v.y()) * std::max(0.05, transform().m11()) < 12.0)
            return;
        const double cursor = qRadiansToDegrees(std::atan2(v.y(), v.x()));
        if (!mGrabTurnFrom)
            mGrabTurnFrom = cursor;
        const double a = mGrabTieBefore.angle + cursor - *mGrabTurnFrom;
        // Ctrl turns freely; otherwise it steps by 15 degrees, as a Shift+drag does.
        tie.angle = (mods & Qt::ControlModifier) ? a : std::round(a / 15.0) * 15.0;
        layoutTies();
        return;
    }
    QPointF delta = sp - mGrabAnchor;
    if (mGrabAxis == 1)
        delta.setY(0.0);
    else if (mGrabAxis == 2)
        delta.setX(0.0);
    if (mGrab == Grab::Move)
    {
        if (mods & Qt::ShiftModifier)
            delta = QPointF(std::round(delta.x() / kGrabGrid) * kGrabGrid, std::round(delta.y() / kGrabGrid) * kGrabGrid);
        for (auto& [n, start] : mGrabStart)
            n->setPos(start + delta);
    }
    else
    {
        const double zoom = std::max(0.05, transform().m11());
        const double base = std::max(QLineF(mGrabAnchor, mGrabCentre).length(), 60.0 / zoom);
        double f = QLineF(sp, mGrabCentre).length() / base;
        if (mGrabAxis == 1)
            f = std::abs(sp.x() - mGrabCentre.x()) / std::max(std::abs(mGrabAnchor.x() - mGrabCentre.x()), 60.0 / zoom);
        else if (mGrabAxis == 2)
            f = std::abs(sp.y() - mGrabCentre.y()) / std::max(std::abs(mGrabAnchor.y() - mGrabCentre.y()), 60.0 / zoom);
        if (mods & Qt::ShiftModifier)
            f = std::round(f * 10.0) / 10.0;
        mGrabFactor = std::clamp(f, minSpacing(mGrabAxis), kMaxSpacing);
        const double fx = mGrabAxis == 2 ? 1.0 : mGrabFactor, fy = mGrabAxis == 1 ? 1.0 : mGrabFactor;
        for (auto& [n, start] : mGrabStart)
        {
            const QPointF c = grabBox(n, start).center() - mGrabCentre;
            n->setPos(start + QPointF(c.x() * (fx - 1.0), c.y() * (fy - 1.0)));
        }
    }
    viewport()->update();
}

void NodeCanvas::turnTieAt(int tie, QPointF at)
{
    mGrab = Grab::TieTurn;
    mGrabTie = tie;
    mGrabTieBefore = mTies[size_t(tie)];
    mGrabTurnFrom.reset();
    mGrabAnchor = at;
    mGrabCursor = mapFromScene(at);
}

bool NodeCanvas::grabPress(QMouseEvent* e)
{
    if (mGrab == Grab::None)
        return false;
    const QPointF sp = mapToScene(e->pos());
    if (mGrab == Grab::TieArmed && e->button() == Qt::LeftButton)
        if (const int t = tieAt(sp); t >= 0)
        {
            // Picked by a press: the drag turns it and letting go places it.
            turnTieAt(t, sp);
            mGrabDragging = true;
            return true;
        }
    endGrab(e->button() == Qt::LeftButton);
    mGrabEnded = true;
    return true;
}

bool NodeCanvas::grabRelease(QMouseEvent* e)
{
    if (std::exchange(mGrabEnded, false))
        return true;
    if (mGrab == Grab::TieTurn && mGrabDragging && e->button() == Qt::LeftButton)
    {
        endGrab(true);
        return true;
    }
    return mGrab != Grab::None;
}

void NodeCanvas::endGrab(bool keep)
{
    if (mGrab == Grab::None)
        return;
    const Grab mode = std::exchange(mGrab, Grab::None);
    setProperty("keysCaptured", QVariant());
    unsetCursor();
    mGrabDragging = false;
    viewport()->update();
    if (mode == Grab::TieArmed)
        return;
    if (mode == Grab::TieTurn)
    {
        if (!keep)
        {
            mTies[size_t(mGrabTie)] = mGrabTieBefore;
            layoutTies();
        }
        else if (mTies[size_t(mGrabTie)].angle != mGrabTieBefore.angle)
            commitTies(mTies);
        return;
    }
    if (!keep)
    {
        for (auto& [n, start] : mGrabStart)
            n->setPos(start);
        mGrabStart.clear();
        return;
    }
    mPressPositions.clear();
    for (auto& [n, start] : mGrabStart)
        mPressPositions[n->name] = start;
    mGrabStart.clear();
    UndoChunk chunk(mode == Grab::Move ? "Move Nodes" : "Space Nodes");
    commitMoves();
}

/** A key held this long before its release was a hold, which places the grab; shorter is a tap that leaves it live. */
constexpr qint64 kGrabHoldMs = 250;

bool NodeCanvas::grabKeyRelease(QKeyEvent* e)
{
    if (mGrab == Grab::None || mGrabAction.empty() || e->isAutoRepeat() || !Hotkeys::get().isKeyFor(e, mGrabAction))
        return false;
    if (mGrabHeld.elapsed() >= kGrabHoldMs)
        endGrab(true);
    return true;
}

bool NodeCanvas::grabKey(QKeyEvent* e)
{
    if (mGrab == Grab::None)
        return false;
    if (!mGrabAction.empty() && Hotkeys::get().isKeyFor(e, mGrabAction))
    {
        if (!e->isAutoRepeat())
            endGrab(true);
        return true;
    }
    switch (e->key())
    {
    case Qt::Key_Escape: endGrab(false); break;
    case Qt::Key_Return:
    case Qt::Key_Enter: endGrab(true); break;
    case Qt::Key_X:
    case Qt::Key_Y:
    {
        if (mGrab == Grab::TieTurn || mGrab == Grab::TieArmed)
            break;
        const int axis = e->key() == Qt::Key_X ? 1 : 2;
        mGrabAxis = mGrabAxis == axis ? 0 : axis;
        updateGrab(mapToScene(mGrabCursor), e->modifiers());
        break;
    }
    case Qt::Key_Shift:
    case Qt::Key_Control: updateGrab(mapToScene(mGrabCursor), e->modifiers()); break;
    default: break;
    }
    return true;
}

void NodeCanvas::drawGrab(QPainter* p)
{
    if (mGrab == Grab::None)
        return;
    if (mGrabAxis)
    {
        // The locked axis as a line through where the grab pivots: the anchor for moves, the centre for spacing.
        const QPointF through = mapFromScene(mGrab == Grab::Move ? mGrabAnchor : mGrabCentre);
        const QRectF r = viewport()->rect();
        QColor c = themeColor(mGrabAxis == 1 ? "axisX" : "axisY");
        c.setAlpha(160);
        p->setPen(QPen(c, 1.0, Qt::DashLine));
        if (mGrabAxis == 1)
            p->drawLine(QPointF(r.left(), through.y()), QPointF(r.right(), through.y()));
        else
            p->drawLine(QPointF(through.x(), r.top()), QPointF(through.x(), r.bottom()));
    }
    if (mGrab != Grab::Spacing)
        return;
    const QString text = QString::number(mGrabFactor, 'f', 2) + QChar(0x00D7);
    const QFontMetrics fm(p->font());
    QRectF box(QPointF(mGrabCursor) + QPointF(16, 18), QSizeF(fm.horizontalAdvance(text) + 12, fm.height() + 6));
    p->setPen(Qt::NoPen);
    p->setBrush(gray(20, 220));
    p->drawRoundedRect(box, 4, 4);
    p->setPen(gray(220));
    p->drawText(box, Qt::AlignCenter, text);
}
} // namespace lila::ui
