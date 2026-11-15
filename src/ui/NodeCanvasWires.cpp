#include "ui/NodeCanvas.h"

namespace lila::ui
{
WireDrag NodeCanvas::wireOf(NodeItem* n, int socket, bool input)
{
    const SocketInfo& s = input ? n->inputs[socket] : n->outputs[socket];
    return WireDrag{n->name, s.id, s.type, input, std::nullopt};
}

void NodeCanvas::removeTemp()
{
    if (!mTemp)
        return;
    mScene.removeItem(mTemp);
    delete mTemp;
    mTemp = nullptr;
}

int NodeCanvas::outputIndex(NodeItem* n, const std::string& id)
{
    for (int i = 0; i < int(n->outputs.size()); ++i)
        if (n->outputs[i].id == id)
            return i;
    return -1;
}

int NodeCanvas::inputIndex(NodeItem* n, const std::string& id)
{
    for (int i = 0; i < int(n->inputs.size()); ++i)
        if (n->inputs[i].id == id)
            return i;
    return -1;
}

NodeItem* NodeCanvas::nodeAt(QPointF sp) const
{
    // Sockets stick out of the node rect, so test with a margin.
    for (auto& [name, item] : mNodes)
        if (item->sceneBoundingRect().adjusted(-kSocketR - 4, 0, kSocketR + 4, 0).contains(sp))
            return item;
    return nullptr;
}

void NodeCanvas::startWire(NodeItem* n, int socket, bool input, bool copy)
{
    mDetached.reset();
    if (copy && input && n->inputs[socket].linked)
        for (LinkItem* l : mLinks)
            if (l->to == n && l->in == socket)
            {
                n = l->from;
                socket = l->out;
                input = false;
                break;
            }
    if (input && n->inputs[socket].linked)
    {
        // Grabbing a linked input picks the link up from its source, like Blender.
        for (LinkItem* l : mLinks)
            if (l->to == n && l->in == socket)
            {
                mDetached = std::make_pair(l->from->name + "." + l->from->outputs[l->out].id, n->name + "." + n->inputs[socket].id);
                n = l->from;
                socket = l->out;
                input = false;
                l->hide();
            }
    }
    mWireFrom = n;
    mWireSocket = socket;
    mWireInput = input;
    mTemp = mScene.addPath(QPainterPath(), QPen(gray(220), 2.0));
}

void NodeCanvas::finishWire(QPointF sp)
{
    NodeItem* from = mWireFrom;
    int fs = mWireSocket;
    bool fromInput = mWireInput;
    mWireFrom = nullptr;
    removeTemp();
    NodeItem* target = nodeAt(sp);
    bool tin = false;
    int ts = target ? target->socketAt(sp, tin) : -1;
    if (target && target->reroute && ts < 0)
    {
        tin = !fromInput;
        ts = 0;
    }
    if (!target && !mDetached && onWireDropped)
    {
        onWireDropped(sp, wireOf(from, fs, fromInput));
        return;
    }
    UndoChunk chunk("Link");
    if (mDetached)
        runVerb(*backend, "Disconnect", {{"args", {mGroup, mDetached->second}}, {"source", mDetached->first}});
    mDetached.reset();
    if (!target || ts < 0 || target == from || tin == fromInput)
    {
        if (!target)
            QTimer::singleShot(0, this, [this] { viewport()->update(); });
        return;
    }
    std::string out = fromInput ? target->name + "." + target->outputs[ts].id : from->name + "." + from->outputs[fs].id;
    std::string in = fromInput ? from->name + "." + from->inputs[fs].id : target->name + "." + target->inputs[ts].id;
    runVerb(*backend, "Connect", args({mGroup, out, in}));
}

bool NodeCanvas::disconnectSocket(NodeItem* n, int socket, bool input)
{
    std::vector<std::pair<std::string, std::string>> hits;
    for (LinkItem* l : mLinks)
        if (input ? (l->to == n && l->in == socket) : (l->from == n && l->out == socket))
            hits.push_back({l->from->name + "." + l->from->outputs[l->out].id, l->to->name + "." + l->to->inputs[l->in].id});
    if (hits.empty())
        return false;
    UndoChunk chunk("Disconnect");
    for (auto& [src, dst] : hits)
        runVerb(*backend, "Disconnect", {{"args", {mGroup, dst}}, {"source", src}});
    return true;
}

void NodeCanvas::finishCut()
{
    if (mTemp)
    {
        mScene.removeItem(mTemp);
        delete mTemp;
        mTemp = nullptr;
    }
    QPainterPath cut(mCut.front());
    for (const QPointF& p : mCut)
        cut.lineTo(p);
    std::vector<std::pair<std::string, std::string>> hits;
    for (LinkItem* l : mLinks)
    {
        // Sample the curve against the stroke's segments.
        const QPainterPath& path = l->path();
        bool hit = false;
        for (int k = 0; k < 24 && !hit; ++k)
        {
            QLineF seg(path.pointAtPercent(k / 24.0), path.pointAtPercent((k + 1) / 24.0));
            for (size_t i = 1; i < mCut.size() && !hit; ++i)
                hit = seg.intersects(QLineF(mCut[i - 1], mCut[i])) == QLineF::BoundedIntersection;
        }
        if (hit)
            hits.push_back({l->from->name + "." + l->from->outputs[l->out].id, l->to->name + "." + l->to->inputs[l->in].id});
    }
    mCut.clear();
    if (hits.empty())
        return;
    UndoChunk chunk("Cut Links");
    for (auto& [src, dst] : hits)
        runVerb(*backend, "Disconnect", {{"args", {mGroup, dst}}, {"source", src}});
}

std::optional<QPointF> NodeCanvas::crossing(const QPainterPath& path, const QLineF& stroke)
{
    for (int k = 0; k < 48; ++k)
    {
        QPointF hit;
        if (QLineF(path.pointAtPercent(k / 48.0), path.pointAtPercent((k + 1) / 48.0)).intersects(stroke, &hit) == QLineF::BoundedIntersection)
            return hit;
    }
    return std::nullopt;
}

QPointF NodeCanvas::nearestOn(const QPainterPath& path, QPointF p)
{
    QPointF best = path.pointAtPercent(0.0);
    for (int k = 1; k <= 64; ++k)
        if (QPointF q = path.pointAtPercent(k / 64.0); QLineF(q, p).length() < QLineF(best, p).length())
            best = q;
    return best;
}

LinkItem* NodeCanvas::linkAt(QPointF sp) const
{
    double reach = 6.0 / std::max(0.05, transform().m11());
    LinkItem* best = nullptr;
    double bestD = reach;
    for (LinkItem* l : mLinks)
    {
        if (!l->isVisible() || !l->path().controlPointRect().adjusted(-reach, -reach, reach, reach).contains(sp))
            continue;
        double d = QLineF(nearestOn(l->path(), sp), sp).length();
        if (d < bestD)
        {
            bestD = d;
            best = l;
        }
    }
    return best;
}

void NodeCanvas::insertReroutes(const std::vector<std::pair<LinkItem*, QPointF>>& at)
{
    if (at.empty())
        return;
    struct Cut
    {
        std::string from, to;
        QPointF p;
    };
    std::vector<Cut> cuts;
    for (auto& [l, p] : at)
        cuts.push_back({l->from->name + "." + l->from->outputs[l->out].id, l->to->name + "." + l->to->inputs[l->in].id, p});
    UndoChunk chunk("Add Reroute");
    for (const Cut& c : cuts)
    {
        json r = runVerb(*backend, "AddNode", {{"args", {mGroup, gn::kRerouteType}}, {"position", {c.p.x(), c.p.y()}}});
        if (!r.is_string())
            return;
        std::string name = r.get<std::string>();
        runVerb(*backend, "Disconnect", {{"args", {mGroup, c.to}}, {"source", c.from}});
        runVerb(*backend, "Connect", args({mGroup, c.from, name + "." + gn::kRerouteIn}));
        runVerb(*backend, "Connect", args({mGroup, name + "." + gn::kRerouteOut, c.to}));
    }
}

std::set<LinkItem*> NodeCanvas::rerouteChains() const
{
    std::set<LinkItem*> lit;
    std::vector<NodeItem*> stack;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i); n && n->reroute)
            stack.push_back(n);
    std::set<NodeItem*> seen(stack.begin(), stack.end());
    while (!stack.empty())
    {
        NodeItem* r = stack.back();
        stack.pop_back();
        for (LinkItem* l : mLinks)
        {
            NodeItem* next = l->to == r ? l->from : l->from == r ? l->to : nullptr;
            if (!next)
                continue;
            lit.insert(l);
            if (next->reroute && seen.insert(next).second)
                stack.push_back(next);
        }
    }
    return lit;
}

