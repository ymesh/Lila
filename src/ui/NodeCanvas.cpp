#include "ui/NodeCanvas.h"
#include "ui/ColorPopup.h"
#include "ui/Hotkeys.h"

namespace lila::ui::canvas
{
namespace
{
/** An input row's value as the node edits it: a slider box, a check, or a swatch, with the input's name. */
void paintField(QPainter& p, const SocketInfo& s, FieldKind kind, QRectF r, bool hot)
{
    QRectF text = r.adjusted(6, 0, -6, 0);
    QFontMetrics fm = p.fontMetrics();
    QString label = pretty(s.id);
    if (kind == FieldKind::Toggle)
    {
        const bool on = s.value.is_boolean() ? s.value.get<bool>() : s.value.get<double>() != 0.0;
        QRectF box(r.left() + 4, r.center().y() - 5.5, 11, 11);
        p.setPen(QPen(gray(on ? 205 : hot ? 160 : 110), 1.0));
        p.setBrush(on ? gray(205) : gray(hot ? 52 : 40));
        p.drawRoundedRect(box, 2, 2);
        if (on)
        {
            p.setPen(QPen(gray(30), 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            QPolygonF tick;
            tick << box.topLeft() + QPointF(2.5, 5.8) << box.topLeft() + QPointF(4.7, 8.2) << box.topLeft() + QPointF(8.6, 3.0);
            p.drawPolyline(tick);
        }
        p.setPen(gray(hot ? 225 : 195));
        QRectF t = r.adjusted(box.width() + 10, 0, -4, 0);
        p.drawText(t, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight, int(t.width())));
        return;
    }
    if (kind == FieldKind::Color)
    {
        glm::dvec3 c(s.value[0].get<double>(), s.value[1].get<double>(), s.value[2].get<double>());
        QRectF swatch(r.right() - 38, r.top() + 1, 38, r.height() - 2);
        p.setPen(QPen(gray(hot ? 200 : 20), 1.0));
        p.setBrush(colorMath::display(c, 1.0));
        p.drawRoundedRect(swatch, 3, 3);
        p.setPen(gray(hot ? 225 : 195));
        QRectF t(text.left() - 2, r.top(), swatch.left() - text.left() - 4, r.height());
        p.drawText(t, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight, int(t.width())));
        return;
    }
    p.setPen(QPen(gray(hot ? 75 : 40), 1));
    p.setBrush(gray(hot ? 52 : 44));
    p.drawRoundedRect(r, 3, 3);
    const double v = s.value.get<double>();
    if (auto range = fieldRange(s.limits))
    {
        double t = std::clamp((v - range->first) / (range->second - range->first), 0.0, 1.0);
        QPainterPath box;
        box.addRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
        p.save();
        p.setClipPath(box);
        p.fillRect(QRectF(r.left(), r.top(), r.width() * t, r.height()), gray(hot ? 76 : 66));
        p.restore();
    }
    QString value = valueText(s.value, s.type);
    if (auto names = s.limits.find("enum"); names != s.limits.end() && v >= 0.0 && v < double(names->size()) && (*names)[size_t(v)].is_string())
        value = pretty((*names)[size_t(v)].get<std::string>());
    int valueWidth = fm.horizontalAdvance(value);
    p.setPen(gray(hot ? 240 : 225));
    p.drawText(text, Qt::AlignVCenter | Qt::AlignRight, value);
    p.setPen(gray(hot ? 200 : 170));
    QRectF t = text.adjusted(0, 0, -valueWidth - 6, 0);
    p.drawText(t, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(label, Qt::ElideRight, int(std::max(0.0, t.width()))));
}
} // namespace

NodeItem::NodeItem(NodeCanvas* canvas, const json& info) : mCanvas(canvas)
{
    name = info.value("name", "");
    type = info.value("type", "");
    category = info.value("category", "");
    label = qs(info.value("label", type));
    for (const json& s : info.value("inputs", json::array()))
    {
        inputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), s.value("multi", false), s.value("linked", false),
                          s.value("value", json()), s.value("implicit", ""), s.value("doc", "")});
        for (const char* k : {"min", "max", "softMin", "softMax", "step"})
            if (s.contains(k) && s[k].is_number())
                inputs.back().limits[k] = s[k];
        if (s.contains("enum") && s["enum"].is_array())
            inputs.back().limits["enum"] = s["enum"];
    }
    for (const json& s : info.value("outputs", json::array()))
        outputs.push_back({s.value("id", ""), s.value("type", ""), s.value("field", false), false, false, json(), "", s.value("doc", "")});
    for (const json& p : info.value("props", json::array()))
    {
        props.push_back({p.value("id", ""), p.value("value", "")});
        if (p.contains("editor"))
            textProps.insert(props.back().first);
        std::vector<std::string> options;
        for (const json& o : p.value("options", json::array()))
            if (o.is_string())
                options.push_back(o.get<std::string>());
        propOptions.push_back(std::move(options));
    }
    // Texture nodes keep settings as params; their choices (enums and checks) edit on the node like props.
    for (const json& p : info.value("params", json::array()))
    {
        const std::string kind = p.value("type", ""), id = p.value("id", "");
        const json v = p.contains("value") ? p["value"] : p.value("default", json());
        if (kind == "enum" && v.is_string())
        {
            props.push_back({id, v.get<std::string>()});
            propOptions.push_back(p.value("options", std::vector<std::string>{}));
        }
        else if (kind == "bool" && v.is_boolean())
        {
            props.push_back({id, v.get<bool>() ? "true" : "false"});
            propOptions.push_back({"false", "true"});
            boolParams.insert(id);
        }
    }
    viewer = !canvas->backend->viewerType().empty() && type == canvas->backend->viewerType();
    previewSize = canvas->backend->thumbnailSize(type);
    if (canvas->backend->runsSubgroup(type))
    {
        for (auto& [id, v] : props)
            if (id == "group")
                groupRef = v;
        for (const json& p : info.value("params", json::array()))
            if (p.value("id", "") == "group" && p.value("value", json()).is_string())
                groupRef = p["value"].get<std::string>();
    }
    // A loop node keeps its kind in the title so a Repeat and a For Each over the same group tell apart.
    if (!groupRef.empty())
        label = type == canvas->backend->subgroupNodeType() ? qs(groupRef) : label + ": " + qs(groupRef);
    typeLabel = label;
    reroute = type == gn::kRerouteType;
    portal = type == gn::kSendType || type == gn::kReceiveType;
    problem = qs(info.value("problem", ""));
    if (portal)
    {
        props.clear();
        propOptions.clear();
    }
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    layoutRows();
    const json& pos = info["position"];
    setPos(pos[0].get<double>(), pos[1].get<double>());
    QString tip = label;
    if (!groupRef.empty())
    {
        QString back = Hotkeys::get().keyFor("nodeEditor.parentGroup");
        tip += "\nGroup node: double-click to open " + qs(groupRef) + (back.isEmpty() ? "." : ", " + back + " to come back.");
    }
    if (isViewer())
        tip += "\nClick the eye to preview in the viewport.";
    if (reroute)
        tip = "Reroute" + (outputs.empty() ? QString() : " (" + qs(outputs[0].type) + ")") + "\nDrag the middle to move it, the rim to wire it to more inputs.";
    setToolTip(tip);
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant& value)
{
    if (change == ItemPositionHasChanged)
        mCanvas->nodeMoved(this);
    return QGraphicsItem::itemChange(change, value);
}

void NodeItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    if (reroute)
    {
        QColor c = outputs.empty() ? gray(165) : mCanvas->backend->socketStyle(outputs[0].type).color;
        p->setPen(isSelected() ? QPen(themeColor("nodeSelected"), 2.0) : QPen(gray(18), 1.2));
        p->setBrush(c);
        p->drawEllipse(QPointF(0.0, 0.0), kRerouteR, kRerouteR);
        return;
    }
    if (muted)
        p->setOpacity(p->opacity() * 0.45);
    QRectF body(0, 0, kNodeWidth, height());
    p->setPen(isSelected() ? QPen(themeColor("nodeSelected"), 1.6) : QPen(gray(20), 1.0));
    p->setBrush(themeColor("nodeBody"));
    p->drawRoundedRect(body, 6, 6);
    QPainterPath head;
    head.addRoundedRect(QRectF(0, 0, kNodeWidth, kHeader), 6, 6);
    if (!collapsed)
        head.addRect(QRectF(0, kHeader - 6, kNodeWidth, 6));
    p->setPen(Qt::NoPen);
    const GraphBackend& backend = *mCanvas->backend;
    p->setBrush(backend.headerColor(type, category));
    p->drawPath(head.simplified());
    p->setPen(gray(232));
    QFont f = p->font();
    f.setBold(true);
    p->setFont(f);
    p->drawText(QRectF(8, 0, kNodeWidth - 16 - (isViewer() || !problem.isEmpty() ? kHeader : 0.0), kHeader), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(shownTitle(), Qt::ElideRight, int(kNodeWidth - 16 - (problem.isEmpty() ? 0.0 : kHeader))));
    if (!problem.isEmpty())
    {
        QRectF b = badgeRect();
        p->setPen(Qt::NoPen);
        p->setBrush(themeColor("statusError"));
        p->drawEllipse(b);
        p->setPen(QPen(gray(15), 1.0));
        p->drawText(b, Qt::AlignCenter, "!");
    }
    f.setBold(false);
    p->setFont(f);
    if (isViewer())
    {
        QRectF eye = eyeRect();
        if (viewerActive)
        {
            p->setPen(Qt::NoPen);
            p->setBrush(gray(150));
            p->drawRoundedRect(eye, 3, 3);
        }
        QIcon ic = icon(viewerActive ? "eye" : "eye-off");
        p->drawPixmap(eye.adjusted(2, 2, -2, -2).toRect(), ic.pixmap(QSize(32, 32), viewerActive ? QIcon::Active : QIcon::Normal));
    }

    if (collapsed)
    {
        // One socket mark on each side that has links, where the wires meet.
        for (bool in : {true, false})
            for (const SocketInfo& s : in ? inputs : outputs)
                if (s.linked)
                {
                    drawSocket(*p, QPointF(in ? 0.0 : kNodeWidth, kHeader * 0.5), backend.socketStyle(s.type), false);
                    break;
                }
        return;
    }
    if (thumbSize > 0)
    {
        QRectF r((kNodeWidth - thumbSize) * 0.5, kHeader + 4.0, thumbSize, thumbSize);
        p->setPen(QPen(gray(20), 1.0));
        p->setBrush(gray(28));
        p->drawRect(r);
        if (!thumb.isNull())
            p->drawImage(r, thumb);
    }

    double y = top();
    for (int oi : shownOut)
    {
        size_t i = size_t(oi);
        p->setPen(gray(205));
        p->drawText(QRectF(8, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignRight, pretty(outputs[i].id));
        drawSocket(*p, QPointF(kNodeWidth, y + kRow * 0.5), backend.socketStyle(outputs[i].type), outputs[i].field);
        y += kRow;
    }
    for (size_t i = 0; i < props.size(); ++i, y += kRow)
    {
        QRectF r(8, y + 2, kNodeWidth - 16, kRow - 4);
        if (!propOptions[i].empty() && !textProps.contains(props[i].first))
        {
            paintChoice(*p, i, r);
            continue;
        }
        p->setPen(QPen(gray(40), 1));
        p->setBrush(gray(44));
        p->drawRoundedRect(r, 3, 3);
        p->setPen(gray(200));
        QString text = props[i].second.empty() ? pretty(props[i].first) : pretty(props[i].second);
        if (textProps.contains(props[i].first) && !props[i].second.empty())
        {
            QStringList lines = qs(props[i].second).split('\n');
            text = lines.size() > 1 ? lines.front() + " ..." : lines.front();
        }
        QRectF textRect = r.adjusted(6, 0, -6, 0);
        p->drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, p->fontMetrics().elidedText(text, Qt::ElideRight, int(textRect.width())));
    }
    for (int ii : shownIn)
    {
        const SocketInfo& s = inputs[size_t(ii)];
        if (FieldKind kind = fieldKind(ii); kind != FieldKind::None)
            paintField(*p, s, kind, QRectF(8, y + 2, kNodeWidth - 16, kRow - 4), ii == hotField);
        else
        {
            QString text = pretty(s.id);
            bool valueless = backend.valueless(s.type);
            if (!s.linked && !valueless)
            {
                QString v = !s.implicit.empty() && s.value.is_null() ? pretty(s.implicit) : valueText(s.value, s.type);
                if (!v.isEmpty())
                    text += "  " + v;
            }
            p->setPen(s.linked || valueless ? gray(205) : gray(170));
            p->drawText(QRectF(12, y, kNodeWidth - 18, kRow), Qt::AlignVCenter | Qt::AlignLeft,
                        p->fontMetrics().elidedText(text, Qt::ElideRight, int(kNodeWidth - 20)));
        }
        drawSocket(*p, QPointF(0, y + kRow * 0.5), backend.socketStyle(s.type), s.field);
        if (s.multi)
        {
            p->setPen(QPen(gray(18), 1));
            p->drawLine(QPointF(-2, y + kRow * 0.5), QPointF(2, y + kRow * 0.5));
        }
        y += kRow;
    }
}

