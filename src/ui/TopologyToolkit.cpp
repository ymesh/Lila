#include "ui/TopologyToolkit.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "retopo/Retopo.h"
#include "retopo/RetopoCommands.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "ui/Actions.h"
#include "ui/DockSidebar.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/NodeLookup.h"
#include "ui/OptionBox.h"
#include "ui/RetopoDrawTool.h"
#include "ui/SpinScrub.h"
#include "ui/ToolkitLayout.h"
#include "ui/TopologyTool.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QShortcut>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <functional>
#include <memory>

namespace lila::ui
{
namespace
{
using retopo::ConstructKind;

constexpr const char* kTopologyCollapsed = "/topologyToolkit/collapsed";

/** The reference and retopo mesh name the toolkit shows: the running tool's, else the ones the selection resolves to. */
std::optional<retopo::RetopoDrawTargets> toolkitTargets()
{
    if (TopologyTool* tool = activeTopologyTool())
        return retopo::RetopoDrawTargets{tool->reference(), tool->retopoName()};
    if (RetopoDrawTool* draw = activeRetopoDraw())
        return retopo::RetopoDrawTargets{draw->reference(), draw->retopoName()};
    return retopo::resolveRetopoDraw(scene::Graph::current(), scene::Graph::current().selection());
}

/** The retopo mesh the toolkit shows, null until it exists. */
scene::Node* sessionRetopo(scene::NodeId* reference = nullptr)
{
    std::optional<retopo::RetopoDrawTargets> t = toolkitTargets();
    if (!t)
        return nullptr;
    if (reference)
        *reference = t->reference;
    return findNode(t->retopo);
}

/** One setting of a construct kind, edited in the active construct's panel. */
struct Field
{
    const char* key;
    const char* label;
    char type; // i(nt), d(ouble), b(ool)
    double min, max, fallback;
    /** Shown from the record's generated counts when the spec leaves it on auto (0); -1 for none. */
    int countIndex;
    const char* tip;
    const char* unit = "";
    /** The range is a real bound, so the field shows where the value sits in it. */
    bool slider = false;
};

std::vector<Field> fieldsFor(ConstructKind k)
{
    switch (k)
    {
    case ConstructKind::Patch:
        return {{"padding", "Padding", 'i', -1, 50, -1, -1, "Loops of quads kept along the whole border before the pattern; -1 uses the fewest that fit.", "", true},
                {"variant", "Variant", 'i', 0, 999, 0, -1, "Which pattern fills the border; Ctrl+Alt-wheel over the patch cycles them."}};
    case ConstructKind::Strip:
        return {{"divisions", "Along", 'i', 0, 999, 0, 0, "Quads along the strip; 0 keeps them near square."},
                {"rows", "Across", 'i', 1, 50, 1, -1, "Quads across the strip.", "", true},
                {"width", "Width", 'd', 0.01, 1e5, 1, -1, "Strip width.", "cm"}};
    case ConstructKind::Stroke: return {{"divisions", "Divisions", 'i', 1, 999, 8, -1, "Edges along the stroke."}};
    case ConstructKind::Bridge:
        return {{"divisions", "Along", 'i', 0, 999, 0, 0, "Edges along when neither side is an existing border; 0 from the length."},
                {"rows", "Across", 'i', 0, 999, 0, 1, "Edges across the gap; 0 from its width."}};
    case ConstructKind::Sweep:
        return {{"span", "Span", 'i', -999, 999, 1, -1, "Border edges carried along; negative takes the other side of the anchor."},
                {"rows", "Rows", 'i', 0, 999, 0, 0, "Rows along the stroke; 0 from its length."}};
    case ConstructKind::Cylinder:
        return {{"points", "Points", 'i', 3, 512, 8, -1, "Points around each ring."},
                {"rotate", "Rotate", 'd', -360, 360, 0, -1, "Turns where the rings start.", "°", true},
                {"twist", "Twist", 'd', -180, 180, 0, -1, "Extra turn per ring.", "°", true},
                {"rowDensity", "Row Density", 'd', 0.1, 10, 1, -1, "Rows between rings; 1 keeps quads near square.", "", true},
                {"capStart", "Cap Start", 'b', 0, 1, 0, -1, "Fill the first ring with a patch."},
                {"capEnd", "Cap End", 'b', 0, 1, 0, -1, "Fill the last ring with a patch."}};
    case ConstructKind::Region:
        return {{"targetFaces", "Faces", 'i', 0, 50000, 0, 0, "Faces the region aims for (Ctrl-wheel over it to step); editing Edge Length switches back to that."},
                {"edgeLength", "Edge Length", 'd', 0.01, 1e5, 1, -1, "Edge length the region aims for while Faces is on auto.", "cm"}};
    }
    return {};
}

/** The value a construct's field shows: the spec's, else its generated count while the spec leaves it on auto. */
double fieldValue(const retopo::ConstructRecord& r, const Field& f)
{
    double value = f.fallback;
    if (r.spec.contains(f.key) && r.spec[f.key].is_number())
        value = r.spec[f.key].get<double>();
    else if (r.spec.contains(f.key) && r.spec[f.key].is_boolean())
        value = r.spec[f.key].get<bool>() ? 1.0 : 0.0;
    if (value == 0.0 && f.countIndex >= 0 && size_t(f.countIndex) < r.counts.size())
        value = r.counts[f.countIndex];
    return value;
}

/** A label for a value, dim, for read-outs and empty states. */
QLabel* topologyHint(const QString& text, QWidget* parent)
{
    auto* l = new QLabel(text, parent);
    l->setWordWrap(true);
    controls::setRole(l, "hint");
    return l;
}

QFormLayout* topologyForm(QWidget* parent)
{
    auto* form = new QFormLayout(parent);
    form->setContentsMargins(0, 0, 0, 4);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    return form;
}

/** A push-styled button for a one-shot that isn't a registered action. */
QToolButton* topologyPush(const QString& text, const QString& tip, QWidget* parent)
{
    auto* b = new QToolButton(parent);
    b->setText(text);
    b->setToolTip(tip);
    b->setToolButtonStyle(Qt::ToolButtonTextOnly);
    b->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    controls::setRole(b, "push");
    return b;
}

const char* constructLabel(ConstructKind k)
{
    static const char* labels[] = {"Patch", "Strip", "Stroke", "Bridge", "Sweep", "Cylinder", "Region"};
    return labels[size_t(k)];
}

/** The grid of a group's buttons, skipping actions that aren't registered, with its overflow menu in the last cell. */
QWidget* topologyGroup(std::vector<QToolButton*> buttons, const std::vector<std::string>& more, int columns, QWidget* parent)
{
    std::vector<QWidget*> items;
    for (QToolButton* b : buttons)
        if (b)
            items.push_back(b);
    if (!more.empty())
        items.push_back(controls::overflow(more, parent));
    return controls::grid(items, columns, parent);
}
} // namespace

TopologyToolkit::TopologyToolkit(QWidget* parent) : QWidget(parent)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    outer->addWidget(scroll);
    auto* body = new QWidget(scroll);
    auto* root = new QVBoxLayout(body);
    root->setContentsMargins(10, 4, 10, 8);
    root->setSpacing(4);
    scroll->setWidget(body);
    controls::fitScrollWidth(scroll);

