#include "ui/NodeCanvas.h"
#include "ui/ColorPopup.h"
#include "ui/ScrubGesture.h"
#include "ui/ValueReset.h"

namespace lila::ui
{
namespace
{
/** The number box typed into over a field; Esc tells a cancel from a commit. */
class FieldLineEdit final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    bool cancelled = false;

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
        {
            cancelled = true;
            clearFocus();
            return;
        }
        QLineEdit::keyPressEvent(e);
    }
};

/** A title typed in place. finish runs once: kept on Enter or focus loss, cancelled on Esc. */
class InPlaceLine final : public QLineEdit
{
public:
    using QLineEdit::QLineEdit;
    std::function<void(bool keep)> finish;

    void end(bool keep)
    {
        if (auto f = std::exchange(finish, nullptr))
            f(keep);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
            return end(false);
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter)
            return end(true);
        QLineEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QLineEdit::focusOutEvent(e);
        end(true);
    }
};

/** A note's text typed in place: Enter starts a new line, Ctrl+Enter or focus loss keeps it, Esc cancels. */
class InPlaceText final : public QPlainTextEdit
{
public:
    using QPlainTextEdit::QPlainTextEdit;
    std::function<void(bool keep)> finish;

    void end(bool keep)
    {
        if (auto f = std::exchange(finish, nullptr))
            f(keep);
    }

protected:
    void keyPressEvent(QKeyEvent* e) override
    {
        if (e->key() == Qt::Key_Escape)
            return end(false);
        if ((e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) && (e->modifiers() & Qt::ControlModifier))
            return end(true);
        QPlainTextEdit::keyPressEvent(e);
    }
    void focusOutEvent(QFocusEvent* e) override
    {
        QPlainTextEdit::focusOutEvent(e);
        // The box's own context menu takes focus without ending the edit.
        if (e->reason() != Qt::PopupFocusReason)
            end(true);
    }
};

json fieldNumber(double v, bool integer) { return integer ? json(int64_t(std::llround(v))) : json(v); }

double clampFieldValue(double v, const json& limits)
{
    if (auto it = limits.find("min"); it != limits.end() && it->is_number())
        v = std::max(v, it->get<double>());
    if (auto it = limits.find("max"); it != limits.end() && it->is_number())
        v = std::min(v, it->get<double>());
    return v;
}

/** How far one pixel of travel moves a number: a bounded slider spans its field, else its step, else a small fixed amount. */
double fieldPerPixel(const json& limits, double fieldPx)
{
    if (auto range = fieldRange(limits))
        return (range->second - range->first) / std::max(20.0, fieldPx);
    // A step per kScrubPixelsPerStep pixels, as on every other number field.
    if (auto it = limits.find("step"); it != limits.end() && it->is_number() && it->get<double>() > 0.0)
        return it->get<double>() / kScrubPixelsPerStep;
    return 1.0 / kScrubPixelsPerStep;
}
} // namespace

void NodeCanvas::setFieldValue(const std::string& node, const std::string& socket, const json& value, bool live)
{
    json call = args({mGroup, node + "." + socket, value});
    if (live)
        setFromField(*backend, call);
    else
        runVerb(*backend, "SetValue", call);
}

void NodeCanvas::pressProp(NodeItem* n, int prop)
{
    if (!n->isSelected())
    {
        mScene.clearSelection();
        n->setSelected(true);
    }
    const std::string id = n->props[size_t(prop)].first, current = n->props[size_t(prop)].second;
    const std::string node = n->name;
    if (n->propIsToggle(size_t(prop)))
    {
        const bool on = current != "true";
        setFieldValue(node, id, n->boolParams.contains(id) ? json(on) : json(on ? "true" : "false"), false);
        return;
    }
    QMenu menu(this);
    for (const std::string& o : n->propOptions[size_t(prop)])
    {
        QAction* a = menu.addAction(pretty(o));
        a->setCheckable(true);
        a->setChecked(o == current);
        a->setData(qs(o));
    }
    const QRectF row = n->mapRectToScene(n->propRect(size_t(prop)));
    QAction* picked = menu.exec(viewport()->mapToGlobal(mapFromScene(row.bottomLeft())));
    if (picked && picked->data().toString().toStdString() != current)
        setFieldValue(node, id, picked->data().toString().toStdString(), false);
}