bool NodeItem::propIsToggle(size_t i) const
{
    const std::vector<std::string>& o = propOptions[i];
    return o.size() == 2 && o[0] == "false" && o[1] == "true";
}

/** A prop with choices: a check with its name for false/true, else a dropdown showing the current choice. */
void NodeItem::paintChoice(QPainter& p, size_t i, QRectF r) const
{
    const bool hot = int(i) == hotProp;
    QFontMetrics fm = p.fontMetrics();
    if (propIsToggle(i))
    {
        SocketInfo s;
        s.id = props[i].first;
        s.value = props[i].second == "true";
        paintField(p, s, FieldKind::Toggle, r, hot);
        return;
    }
    p.setPen(QPen(gray(hot ? 75 : 40), 1));
    p.setBrush(gray(hot ? 52 : 44));
    p.drawRoundedRect(r, 3, 3);
    const double cx = r.right() - 9.0, cy = r.center().y();
    p.setPen(QPen(gray(hot ? 215 : 160), 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawPolyline(QPolygonF({QPointF(cx - 3.5, cy - 1.8), QPointF(cx, cy + 1.8), QPointF(cx + 3.5, cy - 1.8)}));
    p.setPen(gray(hot ? 225 : 200));
    QRectF t = r.adjusted(6, 0, -18, 0);
    p.drawText(t, Qt::AlignVCenter | Qt::AlignLeft, fm.elidedText(pretty(props[i].second), Qt::ElideRight, int(t.width())));
}

QString NodeItem::title() const
{
    // Collapsed, the operation dropdown is hidden, so the header carries it.
    if (collapsed && label == typeLabel)
        for (auto& [id, v] : props)
            if (id == "operation" && !v.empty())
                return label + ": " + pretty(v);
    if (!portal)
        return label;
    return label + QString(" %1 ").arg(QChar(0x00b7)) + (portalName.empty() ? QString("(unnamed)") : qs(portalName));
}

QString NodeItem::socketTip(QPointF scenePos) const
{
    bool input = false;
    int i = socketAt(scenePos, input);
    if (i < 0)
        return {};
    const SocketInfo& s = input ? inputs[i] : outputs[i];
    QString t = qs(s.id) + ": " + qs(s.type) + (s.field ? " (field)" : "") + (s.multi ? ", takes several links" : "");
    if (!s.implicit.empty())
        t += "\nUnlinked, it reads the " + qs(s.implicit) + ".";
    if (!s.doc.empty())
        t += "\n" + qs(s.doc);
    return t;
}

QRectF NodeItem::boundingRect() const
{
    if (reroute)
        return QRectF(-kRerouteR - 4, -kRerouteR - 4, 2 * kRerouteR + 8, 2 * kRerouteR + 8);
    return QRectF(-2, -2, kNodeWidth + 4, height() + 4);
}

void NodeItem::layoutRows()
{
    prepareGeometryChange();
    shownOut.clear();
    shownIn.clear();
    thumbSize = hidePreview || !mCanvas->previews() ? 0 : previewSize;
    if (collapsed)
        return;
    const GraphBackend& backend = *mCanvas->backend;
    auto anchors = [&](const SocketInfo& s) { return s.linked || backend.primarySocket(s.type); };
    const bool all = !hideUnused || (std::none_of(inputs.begin(), inputs.end(), anchors) && std::none_of(outputs.begin(), outputs.end(), anchors));
    for (int i = 0; i < int(outputs.size()); ++i)
        if (all || anchors(outputs[size_t(i)]))
            shownOut.push_back(i);
    for (int i = 0; i < int(inputs.size()); ++i)
        if (all || anchors(inputs[size_t(i)]))
            shownIn.push_back(i);
}

FieldKind NodeItem::fieldKind(int i) const
{
    const SocketInfo& s = inputs[size_t(i)];
    if (s.linked || s.value.is_null() || mCanvas->backend->valueless(s.type))
        return FieldKind::None;
    if (s.type == "bool")
        return s.value.is_boolean() || s.value.is_number() ? FieldKind::Toggle : FieldKind::None;
    if (s.type == "color")
        return s.value.is_array() && (s.value.size() == 3 || s.value.size() == 4) && s.value[0].is_number() && s.value[1].is_number() &&
                       s.value[2].is_number()
                   ? FieldKind::Color
                   : FieldKind::None;
    return s.value.is_number() ? FieldKind::Number : FieldKind::None;
}

QRectF NodeItem::fieldRect(int i) const
{
    auto it = std::find(shownIn.begin(), shownIn.end(), i);
    if (collapsed || reroute || it == shownIn.end())
        return {};
    double row = double(shownOut.size() + props.size()) + double(it - shownIn.begin());
    return QRectF(8, top() + kRow * row + 2, kNodeWidth - 16, kRow - 4);
}

QRectF NodeItem::propRect(size_t i) const
{
    if (collapsed || reroute || i >= props.size())
        return {};
    return QRectF(8, top() + kRow * double(shownOut.size() + i) + 2, kNodeWidth - 16, kRow - 4);
}

int NodeItem::propAt(QPointF scenePos) const
{
    QPointF local = mapFromScene(scenePos);
    for (size_t i = 0; i < props.size(); ++i)
        if (!propOptions[i].empty() && !textProps.contains(props[i].first) && propRect(i).contains(local))
            return int(i);
    return -1;
}

int NodeItem::fieldAt(QPointF scenePos) const
{
    QPointF local = mapFromScene(scenePos);
    for (int i : shownIn)
        if (fieldKind(i) != FieldKind::None && fieldRect(i).contains(local))
            return i;
    return -1;
}

double NodeItem::height() const
{
    if (collapsed)
        return kHeader;
    return top() + kRow * double(shownOut.size() + props.size() + shownIn.size()) + 6.0;
}

QPointF NodeItem::socketPos(bool input, int index) const
{
    if (input ? inProxy.has_value() : outProxy.has_value())
        return input ? *inProxy : *outProxy;
    if (reroute)
        return mapToScene(QPointF(0.0, 0.0));
    const std::vector<int>& shown = input ? shownIn : shownOut;
    auto it = std::find(shown.begin(), shown.end(), index);
    double x = input ? 0.0 : kNodeWidth;
    if (it == shown.end())
        return mapToScene(QPointF(x, kHeader * 0.5));
    double k = double(it - shown.begin());
    double row = input ? double(shownOut.size() + props.size()) + k : k;
    return mapToScene(QPointF(x, top() + kRow * row + kRow * 0.5));
}

int NodeItem::socketAt(QPointF scenePos, bool& input) const
{
    // A reroute's middle drags it; its rim starts a new wire from its output.
    if (reroute)
    {
        double d = QLineF(mapToScene(QPointF(0.0, 0.0)), scenePos).length();
        input = false;
        return d > kRerouteR - 1.0 && d <= kRerouteR + 5.0 && !outputs.empty() ? 0 : -1;
    }
    for (int pass = 0; pass < 2; ++pass)
    {
        bool in = pass == 1;
        for (int i : in ? shownIn : shownOut)
            if (QLineF(socketPos(in, i), scenePos).length() <= kSocketR + 4.0)
            {
                input = in;
                return i;
            }
    }
    return -1;
}

void LinkItem::setHighlight(bool on)
{
    if (on == highlight)
        return;
    highlight = on;
    QPen p = base;
    if (on)
    {
        p.setColor(themeColor("nodeSelected"));
        p.setWidthF(base.widthF() + 1.0);
    }
    setPen(p);
    setZValue(on ? -0.8 : -1.0);
}

void LinkItem::update()
{
    QPointF a = from->socketPos(false, out), b = to->socketPos(true, in);
    QPainterPath path(a);
    if (passes.empty() && gWireStyle == WireStyle::Curved)
    {
        double dx = std::max(40.0, std::abs(b.x() - a.x()) * 0.5);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);
        setPath(path);
        return;
    }
    QPointF at = a, along(1.0, 0.0);
    for (auto& [entry, exit] : passes)
    {
        QPointF d = exit - entry;
        d /= std::max(1e-9, std::hypot(d.x(), d.y()));
        bend(path, at, along, entry, d);
        path.lineTo(exit);
        at = exit;
        along = d;
    }
    bend(path, at, along, b, QPointF(1.0, 0.0));
    setPath(path);
}

void FrameItem::setBox(QSizeF s, bool fold)
{
    prepareGeometryChange();
    size = s;
    collapsed = fold;
}

void FrameItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    QColor body = color, title = color;
    body.setAlpha(collapsed ? 200 : 55);
    title.setAlpha(170);
    p->setPen(QPen(color.darker(160), 1.0));
    p->setBrush(body);
    p->drawRoundedRect(box(), 5, 5);
    QPainterPath head;
    head.addRoundedRect(titleRect(), 5, 5);
    if (!collapsed)
        head.addRect(QRectF(0.0, kTitle - 5.0, size.width(), 5.0));
    p->setPen(Qt::NoPen);
    p->setBrush(title);
    p->drawPath(head.simplified());
    if (isSelected())
    {
        p->setPen(QPen(themeColor("nodeSelected"), 2.0));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(collapsed ? titleRect() : box(), 5, 5);
    }
    QColor ink = title.lightness() > 140 ? gray(25) : gray(235);
    p->setPen(ink);
    p->setBrush(ink);
    QRectF a = arrowRect().adjusted(4, 4, -4, -4);
    QPolygonF tri;
    if (collapsed)
        tri << a.topLeft() << QPointF(a.right(), a.center().y()) << a.bottomLeft();
    else
        tri << a.topLeft() << a.topRight() << QPointF(a.center().x(), a.bottom());
    p->drawPolygon(tri);
    QFont f = p->font();
    f.setBold(true);
    p->setFont(f);
    p->drawText(titleRect().adjusted(kTitle, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft,
                p->fontMetrics().elidedText(label, Qt::ElideRight, int(size.width() - kTitle - 6)));
    if (!collapsed && !text.isEmpty())
    {
        f.setBold(false);
        p->setFont(f);
        p->setPen(gray(215));
        p->save();
        p->setClipRect(textRect());
        p->drawText(textRect(), Qt::AlignTop | Qt::AlignLeft | Qt::TextWordWrap, text);
        p->restore();
    }
    if (!collapsed)
    {
        QRectF g = gripRect();
        p->setPen(QPen(ink, 1.0));
        for (double k = 4.0; k < 13.0; k += 4.0)
            p->drawLine(QPointF(g.right() - k, g.bottom() - 2), QPointF(g.right() - 2, g.bottom() - k));
    }
}

void TieItem::setCount(int n)
{
    prepareGeometryChange();
    count = n;
}

QRectF TieItem::band() const
{
    double h = double(std::max(count, 2) - 1) * kTieSpacing + 12.0;
    return QRectF(-kTieBand * 0.5, -h * 0.5, kTieBand, h);
}

QPainterPath TieItem::shape() const
{
    QPainterPath p;
    p.addRect(band().adjusted(-3, -6, 3, 3));
    return p;
}

void TieItem::paint(QPainter* p, const QStyleOptionGraphicsItem*, QWidget*)
{
    QRectF r = band();
    p->setPen(QPen(hot ? gray(235) : gray(18), hot ? 1.6 : 1.0));
    p->setBrush(color);
    p->drawRoundedRect(r, 3, 3);
    // The tie's lock block at one end.
    p->drawRoundedRect(QRectF(-kTieBand * 0.5 - 1.0, r.top() - 5.0, kTieBand + 2.0, 6.0), 1.5, 1.5);
    p->setPen(QPen(color.lightness() > 128 ? color.darker(150) : color.lighter(160), 0.8));
    for (double y = r.top() + 4.0; y < r.bottom() - 2.0; y += 3.0)
        p->drawLine(QPointF(r.left() + 2.0, y), QPointF(r.right() - 2.0, y));
}
} // namespace lila::ui::canvas

