#include "ui/LayoutPencil.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Normals.h"
#include "geo/Triangulate.h"
#include "layout/Edit.h"
#include "layout/LayoutNode.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/CurveToolsCommon.h"
#include "ui/FileDialog.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/ToolColors.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportToolMath.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

namespace lila::ui
{
// Named rather than anonymous: unity builds share a translation unit with other tools' palettes.
namespace layoutPencil
{
using scene::Graph;
using scene::NodeId;
using namespace lila::layout;

constexpr const char* kPaletteId = "layoutPencilPalette";
constexpr const char* kToolId = "layout.pencil";
constexpr const char* kPrefs = "/layoutPencil/";
const std::vector<std::string> kOutputLabels = {"Geometry", "Sculpt", "Texture", "Decal", "None"};
const std::vector<std::string> kProfileLabels = {"V", "U", "Square", "Step", "Chamfer"};

double prefD(const char* key, double def) { return PrefStore::get().value<double>("general", std::string(kPrefs) + key, def); }
bool prefB(const char* key, bool def) { return PrefStore::get().value<bool>("general", std::string(kPrefs) + key, def); }
void setPref(const char* key, const json& v) { PrefStore::get().set("general", std::string(kPrefs) + key, v); }

enum class Mode
{
    Lines,
    Plates,
    Fasteners,
    Patterns,
};

/** What the pencil draws with, kept in the prefs. */
struct PencilSettings
{
    Mode mode = Mode::Lines;
    double cleanup = 0.5;
    double cornerAngle = 50.0;
    /** Join and pick distance, screen pixels. */
    double joinPx = 10.0;
    double smoothRadiusPx = 40.0;
    double smoothStrength = 0.6;
    bool keys = true;
    /** A closed stroke also gets spokes running out from it (panelLayoutLoopSpokes). */
    bool spokes = false;
    int spokeCount = 8;
    double spokeLength = 0.0;
    double spokeBias = 1.0;

    void load()
    {
        cleanup = prefD("cleanup", 0.5);
        cornerAngle = prefD("cornerAngle", 50.0);
        joinPx = prefD("joinPx", 10.0);
        smoothRadiusPx = prefD("smoothRadiusPx", 40.0);
        smoothStrength = prefD("smoothStrength", 0.6);
        keys = prefB("keys", true);
        spokes = prefB("spokes", false);
        spokeCount = std::clamp(int(std::lround(prefD("spokeCount", 8.0))), 3, 64);
        spokeLength = prefD("spokeLength", 0.0);
        spokeBias = prefD("spokeBias", 1.0);
        mode = Mode(std::clamp(int(std::lround(prefD("mode", 0.0))), 0, 3));
    }
};

PencilSettings& settings()
{
    static PencilSettings s = [] {
        PencilSettings p;
        p.load();
        return p;
    }();
    return s;
}

json run(const char* cmd, json call)
{
    try
    {
        return ops::execute(cmd, call, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::error("{}", e.what());
        return nullptr;
    }
}

json jsonPoint(const glm::dvec3& p)
{
    auto r = [](double v) { return std::round(v * 1000.0) / 1000.0; };
    return {r(p.x), r(p.y), r(p.z)};
}

/** The mesh the pencil draws on, its layout if it has one, and the surface lines lie on. */
struct Target
{
    scene::Node* mesh = nullptr;
    scene::Node* layout = nullptr;
    glm::dmat4 world{1.0};
    glm::dmat4 toLocal{1.0};
    scene::MeshPtr surface;

    explicit operator bool() const { return mesh && surface && surface->numFaces() > 0; }
    glm::dvec3 toWorld(const glm::dvec3& p) const { return glm::dvec3(world * glm::dvec4(p, 1.0)); }
    glm::dvec3 local(const glm::dvec3& p) const { return glm::dvec3(toLocal * glm::dvec4(p, 1.0)); }
    glm::dvec3 localDir(const glm::dvec3& d) const { return glm::dvec3(toLocal * glm::dvec4(d, 0.0)); }
    std::shared_ptr<const Resolved> resolved() const
    {
        return layout ? resolvedOf(layout->as<scene::DataPtr>(PanelLayoutNode::kOutLayout)) : nullptr;
    }
    const PanelLayout* authored() const { return layout ? layoutOf(layout->as<scene::DataPtr>(PanelLayoutNode::kLayout)) : nullptr; }
};

Target currentTarget()
{
    Graph& g = Graph::current();
    Target t;
    for (NodeId id : g.selection())
    {
        scene::Node* n = g.node(id);
        if (!n)
            continue;
        if (n->type().name == kLayoutType)
        {
            t.layout = n;
            t.mesh = g.node(scene::modifiers::meshOf(g, id));
            break;
        }
        if (n->type().isA("mesh"))
        {
            t.mesh = n;
            t.layout = findLayoutNode(g, id);
            break;
        }
    }
    if (!t.mesh)
        return t;
    if (t.mesh->type().isA("transform"))
        t.world = t.mesh->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    t.toLocal = glm::inverse(t.world);
    t.surface = t.layout ? t.layout->as<scene::MeshPtr>(PanelLayoutNode::kInMesh) : t.mesh->as<scene::MeshPtr>(scene::MeshNode::kOutMesh);
    return t;
}

/** The surface of a mesh, kept while the mesh stays the same. */
const Surface* surfaceFor(const scene::MeshPtr& mesh)
{
    static scene::MeshPtr cached;
    static std::unique_ptr<Surface> surface;
    if (!mesh)
        return nullptr;
    if (mesh != cached)
    {
        cached = mesh;
        surface = std::make_unique<Surface>(*mesh);
    }
    return surface.get();
}

std::string setName(const PanelLayout* l, uint32_t id)
{
    if (l)
        if (const LineSet* s = l->set(id))
            return s->name;
    return {};
}

void paletteChanged();

/** The panel clicked in Plates mode: its mesh and a point inside it, in the mesh's space. */
struct PanelPick
{
    NodeId mesh = scene::kNoNode;
    glm::dvec3 local{0.0};
};

std::optional<PanelPick>& pickedPanel()
{
    static std::optional<PanelPick> pick;
    return pick;
}

/** The plates modifier on the target mesh's stack, or null. */
scene::Node* platesNode(const Target& t)
{
    if (!t.mesh)
        return nullptr;
    Graph& g = Graph::current();
    const std::vector<NodeId> stack = scene::modifiers::stack(g, t.mesh->id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().name == "platesModifier")
            return n;
    return nullptr;
}

/** The fasteners modifier on the target mesh's stack, or null. */
scene::Node* fastenersNode(const Target& t)
{
    if (!t.mesh)
        return nullptr;
    Graph& g = Graph::current();
    const std::vector<NodeId> stack = scene::modifiers::stack(g, t.mesh->id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().name == "fastenersModifier")
            return n;
    return nullptr;
}

/** The surface pattern modifier on the target mesh's stack, or null. */
scene::Node* patternNode(const Target& t)
{
    if (!t.mesh)
        return nullptr;
    Graph& g = Graph::current();
    const std::vector<NodeId> stack = scene::modifiers::stack(g, t.mesh->id());
    for (auto it = stack.rbegin(); it != stack.rend(); ++it)
        if (scene::Node* n = g.node(*it); n && n->type().name == "patternModifier")
            return n;
    return nullptr;
}

/** Modes where a click picks a panel rather than a line. */
bool picksPanels(Mode m) { return m == Mode::Plates || m == Mode::Patterns; }

/** The values the picked panel has of its own, from the layout's panel overrides; null when no panel is picked. */
json pickedValues(const Target& t)
{
    const std::optional<PanelPick>& pick = pickedPanel();
    const PanelLayout* l = t.authored();
    const std::shared_ptr<const Resolved> r = t.resolved();
    if (!pick || !l || !r || !t.mesh || pick->mesh != t.mesh->id())
        return nullptr;
    const PanelSample at = r->panelAt(pick->local);
    if (at.panel < 0)
        return nullptr;
    return at.override >= 0 && size_t(at.override) < l->panels.size() ? l->panels[size_t(at.override)].values : json::object();
}

/**
 * The Layout Pencil's palette: the line sets (where new lines go and how each set renders), the active set's or the
 * selected lines' width, depth and profile, the pencil's cleanup, and the mirror.
 */
class LayoutPalette final : public QWidget
{
public:
    explicit LayoutPalette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "layoutPencilPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);

        mMode = new controls::Segmented(this);
        mMode->add("Lines", {}, "Lines: draw, redraw and shape panel lines.", 0);
        mMode->add("Plates", {}, "Plates: turn the panels into plates; click a panel to give it its own height.", 1);
        mMode->add("Fasteners", {}, "Fasteners: rivets, bolts and screws along the lines or in panel corners.", 2);
        mMode->add("Patterns", {}, "Patterns: hex, squares, diamond plate and more pressed into the panels.", 3);
        connect(mMode, &controls::Segmented::picked, this, [this](int id) {
            settings().mode = Mode(id);
            setPref("mode", id);
            sync();
            viewportTools::requestRedraw();
        });
        v->addWidget(mMode);

        mEmpty = new QLabel("No panel layout on the selection. Draw on a mesh to start one.", this);
        mEmpty->setWordWrap(true);
        controls::setRole(mEmpty, "dim");
        v->addWidget(mEmpty);

        auto* setsBox = new QWidget(this);
        auto* sv = new QVBoxLayout(setsBox);
        sv->setContentsMargins(0, 0, 0, 0);
        sv->setSpacing(4);
        mSets = new QTreeWidget(setsBox);
        mSets->setColumnCount(2);
        mSets->setHeaderHidden(true);
        mSets->setRootIsDecorated(false);
        mSets->setIndentation(0);
        mSets->setSelectionMode(QAbstractItemView::SingleSelection);
        mSets->setContextMenuPolicy(Qt::CustomContextMenu);
        mSets->header()->setStretchLastSection(false);
        mSets->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mSets->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        mSets->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        mSets->setToolTip("Line sets: new lines go to the highlighted set. Right-click to rename, change the output or remove.");
        connect(mSets, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item) {
            if (!mSyncing && item)
                layoutCall("panelLayoutLineSet", {{"action", "activate"}, {"set", item->text(0).toStdString()}});
        });
        connect(mSets, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) { setMenu(at); });
        sv->addWidget(mSets);
        auto* add = new QToolButton(setsBox);
        add->setIcon(icon("plus"));
        add->setToolTip("Add a line set");
        add->setAutoRaise(true);
        connect(add, &QToolButton::clicked, this, [this] {
            const std::string name = freeSetName();
            layoutCall("panelLayoutLineSet", {{"action", "add"}, {"set", name}});
        });
        auto* addRow = new QHBoxLayout;
        addRow->addStretch();
        addRow->addWidget(add);
        sv->addLayout(addRow);
        mSetsSection = controls::section("Line Sets", setsBox, this, kSections);
        v->addWidget(mSetsSection);

