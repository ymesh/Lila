#include "ui/NodeEditor.h"

#include "ui/ColorPopup.h"
#include "ui/Controls.h"
#include "ui/NodeCanvas.h"
#include "ui/Hotkeys.h"
#include "ui/PreferencesDialog.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "gn/Group.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "ui/Actions.h"
#include "ui/Deletion.h"
#include "ui/AnimatedSend.h"
#include "ui/CodeEdit.h"
#include "ui/DockSidebar.h"
#include "ui/Icons.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommon.h"
#include "ui/ValueReset.h"
#include "ui/FileDialog.h"
#include "ui/FontPicker.h"
#include "ui/MultilineEdit.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGraphicsItem>
#include <QGraphicsPathItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QScrollBar>
#include <QSpinBox>
#include <QSplitter>
#include <QStyleOptionGraphicsItem>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>

#include <array>
#include <cctype>
#include <cmath>
#include <utility>
#include <map>
#include <memory>
#include <optional>
#include <set>

namespace lila::ui
{
using json = nlohmann::json;

namespace
{
std::function<void()>& showHook()
{
    static std::function<void()> hook;
    return hook;
}

std::vector<NodeEditor*>& editors()
{
    static std::vector<NodeEditor*> list;
    return list;
}

std::map<int, std::function<void(NodeEditor&)>>& groupListeners()
{
    static std::map<int, std::function<void(NodeEditor&)>> list;
    return list;
}

NodeEditor*& lastFocused()
{
    static NodeEditor* e = nullptr;
    return e;
}

/** Search-as-you-type node picker. Enter or a click adds the node. */
class AddNodePopup final : public QFrame
{
public:
    /** types as the backend's NodeTypes returns them; pick gets the type and, for a filtered list, the socket to link. */
    AddNodePopup(QWidget* parent, const json& types, std::function<void(const std::string&, const std::string&)> pick)
        : QFrame(parent, Qt::Popup), mPick(std::move(pick))
    {
        setFrameShape(QFrame::StyledPanel);
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(4, 4, 4, 4);
        mSearch = new QLineEdit(this);
        mSearch->setPlaceholderText("Search nodes");
        mList = new QListWidget(this);
        lay->addWidget(mSearch);
        lay->addWidget(mList);
        resize(300, 380);
        for (const json& t : types)
            mAll.push_back({t.value("type", ""), qs(t.value("category", "") + " / " + t.value("label", "")), qs(t.value("doc", "")),
                            t.value("socket", "")});
        std::sort(mAll.begin(), mAll.end(), [](const Entry& a, const Entry& b) { return a.text < b.text; });
        filter();
        connect(mSearch, &QLineEdit::textChanged, this, [this] { filter(); });
        connect(mSearch, &QLineEdit::returnPressed, this, [this] { choose(mList->currentItem()); });
        connect(mList, &QListWidget::itemActivated, this, [this](QListWidgetItem* i) { choose(i); });
        connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* i) { choose(i); });
        mSearch->installEventFilter(this);
    }

    std::function<void(bool picked)> onClosed;

    /** Opens with its search field under the cursor, kept on screen. */
    void popup(QPoint global)
    {
        QPoint at = global - QPoint(20, 14);
        if (QScreen* s = QGuiApplication::screenAt(global))
        {
            QRect r = s->availableGeometry();
            at.setX(std::clamp(at.x(), r.left(), r.right() - width()));
            at.setY(std::clamp(at.y(), r.top(), r.bottom() - height()));
        }
        move(at);
        show();
        mSearch->setFocus();
    }

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (o == mSearch && e->type() == QEvent::KeyPress)
        {
            auto* k = static_cast<QKeyEvent*>(e);
            if (k->key() == Qt::Key_Down || k->key() == Qt::Key_Up)
            {
                int row = mList->currentRow() + (k->key() == Qt::Key_Down ? 1 : -1);
                mList->setCurrentRow(std::clamp(row, 0, mList->count() - 1));
                return true;
            }
        }
        return QFrame::eventFilter(o, e);
    }

    void hideEvent(QHideEvent* e) override
    {
        QFrame::hideEvent(e);
        if (mPicked)
            return;
        if (onClosed)
            onClosed(false);
        deleteLater();
    }

private:
    struct Entry
    {
        std::string type;
        QString text;
        QString doc;
        std::string socket;
    };

    void filter()
    {
        mList->clear();
        QString q = mSearch->text().trimmed();
        for (const Entry& e : mAll)
            if (q.isEmpty() || e.text.contains(q, Qt::CaseInsensitive) || qs(e.type).contains(q, Qt::CaseInsensitive))
            {
                auto* item = new QListWidgetItem(e.socket.empty() ? e.text : e.text + "  (" + pretty(e.socket) + ")", mList);
                item->setData(Qt::UserRole, qs(e.type));
                item->setData(Qt::UserRole + 1, qs(e.socket));
                item->setToolTip(e.doc);
            }
        mList->setCurrentRow(0);
    }

    void choose(QListWidgetItem* i)
    {
        if (!i)
            return;
        std::string type = i->data(Qt::UserRole).toString().toStdString();
        std::string socket = i->data(Qt::UserRole + 1).toString().toStdString();
        mPicked = true;
        close();
        mPick(type, socket);
        deleteLater();
    }

    std::function<void(const std::string&, const std::string&)> mPick;
    bool mPicked = false;
    std::vector<Entry> mAll;
    QLineEdit* mSearch;
    QListWidget* mList;
};

QDoubleSpinBox* numberBox(QWidget* parent, double v, bool integer)
{
    auto* s = new QDoubleSpinBox(parent);
    s->setRange(-1e9, 1e9);
    s->setDecimals(integer ? 0 : 3);
    s->setSingleStep(integer ? 1.0 : 0.1);
    s->setValue(v);
    s->setKeyboardTracking(false);
    s->setButtonSymbols(QAbstractSpinBox::NoButtons);
    return s;
}

/** A swatch button for a linear RGBA value; the swatch shows the colour itself, so it is data, not chrome. */
QWidget* colorEditor(QWidget* parent, const json& value, std::function<void(json)> set)
{
    auto* b = new QToolButton(parent);
    b->setMinimumWidth(60);
    auto rgba = std::make_shared<std::array<double, 4>>(std::array<double, 4>{0.0, 0.0, 0.0, 1.0});
    if (value.is_array() && value.size() >= 3)
        for (size_t k = 0; k < std::min<size_t>(4, value.size()); ++k)
            (*rgba)[k] = value[k].is_number() ? value[k].get<double>() : 0.0;
    else if (value.is_number())
        (*rgba)[0] = (*rgba)[1] = (*rgba)[2] = value.get<double>();
    auto* bright = new colorPopup::ValueSlider(parent);
    auto paint = [b, rgba, bright] {
        bright->setColor({(*rgba)[0], (*rgba)[1], (*rgba)[2]});
        QColor c = QColor::fromRgbF(float(linearToSrgb((*rgba)[0])), float(linearToSrgb((*rgba)[1])), float(linearToSrgb((*rgba)[2])));
        b->setStyleSheet(QString("QToolButton { background: %1; border: 1px solid rgb(20,20,20); min-height: 16px; }").arg(c.name()));
        b->setToolTip(QString("Linear %1, %2, %3, alpha %4").arg((*rgba)[0], 0, 'f', 3).arg((*rgba)[1], 0, 'f', 3).arg((*rgba)[2], 0, 'f', 3).arg((*rgba)[3], 0, 'f', 3));
    };
    paint();
    QObject::connect(b, &QToolButton::clicked, b, [b, rgba, paint, set] {
        QColor init = QColor::fromRgbF(float(linearToSrgb((*rgba)[0])), float(linearToSrgb((*rgba)[1])), float(linearToSrgb((*rgba)[2])), float((*rgba)[3]));
        QColor c = colorPopup::getColor(init, b, "Color", true);
        if (!c.isValid())
            return;
        *rgba = {srgbToLinear(double(c.redF())), srgbToLinear(double(c.greenF())), srgbToLinear(double(c.blueF())), c.alphaF()};
        paint();
        set(json::array({(*rgba)[0], (*rgba)[1], (*rgba)[2], (*rgba)[3]}));
    });
    bright->dragged = [rgba, paint](const glm::dvec3& c) {
        *rgba = {c.x, c.y, c.z, (*rgba)[3]};
        paint();
    };
    bright->released = [set, rgba](const glm::dvec3& start, const glm::dvec3& c) {
        if (c != start)
            set(json::array({(*rgba)[0], (*rgba)[1], (*rgba)[2], (*rgba)[3]}));
    };
    return colorPopup::withEyedropper(b, [rgba, paint, set](const glm::dvec3& c) {
        *rgba = {c.x, c.y, c.z, (*rgba)[3]};
        paint();
        set(json::array({(*rgba)[0], (*rgba)[1], (*rgba)[2], (*rgba)[3]}));
    }, bright);
}

/** A string's editor named by a decl's editor ("multiline", "code", "font"); null for a one-line box. */
QWidget* textEditor(QWidget* parent, const std::string& editor, const std::string& value, std::function<void(std::string)> set)
{
    if (editor == "code")
    {
        auto* box = new CodeEdit(parent);
        box->showText(value);
        QObject::connect(box, &CodeEdit::applied, box, [set](const QString& t) { set(t.toStdString()); });
        return box;
    }
    if (editor == "multiline")
    {
        auto* box = new MultilineEdit(parent);
        box->showText(value);
        QObject::connect(box, &MultilineEdit::committed, box, [set](const QString& t) { set(t.toStdString()); });
        return box;
    }
    if (editor == "font")
    {
        auto* picker = new FontPicker(parent);
        picker->setValue(value);
        QObject::connect(picker, &FontPicker::chosen, picker, [set](const QString& f) { set(f.toStdString()); });
        return picker;
    }
    return nullptr;
}