namespace lila::ui
{
namespace
{
/**
 * Fits a newly loaded group once the canvas is shown at a real size, and again as the panel settles, until the user
 * pans, zooms or clicks. Framing a hidden or not yet laid out view fits the nodes into nothing, which leaves them
 * off-screen when it appears.
 */
class NodeCanvasFramer : public QObject
{
public:
    explicit NodeCanvasFramer(NodeCanvas* canvas) : QObject(canvas), mCanvas(canvas)
    {
        canvas->viewport()->installEventFilter(this);
        canvas->installEventFilter(this);
    }

    void request()
    {
        mPending = true;
        QTimer::singleShot(0, mCanvas, [self = QPointer(this)] {
            if (self)
                self->tryFrame();
        });
    }

    bool eventFilter(QObject*, QEvent* e) override
    {
        const QEvent::Type t = e->type();
        if (t == QEvent::MouseButtonPress || t == QEvent::Wheel || t == QEvent::KeyPress)
            mPending = false;
        if (mPending && (t == QEvent::Show || t == QEvent::Resize))
            QTimer::singleShot(0, mCanvas, [self = QPointer(this)] {
                if (self)
                    self->tryFrame();
            });
        return false;
    }

private:
    void tryFrame()
    {
        constexpr int kMinSide = 50;
        if (!mPending || !mCanvas->isVisible() || mCanvas->viewport()->width() < kMinSide || mCanvas->viewport()->height() < kMinSide)
            return;
        mCanvas->frame(false);
    }