        auto* lineBox = new QWidget(this);
        auto* form = new QFormLayout(lineBox);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        mOutput = new QComboBox(lineBox);
        for (const std::string& o : kOutputLabels)
            mOutput->addItem(QString::fromStdString(o));
        mOutput->setToolTip("Output: how the active set's lines render. Geometry cuts grooves into the mesh, Sculpt sinks them into a dense "
                            "mesh, Texture and Decal leave the mesh alone for texturing and decals, None only divides panels.");
        connect(mOutput, &QComboBox::activated, this, [this](int i) {
            layoutCall("panelLayoutLineSet", {{"set", mActiveSet}, {"output", outputName(Output(i))}});
        });
        form->addRow("Output", mOutput);
        mMoveTo = new QComboBox(lineBox);
        mMoveTo->setToolTip("Set: the line set the selected lines belong to, which says how they render.");
        connect(mMoveTo, &QComboBox::activated, this, [this](int) {
            if (!mSyncing && !mSelected.empty())
                layoutCall("panelLayoutSetRuns", {{"runs", mSelected}, {"set", mMoveTo->currentText().toStdString()}});
        });
        form->addRow("Set", mMoveTo);
        mForm = form;
        mWidth = controls::valueField(0.0, 1e4, 3, "cm", lineBox);
        mWidth->setObjectName("width");
        connect(mWidth, &QDoubleSpinBox::editingFinished, this, [this] { setValue("width", mWidth->value()); });
        form->addRow("Width", mWidth);
        mDepth = controls::valueField(-1e4, 1e4, 3, "cm", lineBox);
        mDepth->setObjectName("depth");
        connect(mDepth, &QDoubleSpinBox::editingFinished, this, [this] { setValue("depth", mDepth->value()); });
        form->addRow("Depth", mDepth);
        mProfile = new controls::Segmented(lineBox);
        for (size_t k = 0; k < kProfileLabels.size(); ++k)
            mProfile->add(QString::fromStdString(kProfileLabels[k]), {}, QString("Profile: %1 groove").arg(QString::fromStdString(kProfileLabels[k])), int(k));
        connect(mProfile, &controls::Segmented::picked, this, [this](int id) { setValue("profile", profileName(Profile(id))); });
        form->addRow("Profile", mProfile);
        mLineSection = controls::section("Lines", lineBox, this, kSections, true, &mLineHeader);
        v->addWidget(mLineSection);