/** Editor for one socket value; calls set with the new JSON value. A non-null def is restored by a middle click. */
QWidget* valueEditor(QWidget* parent, const std::string& type, const json& value, std::function<void(json)> set, const json& def = json(),
                     const std::string& editor = {})
{
    if (type == "color")
        return colorEditor(parent, value, std::move(set));
    if (type == "bool")
    {
        auto* c = new QCheckBox(parent);
        c->setChecked(value.is_boolean() ? value.get<bool>() : value.is_number() && value.get<double>() != 0.0);
        if (def.is_boolean() || def.is_number())
            setCheckDefault(c, def.is_boolean() ? def.get<bool>() : def.get<double>() != 0.0);
        QObject::connect(c, &QCheckBox::toggled, c, [set](bool on) { set(on); });
        return c;
    }
    if (type == "string")
    {
        if (QWidget* w = textEditor(parent, editor, value.is_string() ? value.get<std::string>() : std::string(), [set](std::string s) { set(std::move(s)); }))
            return w;
        auto* e = new QLineEdit(value.is_string() ? qs(value.get<std::string>()) : QString(), parent);
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, set] { set(e->text().toStdString()); });
        return e;
    }
    if (type == "vector" || type == "rotation")
    {
        auto* w = new QWidget(parent);
        auto* h = new QHBoxLayout(w);
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(2);
        std::array<QDoubleSpinBox*, 3> boxes;
        for (int k = 0; k < 3; ++k)
        {
            double v = value.is_array() && value.size() == 3 ? value[k].get<double>() : value.is_number() ? value.get<double>() : 0.0;
            boxes[k] = numberBox(w, v, false);
            if (def.is_array() && def.size() == 3 && def[k].is_number())
                setSpinDefault(boxes[k], def[k].get<double>());
            else if (def.is_number())
                setSpinDefault(boxes[k], def.get<double>());
            h->addWidget(boxes[k]);
        }
        for (int k = 0; k < 3; ++k)
            QObject::connect(boxes[k], &QDoubleSpinBox::valueChanged, w, [boxes, set] {
                set(json::array({boxes[0]->value(), boxes[1]->value(), boxes[2]->value()}));
            });
        return w;
    }
    bool integer = type == "int";
    auto* s = numberBox(parent, value.is_number() ? value.get<double>() : 0.0, integer);
    if (def.is_number())
        setSpinDefault(s, def.get<double>());
    QObject::connect(s, &QDoubleSpinBox::valueChanged, s, [set, integer](double v) { set(integer ? json(int64_t(std::llround(v))) : json(v)); });
    return s;
}

/** The soft range, or the hard one, of an exposed input; 0 when it has neither end. */
double rangeSpan(const json& in)
{
    auto end = [&](const char* soft, const char* hard) -> std::optional<double> {
        if (in.contains(soft) && in[soft].is_number())
            return in[soft].get<double>();
        if (in.contains(hard) && in[hard].is_number())
            return in[hard].get<double>();
        return std::nullopt;
    };
    std::optional<double> lo = end("softMin", "min"), hi = end("softMax", "max");
    if (!hi)
        return 0.0;
    return *hi - lo.value_or(0.0);
}

/** Rounds up to 1, 2 or 5 times a power of ten. */
double niceStep(double raw)
{
    double p = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0})
        if (raw <= m * p)
            return m * p;
    return 10.0 * p;
}

/** Bounds every spin box under w by an exposed input's min, max and step; with no step, a scrub across ~1200 px covers the range. */
void applyLimits(QWidget* w, const json& in)
{
    QList<QDoubleSpinBox*> boxes = w->findChildren<QDoubleSpinBox*>();
    if (auto* self = qobject_cast<QDoubleSpinBox*>(w))
        boxes.push_back(self);
    for (QDoubleSpinBox* b : boxes)
    {
        QSignalBlocker block(b);
        double v = b->value();
        b->setRange(in.contains("min") ? in["min"].get<double>() : -1e9, in.contains("max") ? in["max"].get<double>() : 1e9);
        if (in.contains("step"))
            b->setSingleStep(in["step"].get<double>());
        else if (double span = rangeSpan(in); span > 0.0 && b->decimals() > 0)
            b->setSingleStep(niceStep(span / 200.0));
        b->setValue(v);
    }
}

QLabel* heading(QWidget* parent, const QString& text)
{
    auto* l = new QLabel(text, parent);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setStyleSheet("color: rgb(225,225,225); padding-top: 6px;");
    return l;
}
} // namespace

NodeEditor::NodeEditor(QWidget* parent, const std::string& backend) : QWidget(parent)
{
    registerGnGraphBackend();
    registerSceneGraphBackend();
    mBackend = graphBackends::find(backend);
    if (!mBackend)
        mBackend = graphBackends::find(kGeometryNodesBackend);

    auto* lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    auto* split = new QSplitter(Qt::Horizontal, this);
    mCanvas = new NodeCanvas(split);
    mCanvas->backend = mBackend.get();
    mSide = new QScrollArea(split);
    mSide->setWidgetResizable(true);
    mSide->setMinimumWidth(230);
    split->addWidget(mCanvas);
    split->addWidget(mSide);
    split->setStretchFactor(0, 1);
    split->setSizes({700, 260});
    lay->addWidget(split, 1);
    buildToolbar();

    // Sends stored after the command returns, so the label is read on the next turn of the event loop.
    mExecuteListener = ops::addExecuteListener([this](const ExecuteEvent& e) {
        if (e.done && (e.name == "unrealMap" || e.name.starts_with("export")))
            QTimer::singleShot(0, this, [this] { refreshSendButton(); });
        if (e.done && (e.name == "undo" || e.name == "redo") && mBackend->rebuildsOnUndo())
            scheduleRefresh();
    });

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(0);
    connect(mRefresh, &QTimer::timeout, this, [this] { refresh(std::exchange(mPanelDirty, false)); });

    setProperty("hotkeyContext", "nodeEditor");
    deletion::claim(this, [this] { deleteSelected(); });
    mCanvas->onAddMenu = [this](QPointF scenePos) { openAddMenu(scenePos, nullptr); };
    mCanvas->onWireDropped = [this](QPointF scenePos, const WireDrag& w) { openAddMenu(scenePos, &w); };
    mCanvas->onFocused = [this] { lastFocused() = this; };
    mCanvas->onOpenGroup = [this](const std::string& g) {
        if (mBackend->hasGroup(scene::Graph::current(), g))
            setGroup(qs(g));
        else
            showError("The group " + qs(g) + " isn't in this scene (only its embedded copy is).");
    };
    mCanvas->onViewNode = [this](const std::string& n) { viewNode(n); };
    mCanvas->onContextMenu = [this](QPoint global) { showCanvasMenu(global); };
    mCanvas->onFilesDropped = [this](const QStringList& files) { importGroups(files); };
    mCanvas->onSelect = [this](const std::string& n) {
        mSelectedNode = qs(n);
        rebuildSidePanel();
    };

    mListener = scene::Graph::current().addListener([this](scene::Change c, scene::NodeId id) {
        scene::Graph& g = scene::Graph::current();
        if (c == scene::Change::Selection)
        {
            QTimer::singleShot(0, this, [this] {
                followSelection();
                refreshSendButton();
            });
            if (mBackend->rebuildsOn(c, nullptr))
                scheduleRefresh();
            return;
        }
        if (c == scene::Change::Reset)
        {
            // A command's batched changes arrive as one Reset, a selection among them.
            QTimer::singleShot(0, this, [this] { followSelection(); });
            scheduleRefresh(gFieldEdits == 0);
            return;
        }
        if (c == scene::Change::Cleared || c == scene::Change::Created || c == scene::Change::Deleted || c == scene::Change::Renamed)
        {
            scheduleRefresh();
            return;
        }
        if (c == scene::Change::Value || c == scene::Change::Connection || c == scene::Change::Attrs || c == scene::Change::Reparented)
            if (mBackend->rebuildsOn(c, g.node(id)))
            {
                scheduleRefresh(gFieldEdits == 0);
                return;
            }
        if (c == scene::Change::Value)
            if (scene::Node* n = g.node(id))
            {
                if (mBackend->isGroupNode(*n))
                    scheduleRefresh(gFieldEdits == 0);
                // Evaluation writes the owner's outputs too; only a change to a shown value rebuilds the panel.
                else if (gFieldEdits == 0 && qs(n->path()) == mOwner && !mValueCheck)
                {
                    mValueCheck = true;
                    QTimer::singleShot(0, this, [this] {
                        mValueCheck = false;
                        if (shownValues() != mShownValues)
                            scheduleRefresh();
                    });
                }
            }
    });
    // The toolbar holds the backend's own buttons, so it goes synchronously, before an extension's code unloads.
    mBackendListener = graphBackends::registry().addListener([this](RegistryChange change, const std::string& id, const std::string&) {
        if (change == RegistryChange::Removed && mBackend && mBackend->id() == id)
        {
            setBackend(kGeometryNodesBackend);
            return;
        }
        buildToolbar();
    });
    mPrefsListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            mCanvas->reloadPrefs();
    });
    editors().push_back(this);
    followSelection();
    refresh();
}

