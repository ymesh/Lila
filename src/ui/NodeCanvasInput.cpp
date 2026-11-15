#include "ui/NodeCanvas.h"

namespace lila::ui
{
void NodeCanvas::drawForeground(QPainter* p, const QRectF&)
{
    drawGhostLines(p);
    if (mPrefs.minimap || mGrab != Grab::None)
    {
        p->save();
        p->resetTransform();
        if (mPrefs.minimap)
            drawMinimap(p);
        drawGrab(p);
        p->restore();
    }
}

void NodeCanvas::drawGhostLines(QPainter* p)
{
    if (mHoverPortal.empty())
        return;
    // Ghost lines from the hovered portal's Send to every Receive of its name.
    std::vector<NodeItem*> sends, receives;
    for (auto& [name, item] : mNodes)
        if (item->portal && item->portalName == mHoverPortal)
            (item->type == gn::kSendType ? sends : receives).push_back(item);
    QPen pen(themeColor("nodeSelected"), 1.5 / std::max(0.05, transform().m11()), Qt::DashLine);
    p->setPen(pen);
    p->setBrush(Qt::NoBrush);
    for (NodeItem* s : sends)
        for (NodeItem* r : receives)
        {
            QPointF a = s->mapToScene(QPointF(kNodeWidth, kHeader * 0.5)), b = r->mapToScene(QPointF(0.0, kHeader * 0.5));
            double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
            QPainterPath path(a);
            path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
            p->drawPath(path);
        }
}

void NodeCanvas::drawBackground(QPainter* p, const QRectF& rect)
{
    p->fillRect(rect, gray(34));
    double step = 25.0;
    p->setPen(QPen(gray(48), 0));
    for (double x = std::floor(rect.left() / step) * step; x < rect.right(); x += step)
        for (double y = std::floor(rect.top() / step) * step; y < rect.bottom(); y += step)
            p->drawPoint(QPointF(x, y));
    drawZones(p);
}

void NodeCanvas::drawZones(QPainter* p)
{
    struct Region
    {
        QRectF rect;
        QColor color;
        QString label;
    };
    std::vector<Region> regions;
    if (!mInfo.is_object())
        return;
    for (const json& z : mInfo.value("zones", json::array()))
    {
        QRectF r;
        auto add = [&](const std::string& name) {
            if (auto it = mNodes.find(name); it != mNodes.end())
                r = r.isNull() ? it->second->sceneBoundingRect() : r.united(it->second->sceneBoundingRect());
        };
        add(z.value("input", ""));
        add(z.value("output", ""));
        for (const json& n : z.value("nodes", json::array()))
            add(n.get<std::string>());
        if (r.isNull())
            continue;
        std::string kind = z.value("kind", "");
        const char* token = kind == "Repeat" ? "zoneRepeat" : kind == "Simulation" ? "zoneSimulation" : kind == "Closure" ? "zoneClosure" : "zoneForEach";
        regions.push_back({r.adjusted(-16, -30, 16, 16), themeColor(token), qs(kind)});
    }
    std::sort(regions.begin(), regions.end(), [](const Region& a, const Region& b) { return a.rect.width() * a.rect.height() > b.rect.width() * b.rect.height(); });
    for (const Region& r : regions)
    {
        QColor fill = r.color, edge = r.color;
        fill.setAlpha(70);
        edge.setAlpha(200);
        p->setPen(QPen(edge, 1.5));
        p->setBrush(fill);
        p->drawRoundedRect(r.rect, 10, 10);
        p->setPen(edge.lighter(170));
        p->drawText(r.rect.adjusted(12, 6, -12, 0), Qt::AlignTop | Qt::AlignLeft, r.label);
    }
}

void NodeCanvas::wheelEvent(QWheelEvent* e)
{
    double f = std::pow(1.0015, e->angleDelta().y());
    double s = transform().m11() * f;
    if (s < 0.1 || s > 4.0)
        return;
    scale(f, f);
}

void NodeCanvas::keyPressEvent(QKeyEvent* e)
{
    if (!grabKey(e))
        QGraphicsView::keyPressEvent(e);
}

void NodeCanvas::keyReleaseEvent(QKeyEvent* e)
{
    if (grabKeyRelease(e))
        return;
    if (mGrab != Grab::None && (e->key() == Qt::Key_Shift || e->key() == Qt::Key_Control))
        updateGrab(mapToScene(mGrabCursor), e->modifiers());
    QGraphicsView::keyReleaseEvent(e);
}

void NodeCanvas::focusOutEvent(QFocusEvent* e)
{
    endGrab(false);
    QGraphicsView::focusOutEvent(e);
}

void NodeCanvas::focusInEvent(QFocusEvent* e)
{
    QGraphicsView::focusInEvent(e);
    if (onFocused)
        onFocused();
}

void NodeCanvas::mousePressEvent(QMouseEvent* e)
{
    setFocus(Qt::MouseFocusReason);
    QPointF sp = mapToScene(e->pos());
    if (grabPress(e))
        return;
    if (e->button() == Qt::MiddleButton)
    {
        mPanning = true;
        mLast = e->pos();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    if (e->button() == Qt::LeftButton && mPrefs.minimap && minimapRect().contains(e->pos()))
    {
        mMinimapDrag = true;
        centerOn(fromMinimap(e->pos()));
        return;
    }
    if (e->button() == Qt::LeftButton && mZipKey && backend->supports("SetTies"))
    {
        mTieStroke = true;
        mTieStart = sp;
        mTieTarget = tieAt(sp);
        mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
        return;
    }
    if (e->button() == Qt::RightButton && (e->modifiers() & Qt::AltModifier))
        if (NodeItem* n = nodeAt(sp))
        {
            mLazyFrom = n;
            mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
            return;
        }
    if (e->button() == Qt::RightButton && !nodeAt(sp))
    {
        if (int t = tieAt(sp); t >= 0)
        {
            tieMenu(t, e->globalPosition().toPoint());
            return;
        }
        if (int f = frameAt(sp, true); f >= 0)
        {
            frameMenu(f, e->globalPosition().toPoint());
            return;
        }
    }
    if (e->button() == Qt::RightButton && onContextMenu)
    {
        if (NodeItem* n = nodeAt(sp); n && !n->isSelected())
        {
            mScene.clearSelection();
            n->setSelected(true);
        }
        onContextMenu(e->globalPosition().toPoint());
        return;
    }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::ShiftModifier))
        if (NodeItem* n = nodeAt(sp); n && onViewNode)
        {
            onViewNode(n->name);
            return;
        }
    if (e->button() == Qt::LeftButton && !(e->modifiers() & Qt::ControlModifier))
        if (NodeItem* n = nodeAt(sp); n && n->isViewer() && n->eyeRect().contains(n->mapFromScene(sp)))
        {
            if (n->viewerActive)
                runVerb(*backend, "SetActiveViewer", args({mGroup}));
            else
                runVerb(*backend, "SetActiveViewer", args({mGroup, n->name}));
            return;
        }
    if (e->button() == Qt::LeftButton && e->modifiers() == Qt::AltModifier)
        if (NodeItem* n = nodeAt(sp))
        {
            bool input = false;
            if (int s = n->socketAt(sp, input); s >= 0)
            {
                disconnectSocket(n, s, input);
                return;
            }
        }
    // Ctrl on a linked input picks its link up rather than starting a cut.
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
        if (NodeItem* n = nodeAt(sp))
        {
            bool input = false;
            int s = n->socketAt(sp, input);
            if (s >= 0 && input && n->inputs[size_t(s)].linked)
            {
                startWire(n, s, true);
                return;
            }
        }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier))
    {
        mCutting = true;
        mCut = {sp};
        mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
        return;
    }
    if (e->button() == Qt::LeftButton)
        if (NodeItem* n = nodeAt(sp))
        {
            bool input = false;
            int s = n->socketAt(sp, input);
            if (s >= 0)
            {
                startWire(n, s, input, e->modifiers() & Qt::ShiftModifier);
                return;
            }
        }
    if (e->button() == Qt::LeftButton && !nodeAt(sp) && tieAt(sp) < 0)
        if (int f = frameAt(sp, false); f >= 0)
        {
            FrameItem* item = mFrameItems[size_t(f)];
            QPointF local = item->mapFromScene(sp);
            if (item->arrowRect().contains(local))
            {
                std::vector<gn::GFrame> next = mFrames;
                next[size_t(f)].collapsed = !next[size_t(f)].collapsed;
                commitFrames(next);
                return;
            }
            bool grip = item->gripRect().contains(local);
            if (grip || item->titleRect().contains(local))
            {
                mFrameDrag = f;
                mFrameResize = grip;
                mFrameGrab = sp;
                mFrameBefore = mFrames[size_t(f)];
                mFrameNodeStart.clear();
                for (const std::string& m : mFrames[size_t(f)].nodes)
                    if (auto it = mNodes.find(m); it != mNodes.end())
                        mFrameNodeStart[m] = it->second->pos();
                return;
            }
        }
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier) && !nodeAt(sp) && tieAt(sp) < 0 &&
        backend->supports("AddNode"))
    {
        mRerouteStroke = true;
        mTieStart = sp;
        mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 1.5, Qt::DashLine));
        return;
    }
    if (e->button() == Qt::LeftButton && !nodeAt(sp))
        if (int t = tieAt(sp); t >= 0)
        {
            mTieDrag = t;
            mTieRotate = e->modifiers() & Qt::ShiftModifier;
            mTieGrab = sp - mTies[t].pos;
            mTieBefore = mTies[t];
            return;
        }
    mPressPositions.clear();
    QGraphicsView::mousePressEvent(e);
    for (auto& [name, item] : mNodes)
        mPressPositions[name] = item->pos();
}