        auto* pencilBox = new QWidget(this);
        auto* pf = new QFormLayout(pencilBox);
        pf->setContentsMargins(0, 0, 0, 0);
        pf->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pf->setVerticalSpacing(4);
        mCleanup = new controls::SliderField(pencilBox);
        mCleanup->setObjectName("cleanup");
        mCleanup->setRange(0.0, 1.0);
        mCleanup->setSingleStep(0.05);
        mCleanup->setDecimals(2);
        mCleanup->setToolTip("Cleanup: 0 keeps the drawn wobble, 1 fits clean straights and arcs. Changing it refits loosened lines.");
        setSpinDefault(mCleanup, 0.5);
        connect(mCleanup, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (mSyncing)
                return;
            settings().cleanup = value;
            setPref("cleanup", value);
            refitLoose();
        });
        pf->addRow("Cleanup", mCleanup);
        mCorner = controls::valueField(5.0, 175.0, 0, QString::fromUtf8("°"), pencilBox);
        mCorner->setObjectName("cornerAngle");
        mCorner->setToolTip("Corner Angle: turns sharper than this become corners.");
        setSpinDefault(mCorner, 50.0);
        connect(mCorner, &QDoubleSpinBox::valueChanged, this, [this](double value) {
            if (mSyncing)
                return;
            settings().cornerAngle = value;
            setPref("cornerAngle", value);
        });
        pf->addRow("Corner Angle", mCorner);
        mMirror = new QCheckBox("Mirror X", pencilBox);
        mMirror->setToolTip("Mirror X: lines drawn on one side appear on the other, across the mesh's YZ plane.");
        connect(mMirror, &QCheckBox::toggled, this, [this](bool on) {
            if (mSyncing)
                return;
            // Without a layout yet, the pencil starts the next one mirrored.
            setPref("mirror", on);
            layoutCall("panelLayoutMirror", {{"axis", on ? "x" : "off"}});
        });
        pf->addRow(mMirror);
        mKeys = new QCheckBox("Show Keys", pencilBox);
        mKeys->setToolTip("Show Keys: list the pencil's keys in the corner of the view.");
        setCheckDefault(mKeys, true);
        connect(mKeys, &QCheckBox::toggled, this, [this](bool on) {
            if (mSyncing)
                return;
            settings().keys = on;
            setPref("keys", on);
            viewportTools::requestRedraw();
        });
        pf->addRow(mKeys);
        mSpokes = new QCheckBox("Loop Spokes", pencilBox);
        mSpokes->setObjectName("spokes");
        mSpokes->setToolTip("Loop Spokes: a stroke that closes on itself also gets spokes running out across the surface, "
                            "crowded toward its corners. For mouths and eyes under a Soft Net.");
        connect(mSpokes, &QCheckBox::toggled, this, [this](bool on) {
            mSpokeBox->setVisible(on);
            if (mSyncing)
                return;
            settings().spokes = on;
            setPref("spokes", on);
        });
        pf->addRow(mSpokes);
        mSpokeBox = new QWidget(pencilBox);
        auto* sf = new QFormLayout(mSpokeBox);
        sf->setContentsMargins(0, 0, 0, 0);
        sf->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        sf->setVerticalSpacing(4);
        mSpokeCount = controls::valueField(3.0, 64.0, 0, QString(), mSpokeBox);
        mSpokeCount->setObjectName("spokeCount");
        mSpokeCount->setToolTip("Spokes: how many run out from the loop.");
        setSpinDefault(mSpokeCount, 8.0);
        connect(mSpokeCount, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (mSyncing)
                return;
            settings().spokeCount = int(std::lround(v));
            setPref("spokeCount", v);
        });
        sf->addRow("Spokes", mSpokeCount);
        mSpokeLength = controls::valueField(0.0, 1e4, 2, "cm", mSpokeBox);
        mSpokeLength->setObjectName("spokeLength");
        mSpokeLength->setToolTip("Spoke Length: 0 is 0.6 of the loop's radius.");
        setSpinDefault(mSpokeLength, 0.0);
        connect(mSpokeLength, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (mSyncing)
                return;
            settings().spokeLength = v;
            setPref("spokeLength", v);
        });
        sf->addRow("Length", mSpokeLength);
        mSpokeBias = controls::valueField(0.0, 10.0, 2, QString(), mSpokeBox);
        mSpokeBias->setObjectName("spokeBias");
        mSpokeBias->setToolTip("Corner Bias: how strongly spokes crowd toward the loop's corners; 0 spaces them evenly.");
        setSpinDefault(mSpokeBias, 1.0);
        connect(mSpokeBias, &QDoubleSpinBox::valueChanged, this, [this](double v) {
            if (mSyncing)
                return;
            settings().spokeBias = v;
            setPref("spokeBias", v);
        });
        sf->addRow("Corner Bias", mSpokeBias);
        pf->addRow(mSpokeBox);
        mSpokeBox->setVisible(settings().spokes);
        buildPlates(v, kSections);
        buildFasteners(v, kSections);
        buildPatterns(v, kSections);
        mPencilSection = controls::section("Pencil", pencilBox, this, kSections);
        v->addWidget(mPencilSection);
        mOverflow = controls::overflow({"layout.loosen", "layout.deleteLines", "layout.bakeMaps", "layout.unrealMaterials", "layout.clear"}, this);
        v->addWidget(mOverflow);
        v->addStretch();

        mRefresh = new QTimer(this);
        mRefresh->setSingleShot(true);
        mRefresh->setInterval(60);
        connect(mRefresh, &QTimer::timeout, this, [this] { sync(); });
        mGraphListener = Graph::current().addListener([this](scene::Change, NodeId) { mRefresh->start(); });
        sync();
    }

    ~LayoutPalette() override { Graph::current().removeListener(mGraphListener); }

    void buildPlates(QVBoxLayout* v, const char* sections)
    {
        auto* box = new QWidget(this);
        auto* bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(4);
        mNoPlates = new QWidget(box);
        auto* nv = new QVBoxLayout(mNoPlates);
        nv->setContentsMargins(0, 0, 0, 0);
        auto* hint = new QLabel("No plates on this mesh.", mNoPlates);
        controls::setRole(hint, "dim");
        nv->addWidget(hint);
        auto* make = new QToolButton(mNoPlates);
        make->setText("Make Plates");
        make->setToolTip("Make Plates: every panel becomes a thick, bevelled plate, split from its neighbours by a gap along the lines.");
        make->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(make, &QToolButton::clicked, this, [] {
            const Target t = currentTarget();
            if (t.mesh)
                run("plates", {{"args", {t.mesh->path()}}});
            paletteChanged();
        });
        nv->addWidget(make);
        bv->addWidget(mNoPlates);

        mPlatesForm = new QWidget(box);
        auto* form = new QFormLayout(mPlatesForm);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        auto field = [&](const char* attr, const char* label, double lo, const char* tip) {
            QDoubleSpinBox* f = controls::valueField(lo, 1e4, 3, "cm", mPlatesForm);
            f->setToolTip(tip);
            connect(f, &QDoubleSpinBox::editingFinished, this, [this, f, attr] { setPlatesAttr(attr, f->value()); });
            form->addRow(label, f);
            mPlateFields.push_back({attr, f});
        };
        field("gap", "Gap", 0.0, "Gap: space between plates along every line.");
        field("thickness", "Thickness", 0.0, "Thickness: how far each plate stands up from the surface.");
        field("elevation", "Elevation", -1e4, "Elevation: moves every plate's base out from the surface first; negative sinks it.");
        field("bevel", "Bevel", 0.0, "Bevel: width of the rounded top edge.");
        field("lipWidth", "Lip", 0.0, "Lip: width of a raised rim inside each plate's edge; 0 for none.");
        mKeepBase = new QCheckBox("Keep Mesh Under", mPlatesForm);
        mKeepBase->setToolTip("Keep Mesh Under: plates sit on the mesh (armour over a body) instead of replacing it.");
        connect(mKeepBase, &QCheckBox::toggled, this, [this](bool on) { setPlatesAttr("keepBase", on); });
        form->addRow(mKeepBase);
        bv->addWidget(mPlatesForm);
        mPlatesSection = controls::section("Plates", box, this, sections);
        v->addWidget(mPlatesSection);

        auto* panelBox = new QWidget(this);
        auto* pv = new QVBoxLayout(panelBox);
        pv->setContentsMargins(0, 0, 0, 0);
        pv->setSpacing(4);
        mPanelHint = new QLabel("Click a panel to give it its own height or hide it.", panelBox);
        mPanelHint->setWordWrap(true);
        controls::setRole(mPanelHint, "dim");
        pv->addWidget(mPanelHint);
        mPanelForm = new QWidget(panelBox);
        auto* pf = new QFormLayout(mPanelForm);
        pf->setContentsMargins(0, 0, 0, 0);
        pf->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        pf->setVerticalSpacing(4);
        mPanelHidden = new QCheckBox("Hide", mPanelForm);
        mPanelHidden->setToolTip("Hide: leave this panel out, a hole in the plating.");
        connect(mPanelHidden, &QCheckBox::toggled, this, [this](bool on) { setPanelValue("hidden", on); });
        pf->addRow(mPanelHidden);
        mPanelElevation = controls::valueField(-1e4, 1e4, 3, "cm", mPanelForm);
        mPanelElevation->setToolTip("Elevation: this panel's own height off the surface.");
        connect(mPanelElevation, &QDoubleSpinBox::editingFinished, this, [this] { setPanelValue("elevation", mPanelElevation->value()); });
        pf->addRow("Elevation", mPanelElevation);
        mPanelThickness = controls::valueField(0.0, 1e4, 3, "cm", mPanelForm);
        mPanelThickness->setToolTip("Thickness: this panel's own thickness.");
        connect(mPanelThickness, &QDoubleSpinBox::editingFinished, this, [this] { setPanelValue("thickness", mPanelThickness->value()); });
        pf->addRow("Thickness", mPanelThickness);
        auto* reset = new QToolButton(mPanelForm);
        reset->setText("Reset Panel");
        reset->setToolTip("Reset Panel: follow the plate settings again.");
        connect(reset, &QToolButton::clicked, this, [this] { setPanelValue("clear", true); });
        pf->addRow(reset);
        pv->addWidget(mPanelForm);
        mPanelSection = controls::section("Panel", panelBox, this, sections);
        v->addWidget(mPanelSection);
    }

    void buildFasteners(QVBoxLayout* v, const char* sections)
    {
        auto* box = new QWidget(this);
        auto* bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(4);
        mNoFasteners = new QWidget(box);
        auto* nv = new QVBoxLayout(mNoFasteners);
        nv->setContentsMargins(0, 0, 0, 0);
        auto* hint = new QLabel("No fasteners on this mesh.", mNoFasteners);
        controls::setRole(hint, "dim");
        nv->addWidget(hint);
        auto* add = new QToolButton(mNoFasteners);
        add->setText("Add Fasteners");
        add->setToolTip("Add Fasteners: rivets along both sides of every line, seated on the plates when there are some.");
        add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(add, &QToolButton::clicked, this, [] {
            const Target t = currentTarget();
            if (t.mesh)
                run("fasteners", {{"args", {t.mesh->path()}}});
            paletteChanged();
        });
        nv->addWidget(add);
        bv->addWidget(mNoFasteners);

        mFastenersForm = new QWidget(box);
        auto* form = new QFormLayout(mFastenersForm);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        mKind = new QComboBox(mFastenersForm);
        for (const char* k : {"Rivet", "Hex Bolt", "Screw", "Washer"})
            mKind->addItem(k);
        mKind->setToolTip("Kind: the fastener's head.");
        connect(mKind, &QComboBox::activated, this, [this](int i) { setFastenersAttr("kind", i); });
        form->addRow("Kind", mKind);
        mPlacement = new controls::Segmented(mFastenersForm);
        mPlacement->add("On Lines", {}, "On Lines: on each line.", 0);
        mPlacement->add("Sides", {}, "Sides: beside each line on both sides, along the plate borders.", 1);
        mPlacement->add("Corners", {}, "Corners: in each panel corner where lines meet.", 2);
        connect(mPlacement, &controls::Segmented::picked, this, [this](int id) { setFastenersAttr("placement", id); });
        form->addRow("Place", mPlacement);
        auto field = [&](const char* attr, const char* label, double lo, const char* tip) {
            QDoubleSpinBox* f = controls::valueField(lo, 1e4, 3, "cm", mFastenersForm);
            f->setToolTip(tip);
            connect(f, &QDoubleSpinBox::editingFinished, this, [this, f, attr] { setFastenersAttr(attr, f->value()); });
            form->addRow(label, f);
            mFastenerFields.push_back({attr, f});
        };
        field("size", "Size", 0.01, "Size: head diameter.");
        field("height", "Height", 0.0, "Height: how far the head stands up.");
        field("spacing", "Spacing", 0.05, "Spacing: distance between fasteners along a line.");
        field("inset", "Inset", 0.0, "Inset: distance from the line, or from the corner.");
        field("recess", "Recess", 0.0, "Recess: depth of a round dip pressed under each head; 0 for none. Needs a dense mesh.");
        mFastenerSet = new QComboBox(mFastenersForm);
        mFastenerSet->setToolTip("Lines: which line set the fasteners follow.");
        connect(mFastenerSet, &QComboBox::activated, this, [this](int i) {
            setFastenersAttr("set", i == 0 ? std::string() : mFastenerSet->itemText(i).toStdString());
        });
        form->addRow("Lines", mFastenerSet);
        auto* instances = new QToolButton(mFastenersForm);
        instances->setText("Convert to Instances");
        instances->setToolTip("Convert to Instances: one fastener mesh placed many times, grouped under the mesh, which Unreal gets as "
                              "instanced static meshes. The fasteners modifier is turned off, not removed.");
        instances->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(instances, &QToolButton::clicked, this, [] {
            const Target t = currentTarget();
            if (t.mesh)
                run("fastenersToInstances", {{"args", {t.mesh->path()}}});
            paletteChanged();
        });
        form->addRow(instances);
        bv->addWidget(mFastenersForm);
        mFastenersSection = controls::section("Fasteners", box, this, sections);
        v->addWidget(mFastenersSection);
    }

    void buildPatterns(QVBoxLayout* v, const char* sections)
    {
        auto* box = new QWidget(this);
        auto* bv = new QVBoxLayout(box);
        bv->setContentsMargins(0, 0, 0, 0);
        bv->setSpacing(4);
        mNoPattern = new QWidget(box);
        auto* nv = new QVBoxLayout(mNoPattern);
        nv->setContentsMargins(0, 0, 0, 0);
        auto* hint = new QLabel("No pattern on this mesh.", mNoPattern);
        controls::setRole(hint, "dim");
        nv->addWidget(hint);
        auto* add = new QToolButton(mNoPattern);
        add->setText("Add Pattern");
        add->setToolTip("Add Pattern: a hex pattern pressed into every panel; pick another pattern or only some panels after.");
        add->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        connect(add, &QToolButton::clicked, this, [] {
            const Target t = currentTarget();
            if (t.mesh)
                run("surfacePattern", {{"args", {t.mesh->path()}}});
            paletteChanged();
        });
        nv->addWidget(add);
        bv->addWidget(mNoPattern);

        mPatternForm = new QWidget(box);
        auto* form = new QFormLayout(mPatternForm);
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        mPatternKind = new QComboBox(mPatternForm);
        for (const char* k : {"Hex", "Squares", "Diamond Plate", "Slots", "Dots", "Grille"})
            mPatternKind->addItem(k);
        mPatternKind->setToolTip("Pattern: the shape pressed into the panels.");
        connect(mPatternKind, &QComboBox::activated, this, [this](int i) { setPatternAttr("pattern", i); });
        form->addRow("Pattern", mPatternKind);
        auto field = [&](const char* attr, const char* label, double lo, double hi, const QString& unit, const char* tip) {
            QDoubleSpinBox* f = controls::valueField(lo, hi, 3, unit, mPatternForm);
            f->setToolTip(tip);
            connect(f, &QDoubleSpinBox::editingFinished, this, [this, f, attr] { setPatternAttr(attr, f->value()); });
            form->addRow(label, f);
            mPatternFields.push_back({attr, f});
        };
        field("scale", "Scale", 0.01, 1e4, "cm", "Scale: size of one cell.");
        field("depth", "Depth", -1e4, 1e4, "cm", "Depth: how far the pattern presses in; negative raises it.");
        field("width", "Width", 0.01, 0.9, "", "Width: line width or feature size, as a fraction of a cell.");
        field("rotation", "Rotation", -360.0, 360.0, QString::fromUtf8("\u00b0"), "Rotation: turn of the pattern.");
        field("margin", "Margin", 0.0, 1e4, "cm", "Margin: the pattern fades out this far from the lines.");
        mProjection = new controls::Segmented(mPatternForm);
        mProjection->add("Panel", {}, "Panel: rows follow each panel's own direction.", 0);
        mProjection->add("Triplanar", {}, "Triplanar: the mesh's own axes, for patterns that continue across panels.", 1);
        connect(mProjection, &controls::Segmented::picked, this, [this](int id) { setPatternAttr("projection", id); });
        form->addRow("Follow", mProjection);
        mPatternWhere = new controls::Segmented(mPatternForm);
        mPatternWhere->add("All Panels", {}, "All Panels: the pattern covers every panel.", 0);
        mPatternWhere->add("Picked", {}, "Picked: only panels you click and tick This Panel.", 1);
        connect(mPatternWhere, &controls::Segmented::picked, this, [this](int id) { setPatternAttr("pickedOnly", id == 1); });
        form->addRow("Where", mPatternWhere);
        mPatternHere = new QCheckBox("This Panel", mPatternForm);
        mPatternHere->setToolTip("This Panel: the clicked panel gets the pattern.");
        connect(mPatternHere, &QCheckBox::toggled, this, [this](bool on) { setPanelValue("pattern", on); });
        form->addRow(mPatternHere);
        bv->addWidget(mPatternForm);
        mPatternSection = controls::section("Pattern", box, this, sections);
        v->addWidget(mPatternSection);
    }

    void setPatternAttr(const char* attr, const json& value)
    {
        if (mSyncing)
            return;
        if (scene::Node* n = patternNode(currentTarget()))
            run("setAttr", {{"args", {n->path() + "." + attr, value}}});
        paletteChanged();
    }

    void setFastenersAttr(const char* attr, const json& value)
    {
        if (mSyncing)
            return;
        if (scene::Node* n = fastenersNode(currentTarget()))
            run("setAttr", {{"args", {n->path() + "." + attr, value}}});
        paletteChanged();
    }

    void setPlatesAttr(const char* attr, const json& value)
    {
        if (mSyncing)
            return;
        if (scene::Node* n = platesNode(currentTarget()))
            run("setAttr", {{"args", {n->path() + "." + attr, value}}});
        paletteChanged();
    }

    void setPanelValue(const char* key, const json& value)
    {
        if (mSyncing)
            return;
        const Target t = currentTarget();
        const std::optional<PanelPick>& pick = pickedPanel();
        if (!t.mesh || !pick || pick->mesh != t.mesh->id())
            return;
        run("platesPanel", {{"args", {t.mesh->path()}}, {"point", jsonPoint(t.toWorld(pick->local))}, {key, value}});
        paletteChanged();
    }

    /** Runs selected in the pencil, shown and edited here instead of the set's defaults. */
    void setSelectedRuns(std::vector<uint32_t> runs)
    {
        mSelected = std::move(runs);
        sync();
    }

    void sync()
    {
        const QSignalBlocker blockCleanup(mCleanup);
        mSyncing = true;
        const Target t = currentTarget();
        const PanelLayout* l = t.authored();
        const bool plates = settings().mode == Mode::Plates;
        const bool fastening = settings().mode == Mode::Fasteners;
        const bool patterning = settings().mode == Mode::Patterns;
        const bool platingLoaded = ops::find("plates") != nullptr;
        mMode->setCurrent(int(settings().mode));
        const bool lines = !plates && !fastening && !patterning;
        mEmpty->setText(!lines && l && !platingLoaded ? "Plates and fasteners need the Plating extension. Turn it on in Extensions."
                        : !lines                      ? "No panel lines on the selection. Switch to Lines and draw on the mesh first."
                                                      : "No panel layout on the selection. Draw on a mesh to start one.");
        mEmpty->setVisible(!l || (!lines && !platingLoaded));
        mSetsSection->setVisible(l && lines);
        mLineSection->setVisible(l && lines);
        mPencilSection->setVisible(lines);
        mOverflow->setVisible(lines);
        scene::Node* patn = patternNode(t);
        mPatternSection->setVisible(l && patterning && platingLoaded);
        mNoPattern->setVisible(!patn);
        mPatternForm->setVisible(patn != nullptr);
        if (patn)
        {
            for (const auto& [attr, f] : mPatternFields)
                f->setValue(patn->as<double>(patn->type().attrIndex(attr)));
            mPatternKind->setCurrentIndex(int(patn->as<int64_t>(patn->type().attrIndex("pattern"))));
            mProjection->setCurrent(int(patn->as<int64_t>(patn->type().attrIndex("projection"))));
            const bool pickedOnly = patn->as<bool>(patn->type().attrIndex("pickedOnly"));
            mPatternWhere->setCurrent(pickedOnly ? 1 : 0);
            const json values = pickedValues(t);
            mPatternHere->setVisible(pickedOnly);
            mPatternHere->setEnabled(!values.is_null());
            mPatternHere->setChecked(!values.is_null() && values.value("pattern", false));
            mPatternHere->setText(values.is_null() ? "This Panel (click a panel)" : "This Panel");
        }
        scene::Node* fn = fastenersNode(t);
        mFastenersSection->setVisible(l && fastening && platingLoaded);
        mNoFasteners->setVisible(!fn);
        mFastenersForm->setVisible(fn != nullptr);
        if (fn)
        {
            for (const auto& [attr, f] : mFastenerFields)
                f->setValue(fn->as<double>(fn->type().attrIndex(attr)));
            mKind->setCurrentIndex(int(fn->as<int64_t>(fn->type().attrIndex("kind"))));
            mPlacement->setCurrent(int(fn->as<int64_t>(fn->type().attrIndex("placement"))));
            mFastenerSet->clear();
            mFastenerSet->addItem("All");
            if (l)
                for (const LineSet& set : l->sets)
                    mFastenerSet->addItem(QString::fromStdString(set.name));
            const std::string& only = fn->as<std::string>(fn->type().attrIndex("set"));
            mFastenerSet->setCurrentText(only.empty() ? QString("All") : QString::fromStdString(only));
        }
        scene::Node* pn = platesNode(t);
        mPlatesSection->setVisible(l && plates && platingLoaded);
        mPanelSection->setVisible(l && plates && pn);
        mNoPlates->setVisible(!pn);
        mPlatesForm->setVisible(pn != nullptr);
        if (pn)
        {
            for (const auto& [attr, f] : mPlateFields)
                f->setValue(pn->as<double>(pn->type().attrIndex(attr)));
            mKeepBase->setChecked(pn->as<bool>(pn->type().attrIndex("keepBase")));
            const json values = pickedValues(t);
            mPanelHint->setVisible(values.is_null());
            mPanelForm->setVisible(!values.is_null());
            if (!values.is_null())
            {
                mPanelHidden->setChecked(values.value("hidden", false));
                mPanelElevation->setValue(values.value("elevation", pn->as<double>(pn->type().attrIndex("elevation"))));
                mPanelThickness->setValue(values.value("thickness", pn->as<double>(pn->type().attrIndex("thickness"))));
            }
        }
        mCleanup->setValue(settings().cleanup);
        mCorner->setValue(settings().cornerAngle);
        mKeys->setChecked(settings().keys);
        mSpokes->setChecked(settings().spokes);
        mSpokeCount->setValue(settings().spokeCount);
        mSpokeLength->setValue(settings().spokeLength);
        mSpokeBias->setValue(settings().spokeBias);
        if (l)
        {
            mSets->clear();
            for (const LineSet& s : l->sets)
            {
                auto* item = new QTreeWidgetItem(mSets, {QString::fromStdString(s.name), QString::fromStdString(kOutputLabels[size_t(s.output)])});
                item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
                if (s.id == l->activeSet)
                    mSets->setCurrentItem(item);
            }
            const int rowHeight = std::max(18, mSets->sizeHintForRow(0));
            mSets->setFixedHeight(rowHeight * std::clamp(int(l->sets.size()), 1, 6) + 2 * mSets->frameWidth() + 2);
            const LineSet* active = l->set(l->activeSet);
            mActiveSet = active ? active->name : std::string();
            std::erase_if(mSelected, [&](uint32_t id) { return !l->run(id); });
            const Run* first = mSelected.empty() ? nullptr : l->run(mSelected.front());
            mLineHeader->setText(first ? QString("Lines: %1 selected").arg(mSelected.size()) : QString("Lines: %1").arg(QString::fromStdString(mActiveSet)));
            mForm->setRowVisible(mOutput, !first);
            mForm->setRowVisible(mMoveTo, first != nullptr);
            mMoveTo->clear();
            for (const LineSet& set : l->sets)
                mMoveTo->addItem(QString::fromStdString(set.name));
            if (first)
                mMoveTo->setCurrentText(QString::fromStdString(setName(l, first->set)));
            if (active)
            {
                mOutput->setCurrentIndex(int(active->output));
                mWidth->setValue(first ? l->widthOf(*first) : active->width);
                mDepth->setValue(first ? l->depthOf(*first) : active->depth);
                mProfile->setCurrent(int(first ? l->profileOf(*first) : active->profile));
            }
            mMirror->setChecked(l->mirrorAxis == 0);
        }
        else
            mMirror->setChecked(prefB("mirror", true));
        mSyncing = false;
    }