    auto add = [&](const char* title, QWidget* content, bool open = true) {
        root->addWidget(collapsibleSection(title, content, body, kTopologyCollapsed, open));
    };

    // Session: which meshes the tools work on, and the retopo mesh's mirror plane.
    auto* sessionBox = new QWidget(body);
    QFormLayout* session = topologyForm(sessionBox);
    mReference = topologyHint({}, sessionBox);
    mRetopo = topologyHint({}, sessionBox);
    session->addRow("Reference", mReference);
    session->addRow("Retopo", mRetopo);
    mSymmetry = new controls::Segmented(sessionBox);
    mSymmetry->setObjectName("topologySymmetry");
    for (auto [text, id] : {std::pair{"Off", 0}, {"X", 1}, {"Y", 2}, {"Z", 3}})
        mSymmetry->add(text, {}, id ? QString("Mirror the retopo mesh across its %1 plane: every topology tool builds both halves").arg(text)
                                    : QString("No mirroring"),
                       id);
    connect(mSymmetry, &controls::Segmented::picked, this, [this](int i) {
        std::optional<retopo::RetopoDrawTargets> t = toolkitTargets();
        scene::Node* ref = t ? scene::Graph::current().node(t->reference) : nullptr;
        if (!ref)
            return;
        static const char* axes[] = {"off", "x", "y", "z"};
        // Picking symmetry before the first edit makes the retopo mesh, so the choice has somewhere to live.
        UndoChunk chunk("Retopo Symmetry");
        if (!findNode(t->retopo))
            runUi("retopoDrawSetReference", {{"mesh", t->retopo}, {"reference", ref->path()}, {"create", true}});
        if (findNode(t->retopo))
            runUi("setSymmetry", {{"args", {t->retopo}}, {"axis", axes[std::clamp(i, 0, 3)]}});
        mRefresh->start();
    });
    session->addRow("Symmetry", mSymmetry);
    if (QToolButton* b = push("retopo.setReference", "Set Reference", sessionBox))
        session->addRow(QString(), b);
    add("Session", sessionBox);