void NodeEditor::buildToolbar()
{
    delete mBar;
    mBar = new QWidget(this);
    auto* bar = new QHBoxLayout(mBar);
    bar->setContentsMargins(6, 4, 6, 4);
    bar->setSpacing(2);
    const GraphBackend& b = *mBackend;
    auto pushButton = [this](const QString& text, const QString& tip) {
        auto* btn = new QToolButton(mBar);
        btn->setText(text);
        btn->setToolTip(tip);
        controls::setRole(btn, "push");
        return btn;
    };

    std::vector<std::string> backendIds = graphBackends::ids();
    if (backendIds.size() > 1)
    {
        auto* picker = new QComboBox(mBar);
        for (const std::string& id : backendIds)
            if (GraphBackendPtr other = graphBackends::find(id))
                picker->addItem(other->title(), qs(id));
        picker->setCurrentIndex(std::max(0, picker->findData(qs(b.id()))));
        picker->setToolTip("Graph Type\nWhat kind of graph this editor shows");
        connect(picker, &QComboBox::activated, this, [this, picker](int i) {
            std::string id = picker->itemData(i).toString().toStdString();
            QTimer::singleShot(0, this, [this, id] { setBackend(id); });
        });
        bar->addWidget(picker);
    }

    mGroups = new QComboBox(mBar);
    mGroups->setMinimumWidth(180);
    auto* groupLabel = new QLabel(b.groupNoun(), mBar);
    groupLabel->setForegroundRole(QPalette::PlaceholderText);
    bar->addWidget(groupLabel);
    bar->addWidget(mGroups);
    connect(mGroups, &QComboBox::currentTextChanged, this, [this](const QString& t) {
        if (!mRefreshing && t != mGroup)
            setGroup(t);
    });
    if (std::string action = b.newGroupAction(); !action.empty())
    {
        QToolButton* newBtn = pushButton("New", QString::fromStdString(actions::tooltip(action)));
        connect(newBtn, &QToolButton::clicked, this, [action] { actions::run(json(action)); });
        bar->addWidget(newBtn);
    }
    bar->addSpacing(10);
    QToolButton* addBtn = pushButton("Add Node", "Add Node  (Tab, Shift+A)\nOver the canvas, the menu opens at the cursor");
    connect(addBtn, &QToolButton::clicked, this, [this] { openAddMenu(mCanvas->mapToScene(mCanvas->viewport()->rect().center()), nullptr); });
    bar->addWidget(addBtn);
    b.addToolbarButtons(bar, *this);
    if (b.supports("SaveGroup"))
    {
        QToolButton* saveBtn = pushButton("Save to Library", "Save to Library\nKeep this group in your library for every scene");
        connect(saveBtn, &QToolButton::clicked, this, [this] {
            if (mGroup.isEmpty())
                return;
            json p = run("SaveGroup", args({mGroup.toStdString()}));
            if (p.is_string())
                log::info("saved {}", p.get<std::string>());
        });
        bar->addWidget(saveBtn);
    }
    bool files = !b.fileSuffixes().empty();
    if (files && (b.supports("ExportGroup") || b.supports("ImportGroup")))
    {
        auto* more = new QToolButton(mBar);
        more->setIcon(icon("dots"));
        more->setToolTip("More\nExport and import node groups");
        more->setPopupMode(QToolButton::InstantPopup);
        controls::setRole(more, "overflow");
        auto* menu = new QMenu(more);
        menu->setToolTipsVisible(true);
        const QString suffix = qs(b.fileSuffixes().front());
        if (b.supports("ExportGroup"))
            menu->addAction(icon("file-export"), "Export Group...", this, [this] { exportGroup(); })
                ->setToolTip("Export this group (and the groups nested in it) to a ." + suffix + " file to share");
        if (b.supports("ImportGroup"))
            menu->addAction(icon("file-import"), "Import Groups...", this, [this] {
                    QStringList patterns;
                    for (const std::string& ext : mBackend->fileSuffixes())
                        patterns << "*." + qs(ext);
                    QStringList files = ui::fileDialog::getOpenFileNames(this, "Import Node Groups", QString(), "Node groups (" + patterns.join(' ') + ");;All files (*)");
                    if (!files.isEmpty())
                        importGroups(files);
                })
                ->setToolTip("Import node groups from a ." + suffix + " file; you can also drop the file on the canvas");
        more->setMenu(menu);
        bar->addWidget(more);
    }
    bar->addStretch(1);
    mSendBtn = nullptr;
    if (b.canSend())
    {
        mSendBtn = new QToolButton(mBar);
        mSendBtn->setIcon(icon("send"));
        mSendBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        mSendBtn->setPopupMode(QToolButton::MenuButtonPopup);
        controls::setRole(mSendBtn, "push");
        auto* sendMenu = new QMenu(mSendBtn);
        animatedSend::fillMenu(sendMenu, this, [this] { return sendTargets(); });
        mSendBtn->setMenu(sendMenu);
        connect(sendMenu, &QMenu::aboutToShow, this, [this] { refreshSendButton(); });
        connect(mSendBtn, &QToolButton::clicked, this, [this] {
            animatedSend::sendAgain(sendTargets());
            refreshSendButton();
        });
        bar->addWidget(mSendBtn);
    }
    static_cast<QVBoxLayout*>(layout())->insertWidget(0, mBar);
    refreshGroups();
    refreshSendButton();
}

bool NodeEditor::setBackend(const std::string& id)
{
    GraphBackendPtr next = graphBackends::find(id);
    if (!next)
        return false;
    if (next == mBackend)
        return true;
    mBackend = std::move(next);
    mCanvas->backend = mBackend.get();
    mGroup.clear();
    mOwner.clear();
    mSelectedNode.clear();
    buildToolbar();
    followSelection();
    refresh();
    return true;
}

NodeEditor::~NodeEditor()
{
    // The canvas outlives this body; its scene emits selectionChanged as it clears on destruction.
    mCanvas->onSelect = nullptr;
    mCanvas->onFocused = nullptr;
    scene::Graph::current().removeListener(mListener);
    graphBackends::registry().removeListener(mBackendListener);
    PrefStore::get().removeOnChanged(mPrefsListener);
    ops::removeExecuteListener(mExecuteListener);
    std::erase(editors(), this);
    if (lastFocused() == this)
        lastFocused() = nullptr;
}

json NodeEditor::run(std::string_view verb, const json& call) const { return runVerb(*mBackend, verb, call); }

void NodeEditor::setGroup(const QString& name)
{
    mGroup = name;
    mSelectedNode.clear();
    refresh();
}

void NodeEditor::showNode(const QString& group, const std::string& node)
{
    mGroup = group;
    mSelectNext = {node};
    mSelectedNode = qs(node);
    refresh();
    frameSelected();
}

void NodeEditor::frameAll() { mCanvas->frame(false); }

const std::vector<NodeEditor*>& NodeEditor::all() { return editors(); }

int NodeEditor::addGroupListener(std::function<void(NodeEditor&)> fn)
{
    static int next = 1;
    groupListeners()[next] = std::move(fn);
    return next++;
}

void NodeEditor::removeGroupListener(int id) { groupListeners().erase(id); }

void NodeEditor::frameSelected() { mCanvas->frame(true); }

NodeEditor* NodeEditor::active()
{
    QWidget* focus = QApplication::focusWidget();
    for (NodeEditor* e : editors())
        if (focus && e->isAncestorOf(focus))
            return e;
    if (lastFocused())
        return lastFocused();
    for (NodeEditor* e : editors())
        if (e->isVisible())
            return e;
    return nullptr;
}

void NodeEditor::addNodeAtCursor()
{
    if (QApplication::activePopupWidget())
        return;
    if (mCanvas->wireActive())
    {
        WireDrag w = mCanvas->takeWire();
        openAddMenu(mCanvas->cursorScenePos(), &w);
        return;
    }
    openAddMenu(mCanvas->cursorScenePos(), nullptr);
}

void NodeEditor::openAddMenu(QPointF at, const WireDrag* wire)
{
    if (mGroup.isEmpty())
        return;
    std::string group = mGroup.toStdString();
    json query = {{"group", group}};
    std::optional<WireDrag> drag;
    if (wire)
    {
        drag = *wire;
        query["linkType"] = wire->type;
        query["linkInput"] = wire->input;
    }
    json types = run("NodeTypes", query);
    if (!types.is_array())
        types = json::array();
    auto* pop = new AddNodePopup(this, types, [this, backend = mBackend, group, at, drag](const std::string& type, const std::string& socket) {
        // The new node sits beside the loose end, on the side its linked socket is on.
        QPointF pos(at.x() - kNodeWidth * 0.5, at.y() - 12.0);
        if (drag)
            pos = drag->input ? QPointF(at.x() - kNodeWidth - 10.0, at.y() - kHeader) : QPointF(at.x() + 10.0, at.y() - kHeader);
        UndoChunk chunk("Add Node");
        if (drag && drag->detached)
            runVerb(*backend, "Disconnect", {{"args", {group, drag->detached->second}}, {"source", drag->detached->first}});
        json r = runVerb(*backend, "AddNode", {{"args", {group, type}}, {"position", {pos.x(), pos.y()}}});
        if (!r.is_string())
            return;
        std::string name = r.get<std::string>();
        if (drag && !socket.empty())
        {
            std::string mine = name + "." + socket, theirs = drag->node + "." + drag->socket;
            runVerb(*backend, "Connect", args({group, drag->input ? mine : theirs, drag->input ? theirs : mine}));
        }
        selectAfterRefresh({name});
    });
    // A cancelled pick shows the picked-up link again.
    if (drag)
        pop->onClosed = [this](bool) { scheduleRefresh(); };
    pop->popup(QCursor::pos());
}

namespace
{
/** {"args": [group, selected...]}, or null when nothing is selected. */
json selectionCall(const QString& group, const std::vector<std::string>& names)
{
    if (group.isEmpty() || names.empty())
        return nullptr;
    json a = json::array({group.toStdString()});
    for (const std::string& n : names)
        a.push_back(n);
    return {{"args", a}};
}
} // namespace

void NodeEditor::deleteSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (!call.is_null())
        run("DeleteNode", call);
}

void NodeEditor::copySelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null())
        return;
    json text = run("CopyNodes", call);
    if (text.is_string())
        QApplication::clipboard()->setText(qs(text.get<std::string>()));
}

void NodeEditor::cutSelected()
{
    UndoChunk chunk("Cut Nodes");
    copySelected();
    deleteSelected();
}

void NodeEditor::paste()
{
    if (mGroup.isEmpty())
        return;
    QString text = QApplication::clipboard()->text();
    if (text.trimmed().isEmpty())
    {
        showError("Nothing to paste: the clipboard has no text.");
        return;
    }
    QPointF at = mCanvas->cursorScenePos();
    try
    {
        json r = ops::execute(mBackend->command("PasteNodes"), {{"args", {mGroup.toStdString(), text.toStdString()}}, {"position", {at.x(), at.y()}}},
                               CallSource::UI);
        if (r.is_array())
            selectAfterRefresh(r.get<std::vector<std::string>>());
    }
    catch (const std::exception& e)
    {
        showError(qs(e.what()));
    }
}

