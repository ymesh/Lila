#include "ui/NodeCanvas.h"

namespace lila::ui
{
QRect NodeCanvas::minimapRect() const
{
    QRect v = viewport()->rect();
    int w = std::min(220, v.width() / 3), h = std::min(150, v.height() / 3);
    return QRect(v.right() - w - 10, v.bottom() - h - 10, w, h);
}

QRectF NodeCanvas::graphBounds() const
{
    QRectF r;
    for (auto& [name, n] : mNodes)
        if (n->isVisible())
            r = r.united(n->sceneBoundingRect());
    for (FrameItem* f : mFrameItems)
        r = r.united(f->sceneBoundingRect());
    return r.isNull() ? QRectF(-200, -150, 400, 300) : r.adjusted(-60, -60, 60, 60);
}

QTransform NodeCanvas::minimapTransform() const
{
    QRectF g = graphBounds().united(mapToScene(viewport()->rect()).boundingRect());
    QRectF m = QRectF(minimapRect()).adjusted(4, 4, -4, -4);
    double s = std::min(m.width() / g.width(), m.height() / g.height());
    QPointF off = m.center() - g.center() * s;
    return QTransform(s, 0, 0, s, off.x(), off.y());
}

void NodeCanvas::drawMinimap(QPainter* p)
{
    QRect box = minimapRect();
    QTransform t = minimapTransform();
    p->setPen(QPen(gray(70), 1.0));
    p->setBrush(gray(26, 225));
    p->drawRoundedRect(box, 4, 4);
    p->setClipRect(box.adjusted(1, 1, -1, -1));
    QFont f = p->font();
    f.setPixelSize(9);
    p->setFont(f);
    for (size_t i = 0; i < mFrameItems.size() && i < mFrames.size(); ++i)
    {
        QRectF r = t.mapRect(mFrameItems[i]->sceneBoundingRect());
        QColor c = mFrameItems[i]->color;
        c.setAlpha(110);
        p->setPen(Qt::NoPen);
        p->setBrush(c);
        p->drawRect(r);
        p->setPen(gray(225));
        p->drawText(r.adjusted(2, 1, -2, 0), Qt::AlignTop | Qt::AlignLeft, mFrameItems[i]->label);
    }
    p->setPen(QPen(gray(120), 1.0));
    for (LinkItem* l : mLinks)
        if (l->isVisible())
            p->drawLine(t.map(l->from->socketPos(false, l->out)), t.map(l->to->socketPos(true, l->in)));
    p->setPen(Qt::NoPen);
    for (auto& [name, n] : mNodes)
        if (n->isVisible())
        {
            p->setBrush(n->isSelected() ? themeColor("nodeSelected") : gray(150));
            p->drawRect(t.mapRect(n->sceneBoundingRect()));
        }
    p->setBrush(Qt::NoBrush);
    p->setPen(QPen(gray(235), 1.0));
    p->drawRect(t.mapRect(mapToScene(viewport()->rect()).boundingRect()));
}

int NodeCanvas::frameAt(QPointF sp, bool titleOnly) const
{
    for (int i = int(mFrameItems.size()) - 1; i >= 0; --i)
    {
        FrameItem* f = mFrameItems[size_t(i)];
        QPointF local = f->mapFromScene(sp);
        if ((titleOnly ? f->titleRect() : f->box().adjusted(0, 0, 2, 2)).contains(local))
            return i;
    }
    return -1;
}

void NodeCanvas::layoutFrames()
{
    for (auto& [name, n] : mNodes)
    {
        n->inProxy.reset();
        n->outProxy.reset();
        n->setVisible(true);
    }
    std::map<NodeItem*, size_t> hiddenIn;
    for (size_t i = 0; i < mFrames.size() && i < mFrameItems.size(); ++i)
    {
        const gn::GFrame& f = mFrames[i];
        FrameItem* item = mFrameItems[i];
        item->label = f.label.empty() ? QString("Frame") : qs(f.label);
        item->text = qs(f.text);
        item->color = paletteColor(f.color, 95);
        item->setBox(QSizeF(f.size.x, f.size.y), f.collapsed);
        item->setPos(f.position.x, f.position.y);
        item->update();
        if (!f.collapsed)
            continue;
        QPointF left(f.position.x, f.position.y + FrameItem::kTitle * 0.5), right(f.position.x + f.size.x, left.y());
        for (const std::string& m : f.nodes)
            if (auto it = mNodes.find(m); it != mNodes.end())
            {
                it->second->inProxy = left;
                it->second->outProxy = right;
                it->second->setVisible(false);
                hiddenIn[it->second] = i;
            }
    }
    for (LinkItem* l : mLinks)
    {
        // Wires inside one collapsed frame have nowhere to show.
        auto a = hiddenIn.find(l->from), b = hiddenIn.find(l->to);
        l->folded = a != hiddenIn.end() && b != hiddenIn.end() && a->second == b->second;
        l->applyVisibility();
        l->update();
    }
}

void NodeCanvas::dragFrame(QPointF sp)
{
    gn::GFrame& f = mFrames[size_t(mFrameDrag)];
    QPointF d = sp - mFrameGrab;
    if (mFrameResize)
        f.size = {std::max(80.0, mFrameBefore.size.x + d.x()), std::max(FrameItem::kTitle + 20.0, mFrameBefore.size.y + d.y())};
    else
    {
        f.position = mFrameBefore.position + glm::dvec2(d.x(), d.y());
        for (auto& [m, start] : mFrameNodeStart)
            if (auto it = mNodes.find(m); it != mNodes.end())
                it->second->setPos(start + d);
    }
    layoutFrames();
    layoutTies();
}

void NodeCanvas::renameFrame(int f)
{
    FrameItem* item = mFrameItems[size_t(f)];
    editTitleInPlace(item->mapRectToScene(item->titleRect().adjusted(FrameItem::kTitle, 0, 0, 0)), item->label, [this, f](const QString& typed) {
        if (size_t(f) >= mFrames.size() || typed.simplified().toStdString() == mFrames[size_t(f)].label)
            return;
        std::vector<gn::GFrame> next = mFrames;
        next[size_t(f)].label = typed.simplified().toStdString();
        commitFrames(next);
    });
}

QSizeF NodeCanvas::noteSize(const QString& text, double width) const
{
    const QRect lines = QFontMetrics(font()).boundingRect(QRect(0, 0, int(width - 2.0 * FrameItem::kTextPad), 100000), Qt::TextWordWrap,
                                                         text.isEmpty() ? QString("M") : text);
    return {width, FrameItem::kTitle + FrameItem::kTextPad * 1.5 + double(lines.height()) + 4.0};
}

void NodeCanvas::editFrameText(int f)
{
    FrameItem* item = mFrameItems[size_t(f)];
    if (item->collapsed)
        return;
    editTextInPlace(item->mapRectToScene(item->textRect()), item->text, [this, f](const QString& typed) {
        if (size_t(f) >= mFrames.size() || typed.toStdString() == mFrames[size_t(f)].text)
            return;
        std::vector<gn::GFrame> next = mFrames;
        gn::GFrame& frame = next[size_t(f)];
        frame.text = typed.toStdString();
        // The box grows to show all of its text; it never shrinks under a size the user dragged.
        frame.size.y = std::max(frame.size.y, noteSize(typed, frame.size.x).height());
        commitFrames(next);
    });
}

void NodeCanvas::addNote(QPointF at)
{
    if (!backend->supports("SetFrames") || mGroup.empty())
        return;
    static constexpr double kWidth = 240.0;
    const QRectF body(at.x() + FrameItem::kTextPad, at.y() + FrameItem::kTitle + FrameItem::kTextPad * 0.5, kWidth - 2.0 * FrameItem::kTextPad, 80.0);
    editTextInPlace(body, {}, [this, at](const QString& typed) {
        if (typed.trimmed().isEmpty())
            return;
        gn::GFrame note;
        note.label = "Note";
        note.text = typed.toStdString();
        note.position = {at.x(), at.y()};
        const QSizeF size = noteSize(typed, kWidth);
        note.size = {size.width(), std::max(size.height(), 90.0)};
        note.color = "light";
        std::vector<gn::GFrame> next = mFrames;
        next.push_back(std::move(note));
        commitFrames(next);
    });
}

void NodeCanvas::frameMenu(int f, QPoint global)
{
    QMenu menu(this);
    menu.addAction("Rename", this, [this, f] { renameFrame(f); });
    if (!mFrames[size_t(f)].collapsed)
        menu.addAction("Edit Text", this, [this, f] { editFrameText(f); });
    QMenu* colors = menu.addMenu("Colour");
    fillPaletteMenu(colors, [this, f](std::string id) {
        std::vector<gn::GFrame> next = mFrames;
        next[size_t(f)].color = std::move(id);
        commitFrames(next);
    });
    menu.addAction(mFrames[size_t(f)].collapsed ? "Expand" : "Collapse", this, [this, f] {
        std::vector<gn::GFrame> next = mFrames;
        next[size_t(f)].collapsed = !next[size_t(f)].collapsed;
        commitFrames(next);
    });
    menu.addAction("Select Contents", this, [this, f] {
        mScene.clearSelection();
        for (const std::string& m : mFrames[size_t(f)].nodes)
            if (auto it = mNodes.find(m); it != mNodes.end())
                it->second->setSelected(true);
    });
    menu.addSeparator();
    menu.addAction("Delete Frame", this, [this, f] {
        std::vector<gn::GFrame> next = mFrames;
        next.erase(next.begin() + f);
        commitFrames(next);
    });
    menu.exec(global);
}

void NodeCanvas::commitFrames(const std::vector<gn::GFrame>& frames)
{
    runVerb(*backend, "SetFrames", {{"args", {mGroup, gn::framesToJson(frames)}}});
}

bool NodeCanvas::updateMembership(const json& moved, std::vector<gn::GFrame>& frames) const
{
    bool changed = false;
    for (auto& [name, pos] : moved.items())
    {
        auto it = mNodes.find(name);
        if (it == mNodes.end())
            continue;
        QPointF c = it->second->sceneBoundingRect().center();
        int into = -1;
        for (int i = int(frames.size()) - 1; i >= 0 && into < 0; --i)
        {
            const gn::GFrame& f = frames[size_t(i)];
            if (!f.collapsed && QRectF(f.position.x, f.position.y, f.size.x, f.size.y).contains(c))
                into = i;
        }
        for (int i = 0; i < int(frames.size()); ++i)
        {
            std::vector<std::string>& m = frames[size_t(i)].nodes;
            bool in = std::find(m.begin(), m.end(), name) != m.end();
            if (in && i != into)
            {
                std::erase(m, name);
                changed = true;
            }
            else if (!in && i == into)
            {
                m.push_back(name);
                changed = true;
            }
        }
    }
    return changed;
}

void NodeCanvas::commitMoves()
{
    json positions = json::object();
    for (auto& [name, item] : mNodes)
    {
        auto it = mPressPositions.find(name);
        if (it != mPressPositions.end() && QLineF(it->second, item->pos()).length() > 0.5)
            positions[name] = {item->pos().x(), item->pos().y()};
    }
    mPressPositions.clear();
    if (positions.empty())
        return;
    UndoChunk chunk("Move Nodes");
    runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
    std::vector<gn::GFrame> next = mFrames;
    if (backend->supports("SetFrames") && updateMembership(positions, next))
        commitFrames(next);
}
} // namespace lila::ui