    add("Draw", topologyGroup({tile("retopo.draw", "pencil", "Draw", body), tile("topo.patch", "square-plus", "Patch", body),
                               tile("topo.strip", "road", "Strip", body), tile("topo.stroke", "line", "Stroke", body),
                               tile("topo.cylinder", "cylinder", "Cylinder", body), tile("topo.cap", "grid-pattern", "Cap", body),
                               tile("topo.pencil", "pencil-plus", "Pencil", body),
                               tile("topo.handles", "hand-move", "Handles", body), tile("topo.relax", "wave-sine", "Relax", body)},
                              {}, kToolkitColumns, body));

    // Settings the tools share.
    auto* settingsBox = new QWidget(body);
    QFormLayout* form = topologyForm(settingsBox);
    form->addRow("Brush", prefField("/topology/brush", 40.0, 4.0, 600.0, 0, "px", true,
                                    "Brush radius on screen: strip width, region and relax brushes, handle reach (Shift-wheel)", settingsBox));
    form->addRow("Edge Length", prefField("/topology/edgeLength", 0.0, 0.0, 1e5, 2, "cm", false,
                                          "Edge length new constructs aim for; 0 matches the retopo mesh's own", settingsBox));
    form->addRow("Strip Rows", prefField("/topology/stripRows", 1, 1, 50, 0, {}, true, "Quads across a new strip", settingsBox));
    form->addRow("Cylinder Points", prefField("/topology/cylinderPoints", 8, 3, 512, 0, {}, true, "Points around a new cylinder's rings", settingsBox));
    form->addRow("Lazy Radius", prefField("/topology/pencil/lazy", 10.0, 0.0, 200.0, 0, "px", true,
                                          "Pencil: the line trails the cursor by this much, steadying it; 0 draws at the cursor", settingsBox));
    form->addRow("Smoothing", prefField("/topology/pencil/smoothing", 0.3, 0.0, 1.0, 2, {}, true,
                                        "Pencil: how much a finished stroke is smoothed", settingsBox));
    form->addRow("Even Size", prefField("/topology/relax/evenSize", 0.5, 0.0, 1.0, 2, {}, true,
                                        "Relax: 0 keeps each face's size, 1 evens every face to one size", settingsBox));
    form->addRow("Squareness", prefField("/topology/relax/squareness", 0.7, 0.0, 1.0, 2, {}, true,
                                         "Relax: 0 only evens spacing, 1 pulls faces fully square", settingsBox));
    form->addRow("Stiffness", prefField("/topology/relax/stiffness", 0.5, 0.05, 1.0, 2, {}, true, "Relax: how far each step goes", settingsBox));
    form->addRow("Handle Falloff", prefField("/topology/handleFalloff", 0.5, 0.0, 1.0, 2, {}, true,
                                             "Handles: how much of the region fades out toward its border", settingsBox));
    add("Settings", settingsBox);