void NodeEditor::duplicateSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null())
        return;
    call["offset"] = {40, 40};
    json r = run("DuplicateNodes", call);
    if (r.is_array())
        selectAfterRefresh(r.get<std::vector<std::string>>());
}

void NodeEditor::selectAll() { mCanvas->selectAll(); }

void NodeEditor::setZipTieKey(bool down) { mCanvas->setZipKey(down); }

std::vector<std::vector<std::array<std::string, 4>>> NodeEditor::shownTies() const
{
    std::vector<std::vector<std::array<std::string, 4>>> out;
    for (const TieView& t : mCanvas->ties())
        out.push_back(t.links);
    return out;
}

void NodeEditor::groupSelected()
{
    json call = selectionCall(mGroup, mCanvas->selectedNames());
    if (call.is_null() || !mBackend->supports("GroupNodes"))
        return;
    call["name"] = "NodeGroup";
    try
    {
        json r = ops::execute(mBackend->command("GroupNodes"), call, CallSource::UI);
        if (r.is_object())
            selectAfterRefresh({r.value("node", "")});
    }
    catch (const std::exception& e)
    {
        showError(qs(e.what()));
    }
}

void NodeEditor::ungroupSelected()
{
    std::string sub = mBackend->subgroupNodeType();
    if (sub.empty())
        return;
    std::vector<std::string> names = mCanvas->selectedNames();
    UndoChunk chunk("Ungroup");
    std::vector<std::string> added;
    for (const std::string& n : names)
        if (mCanvas->typeOf(n) == sub)
        {
            try
            {
                json r = ops::execute(mBackend->command("Ungroup"), args({mGroup.toStdString(), n}), CallSource::UI);
                if (r.is_array())
                    for (const json& x : r)
                        added.push_back(x.get<std::string>());
            }
            catch (const std::exception& e)
            {
                showError(qs(e.what()));
            }
        }
    if (!added.empty())
        selectAfterRefresh(added);
}

void NodeEditor::openSelectedGroup()
{
    for (const std::string& n : mCanvas->selectedNames())
        if (mBackend->runsSubgroup(mCanvas->typeOf(n)))
        {
            json info = run("GroupInfo", args({mGroup.toStdString()}));
            for (const json& d : info.is_object() ? info["definition"].value("nodes", json::array()) : json::array())
                if (d.value("name", "") == n && d.contains("values"))
                {
                    std::string g = d["values"].value("group", "");
                    if (mBackend->hasGroup(scene::Graph::current(), g))
                        setGroup(qs(g));
                    return;
                }
        }
}

void NodeEditor::exportGroup()
{
    std::vector<std::string> suffixes = mBackend->fileSuffixes();
    if (mGroup.isEmpty() || suffixes.empty() || !mBackend->supports("ExportGroup"))
        return;
    QString suffix = qs(suffixes.front());
    QString path = ui::fileDialog::getSaveFileName(this, "Export Node Group", mGroup + "." + suffix, "Node groups (*." + suffix + ")");
    if (path.isEmpty())
        return;
    json r = run("ExportGroup", args({mGroup.toStdString(), path.toStdString()}));
    if (r.is_string())
        log::info("exported {}", r.get<std::string>());
}

void NodeEditor::importGroups(const QStringList& files)
{
    UndoChunk chunk("Import Node Groups");
    QString last;
    for (const QString& f : files)
    {
        try
        {
            json names = ops::execute(mBackend->command("ImportGroup"), args({f.toStdString()}), CallSource::UI);
            if (names.is_array() && !names.empty())
                last = qs(names.back().get<std::string>());
        }
        catch (const std::exception& e)
        {
            showError(qs(e.what()));
        }
    }
    if (!last.isEmpty())
        setGroup(last);
}

void NodeEditor::viewNode(const std::string& node)
{
    if (mGroup.isEmpty() || node.empty() || !mBackend->canViewNodes())
        return;
    mBackend->viewNode({.group = mGroup.toStdString(),
                        .node = node,
                        .nodeType = mCanvas->typeOf(node),
                        .outputs = mCanvas->outputsOf(node),
                        .position = mCanvas->positionOf(node),
                        .nodeWidth = kNodeWidth});
}

void NodeEditor::selectAfterRefresh(const std::vector<std::string>& names)
{
    mSelectNext = std::set<std::string>(names.begin(), names.end());
    mSelectedNode = names.size() == 1 ? qs(names.front()) : QString();
    scheduleRefresh();
}

void NodeEditor::showError(const QString& message)
{
    log::warn("{}", message.toStdString());
    QToolTip::showText(QCursor::pos(), message, mCanvas, QRect(), 8000);
}

void NodeEditor::scheduleRefresh(bool panel)
{
    mPanelDirty |= panel;
    if (!mRefresh->isActive())
        mRefresh->start();
}

void NodeEditor::followSelection()
{
    GraphBackend::Owner owner = mBackend->followSelection(scene::Graph::current());
    if (!owner.path.empty() && !owner.group.empty())
        followOwner(owner);
}

void NodeEditor::followOwner(const GraphBackend::Owner& owner)
{
    QString path = qs(owner.path);
    bool changed = path != mOwner;
    mOwner = path;
    if (qs(owner.group) != mGroup)
        setGroup(qs(owner.group));
    else if (changed)
        scheduleRefresh();
}

nlohmann::json NodeEditor::shownValues() const
{
    json out = json::object();
    std::string owner = ownerInView();
    if (owner.empty() || mGroup.isEmpty())
        return out;
    json info = run("GroupInfo", args({mGroup.toStdString()}));
    if (!info.is_object() || !info.contains("definition"))
        return out;
    scene::Graph& g = scene::Graph::current();
    for (const json& in : info["definition"].value("inputs", json::array()))
        if (std::string plug = owner + "." + in.value("name", ""); hasPlug(plug))
            out[plug] = scene::toJson(g.value(g.plug(plug)));
    return out;
}

std::string NodeEditor::ownerInView() const
{
    if (mOwner.isEmpty() || mGroup.isEmpty())
        return {};
    std::string path = mOwner.toStdString();
    return qs(mBackend->groupOfOwner(scene::Graph::current(), path)) == mGroup ? path : std::string();
}

std::vector<std::string> NodeEditor::sendTargets() const
{
    if (!mBackend->canSend())
        return {};
    return mBackend->sendTargets(scene::Graph::current(), mGroup.toStdString(), ownerInView());
}

void NodeEditor::setValueFromField(const json& call) { setFromField(*mBackend, call); }

void NodeEditor::refreshSendButton()
{
    if (!mSendBtn)
        return;
    std::vector<std::string> targets = sendTargets();
    mSendBtn->setText(qs(animatedSend::lastLabel(targets)));
    mSendBtn->setEnabled(!targets.empty());
    mSendBtn->setToolTip(targets.empty() ? "Apply this group to a mesh to send its result"
                                         : "Send " + qs(targets.size() == 1 ? targets.front() : std::to_string(targets.size()) + " meshes") +
                                               " again the same way; the arrow picks Unreal or files and the kind");
}

void NodeEditor::refreshGroups()
{
    mRefreshing = true;
    std::vector<std::string> names = mBackend->groupNames(scene::Graph::current());
    mGroups->clear();
    for (const std::string& n : names)
        mGroups->addItem(qs(n));
    if (mGroup.isEmpty() && !names.empty())
        mGroup = qs(names.front());
    if (std::find(names.begin(), names.end(), mGroup.toStdString()) == names.end())
        mGroup = names.empty() ? QString() : qs(names.front());
    mGroups->setCurrentText(mGroup);
    mRefreshing = false;
    if (mGroup != mAnnouncedGroup)
    {
        mAnnouncedGroup = mGroup;
        // A copy: a listener may set the group of this or another editor, which announces again.
        const auto listeners = groupListeners();
        for (const auto& [id, fn] : listeners)
            fn(*this);
    }
}

void NodeEditor::refresh(bool panel)
{
    LILA_ZONE();

    refreshGroups();
    refreshSendButton();
    std::set<std::string> selected = std::move(mSelectNext);
    mSelectNext.clear();
    // Edits rebuild the canvas; what was selected stays selected.
    if (selected.empty() && qs(mCanvas->group()) == mGroup)
        for (const std::string& n : mCanvas->selectedNames())
            selected.insert(n);
    if (selected.empty() && !mSelectedNode.isEmpty())
        selected.insert(mSelectedNode.toStdString());
    json info = mGroup.isEmpty() ? json() : run("GroupInfo", args({mGroup.toStdString()}));
    mCanvas->build(mGroup.toStdString(), info, selected);
    if (panel)
        rebuildSidePanel();
}