std::set<NodeItem*> NodeCanvas::selectionFlow() const
{
    std::set<NodeItem*> flow;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i))
            flow.insert(n);
    for (bool up : {true, false})
    {
        std::vector<NodeItem*> stack(flow.begin(), flow.end());
        std::set<NodeItem*> seen(flow.begin(), flow.end());
        while (!stack.empty())
        {
            NodeItem* at = stack.back();
            stack.pop_back();
            for (LinkItem* l : mLinks)
                if (NodeItem* next = up ? (l->to == at ? l->from : nullptr) : (l->from == at ? l->to : nullptr); next && seen.insert(next).second)
                    stack.push_back(next);
        }
        flow.insert(seen.begin(), seen.end());
    }
    return flow;
}

void NodeCanvas::updateEmphasis()
{
    std::set<LinkItem*> lit = rerouteChains();
    bool flowOn = (mFlowToggle || mFlowHold) && !mScene.selectedItems().isEmpty();
    std::set<NodeItem*> flow = flowOn ? selectionFlow() : std::set<NodeItem*>{};
    if (flowOn)
        for (LinkItem* l : mLinks)
            if (flow.contains(l->from) && flow.contains(l->to))
                lit.insert(l);
    // Dim wires off the hovered and selected nodes, by pref.
    std::set<NodeItem*> focus;
    if (mPrefs.dimUnrelated)
    {
        for (QGraphicsItem* i : mScene.selectedItems())
            if (auto* n = dynamic_cast<NodeItem*>(i))
                focus.insert(n);
        if (mHoverNode)
            focus.insert(mHoverNode);
    }
    std::set<NodeItem*> picked;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i))
            picked.insert(n);
    bool refilter = false;
    for (LinkItem* l : mLinks)
    {
        bool filtered = mSelectedWiresOnly && !picked.contains(l->from) && !picked.contains(l->to);
        if (filtered != l->filtered)
        {
            l->filtered = filtered;
            l->applyVisibility();
            refilter = true;
        }
    }
    if (refilter && !mTies.empty())
        layoutTies();
    for (LinkItem* l : mLinks)
    {
        l->setHighlight(lit.contains(l));
        bool unrelated = !focus.empty() && !focus.contains(l->from) && !focus.contains(l->to);
        l->setOpacity((flowOn && !lit.contains(l)) || (unrelated && !lit.contains(l)) ? 0.25 : 1.0);
    }
    for (auto& [name, n] : mNodes)
        n->setOpacity(flowOn && !flow.contains(n) ? 0.35 : 1.0);
}