    // The active construct.
    auto* activeBox = new QWidget(body);
    auto* activeLayout = new QVBoxLayout(activeBox);
    activeLayout->setContentsMargins(0, 0, 0, 4);
    activeLayout->setSpacing(4);
    mActiveLabel = topologyHint({}, activeBox);
    activeLayout->addWidget(mActiveLabel);
    mConstructPanel = new QWidget(activeBox);
    mConstructLayout = new QVBoxLayout(mConstructPanel);
    mConstructLayout->setContentsMargins(0, 0, 0, 0);
    activeLayout->addWidget(mConstructPanel);
    QToolButton* freeze = topologyPush("Freeze", "Freeze: leave the active construct as plain faces (Enter)", activeBox);
    connect(freeze, &QToolButton::clicked, this, [] {
        TopologyTool* t = activeTopologyTool();
        if (t && t->activeConstruct())
            runUi("retopoConstructFreeze", {{"mesh", t->retopoName()}, {"ids", {t->activeConstruct()}}});
    });
    QToolButton* remove = topologyPush("Delete", "Delete: remove the active construct and its faces; the next one up becomes active (Delete in the list)", activeBox);
    connect(remove, &QToolButton::clicked, this, [this] { deleteActiveConstruct(); });
    freeze->setObjectName("topologyFreeze");
    remove->setObjectName("topologyDelete");
    activeLayout->addWidget(controls::grid({freeze, remove}, 2, activeBox));
    add("Active Construct", activeBox);

    // Every live construct, and the guides.
    auto* listBox = new QWidget(body);
    auto* listLayout = new QVBoxLayout(listBox);
    listLayout->setContentsMargins(0, 0, 0, 4);
    listLayout->setSpacing(4);
    mList = new QListWidget(listBox);
    mList->setMinimumHeight(90);
    mList->setToolTip("Live constructs on the retopo mesh; click one to make it active");
    connect(mList, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        int id = item->data(Qt::UserRole).toInt();
        if (TopologyTool* t = activeTopologyTool())
            t->setActiveConstruct(id);
        mRefresh->start();
    });
    listLayout->addWidget(mList);
    auto* del = new QShortcut(QKeySequence::Delete, mList, nullptr, nullptr, Qt::WidgetShortcut);
    connect(del, &QShortcut::activated, this, [this] { deleteActiveConstruct(); });
    QToolButton* clearGuides = topologyPush("Clear Guides", "Clear Guides: remove every guide drawn with the Pencil", listBox);
    connect(clearGuides, &QToolButton::clicked, this, [] {
        if (scene::Node* n = sessionRetopo())
            runUi("retopoSetGuides", {{"mesh", n->path()}, {"guides", json::array()}});
    });
    listLayout->addWidget(topologyGroup({push("topo.freezeAll", "Freeze All", listBox), clearGuides}, {}, 2, listBox));
    add("Constructs", listBox, false);

    add("Auto Retopo", topologyGroup({push("topo.runRegion", "Retopo Region", body), push("topo.region", "Region Brush", body),
                                      push("mesh.quadRemesh", "Quad Remesh", body), push("mesh.voxelRemesh", "Voxel Remesh", body),
                                      push("topo.settle", "Settle", body), push("mesh.relax", "Relax Mesh", body)},
                                     {"retopo.poles"}, 2, body));

    // Quality.
    auto* qualityBox = new QWidget(body);
    auto* qualityLayout = new QVBoxLayout(qualityBox);
    qualityLayout->setContentsMargins(0, 0, 0, 4);
    qualityLayout->setSpacing(4);
    mStats = topologyHint({}, qualityBox);
    qualityLayout->addWidget(mStats);
    if (QToolButton* b = push("topo.qualityOverlay", "Quality Overlay", qualityBox))
        qualityLayout->addWidget(controls::grid({b}, 2, qualityBox));
    add("Quality", qualityBox, false);
    root->addStretch(1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(60);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshState(); });
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) { mRefresh->start(); });
    mToolListener = viewportTools::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    refreshState();
}