QWidget* NodeEditor::buildInterface(QWidget* panel, const json& info)
{
    std::string group = mGroup.toStdString();
    const json& def = info["definition"];
    json inputs = def.value("inputs", json::array());
    auto* box = new QWidget(panel);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(3);
    std::string modPath = ownerInView();
    bool owned = !modPath.empty();
    QString noun = mBackend->ownerNoun();
    mShownValues = shownValues();
    QLabel* title = heading(box, owned ? "Parameters: " + qs(modPath) : "Parameter Defaults");
    title->setToolTip(owned ? "The values this " + noun + " uses. Middle click a field to restore the group's default."
                            : "Defaults for new " + noun + "s using this group. Select what runs this group to edit its values.");
    v->addWidget(title);

    // Panels in first-use order, the unpanelled inputs first.
    std::vector<std::string> panels{""};
    for (const json& in : inputs)
        if (std::string pn = in.value("panel", ""); std::find(panels.begin(), panels.end(), pn) == panels.end())
            panels.push_back(pn);
    for (const std::string& pn : panels)
    {
        QFormLayout* form = nullptr;
        for (size_t i = 0; i < inputs.size(); ++i)
        {
            const json& in = inputs[i];
            if (in.value("panel", "") != pn)
                continue;
            if (!form)
            {
                if (!pn.empty())
                {
                    auto* sub = new QLabel(qs(pn), box);
                    sub->setStyleSheet("color: rgb(190,190,190); padding-top: 4px; border-bottom: 1px solid rgb(70,70,70);");
                    v->addWidget(sub);
                }
                form = new QFormLayout();
                form->setContentsMargins(pn.empty() ? 0 : 8, 0, 0, 0);
                v->addLayout(form);
            }
            std::string name = in.value("name", ""), type = in.value("type", "float");
            QString shown = in.contains("label") ? qs(in["label"].get<std::string>()) : pretty(name);
            QString tip = qs(name) + ": " + qs(type) + (in.contains("subtype") ? " (" + qs(in["subtype"].get<std::string>()) + ")" : "");
            if (in.contains("description"))
                tip += "\n" + qs(in["description"].get<std::string>());
            auto* row = new QWidget(box);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            QWidget* ed = nullptr;
            if (owned && hasPlug(modPath + "." + name))
            {
                std::string plug = modPath + "." + name;
                scene::Graph& g = scene::Graph::current();
                ed = valueEditor(row, type, scene::toJson(g.value(g.plug(plug))), [plug](json val) {
                    ++gFieldEdits;
                    runCmd("setAttr", args({plug, val}));
                    --gFieldEdits;
                }, in.value("default", json()));
            }
            else
                ed = valueEditor(row, type, in.value("default", json()), [backend = mBackend, group, name](json val) {
                    setFromField(*backend, args({group, "GroupInput." + name, val}));
                });
            applyLimits(ed, in);
            ed->setToolTip(tip);
            h->addWidget(ed, 1);
            auto* more = new QToolButton(row);
            more->setIcon(icon("dots"));
            more->setToolTip("Rename, edit, move or remove this input");
            more->setPopupMode(QToolButton::InstantPopup);
            auto* menu = new QMenu(more);
            int index = int(i);
            int count = int(inputs.size());
            json current = in;
            bool labelled = in.contains("label");
            const GraphBackend* b = mBackend.get();
            menu->addAction("Rename...", this, [this, b, group, name, shown, labelled] {
                bool ok = false;
                QString to = QInputDialog::getText(this, "Rename Input", "Name", QLineEdit::Normal, shown, &ok).trimmed();
                if (!ok || to.isEmpty() || to == shown)
                    return;
                std::string id = identifierFrom(to);
                if (labelled || to != pretty(id))
                    runVerb(*b, "SetInputInfo", {{"args", {group, name}}, {"label", to.toStdString()}});
                if (!id.empty() && id != name)
                    runVerb(*b, "RenameInput", args({group, name, id}));
            });
            menu->addAction("Edit Info...", this, [this, name, current] { editInputInfo(name, current); });
            QAction* up = menu->addAction("Move Up", this, [b, group, name, index] { runVerb(*b, "MoveInput", args({group, name, index - 1})); });
            up->setEnabled(index > 0);
            QAction* down = menu->addAction("Move Down", this, [b, group, name, index] { runVerb(*b, "MoveInput", args({group, name, index + 1})); });
            down->setEnabled(index + 1 < count);
            menu->addSeparator();
            menu->addAction("Remove", this, [b, group, name] { runVerb(*b, "RemoveInput", args({group, name})); });
            more->setMenu(menu);
            more->setVisible(mBackend->supports("RenameInput"));
            h->addWidget(more);
            auto* label = new QLabel(shown, box);
            label->setToolTip(tip);
            form->addRow(label, row);
        }
    }
    if (inputs.empty())
    {
        auto* none = new QLabel("Nothing exposed yet. Expose a node's input, or add one below.", box);
        none->setWordWrap(true);
        none->setStyleSheet("color: rgb(150,150,150);");
        v->addWidget(none);
    }

    auto* addRow = new QWidget(box);
    auto* ah = new QHBoxLayout(addRow);
    ah->setContentsMargins(0, 4, 0, 0);
    auto* addName = new QLineEdit(addRow);
    addName->setPlaceholderText("new input name");
    auto* addType = new QComboBox(addRow);
    for (const std::string& t : mBackend->inputTypes())
        addType->addItem(qs(t));
    auto* addBtn = new QToolButton(addRow);
    addBtn->setIcon(icon("plus"));
    addBtn->setToolTip("Add an exposed input; link it from the Group Input node");
    auto add = [backend = mBackend, group, addName, addType] {
        std::string n = addName->text().trimmed().toStdString();
        if (!n.empty())
            runVerb(*backend, "AddInput", args({group, n, addType->currentText().toStdString()}));
    };
    QObject::connect(addBtn, &QToolButton::clicked, addBtn, add);
    QObject::connect(addName, &QLineEdit::returnPressed, addName, add);
    ah->addWidget(addName, 1);
    ah->addWidget(addType);
    ah->addWidget(addBtn);
    v->addWidget(addRow);
    addRow->setVisible(mBackend->supports("AddInput"));

    json outputs = def.value("outputs", json::array());
    if (!outputs.empty())
    {
        v->addWidget(heading(box, "Outputs"));
        for (const json& o : outputs)
        {
            std::string name = o.value("name", "");
            auto* row = new QWidget(box);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            auto* l = new QLabel(pretty(name) + "  (" + qs(o.value("type", "")) + ")", row);
            if (o.contains("description"))
                l->setToolTip(qs(o["description"].get<std::string>()));
            h->addWidget(l, 1);
            auto* rm = new QToolButton(row);
            rm->setIcon(icon("x"));
            rm->setToolTip("Remove this output");
            QObject::connect(rm, &QToolButton::clicked, rm, [backend = mBackend, group, name] { runVerb(*backend, "RemoveOutput", args({group, name})); });
            rm->setVisible(mBackend->supports("RemoveOutput"));
            h->addWidget(rm);
            v->addWidget(row);
        }
    }
    return box;
}

void NodeEditor::editInputInfo(const std::string& input, const json& current)
{
    QDialog dlg(this);
    dlg.setWindowTitle("Input " + qs(input));
    auto* form = new QFormLayout(&dlg);
    auto text = [&](const char* key) { return current.contains(key) ? qs(current[key].get<std::string>()) : QString(); };
    auto number = [&](const char* key) { return current.contains(key) ? QString::number(current[key].get<double>()) : QString(); };
    auto* label = new QLineEdit(text("label"), &dlg);
    label->setPlaceholderText(pretty(input));
    auto* desc = new QLineEdit(text("description"), &dlg);
    desc->setPlaceholderText("Tooltip shown on the " + mBackend->ownerNoun());
    auto* subtype = new QComboBox(&dlg);
    for (const char* st : {"", "factor", "percentage", "angle", "distance", "color"})
        subtype->addItem(*st ? pretty(st) : QString("None"), qs(st));
    subtype->setCurrentIndex(std::max(0, subtype->findData(text("subtype"))));
    std::array<std::pair<const char*, QLineEdit*>, 5> nums{};
    const char* numKeys[5] = {"min", "max", "softMin", "softMax", "step"};
    for (int k = 0; k < 5; ++k)
    {
        auto* e = new QLineEdit(number(numKeys[k]), &dlg);
        e->setPlaceholderText("none");
        nums[k] = {numKeys[k], e};
    }
    auto* panelEdit = new QLineEdit(text("panel"), &dlg);
    panelEdit->setPlaceholderText("none");
    form->addRow("Label", label);
    form->addRow("Description", desc);
    form->addRow("Subtype", subtype);
    form->addRow("Min", nums[0].second);
    form->addRow("Max", nums[1].second);
    form->addRow("Soft Min", nums[2].second);
    form->addRow("Soft Max", nums[3].second);
    form->addRow("Step", nums[4].second);
    form->addRow("Panel", panelEdit);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted)
        return;
    json call = {{"args", {mGroup.toStdString(), input}},
                 {"label", label->text().trimmed().toStdString()},
                 {"description", desc->text().trimmed().toStdString()},
                 {"subtype", subtype->currentData().toString().toStdString()},
                 {"panel", panelEdit->text().trimmed().toStdString()}};
    for (auto& [key, e] : nums)
    {
        bool ok = false;
        double d = e->text().trimmed().toDouble(&ok);
        call[key] = ok ? json(d) : json("");
    }
    run("SetInputInfo", call);
}

QWidget* NodeEditor::portalNameEditor(QWidget* parent, const json& info, const std::string& node, bool receive)
{
    std::string group = mGroup.toStdString(), current;
    std::vector<std::string> sends;
    for (const json& d : info["definition"].value("nodes", json::array()))
    {
        std::string name = d.contains("values") ? d["values"].value("name", "") : std::string();
        if (d.value("name", "") == node)
            current = name;
        if (d.value("type", "") == gn::kSendType && !name.empty() && std::find(sends.begin(), sends.end(), name) == sends.end())
            sends.push_back(name);
    }
    auto set = [backend = mBackend, group, node, this](const std::string& name) {
        try
        {
            json call = args({group, node + ".name", name});
            backend->adaptCall("SetValue", call);
            ops::execute(backend->command("SetValue"), call, CallSource::UI);
        }
        catch (const std::exception& e)
        {
            showError(qs(e.what()));
        }
    };
    if (!receive)
    {
        auto* e = new QLineEdit(qs(current), parent);
        e->setPlaceholderText("name Receives read");
        QObject::connect(e, &QLineEdit::editingFinished, e, [e, set, current] {
            if (e->text().trimmed().toStdString() != current)
                set(e->text().trimmed().toStdString());
        });
        return e;
    }
    auto* combo = new QComboBox(parent);
    for (const std::string& s : sends)
        combo->addItem(qs(s));
    if (!current.empty() && combo->findText(qs(current)) < 0)
        combo->addItem(qs(current) + " (missing)", qs(current));
    combo->setCurrentIndex(current.empty() ? -1 : std::max(combo->findText(qs(current)), combo->findData(qs(current))));
    combo->setToolTip("The Send node this reads. Double-click the node to jump to it.");
    QObject::connect(combo, &QComboBox::activated, combo, [combo, set](int i) {
        QString v = combo->itemData(i).isValid() ? combo->itemData(i).toString() : combo->itemText(i);
        set(v.toStdString());
    });
    return combo;
}