    NodeCanvas* mCanvas;
    bool mPending = false;
};
} // namespace

NodeCanvas::NodeCanvas(QWidget* parent) : QGraphicsView(parent)
{
    setScene(&mScene);
    mScene.setSceneRect(-20000, -20000, 40000, 40000);
    setRenderHint(QPainter::Antialiasing);
    setDragMode(QGraphicsView::RubberBandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setViewportUpdateMode(QGraphicsView::FullViewportUpdate);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setFocusPolicy(Qt::StrongFocus);
    setBackgroundBrush(gray(34));
    setAcceptDrops(true);
    gWireStyle = mPrefs.wires;
    new NodeCanvasFramer(this);
    connect(&mScene, &QGraphicsScene::selectionChanged, this, [this] {
        // The editor clears onSelect as it goes; the scene then deletes items the lists still hold.
        if (!onSelect)
            return;
        updateEmphasis();
        if (mBuilding)
            return;
        const std::vector<std::string> sel = selectedNames();
        onSelect(sel.size() == 1 && mScene.selectedItems().size() == 1 ? sel[0] : std::string());
    });
}

std::vector<std::pair<std::string, std::string>> NodeCanvas::outputsOf(const std::string& node) const
{
    std::vector<std::pair<std::string, std::string>> out;
    if (auto it = mNodes.find(node); it != mNodes.end())
        for (const SocketInfo& si : it->second->outputs)
            out.push_back({si.id, si.type});
    return out;
}

std::string NodeCanvas::typeOf(const std::string& node) const
{
    auto it = mNodes.find(node);
    return it == mNodes.end() ? std::string() : it->second->type;
}

QPointF NodeCanvas::positionOf(const std::string& node) const
{
    auto it = mNodes.find(node);
    return it == mNodes.end() ? QPointF() : it->second->pos();
}

void NodeCanvas::build(const std::string& group, const json& info, const std::set<std::string>& selected)
{
    mBuilding = true;
    bool newGroup = group != mGroup;
    // Frames have no names: they keep their selection by index across an edit's rebuild.
    std::vector<int> framesSelected = std::exchange(mSelectFramesNext, {});
    if (framesSelected.empty() && !newGroup)
        framesSelected = selectedFrames();
    mGroup = group;
    mLinks.clear();
    mNodes.clear();
    mTieItems.clear();
    mTies.clear();
    mInsertLink = nullptr;
    mFrameItems.clear();
    mFrames.clear();
    mFrameDrag = -1;
    mLazyFrom = nullptr;
    mHoverNode = nullptr;
    mHotFieldNode = nullptr;
    mTieDrag = -1;
    mTieStroke = false;
    mHotTie = -1;
    mScene.clear();
    mTemp = nullptr;
    if (info.is_object())
    {
        std::map<std::string, bool> active;
        std::string viewerType = backend->viewerType();
        for (const json& d : info["definition"].value("nodes", json::array()))
            if (!viewerType.empty() && d.value("type", "") == viewerType)
                active[d.value("name", "")] = d.contains("values") && d["values"].value("active", false);
        std::map<std::string, std::string> portalNames;
        std::map<std::string, gn::GNode> flags;
        for (const json& d : info["definition"].value("nodes", json::array()))
        {
            if (gn::isRoutingType(d.value("type", "")) && d.contains("values"))
                portalNames[d.value("name", "")] = d["values"].value("name", "");
            gn::getNodeFlags(flags[d.value("name", "")], d);
        }
        for (const json& n : info.value("nodes", json::array()))
        {
            auto* item = new NodeItem(this, n);
            item->portalName = portalNames[item->name];
            const gn::GNode& f = flags[item->name];
            item->collapsed = f.collapsed;
            item->hideUnused = f.hideUnused;
            item->hidePreview = f.hidePreview;
            item->muted = f.muted;
            if (!f.label.empty())
                item->label = qs(f.label);
            item->viewerActive = active[item->name];
            mScene.addItem(item);
            mNodes[item->name] = item;
            item->setSelected(selected.contains(item->name));
        }
        for (const json& l : info["definition"].value("links", json::array()))
        {
            auto f = mNodes.find(l[0].get<std::string>()), t = mNodes.find(l[2].get<std::string>());
            if (f == mNodes.end() || t == mNodes.end())
                continue;
            int out = outputIndex(f->second, l[1].get<std::string>()), in = inputIndex(t->second, l[3].get<std::string>());
            if (out < 0 || in < 0)
                continue;
            f->second->outputs[size_t(out)].linked = true;
            const std::string& type = f->second->outputs[size_t(out)].type;
            auto* link = new LinkItem(f->second, out, t->second, in, wireColor(type), backend->wireWidth(type));
            mScene.addItem(link);
            mLinks.push_back(link);
        }
        for (auto& [name, item] : mNodes)
            item->layoutRows();
        for (LinkItem* l : mLinks)
            l->update();
        mTies = tiesFrom(info["definition"]);
        for (int i = 0; i < int(mTies.size()); ++i)
        {
            auto* item = new TieItem(i);
            mScene.addItem(item);
            mTieItems.push_back(item);
        }
        mFrames = gn::framesFromJson(info["definition"].value("frames", json::array()));
        for (int i = 0; i < int(mFrames.size()); ++i)
        {
            auto* item = new FrameItem(i);
            mScene.addItem(item);
            mFrameItems.push_back(item);
            item->setSelected(std::ranges::find(framesSelected, i) != framesSelected.end());
        }
        layoutFrames();
        layoutTies();
        updateEmphasis();
    }
    mInfo = info;
    refreshThumbs();
    mBuilding = false;
    if (newGroup)
        for (QObject* child : children())
            if (auto* framer = dynamic_cast<NodeCanvasFramer*>(child))
                framer->request();
}

void NodeCanvas::refreshThumbs()
{
    QPointer<NodeCanvas> self(this);
    for (auto& [name, item] : mNodes)
    {
        if (item->thumbSize <= 0)
            continue;
        item->thumb = backend->thumbnail(mGroup, mInfo, name, [self, group = mGroup] {
            if (self && self->mGroup == group && !self->mThumbsQueued)
            {
                self->mThumbsQueued = true;
                QTimer::singleShot(30, self, [self] {
                    self->mThumbsQueued = false;
                    self->refreshThumbs();
                });
            }
        });
        item->update();
    }
}

WireDrag NodeCanvas::takeWire()
{
    WireDrag w = wireOf(mWireFrom, mWireSocket, mWireInput);
    w.detached = mDetached;
    mDetached.reset();
    mWireFrom = nullptr;
    removeTemp();
    return w;
}

QPointF NodeCanvas::cursorScenePos() const
{
    QPoint p = viewport()->mapFromGlobal(QCursor::pos());
    return mapToScene(viewport()->rect().contains(p) ? p : viewport()->rect().center());
}

std::vector<std::string> NodeCanvas::selectedNames() const
{
    std::vector<std::string> out;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i))
            out.push_back(n->name);
    return out;
}