NodeItem* NodeCanvas::otherEnd(LinkItem* l, QPointF sp) const
{
    double reach = 40.0 / std::max(0.05, transform().m11());
    QPointF a = l->from->socketPos(false, l->out), b = l->to->socketPos(true, l->in);
    double da = QLineF(a, sp).length(), db = QLineF(b, sp).length();
    if (std::min(da, db) > reach)
        return nullptr;
    return da < db ? l->to : l->from;
}

QString NodeCanvas::wireTip(LinkItem* l) const
{
    const SocketInfo& s = l->from->outputs[size_t(l->out)];
    QString tip = qs(l->from->name + "." + s.id) + QString("  %1  ").arg(QChar(0x2192)) + qs(l->to->name + "." + l->to->inputs[size_t(l->in)].id);
    tip += "\n" + qs(s.type) + (s.field ? " field" : "");
    if (QString summary = backend->wireSummary(mInfo, l->from->name, s.id); !summary.isEmpty())
        tip += "\n" + summary;
    return tip;
}

QString NodeCanvas::portalTip(NodeItem* n) const
{
    QString name = n->portalName.empty() ? QString("(no name)") : qs(n->portalName);
    int readers = 0;
    NodeItem* send = nullptr;
    for (auto& [k, item] : mNodes)
        if (item->portal && item->portalName == n->portalName)
        {
            if (item->type == gn::kReceiveType)
                ++readers;
            else
                send = item;
        }
    QString tip;
    if (n->type == gn::kSendType)
        tip = "Send " + name + QString(", read by %1 Receive%2").arg(readers).arg(readers == 1 ? "" : "s");
    else
        tip = "Receive " + name + (send ? " from " + qs(send->name) : QString());
    if (n->type == gn::kReceiveType && send)
        for (LinkItem* l : mLinks)
            if (l->to == send)
                tip += ", fed by " + qs(l->from->name + "." + l->from->outputs[l->out].id);
    if (!n->problem.isEmpty())
        tip += "\n" + n->problem;
    return tip + "\nDouble-click a Receive to jump to its Send.";
}