TopologyToolkit::~TopologyToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    viewportTools::removeListener(mToolListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* TopologyToolkit::track(QToolButton* b, const std::string& id)
{
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

QToolButton* TopologyToolkit::tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::tile(id, iconName, parent, label), id) : nullptr;
}

QToolButton* TopologyToolkit::push(const std::string& id, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::push(id, parent, label), id) : nullptr;
}

QDoubleSpinBox* TopologyToolkit::prefField(const char* key, double def, double lo, double hi, int decimals, const QString& unit, bool slider,
                                           const char* tip, QWidget* parent)
{
    QDoubleSpinBox* s = nullptr;
    if (slider)
    {
        s = new controls::SliderField(parent);
        s->setRange(lo, hi);
        s->setDecimals(decimals);
        if (!unit.isEmpty())
            s->setSuffix(" " + unit);
    }
    else
        s = controls::valueField(lo, hi, decimals, unit, parent);
    s->setValue(PrefStore::get().value<double>("general", key, def));
    s->setToolTip(tip);
    s->setObjectName(key);
    setSpinDefault(s, def);
    connect(s, &QDoubleSpinBox::valueChanged, this, [key, decimals](double v) {
        if (decimals == 0)
            PrefStore::get().set("general", key, int(std::lround(v)));
        else
            PrefStore::get().set("general", key, v);
    });
    return s;
}

void TopologyToolkit::deleteActiveConstruct()
{
    TopologyTool* t = activeTopologyTool();
    scene::Node* retopo = sessionRetopo();
    if (!t || !retopo || !t->activeConstruct())
        return;
    // The one listed above becomes active (or below, for the first), so a run of deletes needs no clicking in between.
    const int id = t->activeConstruct();
    std::vector<retopo::ConstructRecord> records = retopo::retopoConstructs(scene::Graph::current(), *retopo);
    int next = 0;
    for (size_t i = 0; i < records.size(); ++i)
        if (records[i].id == id)
            next = i > 0 ? records[i - 1].id : i + 1 < records.size() ? records[i + 1].id : 0;
    runUi("retopoConstructDelete", {{"mesh", t->retopoName()}, {"ids", {id}}});
    if (TopologyTool* still = activeTopologyTool())
        still->setActiveConstruct(next);
    mRefresh->start();
}

void TopologyToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshState();
}