std::vector<int> NodeCanvas::selectedFrames() const
{
    std::vector<int> out;
    for (FrameItem* f : mFrameItems)
        if (f->isSelected())
            out.push_back(f->index);
    return out;
}

void NodeCanvas::selectAll()
{
    for (auto& [n, item] : mNodes)
        item->setSelected(true);
    for (FrameItem* f : mFrameItems)
        f->setSelected(true);
}

void NodeCanvas::nodeMoved(NodeItem* n)
{
    if (mInfo.is_object() && !mInfo.value("zones", json::array()).empty())
        mScene.invalidate(QRectF(), QGraphicsScene::BackgroundLayer);
    if (!mTies.empty())
    {
        layoutTies();
        return;
    }
    for (LinkItem* l : mLinks)
        if (l->from == n || l->to == n)
            l->update();
}

void NodeCanvas::setZipKey(bool down)
{
    if (down == mZipKey)
        return;
    mZipKey = down;
    if (down)
        viewport()->setCursor(Qt::CrossCursor);
    else if (!mTieStroke)
        viewport()->unsetCursor();
}

bool NodeCanvas::jumpToPortal(bool source)
{
    NodeItem* at = nullptr;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i); n && n->portal)
            at = n;
    if (!at || at->portalName.empty())
        return false;
    std::vector<NodeItem*> hits;
    for (auto& [name, item] : mNodes)
        if (item->portal && item->portalName == at->portalName && (item->type == gn::kSendType) == source)
            hits.push_back(item);
    if (hits.empty())
        return false;
    mScene.clearSelection();
    QRectF r;
    for (NodeItem* n : hits)
    {
        n->setSelected(true);
        r = r.united(n->sceneBoundingRect());
    }
    centerOn(r.center());
    return true;
}