private:
    void layoutCall(const char* cmd, json call)
    {
        const Target t = currentTarget();
        if (!t.layout)
            return;
        call["args"] = {t.layout->path()};
        run(cmd, call);
        paletteChanged();
    }

    /** Width, depth or profile: on the selected lines when there are some, else the active set's defaults. */
    void setValue(const char* key, const json& value)
    {
        if (mSyncing)
            return;
        if (!mSelected.empty())
            layoutCall("panelLayoutSetRuns", {{"runs", mSelected}, {key, value}});
        else
            layoutCall("panelLayoutLineSet", {{"set", mActiveSet}, {key, value}});
    }

    void refitLoose()
    {
        const Target t = currentTarget();
        const PanelLayout* l = t.authored();
        if (!l)
            return;
        std::vector<uint32_t> loose;
        for (const Run& r : l->runs)
            if (r.loose)
                loose.push_back(r.id);
        if (loose.empty())
            return;
        // Refits from the loosened shape each time the slider moves, not from the last refit.
        const bool amend = mLastRefit != 0 && mLastRefit == UndoStack::get().newestId();
        if (amend)
            UndoStack::get().undo();
        layoutCall("panelLayoutEdit", {{"operation", "refit"}, {"runs", loose}, {"cleanup", settings().cleanup}});
        mLastRefit = UndoStack::get().newestId();
    }

    std::string freeSetName() const
    {
        const PanelLayout* l = currentTarget().authored();
        for (int n = 1;; ++n)
        {
            const std::string name = "Set " + std::to_string(n);
            if (!l || std::none_of(l->sets.begin(), l->sets.end(), [&](const LineSet& s) { return s.name == name; }))
                return name;
        }
    }

    void setMenu(const QPoint& at)
    {
        QTreeWidgetItem* item = mSets->itemAt(at);
        if (!item)
            return;
        const std::string name = item->text(0).toStdString();
        QMenu menu(this);
        menu.addAction("Rename...", [this, name] {
            bool ok = false;
            const QString next = QInputDialog::getText(this, "Rename Line Set", "Name", QLineEdit::Normal, QString::fromStdString(name), &ok);
            if (ok && !next.trimmed().isEmpty())
                layoutCall("panelLayoutLineSet", {{"set", name}, {"rename", next.trimmed().toStdString()}});
        });
        QMenu* outputs = menu.addMenu("Output");
        for (size_t k = 0; k < kOutputLabels.size(); ++k)
            outputs->addAction(QString::fromStdString(kOutputLabels[k]),
                               [this, name, k] { layoutCall("panelLayoutLineSet", {{"set", name}, {"output", outputName(Output(k))}}); });
        menu.addSeparator();
        menu.addAction("Remove", [this, name] { layoutCall("panelLayoutLineSet", {{"action", "remove"}, {"set", name}}); });
        menu.exec(mSets->viewport()->mapToGlobal(at));
    }

    QLabel* mEmpty = nullptr;
    controls::Segmented* mMode = nullptr;
    QWidget *mPatternSection = nullptr, *mNoPattern = nullptr, *mPatternForm = nullptr;
    QComboBox* mPatternKind = nullptr;
    controls::Segmented *mProjection = nullptr, *mPatternWhere = nullptr;
    QCheckBox* mPatternHere = nullptr;
    std::vector<std::pair<const char*, QDoubleSpinBox*>> mPatternFields;
    QWidget *mFastenersSection = nullptr, *mNoFasteners = nullptr, *mFastenersForm = nullptr;
    QComboBox *mKind = nullptr, *mFastenerSet = nullptr;
    controls::Segmented* mPlacement = nullptr;
    std::vector<std::pair<const char*, QDoubleSpinBox*>> mFastenerFields;
    QToolButton* mOverflow = nullptr;
    QWidget *mPencilSection = nullptr, *mPlatesSection = nullptr, *mPanelSection = nullptr, *mNoPlates = nullptr, *mPlatesForm = nullptr,
            *mPanelForm = nullptr;
    std::vector<std::pair<const char*, QDoubleSpinBox*>> mPlateFields;
    QCheckBox *mKeepBase = nullptr, *mPanelHidden = nullptr;
    QDoubleSpinBox *mPanelElevation = nullptr, *mPanelThickness = nullptr;
    QLabel* mPanelHint = nullptr;
    QWidget *mSetsSection = nullptr, *mLineSection = nullptr;
    QToolButton* mLineHeader = nullptr;
    QTreeWidget* mSets = nullptr;
    QComboBox *mOutput = nullptr, *mMoveTo = nullptr;
    QFormLayout* mForm = nullptr;
    QDoubleSpinBox *mWidth = nullptr, *mDepth = nullptr, *mCorner = nullptr;
    controls::Segmented* mProfile = nullptr;
    controls::SliderField* mCleanup = nullptr;
    QCheckBox *mMirror = nullptr, *mKeys = nullptr, *mSpokes = nullptr;
    QWidget* mSpokeBox = nullptr;
    QDoubleSpinBox *mSpokeCount = nullptr, *mSpokeLength = nullptr, *mSpokeBias = nullptr;
    QTimer* mRefresh = nullptr;
    int mGraphListener = 0;
    bool mSyncing = false;
    std::string mActiveSet;
    std::vector<uint32_t> mSelected;
    uint64_t mLastRefit = 0;
};