void NodeCanvas::pressField(NodeItem* n, int input, QMouseEvent* e)
{
    if (!n->isSelected())
    {
        mScene.clearSelection();
        n->setSelected(true);
    }
    const SocketInfo& s = n->inputs[size_t(input)];
    switch (n->fieldKind(input))
    {
    case FieldKind::Toggle:
    {
        const bool on = s.value.is_boolean() ? s.value.get<bool>() : s.value.get<double>() != 0.0;
        setFieldValue(n->name, s.id, s.value.is_boolean() ? json(!on) : json(on ? 0 : 1), false);
        return;
    }
    case FieldKind::Color:
        pickFieldColor(n, input, e->globalPosition().toPoint());
        return;
    case FieldKind::Number:
    {
        FieldDrag d;
        d.node = n->name;
        d.socket = s.id;
        d.integer = s.type == "int";
        d.limits = s.limits;
        d.value = d.sent = s.value.get<double>();
        d.press = d.last = e->pos();
        d.pxPerField = mapFromScene(n->mapRectToScene(n->fieldRect(input))).boundingRect().width();
        mField = d;
        return;
    }
    case FieldKind::None:
        return;
    }
}

void NodeCanvas::dragField(QMouseEvent* e)
{
    FieldDrag& d = *mField;
    if (!d.dragging)
    {
        if (std::abs(e->pos().x() - d.press.x()) < valueReset::kMaxTravel)
            return;
        d.dragging = true;
        d.last = d.press;
        UndoStack::get().openChunk("Drag " + pretty(d.socket).toStdString());
    }
    double mult = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : (e->modifiers() & Qt::ControlModifier) ? 0.1 : 1.0;
    double perPx = fieldPerPixel(d.limits, d.pxPerField) * mult;
    d.value = clampFieldValue(d.value + double(e->pos().x() - d.last.x()) * perPx, d.limits);
    d.last = e->pos();
    // Rounded to a tenth of a pixel's worth, so a float doesn't show its last bits.
    double precision = std::pow(10.0, std::floor(std::log10(std::max(perPx * 0.1, 1e-6))));
    double out = d.integer ? std::round(d.value) : std::round(d.value / precision) * precision;
    if (out != d.sent)
    {
        d.sent = out;
        setFieldValue(d.node, d.socket, fieldNumber(out, d.integer), true);
    }
    // Each edit rebuilds the items: light the field again on the new one.
    if (auto it = mNodes.find(d.node); it != mNodes.end())
        for (int i = 0; i < int(it->second->inputs.size()); ++i)
            if (it->second->inputs[size_t(i)].id == d.socket && it->second->hotField != i)
            {
                it->second->hotField = i;
                it->second->update();
                mHotFieldNode = it->second;
            }
}

void NodeCanvas::releaseField()
{
    std::optional<FieldDrag> d = std::exchange(mField, std::nullopt);
    if (!d)
        return;
    // A click without a drag types, as on every other number field.
    if (!d->dragging)
    {
        if (auto it = mNodes.find(d->node); it != mNodes.end())
            for (int i = 0; i < int(it->second->inputs.size()); ++i)
                if (it->second->inputs[size_t(i)].id == d->socket)
                    editFieldText(it->second, i);
        return;
    }
    UndoStack::get().closeChunk();
    if (onValuesEdited)
        onValuesEdited();
}

void NodeCanvas::editFieldText(NodeItem* n, int input)
{
    if (mFieldEdit)
        mFieldEdit->deleteLater();
    const SocketInfo& s = n->inputs[size_t(input)];
    QRect r = mapFromScene(n->mapRectToScene(n->fieldRect(input))).boundingRect();
    auto* edit = new FieldLineEdit(viewport());
    edit->setGeometry(r.adjusted(0, std::min(0, (r.height() - 22) / 2), 0, std::max(0, (22 - r.height()) / 2)));
    edit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    edit->setText(valueText(s.value, s.type));
    edit->selectAll();
    edit->show();
    edit->setFocus();
    mFieldEdit = edit;
    const bool integer = s.type == "int";
    const double old = s.value.get<double>();
    connect(edit, &QLineEdit::editingFinished, this, [this, edit, integer, old, limits = s.limits, node = n->name, socket = s.id] {
        if (edit->property("done").toBool())
            return;
        edit->setProperty("done", true);
        bool ok = false;
        double v = edit->text().trimmed().toDouble(&ok);
        if (!edit->cancelled && ok)
        {
            v = clampFieldValue(integer ? std::round(v) : v, limits);
            if (v != old)
                setFieldValue(node, socket, fieldNumber(v, integer), false);
        }
        edit->deleteLater();
        setFocus();
    });
}