void NodeCanvas::setFlowHighlight(bool on, bool hold)
{
    (hold ? mFlowHold : mFlowToggle) = on;
    updateEmphasis();
}

void NodeCanvas::setSelectedWiresOnly(bool on)
{
    mSelectedWiresOnly = on;
    updateEmphasis();
}

void NodeCanvas::frameSelection()
{
    if (!backend->supports("SetFrames") || mGroup.empty())
        return;
    gn::GFrame f;
    f.label = "Frame";
    QRectF r;
    for (QGraphicsItem* i : mScene.selectedItems())
        if (auto* n = dynamic_cast<NodeItem*>(i))
        {
            r = r.united(n->sceneBoundingRect());
            f.nodes.push_back(n->name);
        }
    if (r.isNull())
        r = QRectF(cursorScenePos() - QPointF(150, 100), QSizeF(300, 200));
    else
        r.adjust(-30, -30 - FrameItem::kTitle, 30, 30);
    f.position = {r.x(), r.y()};
    f.size = {r.width(), r.height()};
    std::vector<gn::GFrame> next = mFrames;
    // Nodes going into the new frame leave the ones they were in.
    for (gn::GFrame& other : next)
        std::erase_if(other.nodes, [&](const std::string& m) { return std::find(f.nodes.begin(), f.nodes.end(), m) != f.nodes.end(); });
    next.push_back(std::move(f));
    commitFrames(next);
}

namespace
{
/** The selected nodes a flag applies to: not reroutes or portals, nothing that can't mute for "muted", only previewed ones for "hidePreview". */
std::vector<NodeItem*> flagTargets(std::vector<NodeItem*> nodes, std::string_view flag)
{
    std::erase_if(nodes, [&](NodeItem* n) {
        return n->reroute || n->portal || (flag == "muted" && (n->type == "GroupInput" || n->type == "GroupOutput" || n->type == "materialOutput")) ||
               (flag == "hidePreview" && n->previewSize <= 0);
    });
    return nodes;
}

bool nodeFlagOf(const NodeItem* n, std::string_view flag)
{
    if (flag == "muted")
        return n->muted;
    if (flag == "collapsed")
        return n->collapsed;
    if (flag == "hidePreview")
        return n->hidePreview;
    return n->hideUnused;
}
} // namespace

bool NodeCanvas::flagOn(const char* flag) const
{
    std::vector<NodeItem*> nodes = flagTargets(selectedNodes(), flag);
    return !nodes.empty() && std::all_of(nodes.begin(), nodes.end(), [&](NodeItem* n) { return nodeFlagOf(n, flag); });
}

void NodeCanvas::setPreviews(bool on)
{
    PrefStore::get().set("general", kPrefPreviews, on);
    reloadPrefs();
}

bool NodeCanvas::hasPreviews(bool selectedOnly) const
{
    for (auto& [name, n] : mNodes)
        if (n->previewSize > 0 && (!selectedOnly || n->isSelected()))
            return true;
    return false;
}

void NodeCanvas::toggleFlag(const char* flag)
{
    std::vector<NodeItem*> nodes = flagTargets(selectedNodes(), flag);
    if (nodes.empty() || !backend->supports("SetNodeFlags"))
        return;
    auto get = [flag](NodeItem* n) { return nodeFlagOf(n, flag); };
    bool on = std::any_of(nodes.begin(), nodes.end(), [&](NodeItem* n) { return !get(n); });
    json a = json::array({mGroup});
    for (NodeItem* n : nodes)
        a.push_back(n->name);
    runVerb(*backend, "SetNodeFlags", {{"args", a}, {flag, on}});
}