QPointer<LayoutPalette> gPalette;

void paletteChanged()
{
    if (gPalette)
        gPalette->sync();
    viewportTools::requestRedraw();
}

QDockWidget* paletteDock()
{
    for (QWidget* w = gPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

/** Opens the pencil in a mode, or switches the running one. */
void startPencil(Mode m)
{
    settings().mode = m;
    setPref("mode", int(m));
    if (viewportTools::activeId() != kToolId)
        viewportTools::activate(kToolId);
    paletteChanged();
}

/** The runs the pencil has selected, shared with the actions so menus and search act on them too. */
std::vector<uint32_t>& selectedRuns()
{
    static std::vector<uint32_t> runs;
    return runs;
}

void selectRuns(std::vector<uint32_t> runs)
{
    for (uint32_t& id : runs)
        id &= ~kMirrored;
    std::sort(runs.begin(), runs.end());
    runs.erase(std::unique(runs.begin(), runs.end()), runs.end());
    selectedRuns() = runs;
    if (gPalette)
        gPalette->setSelectedRuns(runs);
    viewportTools::requestRedraw();
}

/**
 * Layout Pencil: draw lines on the selected mesh's surface and they come out clean. Strokes snap to the lines they meet
 * (T joins, crossings, closed loops); starting on a line heading along it redraws it. Ctrl-stroke erases, Shift-stroke
 * smooths, holding still at the end of a stroke straightens its last stretch. Junction dots near the cursor drag; a
 * selected line shows its points to drag and an offset handle for a parallel copy.
 */
class LayoutPencilTool final : public ViewportTool
{
public:
    const char* name() const override { return "Layout Pencil"; }
    std::optional<QCursor> cursor() const override { return QCursor(Qt::CrossCursor); }

    void activate() override
    {
        settings().load();
        panels::show(kPaletteId);
        panels::placeToolPalette(paletteDock());
        mHold = std::make_unique<QTimer>();
        mHold->setSingleShot(true);
        mHold->setInterval(450);
        QObject::connect(mHold.get(), &QTimer::timeout, [this] {
            if (mDrag == Drag::Draw && mStroke.size() > 3)
            {
                mHeld = true;
                viewportTools::requestRedraw();
            }
        });
    }

    void deactivate() override
    {
        mHold.reset();
        mDrag = Drag::None;
        if (QDockWidget* d = paletteDock())
            d->hide();
        viewportTools::requestRedraw();
    }

    bool showsSelection(scene::NodeId) const override { return false; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton)
            return false;
        mTarget = currentTarget();
        if (!mTarget)
        {
            log::warn("Layout Pencil: select a mesh to draw on");
            return true;
        }
        const QPointF px = e->position();
        const bool ctrl = e->modifiers() & Qt::ControlModifier, shift = e->modifiers() & Qt::ShiftModifier;
        updateHover(px, view);

        if (e->type() == QEvent::MouseButtonDblClick)
        {
            if (mHoverJunction)
                layoutCall("panelLayoutEdit", {{"operation", "corner"}, {"junction", mHoverJunction}});
            else if (mHoverRun)
                if (const PanelLayout* l = mTarget.authored())
                    selectRuns(chainOf(*l, mHoverRun & ~kMirrored));
            return true;
        }

        mPressPx = mNowPx = px;
        mView = view.camera;
        mTolerance = settings().joinPx * view.unitsPerPixel(hitOr(px, view));
        if (!ctrl && !shift)
        {
            if (mHoverOffset)
                return startDrag(Drag::Offset, px, view);
            if (mHoverControl >= 0)
                return startDrag(Drag::Bend, px, view);
            if (mHoverJunction)
                return startDrag(Drag::Junction, px, view);
        }
        mDrag = ctrl ? Drag::Erase : shift ? Drag::Smooth : Drag::Draw;
        mStraight = false;
        mHeld = false;
        mStroke.clear();
        mStrokePx.clear();
        addStrokePoint(px, view);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        const QPointF px = e->position();
        mNowPx = px;
        if (mDrag == Drag::None)
        {
            mTarget = currentTarget();
            updateHover(px, view);
            mHoverPanel = -1;
            if (picksPanels(settings().mode))
                if (const std::shared_ptr<const Resolved> r = mTarget.resolved())
                    if (auto h = surfaceHit(px, view))
                        mHoverPanel = r->panelAt(h->point).panel;
            viewportTools::requestRedraw();
            return false;
        }
        if (mDrag == Drag::Draw || mDrag == Drag::Erase || mDrag == Drag::Smooth)
        {
            if (mDrag == Drag::Draw && (e->modifiers() & Qt::ShiftModifier))
                mStraight = true;
            if (distPx(px, mStrokePx.empty() ? mPressPx : mStrokePx.back()) >= 2.0)
            {
                addStrokePoint(px, view);
                mHeld = false;
                if (mHold)
                    mHold->start();
            }
        }
        else
            dragTo(px, view);
        updatePreview();
        viewportTools::requestRedraw();
        return true;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo& view) override
    {
        if (e->button() != Qt::LeftButton || mDrag == Drag::None)
            return false;
        const Drag drag = mDrag;
        mDrag = Drag::None;
        if (mHold)
            mHold->stop();
        mPreview.reset();
        const bool click = distPx(mPressPx, e->position()) < 4.0;
        switch (drag)
        {
        case Drag::Draw:
            if (click && picksPanels(settings().mode))
                pickPanel(e->position(), view);
            else if (click)
                clickSelect(e->modifiers() & Qt::ShiftModifier);
            else
                commitStroke("draw");
            break;
        case Drag::Erase:
            commitStroke("erase");
            break;
        case Drag::Smooth:
            commitStroke("smooth");
            break;
        case Drag::Junction:
            if (!click)
                layoutCall("panelLayoutEdit", {{"operation", "moveJunction"}, {"junction", mDragId}, {"to", jsonPoint(mDragTo)}});
            break;
        case Drag::Bend:
            if (!click)
                layoutCall("panelLayoutEdit", {{"operation", "bend"}, {"run", mDragId}, {"grab", jsonPoint(mDragFrom)}, {"to", jsonPoint(mDragTo)}});
            break;
        case Drag::Offset:
            if (std::abs(mOffset) > 1e-6)
            {
                const json r = layoutCall("panelLayoutEdit", {{"operation", "offset"}, {"runs", selectedRuns()}, {"distance", mOffset}});
                if (r.is_object())
                    selectRuns(r.value("runs", std::vector<uint32_t>{}));
            }
            break;
        case Drag::None: break;
        }
        mStroke.clear();
        mStrokePx.clear();
        mTarget = currentTarget();
        updateHover(e->position(), view);
        viewportTools::requestRedraw();
        return true;
    }

    bool wheel(QWheelEvent* e, const ViewInfo&) override
    {
        if (!(e->modifiers() & Qt::ShiftModifier) || mDrag != Drag::None)
            return false;
        PencilSettings& s = settings();
        s.smoothRadiusPx = std::clamp(s.smoothRadiusPx * std::pow(1.1, e->angleDelta().y() / 120.0), 5.0, 400.0);
        setPref("smoothRadiusPx", s.smoothRadiusPx);
        viewportTools::requestRedraw();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        switch (e->key())
        {
        case Qt::Key_Escape:
            if (mDrag != Drag::None)
            {
                mDrag = Drag::None;
                mPreview.reset();
                mStroke.clear();
                mStrokePx.clear();
            }
            else if (!selectedRuns().empty())
                selectRuns({});
            else
                viewportTools::set(nullptr);
            break;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            if (!selectedRuns().empty())
            {
                layoutCall("panelLayoutEdit", {{"operation", "delete"}, {"runs", selectedRuns()}});
                selectRuns({});
            }
            else if (mHoverJunction)
                layoutCall("panelLayoutEdit", {{"operation", "delete"}, {"junctions", {mHoverJunction}}});
            break;
        case Qt::Key_L:
            if (!selectedRuns().empty())
                layoutCall("panelLayoutEdit", {{"operation", "loosen"}, {"runs", selectedRuns()}});
            break;
        default: return false;
        }
        viewportTools::requestRedraw();
        return true;
    }

    bool hotkey(const QKeyEvent& e, std::string_view) override
    {
        const int k = e.key();
        // Delete, Backspace and L act on the pencil's lines while it is active.
        if (curveTools::plainKey(e) && (k == Qt::Key_Delete || k == Qt::Key_Backspace || k == Qt::Key_L))
            return keyPress(const_cast<QKeyEvent*>(&e), ViewInfo{});
        return false;
    }

    void draw(render::FrameState& f, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();
        if (mDrag == Drag::None)
            mTarget = currentTarget();
        const Target& t = mTarget;
        const PanelLayout* authored = t.authored();
        const std::shared_ptr<const Resolved> r = t.resolved();

        const glm::vec4 hi = themeLinear("wireSelected", glm::vec4(0.25f, 1.0f, 0.4f, 1.0f));
        const glm::vec4 hoverColor = themeLinear("wirePreselect", kHotLinear);
        auto addPath = [&](const std::vector<glm::dvec3>& pts, std::vector<render::LineVertex>& halo, std::vector<render::LineVertex>& lines,
                           const glm::vec4& color) {
            for (size_t k = 0; k + 1 < pts.size(); ++k)
            {
                const glm::vec3 a(t.toWorld(pts[k])), b(t.toWorld(pts[k + 1]));
                halo.push_back({a, {0.02f, 0.02f, 0.02f, 0.9f}});
                halo.push_back({b, {0.02f, 0.02f, 0.02f, 0.9f}});
                lines.push_back({a, color});
                lines.push_back({b, color});
            }
        };
        const bool previewing = mPreview && mPreviewResolved;
        const Resolved* shown = previewing ? mPreviewResolved.get() : r.get();
        const PanelLayout* shownLayout = previewing ? mPreview.get() : authored;
        if (shown)
            for (const ResolvedRun& run : shown->runs())
            {
                const bool selected = std::find(selectedRuns().begin(), selectedRuns().end(), run.source()) != selectedRuns().end();
                const bool hovered = mDrag == Drag::None && (mHoverRun & ~kMirrored) == run.source();
                const bool active = shownLayout && run.set == shownLayout->activeSet;
                const float v = active ? 0.85f : 0.5f;
                glm::vec4 color(v, v, v, 1.0f);
                if (run.mirrored())
                    color *= glm::vec4(0.75f, 0.75f, 0.75f, 1.0f);
                if (selected)
                    color = hi;
                else if (hovered)
                    color = hoverColor;
                if (selected || hovered)
                    addPath(run.path.points, f.curveHaloBold, f.curveLinesBold, color);
                else
                    addPath(run.path.points, f.curveHalo, f.curveLines, color);
            }

        if (picksPanels(settings().mode) && r)
        {
            int picked = -1;
            if (const std::optional<PanelPick>& pick = pickedPanel(); pick && t.mesh && pick->mesh == t.mesh->id())
                picked = r->panelAt(pick->local).panel;
            const glm::vec4 sel = themeLinear("wireSelected", glm::vec4(0.25f, 1.0f, 0.4f, 1.0f));
            tintPanel(f, picked, glm::vec4(glm::vec3(sel), 0.22f));
            if (mHoverPanel != picked)
                tintPanel(f, mHoverPanel, glm::vec4(1.0f, 1.0f, 1.0f, 0.10f));
        }

        // Junctions show only near the cursor, so the lines stay clean.
        if (shown && mDrag == Drag::None)
            for (const ResolvedJunction& j : shown->junctions())
                if (auto s = view.project(t.toWorld(j.position)); s && distPx(*s, mNowPx) < 80.0)
                {
                    const bool hot = j.id == mHoverJunction || (j.id & ~kMirrored) == mHoverJunction;
                    const float radius = hot ? 5.0f : 3.5f;
                    p.disc(*s, radius + 1.5f, kHandleRim);
                    p.disc(*s, radius, hot ? kHotColor : (j.corner ? kHandleGray : glm::vec4(0.9f, 0.9f, 0.9f, 1.0f)));
                }

        // A selected line's points and its offset handle.
        if (authored && !selectedRuns().empty() && mDrag == Drag::None)
        {
            for (const ControlSpot& c : controlSpots(view))
            {
                const bool hot = int(&c - mControls.data()) == mHoverControl;
                p.disc(c.px, hot ? 5.0f : 4.0f, kHandleRim);
                p.disc(c.px, hot ? 3.5f : 2.5f, hot ? kHotColor : kHandleGray);
            }
            if (auto h = offsetHandle(view))
            {
                p.line(h->from, h->to, mHoverOffset ? 2.5f : 1.5f, mHoverOffset ? kHotColor : kHandleGray);
                p.disc(h->to, mHoverOffset ? 6.0f : 5.0f, kHandleRim);
                p.disc(h->to, mHoverOffset ? 4.5f : 3.5f, mHoverOffset ? kHotColor : kHandleGray);
            }
        }

        if (view.camera == mView)
        {
            if (mDrag == Drag::Draw || mDrag == Drag::Erase || mDrag == Drag::Smooth)
                p.polyline(mStrokePx, mDrag == Drag::Draw ? 1.0f : 1.5f, glm::vec4(1.0f, 1.0f, 1.0f, mDrag == Drag::Draw ? 0.35f : 0.6f));
            if (mDrag == Drag::Draw)
                for (const std::vector<glm::dvec3>& g : mGhost)
                    addPath(g, f.curveHaloBold, f.curveLinesBold, hi);
            if (mDrag == Drag::Draw && mHeld)
                p.label(mNowPx + QPointF(14, -14), "Straight");
            if (mDrag == Drag::Offset)
                p.label(mNowPx + QPointF(14, -14), QString("%1 cm").arg(mOffset, 0, 'f', 2));
        }
        if (mDrag == Drag::Smooth || (mDrag == Drag::None && (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier)))
        {
            std::vector<QPointF> ring;
            for (int i = 0; i <= 48; ++i)
            {
                const double a = 2.0 * 3.14159265358979 * i / 48.0;
                ring.push_back(mNowPx + QPointF(std::cos(a), std::sin(a)) * settings().smoothRadiusPx);
            }
            p.polyline(ring, 1.0f, kTopoBrush);
        }

        if (t.layout)
            if (const std::string& w = t.layout->as<std::string>(PanelLayoutNode::kWarning); !w.empty())
                drawToolMessage(p, view, QString::fromStdString("Layout: " + w.substr(0, w.find('\n'))), {1.0f, 0.8f, 0.4f, 1.0f});

        const bool platesMode = picksPanels(settings().mode);
        std::vector<KeyHint> keys = platesMode ? std::vector<KeyHint>{{"Click", "pick panel"}, {"Drag", "draw lines"}, {"Ctrl+drag", "erase lines"}}
                                               : std::vector<KeyHint>{{"Drag", "draw"},          {"Drag along a line", "redraw it"}, {"Hold at end", "straighten"},
                                     {"Ctrl+drag", "erase"},     {"Shift+drag", "smooth"},          {"Shift+scroll", "smooth size"},
                                     {"Double-click", "corner / chain"}, {"L", "loosen"},           {"Delete", "delete"}};
        const QString set = QString::fromStdString(setName(authored, authored ? authored->activeSet : 0));
        drawToolStatus(p, view,
                       {.tool = "Layout Pencil",
                        .subject = t.mesh ? QString::fromStdString(t.mesh->name()) : QString(),
                        .mode = settings().mode == Mode::Patterns ? QString("Patterns")
                                : platesMode                          ? QString("Plates")
                                : settings().mode == Mode::Fasteners ? QString("Fasteners")
                                : set.isEmpty()                      ? QString("Major")
                                                                     : set,
                        .fields = {{"cleanup", QString::number(settings().cleanup, 'f', 2)},
                                   {"selected", platesMode ? QString("-") : QString::number(selectedRuns().size())}},
                        .prompt = !t.mesh ? "select a mesh" : platesMode ? "click a panel to give it its own height" : "draw lines on the surface",
                        .keys = settings().keys ? std::vector<KeyHint>{} : keys,
                        .symmetry = authored && authored->mirrorAxis >= 0 ? std::optional<ToolSymmetry>(ToolSymmetry{true, "X"}) : std::nullopt});
        if (settings().keys)
            drawKeyList(p, QPointF(16.0, view.height - 84.0), keys);
    }

private:
    enum class Drag
    {
        None,
        Draw,
        Erase,
        Smooth,
        Junction,
        Bend,
        Offset,
    };

    struct ControlSpot
    {
        uint32_t run = 0;
        glm::dvec3 local{0.0};
        QPointF px;
    };

    struct OffsetHandle
    {
        QPointF from, to;
        glm::dvec3 base{0.0}, side{0.0};
    };

    json layoutCall(const char* cmd, json call)
    {
        Target t = currentTarget();
        if (!t.layout)
            return nullptr;
        call["args"] = {t.layout->path()};
        if (mTolerance > 0.0)
            call["tolerance"] = mTolerance;
        const json r = run(cmd, call);
        paletteChanged();
        return r;
    }

    glm::dvec3 hitOr(QPointF px, const ViewInfo& view) const
    {
        if (auto h = surfaceHit(px, view))
            return mTarget.toWorld(h->point);
        glm::dvec3 o, d;
        view.ray(px, o, d);
        return o + d * 100.0;
    }

    std::optional<SurfaceHit> surfaceHit(QPointF px, const ViewInfo& view) const
    {
        const Surface* s = surfaceFor(mTarget.surface);
        if (!s)
            return std::nullopt;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        return s->raycast(mTarget.local(o), mTarget.localDir(d));
    }

    void addStrokePoint(QPointF px, const ViewInfo& view)
    {
        if (auto h = surfaceHit(px, view))
        {
            mStroke.push_back(h->point);
            mStrokePx.push_back(px);
        }
    }

    bool startDrag(Drag d, QPointF px, const ViewInfo& view)
    {
        mDrag = d;
        mDragFrom = mDragTo = surfaceHit(px, view).value_or(SurfaceHit{}).point;
        mOffset = 0.0;
        if (d == Drag::Junction)
            mDragId = mHoverJunction & ~kMirrored;
        if (d == Drag::Bend)
        {
            const ControlSpot& c = mControls[size_t(mHoverControl)];
            mDragId = c.run;
            mDragFrom = mDragTo = c.local;
        }
        return true;
    }

    void dragTo(QPointF px, const ViewInfo& view)
    {
        if (mDrag == Drag::Offset)
        {
            if (auto h = mOffsetHandle)
            {
                const QPointF axis = h->to - h->from;
                const double len = std::hypot(axis.x(), axis.y());
                if (len > 1e-6)
                {
                    const double along = ((px - h->from).x() * axis.x() + (px - h->from).y() * axis.y()) / len;
                    const double unit = view.unitsPerPixel(mTarget.toWorld(h->base));
                    mOffset = std::round(along * unit * 100.0) / 100.0;
                }
            }
            return;
        }
        if (auto h = surfaceHit(px, view))
            mDragTo = h->point;
    }

    /** Rebuilds the local preview of the edit in progress: what releasing now would give. */
    void updatePreview()
    {
        const PanelLayout* l = mTarget.authored();
        const Surface* s = surfaceFor(mTarget.surface);
        mGhost.clear();
        if (!s)
            return;
        const EditContext ctx{*s, mToleranceLocal()};
        if (mDrag == Drag::Draw)
        {
            FitOptions o;
            o.tolerance = ctx.tolerance;
            o.cleanup = settings().cleanup;
            o.cornerAngle = settings().cornerAngle;
            o.straight = mStraight;
            o.straightenLast = mHeld;
            const FitResult fit = fitStroke(mStroke, o);
            const double spacing = s->size() / 800.0;
            for (const FitPiece& piece : fit.pieces)
                mGhost.push_back(surfaceCurve(*s, piece.points, spacing).points);
            return;
        }
        if (!l)
            return;
        auto copy = std::make_shared<PanelLayout>(*l);
        EditResult r;
        switch (mDrag)
        {
        case Drag::Erase: r = eraseStroke(*copy, ctx, mStroke); break;
        case Drag::Smooth: r = smoothStroke(*copy, ctx, mStroke, smoothRadiusLocal(), settings().smoothStrength); break;
        case Drag::Junction: r = moveJunction(*copy, ctx, mDragId, mDragTo); break;
        case Drag::Bend: r = bendRun(*copy, ctx, mDragId, mDragFrom, mDragTo); break;
        case Drag::Offset: r = offsetRuns(*copy, ctx, selectedRuns(), mOffset / scale()); break;
        default: break;
        }
        mPreview = copy;
        mPreviewResolved = Resolved::build(*copy, mTarget.surface);
    }

    double scale() const { return std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(mTarget.world))))); }
    double mToleranceLocal() const { return (mTolerance > 0.0 ? mTolerance : 1.0) / scale(); }
    double smoothRadiusLocal() const { return mTolerance / std::max(1.0, settings().joinPx) * settings().smoothRadiusPx / scale(); }

    void commitStroke(const char* mode)
    {
        if (mStroke.size() < 2)
            return;
        json pts = json::array();
        for (const glm::dvec3& q : mStroke)
            pts.push_back(jsonPoint(mTarget.toWorld(q)));
        UndoChunk chunk("Layout Pencil");
        if (!currentTarget().layout)
        {
            if (std::string(mode) != "draw" || !mTarget.mesh)
                return;
            run("panelLayout", {{"args", {mTarget.mesh->path()}}, {"mirror", prefB("mirror", true) ? "x" : "off"}});
        }
        // A closed stroke with Loop Spokes on becomes the loop and its spokes.
        if (std::string(mode) == "draw" && settings().spokes && mStroke.size() > 3 &&
            glm::length(mStroke.front() - mStroke.back()) < mToleranceLocal() * 3.0)
        {
            layoutCall("panelLayoutLoopSpokes", {{"points", pts},
                                                 {"count", settings().spokeCount},
                                                 {"length", settings().spokeLength},
                                                 {"cornerBias", settings().spokeBias}});
            return;
        }
        json call = {{"points", pts}, {"mode", mode}};
        if (std::string(mode) == "draw")
        {
            call["cleanup"] = settings().cleanup;
            call["cornerAngle"] = settings().cornerAngle;
            call["straight"] = mStraight;
            call["straightenLast"] = mHeld;
        }
        else if (std::string(mode) == "smooth")
        {
            call["radius"] = smoothRadiusLocal() * scale();
            call["strength"] = settings().smoothStrength;
        }
        const json r = layoutCall("panelLayoutStroke", call);
        if (std::string(mode) == "draw" && r.is_object())
            selectRuns(r.value("runs", std::vector<uint32_t>{}));
        else if (std::string(mode) == "erase")
            selectRuns({});
    }

    void pickPanel(QPointF px, const ViewInfo& view)
    {
        std::optional<PanelPick>& pick = pickedPanel();
        pick.reset();
        if (const std::shared_ptr<const Resolved> r = mTarget.resolved(); r && mTarget.mesh)
            if (auto h = surfaceHit(px, view); h && r->panelAt(h->point).panel >= 0)
                pick = PanelPick{mTarget.mesh->id(), h->point};
        paletteChanged();
    }

    /** Fills tris3d with the panel's triangles, in world space. */
    void tintPanel(render::FrameState& f, int panel, const glm::vec4& color)
    {
        const std::shared_ptr<const Resolved> r = mTarget.resolved();
        if (panel < 0 || !r || !r->mesh())
            return;
        if (mTintFor != r)
        {
            mTintFor = r;
            mTintTris = geo::triangulate(*r->mesh());
            mTintPanels = r->trianglePanels();
            const std::vector<glm::vec3> n = geo::pointNormals(*r->mesh());
            mTintNormals.assign(n.begin(), n.end());
        }
        // With plates on, the tint sits on their top face rather than under them on the surface.
        double lift = 0.0;
        if (scene::Node* plates = platesNode(mTarget); plates && plates->as<bool>(plates->type().attrIndex("enabled")))
            lift = plates->as<double>(plates->type().attrIndex("elevation")) + plates->as<double>(plates->type().attrIndex("thickness"));
        const geo::Mesh& m = *r->mesh();
        for (size_t t = 0; t < mTintPanels.size() && t < mTintTris.triangleCount(); ++t)
            if (mTintPanels[t] == panel)
                for (int k = 0; k < 3; ++k)
                {
                    const geo::Index pt = m.cornerPoint(mTintTris.corners[t * 3 + size_t(k)]);
                    const glm::dvec3 up = pt < mTintNormals.size() ? mTintNormals[pt] : glm::dvec3(0.0);
                    f.tris3d.push_back({glm::dvec3(mTarget.toWorld(m.position(pt) + up * lift)), color});
                }
    }

    void clickSelect(bool add)
    {
        std::vector<uint32_t> next = add ? selectedRuns() : std::vector<uint32_t>{};
        if (mHoverRun)
        {
            const uint32_t id = mHoverRun & ~kMirrored;
            auto it = std::find(next.begin(), next.end(), id);
            if (add && it != next.end())
                next.erase(it);
            else
                next.push_back(id);
        }
        selectRuns(next);
    }

    /** Screen positions of the selected runs' control points. */
    const std::vector<ControlSpot>& controlSpots(const ViewInfo& view)
    {
        mControls.clear();
        const PanelLayout* l = mTarget.authored();
        const Surface* s = surfaceFor(mTarget.surface);
        if (!l || !s)
            return mControls;
        for (uint32_t id : selectedRuns())
            if (const Run* r = l->run(id))
                for (const Anchor& a : r->controls)
                {
                    const glm::dvec3 q = s->resolve(a);
                    if (auto px = view.project(mTarget.toWorld(q)))
                        mControls.push_back({id, q, *px});
                }
        return mControls;
    }

    /** An arrow off the middle of the first selected run, across it along the surface. */
    std::optional<OffsetHandle> offsetHandle(const ViewInfo& view)
    {
        mOffsetHandle.reset();
        const std::shared_ptr<const Resolved> r = mTarget.resolved();
        if (!r || selectedRuns().empty())
            return std::nullopt;
        for (const ResolvedRun& run : r->runs())
        {
            if (run.mirrored() || run.source() != selectedRuns().front() || run.path.empty())
                continue;
            const double s = run.path.length() * 0.5;
            const glm::dvec3 base = run.path.pointAt(s);
            glm::dvec3 side = glm::cross(run.path.normalAt(s), run.path.tangentAt(s));
            if (glm::length(side) < 1e-12)
                return std::nullopt;
            side = glm::normalize(side);
            const double unit = view.unitsPerPixel(mTarget.toWorld(base)) / scale();
            auto a = view.project(mTarget.toWorld(base));
            auto b = view.project(mTarget.toWorld(base + side * unit * 36.0));
            if (!a || !b)
                return std::nullopt;
            mOffsetHandle = OffsetHandle{*a, *b, base, side};
            return mOffsetHandle;
        }
        return std::nullopt;
    }

    void updateHover(QPointF px, const ViewInfo& view)
    {
        mHoverJunction = 0;
        mHoverRun = 0;
        mHoverControl = -1;
        mHoverOffset = false;
        const std::shared_ptr<const Resolved> r = mTarget.resolved();
        if (!r)
            return;
        if (!selectedRuns().empty())
        {
            if (auto h = offsetHandle(view); h && distPx(h->to, px) < 8.0)
            {
                mHoverOffset = true;
                return;
            }
            const std::vector<ControlSpot>& spots = controlSpots(view);
            for (size_t k = 0; k < spots.size(); ++k)
                if (distPx(spots[k].px, px) < 7.0)
                {
                    mHoverControl = int(k);
                    return;
                }
        }
        double best = 8.0;
        for (const ResolvedJunction& j : r->junctions())
            if (auto s = view.project(mTarget.toWorld(j.position)); s && distPx(*s, px) < best)
            {
                best = distPx(*s, px);
                mHoverJunction = j.id & ~kMirrored;
            }
        if (mHoverJunction)
            return;
        best = 6.0;
        for (const ResolvedRun& run : r->runs())
        {
            std::optional<QPointF> prev;
            for (const glm::dvec3& q : run.path.points)
            {
                const std::optional<QPointF> s = view.project(mTarget.toWorld(q));
                if (s && prev)
                    if (const double d = segmentDistPx(px, *prev, *s); d < best)
                    {
                        best = d;
                        mHoverRun = run.id;
                    }
                prev = s;
            }
        }
    }

    Target mTarget;
    Drag mDrag = Drag::None;
    std::vector<glm::dvec3> mStroke;
    std::vector<QPointF> mStrokePx;
    QPointF mPressPx, mNowPx;
    std::string mView;
    double mTolerance = 0.0;
    bool mStraight = false, mHeld = false;
    std::unique_ptr<QTimer> mHold;
    std::vector<std::vector<glm::dvec3>> mGhost;
    std::shared_ptr<PanelLayout> mPreview;
    std::shared_ptr<const Resolved> mPreviewResolved;
    uint32_t mHoverJunction = 0, mHoverRun = 0;
    int mHoverPanel = -1;
    std::shared_ptr<const Resolved> mTintFor;
    geo::Triangulation mTintTris;
    std::vector<int> mTintPanels;
    std::vector<glm::dvec3> mTintNormals;
    int mHoverControl = -1;
    bool mHoverOffset = false;
    std::vector<ControlSpot> mControls;
    std::optional<OffsetHandle> mOffsetHandle;
    uint32_t mDragId = 0;
    glm::dvec3 mDragFrom{0.0}, mDragTo{0.0};
    double mOffset = 0.0;
};