void TopologyToolkit::refreshState()
{
    TopologyTool* tool = activeTopologyTool();
    for (QToolButton* b : mButtons)
    {
        const std::string id = b->property("lilaAction").toString().toStdString();
        const Action* a = actions::find(id);
        if (!a)
            continue;
        // Hotkeys are live-editable, so tooltips pick up the current keys.
        QString tip = QString::fromStdString(actions::tooltip(id));
        if (optionBox::available(id))
            tip += "\nClick corner, right-click or Shift+click for options";
        b->setToolTip(tip);
        if (a->checked)
        {
            b->setCheckable(true);
            b->setChecked(a->checked());
        }
    }
    scene::NodeId refId = scene::kNoNode;
    scene::Node* retopo = sessionRetopo(&refId);
    scene::Node* ref = scene::Graph::current().node(refId);
    mReference->setText(ref ? QString::fromStdString(ref->path()) : QString("None. Select a mesh and click Set Reference."));
    mRetopo->setText(retopo ? QString::fromStdString(retopo->path()) : QString("None yet"));
    {
        // Until the retopo mesh exists it will take the reference's symmetry, so that is what shows.
        const scene::Node* symmetrySource = retopo ? retopo : ref;
        QSignalBlocker block(mSymmetry);
        mSymmetry->setEnabled(ref != nullptr);
        mSymmetry->setCurrent(symmetrySource ? int(std::clamp<int64_t>(symmetrySource->as<int64_t>(scene::MeshNode::kSymmetry), 0, 3)) : 0);
    }
    for (QDoubleSpinBox* s : findChildren<QDoubleSpinBox*>())
        if (s->objectName().startsWith("/topology/") && !s->hasFocus())
        {
            QSignalBlocker block(s);
            s->setValue(PrefStore::get().value<double>("general", s->objectName().toStdString(), s->value()));
        }

    std::vector<retopo::ConstructRecord> records;
    if (retopo)
        records = retopo::retopoConstructs(scene::Graph::current(), *retopo);
    mList->clear();
    for (const retopo::ConstructRecord& r : records)
    {
        auto* item = new QListWidgetItem(QString("#%1  %2%3").arg(r.id).arg(constructLabel(r.kind)).arg(r.live ? "" : "  (edited, freezes on next change)"), mList);
        item->setData(Qt::UserRole, r.id);
        if (tool && tool->activeConstruct() == r.id)
            item->setSelected(true);
    }
    rebuildConstructPanel();

    QString stats = "No retopo mesh yet.";
    if (retopo)
    {
        scene::Graph::current().evaluate();
        if (scene::MeshPtr m = retopo->as<scene::MeshPtr>(scene::MeshNode::kOutMesh))
        {
            retopo::QualityMetrics q = retopo::measureQuality(*m);
            auto valence = [&](size_t k) { return k < q.valence.size() ? q.valence[k] : 0; };
            geo::Index high = 0;
            for (size_t k = 6; k < q.valence.size(); ++k)
                high += q.valence[k];
            stats = QString("%1 faces, %2% quads, %3 tris, %4 n-gons\nPoles: %5 three, %6 five, %7 six or more")
                        .arg(q.faces)
                        .arg(q.quadPercent, 0, 'f', 1)
                        .arg(q.triangles)
                        .arg(q.ngons)
                        .arg(valence(3))
                        .arg(valence(5))
                        .arg(high);
        }
    }
    mStats->setText(stats);
}