void NodeCanvas::mouseMoveEvent(QMouseEvent* e)
{
    QPointF sp = mapToScene(e->pos());
    mHoverScene = sp;
    if (mGrab != Grab::None)
    {
        updateGrab(sp, e->modifiers());
        return;
    }
    if (mPanning)
    {
        QPoint d = e->pos() - mLast;
        mLast = e->pos();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - d.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - d.y());
        return;
    }
    if (mMinimapDrag)
    {
        centerOn(fromMinimap(e->pos()));
        return;
    }
    if (mTieStroke || mRerouteStroke)
    {
        QPainterPath path(mTieStart);
        path.lineTo(sp);
        mTemp->setPath(path);
        return;
    }
    if (mTieDrag >= 0)
    {
        dragTie(sp, e->modifiers());
        return;
    }
    if (mFrameDrag >= 0)
    {
        dragFrame(sp);
        return;
    }
    if (mLazyFrom)
    {
        QPointF a = mLazyFrom->sceneBoundingRect().center();
        QPainterPath path(a);
        path.lineTo(sp);
        mTemp->setPath(path);
        return;
    }
    if (e->buttons() == Qt::NoButton)
    {
        hoverTie(tieAt(sp));
        NodeItem* over = nodeAt(sp);
        if (over != mHoverNode)
        {
            mHoverNode = over;
            if (mPrefs.dimUnrelated)
                updateEmphasis();
        }
        std::string portal = over && over->portal ? over->portalName : std::string();
        if (portal != mHoverPortal)
        {
            mHoverPortal = portal;
            viewport()->update();
        }
    }
    if (mCutting)
    {
        mCut.push_back(sp);
        QPainterPath path(mCut.front());
        for (const QPointF& p : mCut)
            path.lineTo(p);
        mTemp->setPath(path);
        return;
    }
    if (mWireFrom)
    {
        QPointF a = mWireFrom->socketPos(mWireInput, mWireSocket);
        QPointF from = mWireInput ? sp : a, to = mWireInput ? a : sp;
        double dx = std::max(40.0, std::abs(to.x() - from.x()) * 0.5);
        QPainterPath path(from);
        path.cubicTo(from + QPointF(dx, 0), to - QPointF(dx, 0), to);
        mTemp->setPath(path);
        return;
    }
    QGraphicsView::mouseMoveEvent(e);
    if (e->buttons() & Qt::LeftButton)
        findInsertLink();
}