QWidget* NodeEditor::itemListEditor(QWidget* parent, const json& list)
{
    static const char* kTypes[] = {"float", "int", "bool", "vector", "rotation", "color", "string", "matrix", "geometry", "bundle", "closure"};
    const std::string group = mGroup.toStdString(), node = list.value("node", ""), key = list.value("key", "");
    const json items = list.value("items", json::array());
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 6, 0, 0);
    v->setSpacing(2);
    v->addWidget(heading(box, pretty(key)));
    auto set = [backend = mBackend, group, node, key](json next, json renames) {
        json call = args({group, node, key, std::move(next)});
        if (!renames.empty())
            call["renames"] = std::move(renames);
        runVerb(*backend, "SetItems", call);
    };
    for (size_t i = 0; i < items.size(); ++i)
    {
        auto* row = new QWidget(box);
        auto* h = new QHBoxLayout(row);
        h->setContentsMargins(0, 0, 0, 0);
        auto* name = new QLineEdit(qs(items[i].value("name", "")), row);
        name->setToolTip("The item's name, which is also its socket's.");
        QObject::connect(name, &QLineEdit::editingFinished, name, [set, items, i, name] {
            std::string old = items[i].value("name", ""), fresh = name->text().trimmed().toStdString();
            if (fresh.empty() || fresh == old)
                return;
            json next = items;
            next[i]["name"] = fresh;
            set(next, json{{old, fresh}});
        });
        auto* type = new QComboBox(row);
        for (const char* t : kTypes)
            type->addItem(pretty(t), t);
        type->setCurrentIndex(std::max(0, type->findData(qs(items[i].value("type", "float")))));
        QObject::connect(type, &QComboBox::activated, type, [set, items, i, type](int) {
            json next = items;
            next[i]["type"] = type->currentData().toString().toStdString();
            set(next, json::object());
        });
        auto* remove = new QToolButton(row);
        remove->setText("Remove");
        remove->setToolTip("Remove this item and its links.");
        QObject::connect(remove, &QToolButton::clicked, remove, [set, items, i] {
            json next = items;
            next.erase(i);
            set(next, json::object());
        });
        h->addWidget(name, 1);
        h->addWidget(type);
        h->addWidget(remove);
        v->addWidget(row);
    }
    auto* add = new QToolButton(box);
    add->setText("Add Item");
    add->setToolTip("Add a value that travels through: a socket on both sides.");
    QObject::connect(add, &QToolButton::clicked, add, [set, items] {
        std::set<std::string> taken;
        for (const json& it : items)
            taken.insert(it.value("name", ""));
        std::string name = "value";
        for (int k = 1; taken.contains(name); ++k)
            name = "value" + std::to_string(k);
        json next = items;
        next.push_back({{"name", name}, {"type", "float"}});
        set(next, json::object());
    });
    v->addWidget(add, 0, Qt::AlignLeft);
    return box;
}

void NodeEditor::jumpToPortalSource() { mCanvas->jumpToPortal(true); }

void NodeEditor::frameSelection() { mCanvas->frameSelection(); }

void NodeEditor::swapInputs()
{
    if (!mCanvas->swapInputs())
        showError("Select one node with two inputs of the same type to swap.");
}

void NodeEditor::toggleNodeFlag(const char* flag) { mCanvas->toggleFlag(flag); }

void NodeEditor::alignSelected(int how) { mCanvas->align(NodeCanvas::Align(how)); }

void NodeEditor::distributeSelected(bool horizontal) { mCanvas->distribute(horizontal); }

void NodeEditor::straightenSelected() { mCanvas->straighten(); }

namespace
{
/** The action a grab started from, when its key started it; a menu or script start has none to release or tap again. */
std::string grabAction(const char* action) { return Hotkeys::fromKey() ? action : std::string(); }
} // namespace

void NodeEditor::grabMove() { mCanvas->beginGrab(NodeCanvas::Grab::Move, grabAction("nodeEditor.grabMove")); }

void NodeEditor::grabSpacing() { mCanvas->beginGrab(NodeCanvas::Grab::Spacing, grabAction("nodeEditor.grabSpacing")); }

void NodeEditor::turnTie() { mCanvas->beginGrab(NodeCanvas::Grab::TieTurn, grabAction("nodeEditor.turnTie")); }

void NodeEditor::autoLayout() { mCanvas->autoLayout(); }

void NodeEditor::setMinimap(bool on) { mCanvas->setMinimap(on); }

void NodeEditor::mergeSelected(const std::string& op)
{
    if (QString error = mCanvas->mergeSelected(op); !error.isEmpty())
        showError(error);
}

void NodeEditor::setSelectedWiresOnly(bool on) { mCanvas->setSelectedWiresOnly(on); }

bool NodeEditor::selectedWiresOnly() const { return mCanvas->selectedWiresOnly(); }

bool NodeEditor::minimap() const { return mCanvas->minimap(); }

void NodeEditor::storeBookmark(int slot) { mCanvas->storeBookmark(slot); }

void NodeEditor::recallBookmark(int slot)
{
    if (!mCanvas->recallBookmark(slot))
        showError(QString("No view saved under %1 yet: Ctrl+%1 saves one.").arg(slot));
}

void NodeEditor::focusNode(const std::string& name) { mCanvas->focusNode(name); }

void NodeEditor::findNode()
{
    if (mGroup.isEmpty() || QApplication::activePopupWidget())
        return;
    json entries = json::array();
    for (auto& [name, title] : mCanvas->nodeList())
        entries.push_back({{"type", name}, {"category", title.toStdString()}, {"label", name}});
    auto* pop = new AddNodePopup(this, entries, [this](const std::string& name, const std::string&) { focusNode(name); });
    pop->popup(QCursor::pos());
}

void NodeEditor::showCanvasMenu(QPoint global)
{
    QMenu menu(this);
    auto item = [&menu](QMenu* into, const char* id) {
        const Action* a = actions::find(id);
        if (!a)
            return;
        QString key = Hotkeys::get().keyFor(id);
        QAction* q = into->addAction(qs(a->label) + (key.isEmpty() ? QString() : "\t" + key));
        q->setToolTip(qs(a->doc));
        if (a->checked)
        {
            q->setCheckable(true);
            q->setChecked(a->checked());
        }
        QObject::connect(q, &QAction::triggered, q, [id] { actions::run(json(id)); });
    };
    for (const char* id : {"nodeEditor.addNode", "nodeEditor.duplicate", "nodeEditor.copy", "nodeEditor.paste", "nodeEditor.delete"})
        item(&menu, id);
    menu.addSeparator();
    QMenu* arrange = menu.addMenu("Arrange");
    for (const char* id : {"nodeEditor.alignLeft", "nodeEditor.alignCenter", "nodeEditor.alignRight", "nodeEditor.alignTop", "nodeEditor.alignMiddle",
                           "nodeEditor.alignBottom", "nodeEditor.distributeHorizontal", "nodeEditor.distributeVertical", "nodeEditor.grabSpacing", "nodeEditor.straighten", "nodeEditor.autoLayout"})
        item(arrange, id);
    QMenu* merge = menu.addMenu("Merge");
    for (const char* id : {"nodeEditor.mergeAdd", "nodeEditor.mergeSubtract", "nodeEditor.mergeMultiply", "nodeEditor.mergeDivide"})
        item(merge, id);
    item(&menu, "nodeEditor.frameNodes");
    item(&menu, "nodeEditor.swapInputs");
    item(&menu, "nodeEditor.toggleMute");
    item(&menu, "nodeEditor.toggleCollapse");
    item(&menu, "nodeEditor.toggleUnusedSockets");
    menu.addSeparator();
    item(&menu, "nodeEditor.toggleFlow");
    item(&menu, "nodeEditor.toggleSelectedWires");
    item(&menu, "nodeEditor.toggleMinimap");
    item(&menu, "nodeEditor.find");
    item(&menu, "nodeEditor.portalSource");
    item(&menu, "nodeEditor.portalReaders");
    menu.exec(global);
}

void NodeEditor::setFlowHighlight(bool on, bool hold) { mCanvas->setFlowHighlight(on, hold); }

bool NodeEditor::flowHighlight() const { return mCanvas->flowHighlight(); }

void NodeEditor::jumpToPortalReaders() { mCanvas->jumpToPortal(false); }