void TopologyToolkit::rebuildConstructPanel()
{
    TopologyTool* tool = activeTopologyTool();
    scene::Node* retopo = sessionRetopo();
    const retopo::ConstructRecord* active = nullptr;
    std::vector<retopo::ConstructRecord> records;
    if (tool && retopo && tool->activeConstruct())
    {
        records = retopo::retopoConstructs(scene::Graph::current(), *retopo);
        for (const retopo::ConstructRecord& r : records)
            if (r.id == tool->activeConstruct())
                active = &r;
    }
    if (QToolButton* b = findChild<QToolButton*>("topologyFreeze"))
        b->setEnabled(active != nullptr);
    if (QToolButton* b = findChild<QToolButton*>("topologyDelete"))
        b->setEnabled(active != nullptr);
    // Rebuilt only when the construct changes, so a field being dragged or typed in keeps its focus; values update in place.
    std::string key = active ? std::to_string(active->id) + std::to_string(active->live) : tool ? "tool" : "none";
    if (key == mPanelKey)
    {
        if (active)
            for (const Field& f : fieldsFor(active->kind))
            {
                QWidget* w = mConstructPanel->findChild<QWidget*>(QString("construct/") + f.key);
                if (!w || w->hasFocus() || w->isAncestorOf(QApplication::focusWidget()))
                    continue;
                QSignalBlocker block(w);
                if (auto* c = qobject_cast<QCheckBox*>(w))
                    c->setChecked(fieldValue(*active, f) != 0.0);
                else if (auto* sp = qobject_cast<QDoubleSpinBox*>(w))
                    sp->setValue(fieldValue(*active, f));
            }
        return;
    }
    mPanelKey = key;
    while (QLayoutItem* item = mConstructLayout->takeAt(0))
    {
        delete item->widget();
        delete item;
    }
    if (!active)
    {
        mActiveLabel->setText(tool ? "No active construct. Make one, or click one in the viewport." : "No topology tool running. Pick one under Draw.");
        return;
    }
    mActiveLabel->setText(QString("#%1 %2%3").arg(active->id).arg(constructLabel(active->kind)).arg(active->live ? "" : " (edited by hand)"));
    auto* form = new QWidget(mConstructPanel);
    QFormLayout* layout = topologyForm(form);
    const int id = active->id;
    for (const Field& f : fieldsFor(active->kind))
    {
        std::string fieldKey = f.key;
        const double value = fieldValue(*active, f);
        auto edit = [id, fieldKey](json v) {
            if (TopologyTool* t = activeTopologyTool())
                t->editConstruct(id, [&](json& spec) {
                    spec[fieldKey] = v;
                    if (fieldKey == "edgeLength")
                        spec.erase("targetFaces");
                });
        };
        // An auto region takes seconds to remesh, so a scrub applies once it settles rather than at every step.
        std::function<void(json)> apply = edit;
        if (active->kind == ConstructKind::Region)
        {
            auto* settle = new QTimer(form);
            settle->setSingleShot(true);
            settle->setInterval(400);
            auto pending = std::make_shared<json>();
            connect(settle, &QTimer::timeout, form, [edit, pending] { edit(*pending); });
            apply = [settle, pending](json v) {
                *pending = std::move(v);
                settle->start();
            };
        }
        QWidget* w = nullptr;
        if (f.type == 'b')
        {
            auto* c = new QCheckBox(form);
            c->setChecked(value != 0.0);
            setCheckDefault(c, f.fallback != 0.0);
            connect(c, &QCheckBox::toggled, form, [apply](bool on) { apply(on); });
            w = c;
        }
        else
        {
            const int decimals = f.type == 'i' ? 0 : 2;
            QDoubleSpinBox* s = nullptr;
            if (f.slider)
            {
                s = new controls::SliderField(form);
                s->setRange(f.min, f.max);
                s->setDecimals(decimals);
                if (*f.unit)
                    s->setSuffix(QString(" ") + f.unit);
            }
            else
                s = controls::valueField(f.min, f.max, decimals, f.unit, form);
            s->setValue(value);
            // Typing applies on Enter, not per keystroke: every change regenerates the construct.
            s->setKeyboardTracking(false);
            setSpinDefault(s, f.fallback);
            if (f.type == 'i')
                connect(s, &QDoubleSpinBox::valueChanged, form, [apply](double v) { apply(int(std::lround(v))); });
            else
                connect(s, &QDoubleSpinBox::valueChanged, form, [apply](double v) { apply(v); });
            w = s;
        }
        w->setToolTip(f.tip);
        w->setObjectName(QString("construct/") + f.key);
        layout->addRow(f.label, w);
    }
    if (active->kind == ConstructKind::Patch)
    {
        QToolButton* reset = topologyPush("Reset Pull", "Reset Pull: put the patch's poles back where its pattern places them (double-click the patch)", form);
        connect(reset, &QToolButton::clicked, form, [id] {
            if (TopologyTool* t = activeTopologyTool())
                t->editConstruct(id, [](json& spec) { spec.erase("pull"); });
        });
        layout->addRow(QString(), reset);
    }
    if (active->kind == ConstructKind::Cylinder)
    {
        QToolButton* finish = topologyPush("Finish Cylinder", "Finish Cylinder: the next line starts a new cylinder (Enter freezes this one instead)", form);
        connect(finish, &QToolButton::clicked, form, [] {
            if (TopologyTool* t = activeTopologyTool())
                t->finishCylinder();
        });
        layout->addRow(QString(), finish);
    }
    mConstructLayout->addWidget(form);
}

void registerTopologyToolkitActions()
{
    actions::add({.id = "window.topologyToolkit", .label = "Topology Toolkit", .category = "Windows", .run = [] {
                      toggleDock(findDock(kTopologyToolkitDock));
                  },
                  .doc = "The retopo session, the topology tools and their settings, live constructs, guides, auto retopo and quality.",
                  .python = "lila.ui.run_action(\"window.topologyToolkit\")"});
}
} // namespace lila::ui