std::vector<std::pair<std::string, QString>> NodeCanvas::nodeList() const
{
    std::vector<std::pair<std::string, QString>> out;
    for (auto& [name, n] : mNodes)
        out.push_back({name, n->title()});
    return out;
}

void NodeCanvas::focusNode(const std::string& name)
{
    auto it = mNodes.find(name);
    if (it == mNodes.end())
        return;
    mScene.clearSelection();
    it->second->setSelected(true);
    centerOn(it->second->sceneBoundingRect().center());
}

void NodeCanvas::storeBookmark(int slot)
{
    if (mGroup.empty() || !backend->supports("SetBookmarks"))
        return;
    json marks = mInfo.is_object() ? mInfo["definition"].value("bookmarks", json::object()) : json::object();
    QPointF c = mapToScene(viewport()->rect().center());
    marks[std::to_string(slot)] = {c.x(), c.y(), transform().m11()};
    runVerb(*backend, "SetBookmarks", {{"args", {mGroup, marks}}});
}

bool NodeCanvas::recallBookmark(int slot)
{
    json marks = mInfo.is_object() ? mInfo["definition"].value("bookmarks", json::object()) : json::object();
    auto it = marks.find(std::to_string(slot));
    if (it == marks.end())
        return false;
    double zoom = std::clamp((*it)[2].get<double>(), 0.1, 4.0);
    resetTransform();
    scale(zoom, zoom);
    centerOn((*it)[0].get<double>(), (*it)[1].get<double>());
    return true;
}

void NodeCanvas::setMinimap(bool on)
{
    PrefStore::get().set("general", kPrefMinimap, on);
    mPrefs.minimap = on;
    viewport()->update();
}

QString NodeCanvas::mergeSelected(const std::string& op)
{
    std::vector<NodeItem*> nodes = selectedNodes();
    std::erase_if(nodes, [](NodeItem* n) { return n->outputs.empty(); });
    if (nodes.size() < 2)
        return "Select two or more nodes with outputs to merge.";
    std::sort(nodes.begin(), nodes.end(), [](NodeItem* a, NodeItem* b) { return a->pos().y() < b->pos().y(); });
    std::string type = nodes.front()->outputs.front().type;
    std::vector<std::string> sources;
    for (NodeItem* n : nodes)
    {
        int pick = -1;
        for (int pass = 0; pass < 2 && pick < 0; ++pass)
            for (int o = 0; o < int(n->outputs.size()) && pick < 0; ++o)
                if (pass == 0 ? n->outputs[size_t(o)].type == type : backend->canLink(n->outputs[size_t(o)].type, type))
                    pick = o;
        if (pick < 0)
            return qs(n->name + " has no output that merges with " + type);
        sources.push_back(n->name + "." + n->outputs[size_t(pick)].id);
    }
    std::optional<GraphBackend::MergeNode> plan = backend->mergeNode(type, op);
    if (!plan)
        return qs("Nothing here merges " + type + " values by " + op);
    double right = -1e18, y = 0.0;
    for (NodeItem* n : nodes)
    {
        right = std::max(right, n->sceneBoundingRect().right());
        y += n->pos().y();
    }
    y /= double(nodes.size());
    QPointF at(right + 80.0, y);
    UndoChunk chunk("Merge Nodes");
    auto addNode = [&](QPointF p) -> std::string {
        json r = runVerb(*backend, "AddNode", {{"args", {mGroup, plan->type}}, {"position", {p.x(), p.y()}}});
        if (!r.is_string())
            return {};
        std::string name = r.get<std::string>();
        for (auto& [k, v] : plan->values.items())
            runVerb(*backend, "SetValue", args({mGroup, name + "." + k, v}));
        return name;
    };
    if (plan->multi)
    {
        std::string m = addNode(at);
        for (const std::string& s : sources)
            runVerb(*backend, "Connect", args({mGroup, s, m + "." + plan->inA}));
        return {};
    }
    std::string prev = sources[0];
    for (size_t i = 1; i < sources.size(); ++i)
    {
        std::string m = addNode(at + QPointF(220.0 * double(i - 1), 0.0));
        if (m.empty())
            return "Couldn't add " + qs(plan->type);
        runVerb(*backend, "Connect", args({mGroup, prev, m + "." + plan->inA}));
        runVerb(*backend, "Connect", args({mGroup, sources[i], m + "." + plan->inB}));
        prev = m + "." + plan->out;
    }
    return {};
}

void NodeCanvas::reloadPrefs()
{
    mPrefs = readCanvasPrefs();
    gWireStyle = mPrefs.wires;
    for (auto& [name, item] : mNodes)
        if (item->previewSize > 0)
            item->layoutRows();
    for (LinkItem* l : mLinks)
    {
        l->base.setColor(wireColor(l->from->outputs[size_t(l->out)].type));
        l->setPen(l->base);
        l->highlight = false;
        l->update();
    }
    layoutTies();
    updateEmphasis();
    refreshThumbs();
    viewport()->update();
}

void NodeCanvas::frame(bool selectedOnly)
{
    QRectF r;
    for (auto& [name, item] : mNodes)
        if (!selectedOnly || item->isSelected())
            r = r.united(item->sceneBoundingRect());
    if (r.isEmpty())
    {
        if (selectedOnly)
            frame(false);
        return;
    }
    r.adjust(-60, -60, 60, 60);
    fitInView(r, Qt::KeepAspectRatio);
    // Don't zoom in past 1:1 for a small graph.
    if (transform().m11() > 1.0)
    {
        QPointF c = r.center();
        resetTransform();
        centerOn(c);
    }
}
} // namespace lila::ui