void NodeEditor::rebuildSidePanel()
{
    auto* panel = new QWidget();
    auto* lay = new QVBoxLayout(panel);
    lay->setContentsMargins(8, 4, 8, 8);
    lay->setSpacing(4);
    if (mGroup.isEmpty())
    {
        lay->addWidget(new QLabel("No " + mBackend->groupNoun().toLower() + ". Use New to make one.", panel));
        lay->addStretch(1);
        mSide->setWidget(panel);
        return;
    }
    std::string group = mGroup.toStdString();
    json info = run("GroupInfo", args({group}));
    if (!info.is_object())
    {
        mSide->setWidget(panel);
        return;
    }

    if (mBackend->supports("AddInput") || !info["definition"].value("inputs", json::array()).empty())
        lay->addWidget(buildInterface(panel, info));
    if (info.contains("owners") || info.contains("modifiers"))
    {
        json mods = info.contains("owners") ? info["owners"] : info["modifiers"];
        QString used = mods.empty() ? "Not used yet." : "Used by " + QString::number(mods.size()) + " " + mBackend->ownerNoun() + "(s).";
        auto* usedLabel = new QLabel(used, panel);
        usedLabel->setStyleSheet("color: rgb(150,150,150);");
        lay->addWidget(usedLabel);
    }
    if (info.contains("error"))
    {
        auto* err = new QLabel(qs(info["error"].get<std::string>()), panel);
        err->setWordWrap(true);
        err->setStyleSheet("color: rgb(235,235,235); font-style: italic;");
        lay->addWidget(err);
    }

    const json* node = nullptr;
    for (const json& n : info["nodes"])
        if (qs(n.value("name", "")) == mSelectedNode)
            node = &n;
    bool routing = node && gn::isRoutingType(node->value("type", ""));
    QWidget* custom = node && !routing ? mBackend->nodePanel(panel, *this, *node) : nullptr;
    if (custom)
        lay->addWidget(custom);
    else if (node)
    {
        std::string nodeName = node->value("name", "");
        lay->addWidget(heading(panel, qs(node->value("label", "")) + "  (" + qs(nodeName) + ")"));
        auto* form = new QFormLayout();
        form->setContentsMargins(0, 0, 0, 0);
        if (std::string type = node->value("type", ""); type == gn::kSendType || type == gn::kReceiveType)
            form->addRow("Name", portalNameEditor(panel, info, nodeName, type == gn::kReceiveType));
        for (const json& p : node->value("props", json::array()))
        {
            std::string id = p.value("id", "");
            std::string cur = p.value("value", "");
            json opts = p.value("options", json::array());
            if (routing && id == "name")
                continue;
            if (mBackend->runsSubgroup(node->value("type", "")) && id == "group")
            {
                auto* combo = new QComboBox(panel);
                for (const std::string& g : mBackend->groupNames(scene::Graph::current()))
                    if (g != group)
                        combo->addItem(qs(g));
                if (combo->findText(qs(cur)) < 0 && !cur.empty())
                    combo->addItem(qs(cur));
                combo->setCurrentIndex(cur.empty() ? -1 : combo->findText(qs(cur)));
                combo->setToolTip("The node group this node runs. Double-click the node to open it.");
                QObject::connect(combo, &QComboBox::activated, combo, [backend = mBackend, combo, group, nodeName](int) {
                    runVerb(*backend, "SetValue", args({group, nodeName + ".group", combo->currentText().toStdString()}));
                });
                form->addRow("Group", combo);
                continue;
            }
            if (std::string editor = p.value("editor", ""); opts.empty() && !editor.empty())
            {
                // Typing pauses commit multiline text, so it must not rebuild the panel under the box.
                auto set = [backend = mBackend, group, nodeName, id, editor](std::string v) {
                    json call = args({group, nodeName + "." + id, std::move(v)});
                    if (editor == "multiline")
                        setFromField(*backend, call);
                    else
                        runVerb(*backend, "SetValue", call);
                };
                if (QWidget* w = textEditor(panel, editor, cur, set))
                {
                    if (editor == "font")
                        form->addRow(pretty(id), w);
                    else
                    {
                        form->addRow(new QLabel(pretty(id), panel));
                        form->addRow(w);
                    }
                    continue;
                }
            }
            if (opts.empty())
            {
                auto* e = new QLineEdit(qs(cur), panel);
                QObject::connect(e, &QLineEdit::editingFinished, e, [backend = mBackend, e, group, nodeName, id] {
                    runVerb(*backend, "SetValue", args({group, nodeName + "." + id, e->text().toStdString()}));
                });
                form->addRow(pretty(id), e);
                continue;
            }
            auto* combo = new QComboBox(panel);
            for (const json& o : opts)
                combo->addItem(pretty(o.get<std::string>()), qs(o.get<std::string>()));
            combo->setCurrentIndex(std::max(0, combo->findData(qs(cur))));
            QObject::connect(combo, &QComboBox::activated, combo, [backend = mBackend, combo, group, nodeName, id](int) {
                runVerb(*backend, "SetValue", args({group, nodeName + "." + id, combo->currentData().toString().toStdString()}));
            });
            form->addRow(pretty(id), combo);
        }
        for (const json& s : node->value("inputs", json::array()))
        {
            std::string id = s.value("id", ""), type = s.value("type", "");
            if (mBackend->valueless(type))
                continue;
            if (s.value("linked", false))
            {
                auto* l = new QLabel("linked", panel);
                l->setStyleSheet("color: rgb(140,140,140);");
                form->addRow(pretty(id), l);
                continue;
            }
            auto* row = new QWidget(panel);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 0, 0, 0);
            json value = s.value("value", json());
            if (value.is_null() && s.contains("implicit"))
            {
                auto* l = new QLabel("implicit " + qs(s["implicit"].get<std::string>()), row);
                l->setStyleSheet("color: rgb(150,150,150);");
                h->addWidget(l, 1);
            }
            else
            {
                QWidget* ed = valueEditor(row, type, value, [backend = mBackend, group, nodeName, id](json v) { setFromField(*backend, args({group, nodeName + "." + id, v})); },
                                          s.value("default", json()), s.value("editor", ""));
                applyLimits(ed, s);
                h->addWidget(ed, 1);
            }
            auto* expose = new QToolButton(row);
            if (!mBackend->supports("ExposeInput"))
                expose->hide();
            expose->setText("Expose");
            expose->setToolTip("Make this a group input: a keyable attr on the " + mBackend->ownerNoun());
            QObject::connect(expose, &QToolButton::clicked, expose, [backend = mBackend, group, nodeName, id] { runVerb(*backend, "ExposeInput", args({group, nodeName + "." + id})); });
            h->addWidget(expose);
            auto* label = new QLabel(pretty(id), panel);
            if (json def = s.value("default", json()); !def.is_null() && !value.is_null())
                onMiddleClickReset(label, [backend = mBackend, group, nodeName, id, def] { runVerb(*backend, "SetValue", args({group, nodeName + "." + id, def})); },
                                   QString::fromStdString(valueReset::describe(def)), row);
            form->addRow(label, row);
        }
        lay->addLayout(form);
        for (const json& list : node->value("itemLists", json::array()))
            lay->addWidget(itemListEditor(panel, list));
        if (node->contains("actions"))
        {
            auto* row = new QWidget(panel);
            auto* h = new QHBoxLayout(row);
            h->setContentsMargins(0, 6, 0, 0);
            if (node->value("baked", false))
                h->addWidget(new QLabel("Baked", row));
            for (const json& act : (*node)["actions"])
            {
                auto* b = new QToolButton(row);
                b->setText(qs(act.value("label", "")));
                b->setToolTip(qs(act.value("doc", "")));
                QObject::connect(b, &QToolButton::clicked, b, [backend = mBackend, group, nodeName, verb = act.value("verb", "")] {
                    QApplication::setOverrideCursor(Qt::WaitCursor);
                    runVerb(*backend, verb, args({group, nodeName}));
                    QApplication::restoreOverrideCursor();
                });
                h->addWidget(b);
            }
            h->addStretch(1);
            lay->addWidget(row);
        }
    }
    else
    {
        auto* hint = new QLabel(mBackend->canvasHint(), panel);
        hint->setWordWrap(true);
        hint->setStyleSheet("color: rgb(150,150,150);");
        lay->addWidget(hint);
    }
    lay->addStretch(1);
    mSide->setWidget(panel);
}