void runOnSelection(const char* op)
{
    const Target t = currentTarget();
    if (!t.layout)
    {
        log::warn("Select a mesh with a panel layout");
        return;
    }
    if (std::string(op) == "clear")
        run("panelLayoutClear", {{"args", {t.layout->path()}}});
    else if (!selectedRuns().empty())
    {
        run("panelLayoutEdit", {{"args", {t.layout->path()}}, {"operation", op}, {"runs", selectedRuns()}});
        if (std::string(op) == "delete")
            selectRuns({});
    }
    else
        log::warn("Select lines with the Layout Pencil first");
    paletteChanged();
}
} // namespace layoutPencil

void registerLayoutPencil()
{
    panels::add({.id = layoutPencil::kPaletteId,
                 .title = "Layout Pencil",
                 .create = [](QWidget* parent) -> QWidget* { return layoutPencil::gPalette = new layoutPencil::LayoutPalette(parent); },
                 .area = Qt::LeftDockWidgetArea,
                 .icon = "pencil",
                 .open = false,
                 .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                 .floatFirst = true,
                 .toolPalette = true});
    if (!viewportTools::registry().find(layoutPencil::kToolId))
        viewportTools::add({.id = layoutPencil::kToolId, .label = "Layout Pencil", .icon = "pencil", .create = [] { return std::make_unique<layoutPencil::LayoutPencilTool>(); }});
    const std::vector<std::string> words = {"plate", "plates", "plating", "panel", "panels", "panel lines", "panel loops", "hard surface",
                                            "armour", "armor", "sci-fi", "groove", "layout", "pencil", "seam"};
    std::vector<std::string> fastenerWords = words;
    for (const char* w : {"fastener", "fasteners", "rivet", "rivets", "bolt", "bolts", "screw", "screws", "washer", "imm", "insert mesh",
                          "greeble", "nanomesh"})
        fastenerWords.push_back(w);
    actions::add({.id = layoutPencil::kToolId,
                  .label = "Layout Pencil",
                  .category = "Mesh",
                  .run = [] { layoutPencil::startPencil(layoutPencil::Mode::Lines); },
                  .doc = "Draw panel lines on the selected mesh's surface: strokes come out as clean straights, arcs and corners, join the "
                         "lines they meet and bound panels, cut in as grooves. Switch the palette to Plates to turn the panels into plates.",
                  .python = "lila.ui.run_action(\"layout.pencil\")",
                  .icon = "pencil",
                  .keywords = words});
    actions::add({.id = "layout.plates",
                  .label = "Plates",
                  .category = "Mesh",
                  .run = [] { layoutPencil::startPencil(layoutPencil::Mode::Plates); },
                  .doc = "Turn the panels drawn with the Layout Pencil into thick, bevelled plates split by gaps (Panel Loops); click a panel "
                         "to raise, sink, thicken or hide it.",
                  .python = "lila.ui.run_action(\"layout.plates\")",
                  .icon = "layers-subtract",
                  .keywords = words});
    actions::add({.id = "layout.fasteners",
                  .label = "Fasteners",
                  .category = "Mesh",
                  .run = [] { layoutPencil::startPencil(layoutPencil::Mode::Fasteners); },
                  .doc = "Set rivets, hex bolts, screws or washers along the panel lines drawn with the Layout Pencil, beside them or in "
                         "panel corners, seated on the plates.",
                  .python = "lila.ui.run_action(\"layout.fasteners\")",
                  .icon = "circle-dot",
                  .keywords = fastenerWords});
    std::vector<std::string> patternWords = words;
    for (const char* w : {"pattern", "patterns", "hex", "hexagon", "honeycomb", "diamond plate", "tread plate", "grille", "vent", "slots",
                          "dots", "surface noise", "noisemaker", "texture"})
        patternWords.push_back(w);
    actions::add({.id = "layout.patterns",
                  .label = "Surface Patterns",
                  .category = "Mesh",
                  .run = [] { layoutPencil::startPencil(layoutPencil::Mode::Patterns); },
                  .doc = "Press hex, squares, diamond plate, slots, dots or grille patterns into the panels drawn with the Layout Pencil, "
                         "following each panel; every panel or only the ones you click.",
                  .python = "lila.ui.run_action(\"layout.patterns\")",
                  .icon = "grid-pattern",
                  .keywords = patternWords});
    actions::add({.id = "layout.add",
                  .label = "Panel Layout",
                  .category = "Mesh",
                  .run = [] { layoutPencil::startPencil(layoutPencil::Mode::Lines); },
                  .doc = "Start drawing panel lines on the selected mesh with the Layout Pencil.",
                  .python = "lila.ui.run_action(\"layout.add\")",
                  .icon = "layout-grid",
                  .keywords = words});
    actions::add({.id = "layout.loosen",
                  .label = "Loosen Lines",
                  .category = "Mesh",
                  .run = [] { layoutPencil::runOnSelection("loosen"); },
                  .doc = "Release the selected lines from their fit so Cleanup and redrawing reshape them freely.  (L in the Layout Pencil)",
                  .icon = "line-dashed",
                  .keywords = words});
    actions::add({.id = "layout.deleteLines",
                  .label = "Delete Lines",
                  .category = "Mesh",
                  .run = [] { layoutPencil::runOnSelection("delete"); },
                  .doc = "Delete the lines selected in the Layout Pencil.  (Delete in the Layout Pencil)",
                  .icon = "trash",
                  .keywords = words});
    actions::add({.id = "layout.bakeMaps",
                  .label = "Bake Line Maps...",
                  .category = "Mesh",
                  .run = [] {
                      const layoutPencil::Target t = layoutPencil::currentTarget();
                      if (!t.layout || !t.mesh)
                      {
                          log::warn("Select a mesh with a panel layout");
                          return;
                      }
                      const QString dir = fileDialog::getExistingDirectory(nullptr, "Bake Line Maps To");
                      if (!dir.isEmpty())
                          layoutPencil::run("panelLayoutBakeMaps", {{"args", {t.layout->path()}}, {"path", dir.toStdString()}});
                  },
                  .doc = "Bake the panel layout into textures in the mesh's UVs: a height map of the Texture lines, a panel ID map and a "
                         "line mask, for texturing in Lila, Painter or Unreal.",
                  .icon = "photo",
                  .keywords = words});
    actions::add({.id = "layout.unrealMaterials",
                  .label = "Unreal Panel Line Materials",
                  .category = "Mesh",
                  .doc = "Make MF_LilaPanelLine and the masked and translucent panel line materials in Unreal over LilaLink, for the "
                         "LilaPanelLine slot of Decal lines.",
                  .python = ops::toPython("panelLineMaterials", json::object()),
                  .command = "panelLineMaterials",
                  .call = json::object(),
                  .icon = "cube-send",
                  .keywords = words});
    actions::add({.id = "layout.clear",
                  .label = "Clear Panel Layout",
                  .category = "Mesh",
                  .run = [] { layoutPencil::runOnSelection("clear"); },
                  .doc = "Remove every line from the selected mesh's panel layout, keeping its line sets.",
                  .icon = "eraser",
                  .keywords = words});
}
} // namespace lila::ui