NodeItem* NodeCanvas::loneDraggedNode() const
{
    QList<QGraphicsItem*> sel = mScene.selectedItems();
    if (sel.size() != 1 || mPressPositions.empty())
        return nullptr;
    auto* n = dynamic_cast<NodeItem*>(sel[0]);
    if (!n || n->pos() == mPressPositions.at(n->name))
        return nullptr;
    for (LinkItem* l : mLinks)
        if (l->from == n || l->to == n)
            return nullptr;
    return n;
}

void NodeCanvas::findInsertLink()
{
    LinkItem* best = nullptr;
    if (NodeItem* n = loneDraggedNode())
    {
        QRectF r = n->sceneBoundingRect();
        int most = 0;
        for (LinkItem* l : mLinks)
        {
            // A level wire has a flat control rect, which intersects nothing until padded.
            if (!l->isVisible() || !l->path().controlPointRect().adjusted(-1, -1, 1, 1).intersects(r))
                continue;
            int inside = 0;
            for (int k = 0; k <= 32; ++k)
                inside += r.contains(l->path().pointAtPercent(k / 32.0)) ? 1 : 0;
            auto [in, out] = insertSockets(n, l);
            if (inside > most && in >= 0 && out >= 0)
            {
                most = inside;
                best = l;
            }
        }
    }
    if (best == mInsertLink)
        return;
    if (mInsertLink)
        mInsertLink->setHighlight(false);
    mInsertLink = best;
    if (best)
        best->setHighlight(true);
}

std::pair<int, int> NodeCanvas::insertSockets(NodeItem* n, LinkItem* l) const
{
    const std::string& fromType = l->from->outputs[l->out].type;
    const std::string& toType = l->to->inputs[l->in].type;
    auto pick = [](const std::vector<SocketInfo>& list, auto&& same, auto&& fits) {
        for (int i = 0; i < int(list.size()); ++i)
            if (same(list[i].type))
                return i;
        for (int i = 0; i < int(list.size()); ++i)
            if (fits(list[i].type))
                return i;
        return -1;
    };
    if (n->reroute)
        return {0, 0};
    int in = pick(n->inputs, [&](const std::string& t) { return t == fromType; }, [&](const std::string& t) { return backend->canLink(fromType, t); });
    int out = pick(n->outputs, [&](const std::string& t) { return t == toType; }, [&](const std::string& t) { return backend->canLink(t, toType); });
    return {in, out};
}

void NodeCanvas::insertOnLink(LinkItem* l)
{
    NodeItem* n = nullptr;
    for (QGraphicsItem* i : mScene.selectedItems())
        n = dynamic_cast<NodeItem*>(i);
    if (!n)
        return;
    auto [in, out] = insertSockets(n, l);
    if (in < 0 || out < 0)
        return;
    std::string src = l->from->name + "." + l->from->outputs[l->out].id, dst = l->to->name + "." + l->to->inputs[l->in].id;
    std::string mineIn = n->name + "." + (n->reroute ? std::string(gn::kRerouteIn) : n->inputs[in].id);
    std::string mineOut = n->name + "." + (n->reroute ? std::string(gn::kRerouteOut) : n->outputs[out].id);
    NodeItem* to = l->to;
    runVerb(*backend, "Disconnect", {{"args", {mGroup, dst}}, {"source", src}});
    runVerb(*backend, "Connect", args({mGroup, src, mineIn}));
    runVerb(*backend, "Connect", args({mGroup, mineOut, dst}));
    if (!mPrefs.autoOffset)
        return;
    double shift = n->sceneBoundingRect().right() + 40.0 - to->sceneBoundingRect().left();
    if (shift <= 0.0 || to == n)
        return;
    std::set<NodeItem*> downstream{to};
    std::vector<NodeItem*> stack{to};
    while (!stack.empty())
    {
        NodeItem* at = stack.back();
        stack.pop_back();
        for (LinkItem* k : mLinks)
            if (k->from == at && k->to != n && downstream.insert(k->to).second)
                stack.push_back(k->to);
    }
    json positions = json::object();
    for (NodeItem* d : downstream)
        positions[d->name] = {d->pos().x() + shift, d->pos().y()};
    runVerb(*backend, "MoveNodes", {{"args", {mGroup}}, {"positions", positions}});
}