void NodeCanvas::editTitleInPlace(QRectF r, const QString& text, std::function<void(const QString&)> done)
{
    if (mInPlace)
        mInPlace->deleteLater();
    QRect v = mapFromScene(r).boundingRect();
    auto* edit = new InPlaceLine(viewport());
    edit->setGeometry(v.adjusted(0, std::min(0, (v.height() - 22) / 2), 0, std::max(0, (22 - v.height()) / 2)));
    edit->setText(text);
    edit->selectAll();
    mInPlace = edit;
    edit->finish = [this, edit, done = std::move(done)](bool keep) {
        const QString typed = edit->text();
        edit->hide();
        edit->deleteLater();
        setFocus();
        if (keep)
            done(typed);
    };
    edit->show();
    edit->setFocus();
}

void NodeCanvas::editTextInPlace(QRectF r, const QString& text, std::function<void(const QString&)> done)
{
    if (mInPlace)
        mInPlace->deleteLater();
    QRect v = mapFromScene(r).boundingRect();
    auto* edit = new InPlaceText(viewport());
    edit->setGeometry(v.x(), v.y(), std::max(v.width(), 200), std::max(v.height(), 80));
    edit->setPlaceholderText("Note text. Ctrl+Enter to finish");
    edit->setPlainText(text);
    edit->selectAll();
    mInPlace = edit;
    edit->finish = [this, edit, done = std::move(done)](bool keep) {
        const QString typed = edit->toPlainText();
        edit->hide();
        edit->deleteLater();
        setFocus();
        if (keep)
            done(typed);
    };
    edit->show();
    edit->setFocus();
}

std::string NodeCanvas::renameTarget() const
{
    std::vector<NodeItem*> selected = selectedNodes();
    if (selected.size() == 1)
        return selected.front()->name;
    if (mHoverNode)
        return mHoverNode->name;
    return {};
}

bool NodeCanvas::renameNode(const std::string& name)
{
    auto it = mNodes.find(name);
    if (it == mNodes.end() || it->second->reroute || !canRenameNodes() || mGroup.empty())
        return false;
    NodeItem* n = it->second;
    // A label equal to the type's is no label, so the header follows the type's own.
    const QString typeLabel = n->typeLabel, shown = n->label;
    editTitleInPlace(n->mapRectToScene(n->headerRect()), shown, [this, name, typeLabel, shown](const QString& typed) {
        const QString text = typed.simplified();
        if (text == shown)
            return;
        if (backend->renamesNodes())
        {
            if (text.isEmpty())
                return;
            json r = runVerb(*backend, "SetNodeLabel", args({mGroup, name, text.toStdString()}));
            if (r.is_string() && onRenamed)
                onRenamed(r.get<std::string>());
            return;
        }
        runVerb(*backend, "SetNodeLabel", args({mGroup, name, text == typeLabel ? std::string() : text.toStdString()}));
    });
    return true;
}

std::optional<NodeCanvas::InterfaceSocket> NodeCanvas::interfaceSocketAt(QPointF scenePos) const
{
    if (mGroup.empty() || !backend->supports("RenameInput"))
        return std::nullopt;
    NodeItem* n = nodeAt(scenePos);
    if (!n || (n->type != "GroupInput" && n->type != "GroupOutput"))
        return std::nullopt;
    const bool groupOutput = n->type == "GroupOutput";
    const std::vector<int>& shown = groupOutput ? n->shownIn : n->shownOut;
    const std::vector<SocketInfo>& sockets = groupOutput ? n->inputs : n->outputs;
    for (int i : shown)
    {
        const QPointF at = n->mapFromScene(n->socketPos(groupOutput, i));
        if (std::abs(n->mapFromScene(scenePos).y() - at.y()) <= kRow * 0.5)
            return InterfaceSocket{n->name, sockets[size_t(i)].id, i, int(sockets.size()), groupOutput};
    }
    return std::nullopt;
}