namespace
{
void registerEditActions()
{
    auto add = [](const char* id, const char* label, void (NodeEditor::*fn)(), const char* doc) {
        actions::add({.id = id, .label = label, .category = "Node Editor",
                      .run = [fn] {
                          if (NodeEditor* e = NodeEditor::active())
                              (e->*fn)();
                      },
                      .doc = doc});
    };
    add("nodeEditor.addNode", "Add Node...", &NodeEditor::addNodeAtCursor,
        "Search for a node to add under the cursor. While dragging a wire it lists only nodes that connect to it.");
    add("nodeEditor.delete", "Delete Nodes", &NodeEditor::deleteSelected, "Delete the selected nodes and their links.");
    add("nodeEditor.copy", "Copy Nodes", &NodeEditor::copySelected,
        "Copy the selected nodes as text: paste them in another group or scene, or share them in a post.");
    add("nodeEditor.cut", "Cut Nodes", &NodeEditor::cutSelected, "Copy the selected nodes as text, then delete them.");
    add("nodeEditor.paste", "Paste Nodes", &NodeEditor::paste, "Paste nodes from clipboard text at the cursor.");
    add("nodeEditor.duplicate", "Duplicate Nodes", &NodeEditor::duplicateSelected, "Duplicate the selected nodes with the links between them.");
    add("nodeEditor.selectAll", "Select All Nodes", &NodeEditor::selectAll, "Select every node in the group.");
    add("nodeEditor.frameSelected", "Frame Selected Nodes", &NodeEditor::frameSelected, "Fit the view to the selected nodes.");
    add("nodeEditor.frameAll", "Frame All Nodes", &NodeEditor::frameAll, "Fit the view to the whole group.");
    add("nodeEditor.groupNodes", "Group Selected Nodes", &NodeEditor::groupSelected,
        "Collapse the selected nodes into a new node group, run by a Group node in their place.");
    add("nodeEditor.ungroup", "Ungroup", &NodeEditor::ungroupSelected, "Replace the selected Group nodes with the nodes of their groups.");
    add("nodeEditor.openGroup", "Open Group", &NodeEditor::openSelectedGroup, "Open the group the selected Group node runs.");
    add("nodeEditor.exportGroup", "Export Node Group...", &NodeEditor::exportGroup,
        "Write the group and the groups nested in it to a .lilanodes file to share.");
    struct AlignAction
    {
        const char* id;
        const char* label;
        int how;
        const char* doc;
    };
    for (const AlignAction& a : {AlignAction{"nodeEditor.alignLeft", "Align Left", 0, "Line the selected nodes' left edges up."},
                                 AlignAction{"nodeEditor.alignCenter", "Align Centre", 1, "Line the selected nodes' centres up on their average."},
                                 AlignAction{"nodeEditor.alignRight", "Align Right", 2, "Line the selected nodes' right edges up."},
                                 AlignAction{"nodeEditor.alignTop", "Align Top", 3, "Line the selected nodes' tops up."},
                                 AlignAction{"nodeEditor.alignMiddle", "Align Middle", 4, "Line the selected nodes' middles up on their average."},
                                 AlignAction{"nodeEditor.alignBottom", "Align Bottom", 5, "Line the selected nodes' bottoms up."}})
        actions::add({.id = a.id, .label = a.label, .category = "Node Editor",
                      .run =
                          [how = a.how] {
                              if (NodeEditor* e = NodeEditor::active())
                                  e->alignSelected(how);
                          },
                      .doc = a.doc});
    actions::add({.id = "nodeEditor.distributeHorizontal", .label = "Distribute Horizontally", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->distributeSelected(true);
                      },
                  .doc = "Space three or more selected nodes evenly from left to right."});
    actions::add({.id = "nodeEditor.distributeVertical", .label = "Distribute Vertically", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->distributeSelected(false);
                      },
                  .doc = "Space three or more selected nodes evenly from top to bottom."});
    add("nodeEditor.find", "Find Node...", &NodeEditor::findNode, "Search the group's nodes by name or type, and jump to the one picked.");
    for (const char* op : {"add", "subtract", "multiply", "divide"})
    {
        std::string name = op;
        name[0] = char(std::toupper(name[0]));
        actions::add({.id = "nodeEditor.merge" + name, .label = "Merge Nodes (" + name + ")", .category = "Node Editor",
                      .run =
                          [op] {
                              if (NodeEditor* e = NodeEditor::active())
                                  e->mergeSelected(op);
                          },
                      .doc = "Add the node that merges the selected nodes' outputs, already wired: Join Geometry for geometry, Math for "
                             "numbers, Blend in texture graphs. The operation is " + std::string(op) + "."});
    }
    actions::add({.id = "nodeEditor.toggleSelectedWires", .label = "Only Selected Nodes' Wires", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->setSelectedWiresOnly(!e->selectedWiresOnly());
                      },
                  .doc = "Hide every wire except those on the selected nodes, to read one part of a busy graph.",
                  .checked = [] { return NodeEditor::active() && NodeEditor::active()->selectedWiresOnly(); }});
    actions::add({.id = "nodeEditor.toggleMinimap", .label = "Minimap", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->setMinimap(!e->minimap());
                      },
                  .doc = "Show an overview of the whole graph in the corner; click or drag in it to move the view.",
                  .checked = [] { return NodeEditor::active() && NodeEditor::active()->minimap(); }});
    for (int slot = 1; slot <= 5; ++slot)
    {
        actions::add({.id = "nodeEditor.bookmarkSet" + std::to_string(slot), .label = "Save View " + std::to_string(slot), .category = "Node Editor",
                      .run =
                          [slot] {
                              if (NodeEditor* e = NodeEditor::active())
                                  e->storeBookmark(slot);
                          },
                      .doc = "Save where the view is and its zoom under " + std::to_string(slot) + ", with the group."});
        actions::add({.id = "nodeEditor.bookmarkGo" + std::to_string(slot), .label = "Go to View " + std::to_string(slot), .category = "Node Editor",
                      .run =
                          [slot] {
                              if (NodeEditor* e = NodeEditor::active())
                                  e->recallBookmark(slot);
                          },
                      .doc = "Bring back the view saved under " + std::to_string(slot) + "."});
    }
    add("nodeEditor.autoLayout", "Auto Layout", &NodeEditor::autoLayout,
        "Arrange the selected nodes, or the whole graph when fewer than two are selected, in columns from left to right by data flow.");
    add("nodeEditor.grabMove", "Move Nodes", &NodeEditor::grabMove,
        "Pick the selected nodes up to follow the cursor; click or Enter places them, Esc or right-click puts them back. "
        "X or Y locks an axis, Shift snaps to the grid.");
    add("nodeEditor.grabSpacing", "Scale Spacing", &NodeEditor::grabSpacing,
        "Spread or tighten the gaps between the selected nodes from their centre, without resizing them; click or Enter "
        "places them, Esc or right-click puts them back. X or Y scales one axis, Shift steps in tenths.");
    add("nodeEditor.turnTie", "Turn Zip Tie", &NodeEditor::turnTie,
        "Turn the zip tie under the cursor with it; click or Enter places it, Esc or right-click puts it back. It steps by 15 "
        "degrees, or turns freely with Ctrl.");
    add("nodeEditor.straighten", "Straighten Links", &NodeEditor::straightenSelected,
        "Nudge the selected nodes up or down so the wires between them run level; a lone node lines up with what feeds it.");
    struct FlagAction
    {
        const char* id;
        const char* label;
        const char* flag;
        const char* doc;
    };
    for (const FlagAction& f : {FlagAction{"nodeEditor.toggleMute", "Mute Nodes", "muted",
                                           "Bypass the selected nodes: each passes its first input of a matching type straight to its outputs."},
                                FlagAction{"nodeEditor.toggleCollapse", "Collapse Nodes", "collapsed", "Draw the selected nodes as their headers only, or open them again."},
                                FlagAction{"nodeEditor.toggleUnusedSockets", "Hide Unused Sockets", "hideUnused",
                                           "Draw only the linked sockets of the selected nodes, or all of them again."}})
        actions::add({.id = f.id, .label = f.label, .category = "Node Editor",
                      .run =
                          [flag = f.flag] {
                              if (NodeEditor* e = NodeEditor::active())
                                  e->toggleNodeFlag(flag);
                          },
                      .doc = f.doc});
    add("nodeEditor.swapInputs", "Swap Inputs", &NodeEditor::swapInputs,
        "Trade the links and values of the selected node's first two inputs of one type, such as a Math node's operands.");
    add("nodeEditor.frameNodes", "Frame Selected Nodes", &NodeEditor::frameSelection,
        "Put a labelled frame around the selected nodes, or an empty one at the cursor. Drag its title to move it with what it holds.");
    add("nodeEditor.portalSource", "Jump to Send", &NodeEditor::jumpToPortalSource,
        "From a selected Send or Receive, select and centre the Send of its name.");
    add("nodeEditor.portalReaders", "Jump to Receives", &NodeEditor::jumpToPortalReaders,
        "From a selected Send or Receive, select and centre every Receive reading its name.");
    actions::add({.id = "nodeEditor.toggleFlow", .label = "Highlight Upstream and Downstream", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->setFlowHighlight(!e->flowHighlight(), false);
                      },
                  .doc = "Light up what feeds the selected nodes and what they feed, and dim the rest.",
                  .checked = [] { return NodeEditor::active() && NodeEditor::active()->flowHighlight(); }});
    actions::add({.id = "nodeEditor.flowHoldOn", .label = "Highlight Flow Key Down", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->setFlowHighlight(true, true);
                      },
                  .doc = "While held, light up what feeds the selected nodes and what they feed."});
    actions::add({.id = "nodeEditor.flowHoldOff", .label = "Highlight Flow Key Up", .category = "Node Editor",
                  .run =
                      [] {
                          if (NodeEditor* e = NodeEditor::active())
                              e->setFlowHighlight(false, true);
                      },
                  .doc = "Ends the flow highlight hold."});
    auto zipKey = [](bool down) {
        if (NodeEditor* e = NodeEditor::active())
            e->setZipTieKey(down);
    };
    actions::add({.id = "nodeEditor.zipTieOn", .label = "Zip Tie Key Down", .category = "Node Editor", .run = [zipKey] { zipKey(true); },
                  .doc = "While held, dragging across wires gathers them into a zip tie; start the drag on a tie to add the wires to it."});
    actions::add({.id = "nodeEditor.zipTieOff", .label = "Zip Tie Key Up", .category = "Node Editor", .run = [zipKey] { zipKey(false); },
                  .doc = "Ends the zip tie key hold."});
}
} // namespace

void registerNodeEditorActions()
{
    registerEditActions();
    prefPages::add({.id = "nodeEditor", .title = "Node Editor", .build = [](prefPages::PageBuilder& b) {
                        b.section("Editing");
                        b.toggle("Slide downstream nodes aside when a node drops into a wire", kPrefAutoOffset, true);
                        b.section("Wires");
                        b.choice("Wire style", kPrefWireStyle, {{"Curved", "curved"}, {"Straight", "straight"}, {"Right angle", "rightAngle"}}, "curved");
                        b.toggle("Dim wires away from the hovered and selected nodes", kPrefDimUnrelated, false);
                        b.toggle("One grey for every wire in a grey theme", kPrefGreyWires, false);
                    }});
    registerGnGraphBackend();
    registerSceneGraphBackend();
    actions::add({.id = "window.nodeEditor", .label = "Node Editor", .category = "Windows",
                  .run = [] {
                      for (NodeEditor* e : editors())
                          if (auto* d = qobject_cast<QDockWidget*>(e->parentWidget()); d && d->objectName() == kNodeEditorDock && dockShowing(d))
                          {
                              toggleDock(d);
                              return;
                          }
                      if (showHook())
                          showHook()();
                  },
                  .doc = "Node graph editor: geometry nodes, scene history and other graph types.",
                  .python = "lila.ui.run_action(\"window.nodeEditor\")"});
    actions::add({.id = "nodeEditor.showHistory", .label = "Show in Node Editor", .category = "Node Editor",
                  .run =
                      [] {
                          if (showHook())
                              showHook()();
                          for (NodeEditor* e : editors())
                              if (auto* d = qobject_cast<QDockWidget*>(e->parentWidget()); d && d->objectName() == kNodeEditorDock)
                              {
                                  e->setBackend(kSceneGraphBackend);
                                  e->setGroup("History");
                              }
                      },
                  .doc = "Open the Node Editor on the selection's history: its generator, modifiers and connections.",
                  .python = "lila.ui.run_action(\"nodeEditor.showHistory\")"});
    actions::add({.id = "gn.newGroup", .label = "New Geometry Nodes Group", .category = "Geometry Nodes",
                  .run =
                      [] {
                          GraphBackendPtr gnBackend = graphBackends::find(kGeometryNodesBackend);
                          if (!gnBackend)
                              return;
                          std::vector<std::string> names = gnBackend->groupNames(scene::Graph::current());
                          std::string name = "GeometryNodes";
                          for (int i = 1; std::find(names.begin(), names.end(), name) != names.end() || !scene::Graph::current().findAll(name).empty(); ++i)
                              name = "GeometryNodes" + std::to_string(i);
                          json r = runCmd("gnCreateGroup", args({name}));
                          if (!r.is_string())
                              return;
                          if (showHook())
                              showHook()();
                          for (NodeEditor* e : editors())
                              if (e->backend().id() == kGeometryNodesBackend)
                                  e->setGroup(qs(r.get<std::string>()));
                      },
                  .doc = "Make a geometry-nodes group and open it in the node editor.",
                  .python = "lila.ops.gnCreateGroup(\"GeometryNodes\")"});
}

void setNodeEditorShowHook(std::function<void()> show) { showHook() = std::move(show); }
} // namespace lila::ui