QColor NodeCanvas::wireColor(const std::string& type) const
{
    if (mPrefs.greyWires && !chromaticTheme())
        return gray(165);
    return backend->socketStyle(type).color;
}

void NodeCanvas::lazyConnect(NodeItem* from, NodeItem* to)
{
    int bestOut = -1, bestIn = -1;
    double best = 0.0;
    for (int o = 0; o < int(from->outputs.size()); ++o)
        for (int i = 0; i < int(to->inputs.size()); ++i)
        {
            const std::string& a = from->outputs[size_t(o)].type;
            const SocketInfo& in = to->inputs[size_t(i)];
            double score = a == in.type ? 3.0 : backend->canLink(a, in.type) ? 1.0 : 0.0;
            if (score > 0.0 && !in.linked)
                score += 1.5;
            if (score > best)
            {
                best = score;
                bestOut = o;
                bestIn = i;
            }
        }
    if (bestOut < 0)
    {
        QToolTip::showText(QCursor::pos(), qs(from->name + " has no output that fits an input of " + to->name), this);
        return;
    }
    std::string out = from->name + "." + (from->reroute ? std::string(gn::kRerouteOut) : from->outputs[size_t(bestOut)].id);
    std::string in = to->name + "." + (to->reroute ? std::string(gn::kRerouteIn) : to->inputs[size_t(bestIn)].id);
    runVerb(*backend, "Connect", args({mGroup, out, in}));
}

bool NodeCanvas::swapInputs()
{
    std::vector<NodeItem*> nodes = selectedNodes();
    if (nodes.size() != 1)
        return false;
    NodeItem* n = nodes[0];
    int a = -1, b = -1;
    for (int i = 0; i < int(n->inputs.size()) && b < 0; ++i)
        for (int j = i + 1; j < int(n->inputs.size()); ++j)
            if (n->inputs[size_t(i)].type == n->inputs[size_t(j)].type && !backend->valueless(n->inputs[size_t(i)].type) == !backend->valueless(n->inputs[size_t(j)].type))
            {
                a = i;
                b = j;
                break;
            }
    if (a < 0)
        return false;
    const SocketInfo& sa = n->inputs[size_t(a)];
    const SocketInfo& sb = n->inputs[size_t(b)];
    std::vector<std::string> intoA, intoB;
    for (LinkItem* l : mLinks)
        if (l->to == n && (l->in == a || l->in == b))
            (l->in == a ? intoA : intoB).push_back(l->from->name + "." + l->from->outputs[size_t(l->out)].id);
    std::string pa = n->name + "." + sa.id, pb = n->name + "." + sb.id;
    json va = sa.value, vb = sb.value;
    UndoChunk chunk("Swap Inputs");
    if (!intoA.empty())
        runVerb(*backend, "Disconnect", args({mGroup, pa}));
    if (!intoB.empty())
        runVerb(*backend, "Disconnect", args({mGroup, pb}));
    if (!backend->valueless(sa.type) && !va.is_null() && !vb.is_null() && va != vb)
    {
        runVerb(*backend, "SetValue", args({mGroup, pa, vb}));
        runVerb(*backend, "SetValue", args({mGroup, pb, va}));
    }
    for (const std::string& s : intoA)
        runVerb(*backend, "Connect", args({mGroup, s, pb}));
    for (const std::string& s : intoB)
        runVerb(*backend, "Connect", args({mGroup, s, pa}));
    return true;
}
} // namespace lila::ui