void NodeCanvas::renameInterfaceSocket(const InterfaceSocket& s)
{
    auto it = mNodes.find(s.node);
    if (it == mNodes.end())
        return;
    NodeItem* n = it->second;
    const double y = n->mapFromScene(n->socketPos(s.groupOutput, s.index)).y();
    const QString shown = pretty(s.id);
    editTitleInPlace(n->mapRectToScene(QRectF(0.0, y - kRow * 0.5, kNodeWidth, kRow)), shown, [this, s, shown](const QString& typed) {
        const QString text = typed.simplified();
        const std::string id = identifierFrom(text);
        if (text.isEmpty() || text == shown || id.empty())
            return;
        UndoChunk chunk("Rename " + text.toStdString());
        if (!s.groupOutput && text != pretty(id))
            runVerb(*backend, "SetInputInfo", {{"args", {mGroup, s.id}}, {"label", text.toStdString()}});
        if (id != s.id)
            runVerb(*backend, "RenameInput", {{"args", {mGroup, s.id, id}}, {"output", s.groupOutput}});
    });
}

void NodeCanvas::moveInterfaceSocket(const InterfaceSocket& s, int to)
{
    if (to >= 0 && to < s.count && to != s.index)
        runVerb(*backend, "MoveInput", {{"args", {mGroup, s.id, to}}, {"output", s.groupOutput}});
}

void NodeCanvas::pickFieldColor(NodeItem* n, int input, QPoint global)
{
    const SocketInfo& s = n->inputs[size_t(input)];
    const bool alpha = s.value.size() == 4;
    colorPopup::Options o;
    o.value.color = {s.value[0].get<double>(), s.value[1].get<double>(), s.value[2].get<double>()};
    o.value.alpha = alpha && s.value[3].is_number() ? s.value[3].get<double>() : 1.0;
    o.alpha = alpha;
    o.title = pretty(s.id);
    auto toJson = [alpha](const colorPopup::Value& v) {
        json a = json::array({v.color.x, v.color.y, v.color.z});
        if (alpha)
            a.push_back(v.alpha);
        return a;
    };
    QPointer<NodeCanvas> self(this);
    // The picker reports every change live; they land as one undo step, or none when it is cancelled.
    UndoStack::get().openChunk("Set " + pretty(s.id).toStdString());
    o.changed = [self, toJson, node = n->name, socket = s.id](const colorPopup::Value& v) {
        if (self)
            self->setFieldValue(node, socket, toJson(v), true);
    };
    o.closed = [self](const colorPopup::Value&, bool accepted) {
        if (accepted)
            UndoStack::get().closeChunk();
        else
            UndoStack::get().cancelChunk();
        if (self && self->onValuesEdited)
            self->onValuesEdited();
    };
    colorPopup::show(global, std::move(o));
}

void NodeCanvas::hoverField(QPointF sp)
{
    NodeItem* n = nodeAt(sp);
    bool input = false;
    int f = n && n->socketAt(sp, input) < 0 ? n->fieldAt(sp) : -1;
    int pr = n && f < 0 ? n->propAt(sp) : -1;
    if (mHotPropNode && (mHotPropNode != n || mHotPropNode->hotProp != pr))
    {
        mHotPropNode->hotProp = -1;
        mHotPropNode->update();
        mHotPropNode = nullptr;
    }
    if (n && pr >= 0)
    {
        if (n->hotProp != pr)
        {
            n->hotProp = pr;
            n->update();
        }
        mHotPropNode = n;
    }
    if (mHotFieldNode && (mHotFieldNode != n || mHotFieldNode->hotField != f))
    {
        mHotFieldNode->hotField = -1;
        mHotFieldNode->update();
        mHotFieldNode = nullptr;
    }
    if (n && f >= 0)
    {
        if (n->hotField != f)
        {
            n->hotField = f;
            n->update();
        }
        mHotFieldNode = n;
    }
    if (mZipKey)
        return;
    FieldKind kind = f >= 0 ? n->fieldKind(f) : pr >= 0 ? FieldKind::Toggle : FieldKind::None;
    if (kind != FieldKind::None)
    {
        viewport()->setCursor(kind == FieldKind::Number ? Qt::SizeHorCursor : Qt::PointingHandCursor);
        mFieldCursor = true;
    }
    else if (std::exchange(mFieldCursor, false))
        viewport()->unsetCursor();
}
} // namespace lila::ui