void NodeCanvas::mouseReleaseEvent(QMouseEvent* e)
{
    QPointF sp = mapToScene(e->pos());
    if (grabRelease(e))
        return;
    if (mPanning)
    {
        mPanning = false;
        unsetCursor();
        return;
    }
    if (mCutting)
    {
        mCutting = false;
        finishCut();
        return;
    }
    if (mTieStroke)
    {
        finishTieStroke(sp);
        return;
    }
    if (mRerouteStroke)
    {
        mRerouteStroke = false;
        removeTemp();
        std::vector<std::pair<LinkItem*, QPointF>> at;
        QLineF stroke(mTieStart, sp);
        for (LinkItem* l : mLinks)
            if (auto p = crossing(l->path(), stroke))
                at.push_back({l, *p});
        insertReroutes(at);
        return;
    }
    if (std::exchange(mMinimapDrag, false))
        return;
    if (NodeItem* from = std::exchange(mLazyFrom, nullptr))
    {
        removeTemp();
        if (NodeItem* to = nodeAt(sp); to && to != from)
            lazyConnect(from, to);
        return;
    }
    if (mFrameDrag >= 0)
    {
        int f = std::exchange(mFrameDrag, -1);
        if (mFrames[size_t(f)] == mFrameBefore)
            return;
        UndoChunk chunk(mFrameResize ? "Resize Frame" : "Move Frame");
        json positions = json::object();
        for (auto& [m, start] : mFrameNodeStart)
            if (auto it = mNodes.find(m); it != mNodes.end() && it->second->pos() != start)
                positions[m] = {it->second->pos().x(), it->second->pos().y()};
        if (!positions.empty())
            runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
        commitFrames(mFrames);
        return;
    }
    if (mTieDrag >= 0)
    {
        int t = std::exchange(mTieDrag, -1);
        if (mTies[t].pos != mTieBefore.pos || mTies[t].angle != mTieBefore.angle)
            commitTies(mTies);
        return;
    }
    if (mWireFrom)
    {
        finishWire(sp);
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
    if (LinkItem* l = std::exchange(mInsertLink, nullptr))
    {
        l->setHighlight(false);
        UndoChunk chunk("Insert Node");
        commitMoves();
        insertOnLink(l);
        return;
    }
    commitMoves();
}

void NodeCanvas::mouseDoubleClickEvent(QMouseEvent* e)
{
    QPointF sp = mapToScene(e->pos());
    NodeItem* n = nodeAt(sp);
    if (!n)
        if (int f = frameAt(sp, true); f >= 0)
        {
            renameFrame(f);
            return;
        }
    if (!n)
        if (LinkItem* l = linkAt(sp))
        {
            if (NodeItem* otherNode = otherEnd(l, sp))
            {
                mScene.clearSelection();
                otherNode->setSelected(true);
                centerOn(otherNode->sceneBoundingRect().center());
            }
            else if (backend->supports("AddNode"))
                insertReroutes({{l, nearestOn(l->path(), sp)}});
            return;
        }
    if (!n && onAddMenu)
        onAddMenu(mapToScene(e->pos()));
    else if (n && !n->groupRef.empty() && onOpenGroup)
        onOpenGroup(n->groupRef);
    else if (n && n->type == gn::kReceiveType)
        jumpToPortal(true);
    else
        QGraphicsView::mouseDoubleClickEvent(e);
}

bool NodeCanvas::viewportEvent(QEvent* e)
{
    if (e->type() == QEvent::ToolTip)
    {
        auto* he = static_cast<QHelpEvent*>(e);
        QPointF sp = mapToScene(he->pos());
        if (NodeItem* n = nodeAt(sp))
        {
            if (QString tip = n->socketTip(sp); !tip.isEmpty())
            {
                QToolTip::showText(he->globalPos(), tip, viewport());
                return true;
            }
            if (n->portal)
            {
                QToolTip::showText(he->globalPos(), portalTip(n), viewport());
                return true;
            }
        }
        else if (LinkItem* l = linkAt(sp))
        {
            QToolTip::showText(he->globalPos(), wireTip(l), viewport());
            return true;
        }
    }
    return QGraphicsView::viewportEvent(e);
}

QStringList NodeCanvas::nodeFiles(const QMimeData* m) const
{
    QStringList out;
    if (!m || !backend->supports("ImportGroup"))
        return out;
    std::vector<std::string> suffixes = backend->fileSuffixes();
    for (const QUrl& u : m->urls())
        if (u.isLocalFile())
            for (const std::string& suffix : suffixes)
                if (u.toLocalFile().endsWith("." + qs(suffix), Qt::CaseInsensitive))
                {
                    out << u.toLocalFile();
                    break;
                }
    return out;
}

void NodeCanvas::dragEnterEvent(QDragEnterEvent* e)
{
    if (!nodeFiles(e->mimeData()).isEmpty())
        e->acceptProposedAction();
    else
        QGraphicsView::dragEnterEvent(e);
}

void NodeCanvas::dragMoveEvent(QDragMoveEvent* e)
{
    if (!nodeFiles(e->mimeData()).isEmpty())
        e->acceptProposedAction();
    else
        QGraphicsView::dragMoveEvent(e);
}

void NodeCanvas::dropEvent(QDropEvent* e)
{
    QStringList files = nodeFiles(e->mimeData());
    if (files.isEmpty() || !onFilesDropped)
    {
        QGraphicsView::dropEvent(e);
        return;
    }
    e->acceptProposedAction();
    onFilesDropped(files);
}
} // namespace lila::ui
