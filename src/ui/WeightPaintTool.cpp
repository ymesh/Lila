#include "ui/WeightPaintTool.h"

// Before Qt: Qt's emit macro breaks TBB's headers.
#include <tbb/parallel_for.h>

#include "brush/BrushUi.h"
#include "brush/Surface.h"
#include "brush/Symmetry.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "geo/Bvh.h"
#include "geo/Symmetry.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/DeformWeights.h"
#include "scene/Deformers.h"
#include "scene/Fields.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/Rig.h"
#include "scene/Skin.h"
#include "scene/SkinPaint.h"
#include "scene/WeightProxy.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/Theme.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"
#include "ui/ViewportDisplay.h"
#include "ui/ViewportHooks.h"
#include "ui/ViewportTool.h"

#include <QApplication>
#include <QCheckBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace lila::ui
{
namespace
{
using scene::NodeId;
using scene::kNoNode;
using scene::WeightTable;
using scene::skin::PaintOutcome;
namespace skinpaint = scene::skinpaint;

/** An attr of the painted node: a skinCluster's own, or a weight host's (bend bones chains); -1 when it has none. */
int skinAttr(NodeId skin, int scene::skin::WeightHost::*field)
{
    const auto* h = scene::skin::weightHost(scene::Graph::current(), skin);
    return h ? h->*field : -1;
}

enum class Brush
{
    Add,
    Replace,
    Scale,
    Relax,
    Settle,
};
const char* kBrushNames[] = {"Add", "Replace", "Scale", "Relax", "Settle"};

constexpr const char* kWeightPaintPrefix = "/weightPaint";
constexpr const char* kWeightPaletteId = "rig.weightPaintPalette";
constexpr const char* kShowAutomaticPref = "/rig/weightPaintShowAutomatic";

struct WeightPaintSettings
{
    Brush brush = Brush::Replace;
    float value = 1.0f;
    int mirrorAxis = 0;
    int64_t influence = -1;
    /** Where removed weight goes, -1 for none. Cleared when it is locked or painted. */
    int64_t receiver = -1;
    /** The deformer or field whose map is painted instead of an influence, kNoNode for skin weights. */
    NodeId mapNode = kNoNode;
    int mapLayer = -1;
    /** Paint the active weight layer's mask instead of its weights. */
    bool paintMask = false;
    /** Radius, strength, mirror, falloff (bell: full in the middle, smooth to zero at the rim) and the shared brush. */
    brush::BrushSettings stroke = [] {
        brush::BrushSettings b;
        b.radius = 40.0;
        b.strength = 1.0;
        b.spacing = 0.25;
        b.falloff = "bell";
        b.mirror = false;
        return b;
    }();
};

WeightPaintSettings& weightPaintSettings()
{
    static WeightPaintSettings s = [] {
        WeightPaintSettings out;
        out.stroke.loadPrefs(kWeightPaintPrefix);
        return out;
    }();
    return s;
}

void saveWeightPaintSettings() { weightPaintSettings().stroke.savePrefs(kWeightPaintPrefix); }

bool strict() { return skinpaint::strictDefault(); }
void setStrict(bool on) { PrefStore::get().set("general", skinpaint::kStrictPref, on); }
bool showAutomatic() { return PrefStore::get().value<bool>("general", kShowAutomaticPref, true); }
void setShowAutomatic(bool on) { PrefStore::get().set("general", kShowAutomaticPref, on); }

scene::skin::PaintMode paintMode(Brush b)
{
    switch (b)
    {
    case Brush::Add: return scene::skin::PaintMode::Add;
    case Brush::Scale: return scene::skin::PaintMode::Scale;
    case Brush::Replace: return scene::skin::PaintMode::Replace;
    default: return scene::skin::PaintMode::Smooth;
    }
}

glm::vec4 warningSrgb() { return themeSrgb("issueWarning", {0.95f, 0.76f, 0.31f, 1.0f}); }

QString hintText(PaintOutcome o)
{
    switch (o)
    {
    case PaintOutcome::Ambiguous: return "several joints could take it · set a receiver or lock";
    case PaintOutcome::NowhereToGo: return "nowhere for the weight to go · unlock a joint or set a receiver";
    case PaintOutcome::InfluenceLimit: return "influence limit reached · lock or remove a joint there";
    case PaintOutcome::Locked: return "joint is locked";
    default: return {};
    }
}

class WeightPaintTool;
WeightPaintTool* gWeightTool = nullptr;

class WeightPaintPalette;
QPointer<WeightPaintPalette> gWeightPalette;

void syncPalette();

/** What the palette lists, read off the active tool. */
struct PaletteState
{
    QString target;
    NodeId skin = kNoNode;
    std::vector<scene::skin::Influence> influences;
    std::vector<scene::deformWeights::MapTarget> maps;
    std::shared_ptr<const scene::SkinLayers> layers;
    /** Influences with weight under the brush, for the Under Brush filter. */
    std::unordered_set<uint32_t> underBrush;
    /** The render meshes when the painted mesh is a weight proxy. */
    QStringList proxyFor;
};
PaletteState paletteState();

// ------------------------------------------------------------------------------------------------ palette

/**
 * Paint Weights: the influence tree (lock and receiver per row), the brush strip, its values, Strict and Mirror, Flood
 * and the rarer weight commands behind the overflow, and the weight layers.
 */
class WeightPaintPalette final : public QWidget
{
public:
    explicit WeightPaintPalette(QWidget* parent) : QWidget(parent)
    {
        constexpr const char* kSections = "weightPaintPalette";
        auto* v = new QVBoxLayout(this);
        v->setContentsMargins(12, 8, 12, 8);
        v->setSpacing(4);
        mTarget = new QLabel(this);
        controls::setRole(mTarget, "dim");
        v->addWidget(mTarget);

        // Influences.
        auto* infBox = new QWidget(this);
        auto* infCol = new QVBoxLayout(infBox);
        infCol->setContentsMargins(0, 0, 0, 0);
        infCol->setSpacing(4);
        auto* filterRow = new QHBoxLayout;
        mFilter = new QLineEdit(infBox);
        mFilter->setPlaceholderText("Filter");
        mFilter->setClearButtonEnabled(true);
        connect(mFilter, &QLineEdit::textChanged, this, [this] { applyFilter(); });
        filterRow->addWidget(mFilter, 1);
        mUnderBrush = new QToolButton(infBox);
        mUnderBrush->setText("Under Brush");
        mUnderBrush->setCheckable(true);
        mUnderBrush->setToolTip("Under Brush: list only the joints with weight under the brush");
        connect(mUnderBrush, &QToolButton::toggled, this, [this] { applyFilter(); });
        filterRow->addWidget(mUnderBrush);
        infCol->addLayout(filterRow);
        mTree = new QTreeWidget(infBox);
        mTree->setColumnCount(3);
        mTree->setHeaderHidden(true);
        mTree->setRootIsDecorated(true);
        mTree->setIndentation(12);
        mTree->setUniformRowHeights(true);
        mTree->header()->setStretchLastSection(false);
        mTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
        mTree->header()->setSectionResizeMode(1, QHeaderView::Fixed);
        mTree->header()->setSectionResizeMode(2, QHeaderView::Fixed);
        mTree->header()->resizeSection(1, 22);
        mTree->header()->resizeSection(2, 22);
        mTree->setContextMenuPolicy(Qt::CustomContextMenu);
        mTree->setMinimumHeight(180);
        mTree->setToolTip("The joint painted. Click the lock to lock or unlock; Alt+click it to lock every other joint. Ctrl+click a "
                          "joint here, or Ctrl+Alt+click it in the viewport, to make it the receiver, where removed weight goes.");
        connect(mTree, &QTreeWidget::itemPressed, this, [this](QTreeWidgetItem* item, int column) { pressed(item, column); });
        connect(mTree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item) { picked(item); });
        connect(mTree, &QTreeWidget::customContextMenuRequested, this, [this](QPoint at) { contextMenu(at); });
        infCol->addWidget(mTree, 1);
        mReceiverLabel = new QLabel(infBox);
        controls::setRole(mReceiverLabel, "dim");
        infCol->addWidget(mReceiverLabel);
        v->addWidget(controls::section("Influences", infBox, this, kSections), 1);

        // Brush.
        auto* brushBox = new QWidget(this);
        auto* brushCol = new QVBoxLayout(brushBox);
        brushCol->setContentsMargins(0, 0, 0, 0);
        brushCol->setSpacing(4);
        mBrush = new controls::Segmented(brushBox);
        mBrush->add("Add", {}, "Add: raise the joint's weight by Value (Ctrl lowers it)", int(Brush::Add));
        mBrush->add("Replace", {}, "Replace: move the joint's weight toward Value", int(Brush::Replace));
        mBrush->add("Scale", {}, "Scale: multiply the joint's weight by Value", int(Brush::Scale));
        mBrush->add("Relax", {}, "Relax: ease weights toward their neighbours over the surface  (Shift)", int(Brush::Relax));
        mBrush->add("Settle", {}, "Settle: fit the weights so the posed mesh loses its pinches and collapses", int(Brush::Settle));
        connect(mBrush, &controls::Segmented::picked, this, [](int id) {
            weightPaintSettings().brush = Brush(id);
            changed();
        });
        brushCol->addWidget(mBrush);
        auto* form = new QFormLayout;
        form->setContentsMargins(0, 0, 0, 0);
        form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        form->setVerticalSpacing(4);
        mValue = new controls::SliderField(brushBox);
        mValue->setRange(0.0, 1.0);
        mValue->setSingleStep(0.05);
        mValue->setDecimals(3);
        mValue->setToolTip("Value: the weight Replace paints toward, Add adds, Scale multiplies by");
        connect(mValue, &QDoubleSpinBox::valueChanged, this, [](double x) {
            weightPaintSettings().value = float(x);
            changed();
        });
        form->addRow("Value", mValue);
        mStrength = new controls::SliderField(brushBox);
        mStrength->setRange(0.0, 1.0);
        mStrength->setSingleStep(0.05);
        mStrength->setDecimals(3);
        mStrength->setToolTip("Strength: how far each dab goes  (Shift+B drag)");
        connect(mStrength, &QDoubleSpinBox::valueChanged, this, [](double x) {
            weightPaintSettings().stroke.strength = x;
            changed();
        });
        form->addRow("Strength", mStrength);
        mRadius = controls::valueField(2, 2000, 0, "px", brushBox);
        mRadius->setToolTip("Radius: brush size on screen  (B drag, [ ])");
        connect(mRadius, &QDoubleSpinBox::valueChanged, this, [](double x) {
            weightPaintSettings().stroke.radius = x;
            changed();
        });
        form->addRow("Radius", mRadius);
        brushCol->addLayout(form);
        mStrokeOptions = new brush::ui::StrokeOptions(brushBox, []() -> brush::BrushSettings& { return weightPaintSettings().stroke; }, [] { changed(); });
        brushCol->addWidget(mStrokeOptions);
        v->addWidget(controls::section("Brush", brushBox, this, kSections));

        // Options.
        auto* optBox = new QWidget(this);
        auto* opt = new QVBoxLayout(optBox);
        opt->setContentsMargins(0, 0, 0, 0);
        opt->setSpacing(4);
        mStrict = new QCheckBox("Strict", optBox);
        mStrict->setToolTip("Strict: refuse to paint a vertex where more than one joint could take the weight. Off, Lila splits it in "
                            "proportion and marks those weights automatic.");
        connect(mStrict, &QCheckBox::toggled, this, [](bool on) {
            if (on == strict())
                return;
            setStrict(on);
            changed();
        });
        opt->addWidget(mStrict);
        auto* mirrorRow = new QHBoxLayout;
        mMirror = new QCheckBox("Mirror", optBox);
        mMirror->setToolTip("Mirror: paint the other side too, on the mirrored joint (_l and _r)");
        connect(mMirror, &QCheckBox::toggled, this, [](bool on) {
            weightPaintSettings().stroke.mirror = on;
            changed();
        });
        mirrorRow->addWidget(mMirror);
        mAxis = new controls::Segmented(optBox);
        mAxis->add("X", {}, "Mirror across X", 0);
        mAxis->add("Y", {}, "Mirror across Y", 1);
        mAxis->add("Z", {}, "Mirror across Z", 2);
        connect(mAxis, &controls::Segmented::picked, this, [](int id) {
            weightPaintSettings().mirrorAxis = id;
            changed();
        });
        mirrorRow->addWidget(mAxis, 1);
        opt->addLayout(mirrorRow);
        mShowAutomatic = new QCheckBox("Show Automatic", optBox);
        mShowAutomatic->setToolTip("Show Automatic: dot the vertices whose weights Lila split rather than you set");
        connect(mShowAutomatic, &QCheckBox::toggled, this, [](bool on) {
            if (on == showAutomatic())
                return;
            setShowAutomatic(on);
            viewportTools::requestRedraw();
        });
        opt->addWidget(mShowAutomatic);
        auto* actionsRow = new QHBoxLayout;
        auto* flood = controls::push("rig.floodWeights", optBox, "Flood");
        actionsRow->addWidget(flood);
        actionsRow->addWidget(controls::overflow({"rig.acceptWeights", "rig.selectAutomaticWeights", "rig.relaxWeights", "rig.settleWeights",
                                                  "rig.settleWeightsSimulate", "rig.normalizeWeights", "rig.pruneWeights"},
                                                 optBox));
        opt->addLayout(actionsRow);
        mProxyRow = new QWidget(optBox);
        auto* proxyRow = new QHBoxLayout(mProxyRow);
        proxyRow->setContentsMargins(0, 0, 0, 0);
        mProxyLabel = new QLabel(mProxyRow);
        controls::setRole(mProxyLabel, "dim");
        proxyRow->addWidget(mProxyLabel, 1);
        proxyRow->addWidget(controls::push("rig.pullProxyWeights", mProxyRow, "Pull"));
        proxyRow->addWidget(controls::push("rig.pushProxyWeights", mProxyRow, "Push"));
        opt->addWidget(mProxyRow);
        v->addWidget(controls::section("Options", optBox, this, kSections));

        // Layers.
        auto* layerBox = new QWidget(this);
        auto* lay = new QVBoxLayout(layerBox);
        lay->setContentsMargins(0, 0, 0, 0);
        lay->setSpacing(4);
        mLayers = new QListWidget(layerBox);
        mLayers->setMinimumHeight(70);
        mLayers->setToolTip("Weight layers, top first. The current one is painted; the check shows or hides it.");
        setEmptyHint(mLayers, "No layers. Click + to paint on a layer over the weights.");
        connect(mLayers, &QListWidget::currentItemChanged, this, [this](QListWidgetItem* item) {
            if (!item || mSyncing)
                return;
            editLayer({{"active", true}}, item->data(Qt::UserRole).toInt());
        });
        connect(mLayers, &QListWidget::itemChanged, this, [this](QListWidgetItem* item) {
            if (mSyncing)
                return;
            editLayer({{"enabled", item->checkState() == Qt::Checked}}, item->data(Qt::UserRole).toInt());
        });
        lay->addWidget(mLayers);
        auto* layerRow = new QHBoxLayout;
        auto* addLayer = new QToolButton(layerBox);
        addLayer->setIcon(icon("plus"));
        addLayer->setToolTip("Add a layer above the others");
        connect(addLayer, &QToolButton::clicked, this, [] { runOnTarget("addSkinLayer", {}); });
        auto* removeLayer = new QToolButton(layerBox);
        removeLayer->setIcon(icon("trash"));
        removeLayer->setToolTip("Remove the current layer");
        connect(removeLayer, &QToolButton::clicked, this, [this] {
            if (QListWidgetItem* item = mLayers->currentItem(); item && item->data(Qt::UserRole).toInt() > 0)
                runOnTarget("removeSkinLayer", {{"layer", item->data(Qt::UserRole).toInt()}});
        });
        layerRow->addWidget(addLayer);
        layerRow->addWidget(removeLayer);
        mPaintOn = new controls::Segmented(layerBox);
        mPaintOn->add("Weights", {}, "Paint the layer's weights", 0);
        mPaintOn->add("Mask", {}, "Paint where the layer shows: white shows it, black hides it", 1);
        connect(mPaintOn, &controls::Segmented::picked, this, [](int id) {
            weightPaintSettings().paintMask = id == 1;
            changed();
        });
        layerRow->addWidget(mPaintOn, 1);
        layerRow->addWidget(controls::overflow({"rig.flattenWeightLayers"}, layerBox));
        lay->addLayout(layerRow);
        auto* opacityForm = new QFormLayout;
        opacityForm->setContentsMargins(0, 0, 0, 0);
        opacityForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mOpacity = new controls::SliderField(layerBox);
        mOpacity->setRange(0.0, 1.0);
        mOpacity->setSingleStep(0.05);
        mOpacity->setDecimals(2);
        mOpacity->setToolTip("Opacity: how much the current layer shows");
        connect(mOpacity, &QDoubleSpinBox::editingFinished, this, [this] {
            if (QListWidgetItem* item = mLayers->currentItem(); item && !mSyncing)
                editLayer({{"opacity", mOpacity->value()}}, item->data(Qt::UserRole).toInt());
        });
        opacityForm->addRow("Opacity", mOpacity);
        lay->addLayout(opacityForm);
        v->addWidget(controls::section("Layers", layerBox, this, kSections, false));

        sync();
    }

    void sync()
    {
        mSyncing = true;
        const WeightPaintSettings& s = weightPaintSettings();
        const PaletteState st = paletteState();
        mTarget->setText(st.target);
        {
            QSignalBlocker b1(mValue), b2(mStrength), b3(mRadius), b4(mMirror), b5(mStrict), b6(mShowAutomatic);
            mBrush->setCurrent(int(s.brush));
            mValue->setValue(s.value);
            mStrength->setValue(s.stroke.strength);
            mRadius->setValue(s.stroke.radius);
            mMirror->setChecked(s.stroke.mirror);
            mAxis->setCurrent(s.mirrorAxis);
            mAxis->setEnabled(s.stroke.mirror);
            mStrict->setChecked(strict());
            mShowAutomatic->setChecked(showAutomatic());
            mPaintOn->setCurrent(s.paintMask ? 1 : 0);
            mPaintOn->setEnabled(st.layers && st.layers->active > 0);
            if (mStrokeOptions)
                mStrokeOptions->sync();
        }
        // Value means nothing to Relax and Settle.
        mValue->setEnabled(s.brush != Brush::Relax && s.brush != Brush::Settle);
        rebuildTree(st);
        rebuildLayers(st);
        mProxyRow->setVisible(!st.proxyFor.isEmpty());
        mProxyLabel->setText("Proxy for " + st.proxyFor.join(", "));
        mProxyLabel->setToolTip("Painting a weight proxy: Push copies its weights onto these meshes, Pull copies theirs onto it.");
        mUnder = st.underBrush;
        applyFilter();
        mSyncing = false;
    }

    /** Called as the brush moves, for the Under Brush filter. */
    void setUnderBrush(const std::unordered_set<uint32_t>& infs)
    {
        if (infs == mUnder)
            return;
        mUnder = infs;
        if (mUnderBrush->isChecked())
            applyFilter();
    }

private:
    static constexpr int kInfluenceRole = Qt::UserRole;
    static constexpr int kLinkRole = Qt::UserRole + 1;
    static constexpr int kMapRole = Qt::UserRole + 2;

    static void changed()
    {
        saveWeightPaintSettings();
        syncPalette();
        viewportTools::requestRedraw();
    }

    static void runOnTarget(const char* command, nlohmann::json call);

    void editLayer(nlohmann::json call, int layer)
    {
        call["layer"] = layer;
        runOnTarget("editSkinLayer", std::move(call));
    }

    void rebuildTree(const PaletteState& st)
    {
        QSignalBlocker block(mTree);
        mTree->clear();
        const WeightPaintSettings& s = weightPaintSettings();
        scene::Graph& g = scene::Graph::current();
        std::unordered_map<NodeId, QTreeWidgetItem*> byJoint;
        std::vector<std::pair<const scene::skin::Influence*, QTreeWidgetItem*>> made;
        for (const auto& i : st.influences)
        {
            auto* item = new QTreeWidgetItem;
            item->setText(0, QString::fromStdString(i.name.empty() ? "(disconnected)" : i.name));
            item->setData(0, kInfluenceRole, qlonglong(i.index));
            item->setData(0, kLinkRole, QString::fromStdString(g.node(i.link) ? g.node(i.link)->path() : std::string()));
            item->setIcon(1, icon(i.locked ? "lock" : "lock-open"));
            item->setToolTip(1, i.locked ? "Locked: painting leaves it alone. Click to unlock." : "Click to lock; Alt+click to lock every other joint.");
            if (int64_t(i.index) == s.receiver)
            {
                item->setIcon(2, icon("arrow-bar-to-down"));
                item->setToolTip(2, "Receiver: removed weight goes here");
            }
            if (i.locked)
                item->setForeground(0, themeColor("dimText"));
            byJoint[i.joint] = item;
            made.push_back({&i, item});
        }
        for (auto& [inf, item] : made)
        {
            QTreeWidgetItem* parent = nullptr;
            for (NodeId p = inf->joint != kNoNode ? g.parent(inf->joint) : kNoNode; p != kNoNode && !parent; p = g.parent(p))
                if (auto it = byJoint.find(p); it != byJoint.end())
                    parent = it->second;
            if (parent)
                parent->addChild(item);
            else
                mTree->addTopLevelItem(item);
            if (s.mapNode == kNoNode && int64_t(inf->index) == s.influence)
                mTree->setCurrentItem(item);
        }
        for (const auto& m : st.maps)
        {
            auto* item = new QTreeWidgetItem;
            item->setText(0, QString::fromStdString(m.label));
            item->setData(0, kMapRole, qulonglong(m.node));
            item->setData(0, kMapRole + 1, m.layer);
            item->setToolTip(0, m.layer < 0 ? "The deformer's weight map" : "A painted layer of the field the deformers read");
            mTree->addTopLevelItem(item);
            if (s.mapNode == m.node && s.mapLayer == m.layer)
                mTree->setCurrentItem(item);
        }
        mTree->expandAll();
        QString receiver = "Receiver: none · Ctrl+click a joint";
        for (const auto& i : st.influences)
            if (int64_t(i.index) == s.receiver)
                receiver = "Receiver: " + QString::fromStdString(i.name);
        mReceiverLabel->setText(receiver);
        mReceiverLabel->setVisible(s.mapNode == kNoNode && !st.influences.empty());
    }

    void rebuildLayers(const PaletteState& st)
    {
        QSignalBlocker block(mLayers);
        mLayers->clear();
        if (!st.layers)
        {
            mOpacity->setEnabled(false);
            return;
        }
        // Top first, as the layers stack.
        for (int i = int(st.layers->layers.size()) - 1; i >= 0; --i)
        {
            const scene::SkinLayer& l = st.layers->layers[size_t(i)];
            auto* item = new QListWidgetItem(QString::fromStdString(l.name), mLayers);
            item->setData(Qt::UserRole, i);
            if (i > 0)
            {
                item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
                item->setCheckState(l.enabled ? Qt::Checked : Qt::Unchecked);
            }
            if (i == st.layers->active)
            {
                mLayers->setCurrentItem(item);
                QSignalBlocker b(mOpacity);
                mOpacity->setValue(l.opacity);
                mOpacity->setEnabled(i > 0);
            }
        }
    }

    void applyFilter()
    {
        const QString text = mFilter->text().trimmed();
        const bool under = mUnderBrush->isChecked();
        std::function<bool(QTreeWidgetItem*)> walk = [&](QTreeWidgetItem* item) {
            bool anyChild = false;
            for (int k = 0; k < item->childCount(); ++k)
                anyChild |= walk(item->child(k));
            bool self = text.isEmpty() || item->text(0).contains(text, Qt::CaseInsensitive);
            if (under && item->data(0, kInfluenceRole).isValid())
                self = self && mUnder.contains(uint32_t(item->data(0, kInfluenceRole).toLongLong()));
            item->setHidden(!(self || anyChild));
            return self || anyChild;
        };
        for (int k = 0; k < mTree->topLevelItemCount(); ++k)
            walk(mTree->topLevelItem(k));
    }

    void pressed(QTreeWidgetItem* item, int column)
    {
        if (!item || !item->data(0, kInfluenceRole).isValid())
            return;
        const auto index = item->data(0, kInfluenceRole).toLongLong();
        const Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
        if (column == 1)
        {
            if (mods & Qt::AltModifier)
                soloLock(index);
            else
                setLocked({index}, !lockedNow(index));
            return;
        }
        if (mods & Qt::ControlModifier)
        {
            WeightPaintSettings& s = weightPaintSettings();
            s.receiver = s.receiver == index || lockedNow(index) ? -1 : index;
            changed();
        }
    }

    void picked(QTreeWidgetItem* item)
    {
        if (!item || mSyncing)
            return;
        WeightPaintSettings& s = weightPaintSettings();
        if (item->data(0, kMapRole).isValid())
        {
            s.mapNode = item->data(0, kMapRole).toULongLong();
            s.mapLayer = item->data(0, kMapRole + 1).toInt();
        }
        else
        {
            // Ctrl+click picks the receiver, not the painted joint.
            if (QApplication::keyboardModifiers() & Qt::ControlModifier)
                return;
            s.mapNode = kNoNode;
            s.influence = item->data(0, kInfluenceRole).toLongLong();
            if (s.receiver == s.influence)
                s.receiver = -1;
        }
        changed();
    }

    void contextMenu(QPoint at)
    {
        QTreeWidgetItem* item = mTree->itemAt(at);
        if (!item || !item->data(0, kInfluenceRole).isValid())
            return;
        const auto index = item->data(0, kInfluenceRole).toLongLong();
        QMenu menu(this);
        WeightPaintSettings& s = weightPaintSettings();
        if (s.receiver == index)
            menu.addAction("Clear Receiver", [] {
                weightPaintSettings().receiver = -1;
                changed();
            });
        else
            menu.addAction("Set as Receiver", [index] {
                weightPaintSettings().receiver = index;
                changed();
            })->setEnabled(!lockedNow(index));
        menu.addSeparator();
        menu.addAction(lockedNow(index) ? "Unlock" : "Lock", [index] { setLocked({index}, !lockedNow(index)); });
        menu.addAction("Lock Others", [index] { soloLock(index); });
        menu.addAction("Unlock All", [] {
            std::vector<int64_t> all;
            for (const auto& i : paletteState().influences)
                all.push_back(i.index);
            setLocked(all, false);
        });
        menu.exec(mTree->viewport()->mapToGlobal(at));
    }

    static bool lockedNow(int64_t index)
    {
        for (const auto& i : paletteState().influences)
            if (int64_t(i.index) == index)
                return i.locked;
        return false;
    }

    static void setLocked(const std::vector<int64_t>& indices, bool lock)
    {
        scene::Graph& g = scene::Graph::current();
        const PaletteState st = paletteState();
        UndoChunk chunk(lock ? "Lock Weights" : "Unlock Weights");
        for (const auto& i : st.influences)
            if (std::find(indices.begin(), indices.end(), int64_t(i.index)) != indices.end() && i.locked != lock && g.node(i.link))
                actions::run({{"command", "setAttr"}, {"args", {{"args", {g.node(i.link)->path() + ".lockWeights", lock}}}}});
        if (lock && std::find(indices.begin(), indices.end(), weightPaintSettings().receiver) != indices.end())
            weightPaintSettings().receiver = -1;
        changed();
    }

    /** Locks every joint but this one and unlocks it. */
    static void soloLock(int64_t index)
    {
        scene::Graph& g = scene::Graph::current();
        UndoChunk chunk("Lock Others");
        for (const auto& i : paletteState().influences)
            if (g.node(i.link) && i.locked != (int64_t(i.index) != index))
                actions::run({{"command", "setAttr"}, {"args", {{"args", {g.node(i.link)->path() + ".lockWeights", int64_t(i.index) != index}}}}});
        if (weightPaintSettings().receiver != index)
            weightPaintSettings().receiver = -1;
        changed();
    }

    QLabel *mTarget = nullptr, *mReceiverLabel = nullptr;
    QLineEdit* mFilter = nullptr;
    QToolButton* mUnderBrush = nullptr;
    QTreeWidget* mTree = nullptr;
    controls::Segmented *mBrush = nullptr, *mAxis = nullptr, *mPaintOn = nullptr;
    controls::SliderField *mValue = nullptr, *mStrength = nullptr, *mOpacity = nullptr;
    QDoubleSpinBox* mRadius = nullptr;
    QCheckBox *mStrict = nullptr, *mMirror = nullptr, *mShowAutomatic = nullptr;
    QListWidget* mLayers = nullptr;
    QWidget* mProxyRow = nullptr;
    QLabel* mProxyLabel = nullptr;
    brush::ui::StrokeOptions* mStrokeOptions = nullptr;
    std::unordered_set<uint32_t> mUnder;
    bool mSyncing = false;
};

QDockWidget* weightPaletteDock()
{
    for (QWidget* w = gWeightPalette; w; w = w->parentWidget())
        if (auto* d = qobject_cast<QDockWidget*>(w))
            return d;
    return nullptr;
}

void syncPalette()
{
    if (gWeightPalette)
        gWeightPalette->sync();
}

// ------------------------------------------------------------------------------------------------ tool

class WeightPaintTool final : public ViewportTool, public brush::DabSink
{
public:
    const char* name() const override { return "weightPaint"; }
    // The brush ring and its centre dot are the cursor.
    std::optional<QCursor> cursor() const override { return QCursor(Qt::BlankCursor); }

    void activate() override
    {
        gWeightTool = this;
        mAlive = std::make_shared<bool>(true);
        mControls.activate();
        panels::show(kWeightPaletteId);
        panels::placeToolPalette(weightPaletteDock());
        std::weak_ptr<bool> alive = mAlive;
        mListener = scene::Graph::current().addListener([this, alive](scene::Change c, NodeId id) {
            // Values only matter on the skin (weights, layers) and its influence links (locks); the rest would refresh per frame.
            if (mStroking || mRefreshQueued || (c == scene::Change::Value && !watches(id)))
                return;
            mRefreshQueued = true;
            QTimer::singleShot(0, [this, alive] {
                if (alive.expired())
                    return;
                mRefreshQueued = false;
                retarget();
            });
        });
        retarget();
        scene::skin::reportAutomatic(scene::Graph::current());
    }

    void deactivate() override
    {
        if (mStroking)
            endStroke();
        mControls.deactivate();
        scene::Graph::current().removeListener(mListener);
        mAlive.reset();
        gWeightTool = nullptr;
        if (QDockWidget* d = weightPaletteDock())
            d->hide();
    }

    bool ownsBrushRadius() const override { return true; }

    bool mousePress(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mousePress(e))
            return true;
        if (e->button() != Qt::LeftButton)
            return false;
        if ((e->modifiers() & Qt::ControlModifier) && (e->modifiers() & Qt::AltModifier))
        {
            pickReceiver(e->position(), view);
            return true;
        }
        if (!canPaint())
            return true;
        mMods = e->modifiers();
        beginStroke();
        const ViewInfo v = view;
        mStroke.begin(weightPaintSettings().stroke, mControls.input(e),
                      [this, v](glm::dvec2 px, brush::SurfaceHit& out) { return surfaceQuery(QPointF(px.x, px.y), v, out); }, *this, false);
        return true;
    }

    bool mouseMove(QMouseEvent* e, const ViewInfo& view) override
    {
        mHover = e->position();
        if (mControls.mouseMove(e))
            return true;
        if (mStroking)
        {
            mMods = e->modifiers();
            mStroke.move(mControls.input(e));
            viewportTools::requestRedraw();
            return true;
        }
        previewHover(view, e->modifiers());
        viewportTools::requestRedraw();
        return false;
    }

    bool mouseRelease(QMouseEvent* e, const ViewInfo&) override
    {
        if (mControls.mouseRelease(e))
            return true;
        if (mStroking)
            endStroke();
        return true;
    }

    bool keyPress(QKeyEvent* e, const ViewInfo&) override
    {
        if (mControls.keyPress(e))
            return true;
        switch (e->key())
        {
        case Qt::Key_Escape:
            QTimer::singleShot(0, [] { viewportTools::set(nullptr); });
            return true;
        default: return false;
        }
    }

    void draw(render::FrameState& s, OverlayPainter& p, const ViewInfo& view) override
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        if (!g.node(mMesh) || (mapMode() ? !g.node(mMap.node) : (mSkin == kNoNode || !g.node(mSkin))))
        {
            drawToolStatus(p, view, {.tool = "Weight Paint", .prompt = "select a skinned or deformed mesh"});
            return;
        }
        ensureBvh();
        addWeightTriangles(s);
        addPointDots(s);

        const WeightPaintSettings& ws = weightPaintSettings();
        QString infName = "(none)", receiver = "none";
        for (const auto& i : mInfluences)
        {
            if (int64_t(i.index) == ws.influence)
                infName = QString::fromStdString(i.name);
            if (int64_t(i.index) == ws.receiver)
                receiver = QString::fromStdString(i.name);
        }
        const Qt::KeyboardModifiers mods = mStroking ? mMods : QApplication::keyboardModifiers();
        const Brush b = brushFor(mods);
        ToolStatus st{.tool = "Weight Paint", .subject = QString::fromStdString(g.node(mMesh)->name()), .mode = kBrushNames[int(b)]};
        if (mapMode())
            st.fields = {{"map", QString::fromStdString(mMap.label)}};
        else if (maskMode())
            st.fields = {{"mask", activeLayerName()}};
        else
        {
            st.fields = {{"influence", infName}, {"receiver", receiver}};
            if (mAutomaticCount)
                st.fields.push_back({"automatic", QString::number(mAutomaticCount)});
            st.flags = {{"Strict", strict()}};
            if (auto l = scene::skin::layers(g, mSkin))
                st.fields.push_back({"layer", activeLayerName()});
            if (const auto targets = scene::weightproxy::targetsOf(g, mMesh); !targets.empty())
                st.fields.push_back({"proxy for", QString::fromStdString(g.node(targets.front())->name()) +
                                                      (targets.size() > 1 ? QString(" +%1").arg(targets.size() - 1) : QString())});
        }
        st.keys = {{"Shift", "relax"}, {"Ctrl", "subtract"}, {"Ctrl+Alt+click", "receiver"}};
        st.symmetry = ToolSymmetry{ws.stroke.mirror, QString(QChar('X' + ws.mirrorAxis))};
        drawToolStatus(p, view, st);

        // Refusals: the live stroke's, else what the brush would refuse where it hovers.
        const PaintOutcome refusal = mStroking ? mDabRefusal : mHoverRefusal;
        const bool painting = !mapMode() && !maskMode() && (b == Brush::Add || b == Brush::Replace || b == Brush::Scale);
        if (painting && scene::skin::refused(refusal))
            drawToolMessage(p, view, "Refused: " + hintText(refusal), warningSrgb());
        if (!mapMode() && !maskMode() && !strict())
            drawToolMessage(p, view, "Strict off: split weight is marked automatic", warningSrgb());

        const QPointF at = mControls.anchor(brush::ui::ringPosition(mStroke, mHover));
        std::optional<brush::ui::CursorSurface> surface;
        geo::BvhHit hit;
        if (raycast(at, view, hit))
        {
            const glm::dmat4 w = meshWorld();
            surface = brush::ui::CursorSurface{glm::dvec3(w * glm::dvec4(hit.point, 1.0)),
                                               glm::normalize(glm::dvec3(glm::transpose(glm::inverse(glm::dmat3(w))) * triangleNormal(hit.triangle)))};
        }
        brush::ui::CursorOptions co{.strengthRing = mControls.adjusting() == brush::ui::BrushControls::Adjust::Strength, .stroke = &mStroke};
        if (painting && scene::skin::refused(refusal))
            co.color = warningSrgb();
        brush::ui::drawCursor(p, view, ws.stroke, at, surface, co);
        if (!mFlash.empty() && mFlashClock.isValid() && mFlashClock.elapsed() < kFlashMs)
            QTimer::singleShot(50, [] { viewportTools::requestRedraw(); });
    }

    // Read by the palette.
    PaletteState state() const
    {
        PaletteState st;
        scene::Graph& g = scene::Graph::current();
        st.target = mMesh != kNoNode && g.node(mMesh) ? QString::fromStdString(g.node(mMesh)->name()) : QString("Select a skinned or deformed mesh");
        st.skin = mSkin;
        st.influences = mInfluences;
        st.maps = mMaps;
        st.layers = mSkin != kNoNode && g.node(mSkin) ? scene::skin::layers(g, mSkin) : nullptr;
        st.underBrush = mUnderBrush;
        if (mMesh != kNoNode && g.node(mMesh))
            for (NodeId m : scene::weightproxy::targetsOf(g, mMesh))
                st.proxyFor << QString::fromStdString(g.node(m)->name());
        return st;
    }

    NodeId mesh() const { return mMesh; }

    bool watches(NodeId id) const
    {
        return id != kNoNode && (id == mSkin || std::any_of(mInfluences.begin(), mInfluences.end(), [&](const auto& i) { return i.link == id; }));
    }

    /** Floods the painted influence (or map, or mask) over the selected vertices, else all. */
    void flood()
    {
        scene::Graph& g = scene::Graph::current();
        const WeightPaintSettings& s = weightPaintSettings();
        if (mapMode())
        {
            static const char* kModeArgs[] = {"add", "replace", "scale", "smooth", "smooth"};
            nlohmann::json call = {{"args", {nodeName(mMap.node)}}, {"value", s.value}, {"mode", kModeArgs[int(s.brush)]}, {"strength", s.stroke.strength}};
            if (mMap.layer >= 0)
            {
                call["layer"] = mMap.layer;
                call["mesh"] = nodeName(mMesh);
            }
            if (scene::components::mode() != scene::ComponentMode::Object)
                if (auto ids = scene::components::pointsOf(mMesh); !ids.empty())
                    call["points"] = ids;
            actions::run({{"command", "floodDeformerWeights"}, {"args", call}});
            viewportTools::requestRedraw();
            return;
        }
        if (mSkin == kNoNode || !g.node(mSkin))
            return;
        nlohmann::json call = {{"args", {nodeName(mMesh)}}};
        if (scene::components::mode() != scene::ComponentMode::Object)
            if (auto ids = scene::components::pointsOf(mMesh); !ids.empty())
                call["vertices"] = ids;
        if (maskMode())
        {
            call["value"] = s.value;
            runCommand("setSkinLayerMask", call);
            return;
        }
        if (s.brush == Brush::Relax)
        {
            call["keepAuthored"] = false;
            runCommand("relaxSkinWeights", call);
            return;
        }
        if (s.brush == Brush::Settle)
        {
            runCommand("settleSkinWeights", call);
            return;
        }
        std::string infName, receiverName;
        for (const auto& i : mInfluences)
        {
            if (int64_t(i.index) == s.influence)
                infName = i.name;
            if (int64_t(i.index) == s.receiver)
                receiverName = i.name;
        }
        if (infName.empty())
            return;
        static const char* kModes[] = {"add", "replace", "scale"};
        call["influence"] = infName;
        if (!receiverName.empty())
            call["receiver"] = receiverName;
        call["mode"] = kModes[int(s.brush)];
        call["value"] = s.value;
        call["strength"] = s.stroke.strength;
        call["strict"] = strict();
        const nlohmann::json r = runCommand("floodSkinWeights", call);
        if (r.is_object() && r.contains("refusedPoints"))
        {
            mFlash = r["refusedPoints"].get<std::vector<uint32_t>>();
            mFlashClock.start();
            mFlashCloud.reset();
        }
        viewportTools::requestRedraw();
    }

    nlohmann::json runCommand(const char* command, const nlohmann::json& call)
    {
        try
        {
            nlohmann::json r = ops::execute(command, call, CallSource::UI);
            viewportTools::requestRedraw();
            return r;
        }
        catch (const CommandError& e)
        {
            log::warn("{}", e.what());
            return nullptr;
        }
    }

private:
    static constexpr qint64 kFlashMs = 900;

    static std::string nodeName(NodeId id) { return scene::Graph::current().node(id)->path(); }

    glm::dmat4 meshWorld() const { return scene::Graph::current().node(mMesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix); }

    /** Painting a deformer's or field's map rather than a skin influence. */
    bool mapMode() const { return mMap.node != kNoNode; }
    /** Painting the active weight layer's mask. */
    bool maskMode() const
    {
        if (mapMode() || !weightPaintSettings().paintMask || mSkin == kNoNode)
            return false;
        auto l = scene::skin::layers(scene::Graph::current(), mSkin);
        return l && l->active > 0;
    }
    bool canPaint() const
    {
        if (mapMode())
            return mMap.mesh != nullptr;
        return mSkin != kNoNode && (maskMode() || weightPaintSettings().influence >= 0);
    }

    QString activeLayerName() const
    {
        auto l = scene::skin::layers(scene::Graph::current(), mSkin);
        return l ? QString::fromStdString(l->layers[size_t(std::clamp(l->active, 0, int(l->layers.size()) - 1))].name) : QString();
    }

    Brush brushFor(Qt::KeyboardModifiers mods) const { return (mods & Qt::ShiftModifier) ? Brush::Relax : weightPaintSettings().brush; }

    /**
     * The surface painted on, with point ids matching the weights: the skin's own output, or for a map the mesh as shown
     * when nothing above changes its points, else the deformer's input.
     */
    scene::MeshPtr deformed() const
    {
        scene::Graph& g = scene::Graph::current();
        g.evaluate();
        if (mapMode())
        {
            scene::MeshPtr shown = g.node(mMesh) ? g.node(mMesh)->as<scene::MeshPtr>(scene::MeshNode::kOutMesh) : nullptr;
            return shown && mMap.mesh && shown->pointCapacity() == mMap.mesh->pointCapacity() ? shown : mMap.mesh;
        }
        return g.node(mSkin)->as<scene::MeshPtr>(skinAttr(mSkin, &scene::skin::WeightHost::outMesh));
    }

    /** What changes when the painted map changes, for redraws. */
    scene::DataPtr mapKey() const
    {
        scene::Graph& g = scene::Graph::current();
        if (mapMode())
        {
            if (!g.node(mMap.node))
                return nullptr;
            int attr = mMap.layer < 0 ? int(scene::DeformerNode::kWeights) : int(scene::fields::kLayers);
            return std::get<scene::DataPtr>(g.storedValue({mMap.node, attr, -1}));
        }
        const int layers = skinAttr(mSkin, &scene::skin::WeightHost::layers);
        return layers >= 0 ? std::get<scene::DataPtr>(g.storedValue({mSkin, layers, -1})) : nullptr;
    }

    void retarget()
    {
        scene::Graph& g = scene::Graph::current();
        NodeId mesh = kNoNode, skin = kNoNode;
        const auto& sel = g.selection();
        for (auto it = sel.rbegin(); it != sel.rend() && mesh == kNoNode; ++it)
        {
            scene::Node* n = g.node(*it);
            if (!n)
                continue;
            if (n->type().isA("mesh") && !n->type().isA("lattice"))
            {
                skin = scene::skin::findPainted(g, n->id());
                if (skin != kNoNode || !scene::deformers::list(g, n->id()).empty())
                    mesh = n->id();
            }
            else if (dynamic_cast<scene::SkinClusterNode*>(n))
            {
                skin = n->id();
                mesh = scene::skin::meshOf(g, skin);
            }
            else if (scene::skin::weightHost(g, n->id()))
            {
                skin = n->id();
                mesh = scene::modifiers::meshOf(g, skin);
            }
            else if (scene::deformers::isDeformer(*n))
            {
                mesh = scene::modifiers::meshOf(g, n->id());
                skin = mesh != kNoNode ? scene::skin::findPainted(g, mesh) : kNoNode;
                weightPaintSettings().mapNode = n->id();
                weightPaintSettings().mapLayer = -1;
            }
        }
        // Keep painting the current mesh when the selection moves to a joint (picking influences from the viewport).
        if (mesh == kNoNode && mMesh != kNoNode && g.node(mMesh) && (mSkin == kNoNode || g.node(mSkin)))
        {
            mesh = mMesh;
            skin = mSkin;
        }
        if (mesh == kNoNode)
            skin = kNoNode;
        std::vector<scene::deformWeights::MapTarget> maps;
        if (mesh != kNoNode)
            try
            {
                maps = scene::deformWeights::paintable(g, mesh);
            }
            catch (const std::exception& e)
            {
                log::warn("paint weights: {}", e.what());
            }
        scene::deformWeights::MapTarget map;
        for (const auto& m : maps)
            if (m.node == weightPaintSettings().mapNode && m.layer == weightPaintSettings().mapLayer)
                map = m;
        if (map.node == kNoNode && skin == kNoNode && !maps.empty())
        {
            map = maps.front();
            weightPaintSettings().mapNode = map.node;
            weightPaintSettings().mapLayer = map.layer;
        }
        if (skin != mSkin || mesh != mMesh || map.node != mMap.node || map.layer != mMap.layer || map.mesh != mMap.mesh)
        {
            mBvhMesh.reset();
            mSymmetryFor.reset();
        }
        mMesh = mesh;
        mSkin = skin;
        mMap = map;
        mMaps = maps;
        mInfluences = skin != kNoNode ? scene::skin::influences(g, skin) : std::vector<scene::skin::Influence>{};
        WeightPaintSettings& s = weightPaintSettings();
        // A selected joint that is an influence becomes the one painted, like Maya's paint tool.
        [&] {
            for (auto it = sel.rbegin(); it != sel.rend(); ++it)
                for (const auto& i : mInfluences)
                    if (i.joint == *it)
                    {
                        s.influence = i.index;
                        s.mapNode = kNoNode;
                        mMap = {};
                        return;
                    }
        }();
        if (mMap.node == kNoNode && !mInfluences.empty() &&
            std::none_of(mInfluences.begin(), mInfluences.end(), [&](const auto& i) { return int64_t(i.index) == s.influence; }))
            s.influence = mInfluences.front().index;
        // A receiver that went away or got locked no longer takes weight.
        const bool receiverOk = std::any_of(mInfluences.begin(), mInfluences.end(), [&](const auto& i) { return int64_t(i.index) == s.receiver && !i.locked; });
        if (!receiverOk || s.receiver == s.influence)
            s.receiver = -1;
        countAutomatic();
        syncPalette();
        viewportTools::requestRedraw();
    }

    void countAutomatic()
    {
        mAutomaticCount = 0;
        if (mSkin == kNoNode || !scene::Graph::current().node(mSkin))
            return;
        mAutomaticCount = scene::skin::table(scene::Graph::current(), mSkin).automaticPoints().size();
    }

    void ensureBvh()
    {
        scene::MeshPtr m = deformed();
        if (!m || m == mBvhMesh || mStroking)
            return;
        mBvhMesh = m;
        mBvh.build(*m);
    }

    glm::dvec3 triangleNormal(geo::Index t) const
    {
        const auto& tris = mBvh.triangles();
        const geo::Mesh& m = *mBvhMesh;
        glm::dvec3 a = m.position(m.cornerPoint(tris.corners[size_t(t) * 3]));
        glm::dvec3 b = m.position(m.cornerPoint(tris.corners[size_t(t) * 3 + 1]));
        glm::dvec3 c = m.position(m.cornerPoint(tris.corners[size_t(t) * 3 + 2]));
        glm::dvec3 n = glm::cross(b - a, c - a);
        return glm::length(n) > 1e-18 ? glm::normalize(n) : glm::dvec3(0, 1, 0);
    }

    bool raycast(QPointF px, const ViewInfo& view, geo::BvhHit& hit)
    {
        if (mMesh == kNoNode || !mBvhMesh || mBvh.empty())
            return false;
        glm::dvec3 o, d;
        view.ray(px, o, d);
        glm::dmat4 inv = glm::inverse(meshWorld());
        return mBvh.raycast(glm::dvec3(inv * glm::dvec4(o, 1.0)), glm::dvec3(inv * glm::dvec4(d, 0.0)), hit);
    }

    /** The points under a brush centred at px, with their falloff. */
    std::vector<std::pair<uint32_t, float>> pointsAt(QPointF px, const ViewInfo& view)
    {
        brush::SurfaceHit h;
        if (!surfaceQuery(px, view, h))
            return {};
        brush::Dab d;
        d.center = h.center;
        d.normal = h.normal;
        d.radius = weightPaintSettings().stroke.radius * h.pixelSize;
        d.viewDir = h.viewDir;
        geo::Index tri = geo::kInvalid;
        if (!brush::SurfaceWalk::snap(mBvh, d, tri))
            return {};
        std::vector<std::pair<uint32_t, float>> out;
        for (auto [p, w] : mWalk.gather(*mBvhMesh, mBvh, tri, d, weightPaintSettings().stroke.resolveFalloff()))
            out.push_back({p, w});
        return out;
    }

    /**
     * Ctrl+Alt+click: the joint under the cursor becomes the receiver, else the heaviest other unlocked joint at the
     * vertex clicked. Clicking the receiver again clears it.
     */
    void pickReceiver(QPointF at, const ViewInfo& view)
    {
        if (mapMode() || mSkin == kNoNode)
            return;
        scene::Graph& g = scene::Graph::current();
        WeightPaintSettings& s = weightPaintSettings();
        int64_t pick = -1;
        double best = 1e300;
        for (const auto& picker : viewportHooks::pickers())
            for (const auto& h : picker(at, at, true, view))
                if (const scene::Node* n = g.find(h.name); n && h.pixels < best)
                    for (const auto& i : mInfluences)
                        if (i.joint == n->id() && !i.locked)
                        {
                            best = h.pixels;
                            pick = i.index;
                        }
        if (pick < 0)
        {
            ensureBvh();
            geo::BvhHit hit;
            const auto* w = dynamic_cast<const scene::SkinWeights*>(std::get<scene::DataPtr>(g.storedValue({mSkin, scene::SkinClusterNode::kWeights, -1})).get());
            if (w && raycast(at, view, hit))
            {
                const auto& tris = mBvh.triangles();
                geo::Index point = mBvhMesh->cornerPoint(tris.corners[size_t(hit.triangle) * 3]);
                double near = 1e300;
                for (int k = 0; k < 3; ++k)
                {
                    const geo::Index q = mBvhMesh->cornerPoint(tris.corners[size_t(hit.triangle) * 3 + k]);
                    if (const double d = glm::length(mBvhMesh->position(q) - hit.point); d < near)
                    {
                        near = d;
                        point = q;
                    }
                }
                const std::vector<char> locked = scene::skin::lockMask(mInfluences);
                float heaviest = 0.0f;
                if (point < w->pointCount())
                    for (uint32_t k = w->offsets[point]; k < w->offsets[point + 1]; ++k)
                    {
                        const uint32_t i = w->influences[k];
                        if (int64_t(i) != s.influence && !(i < locked.size() && locked[i]) && w->weights[k] > heaviest)
                        {
                            heaviest = w->weights[k];
                            pick = i;
                        }
                    }
            }
        }
        if (pick < 0 || pick == s.influence)
            return;
        s.receiver = s.receiver == pick ? -1 : pick;
        syncPalette();
        viewportTools::requestRedraw();
    }

    /** What the brush would do under the cursor: its refusal and the joints with weight there. */
    void previewHover(const ViewInfo& view, Qt::KeyboardModifiers mods)
    {
        mHoverRefusal = PaintOutcome::Painted;
        if (mapMode() || mSkin == kNoNode || !scene::Graph::current().node(mSkin))
            return;
        ensureBvh();
        const auto pts = pointsAt(mHover, view);
        const scene::SkinWeights* w =
            dynamic_cast<const scene::SkinWeights*>(std::get<scene::DataPtr>(scene::Graph::current().storedValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1})).get());
        std::unordered_set<uint32_t> under;
        if (w)
            for (auto [p, f] : pts)
                if (p < w->pointCount())
                    for (uint32_t k = w->offsets[p]; k < w->offsets[p + 1]; ++k)
                        under.insert(w->influences[k]);
        if (under != mUnderBrush)
        {
            mUnderBrush = under;
            if (gWeightPalette)
                gWeightPalette->setUnderBrush(mUnderBrush);
        }
        const Brush b = brushFor(mods);
        if (maskMode() || (b != Brush::Add && b != Brush::Replace && b != Brush::Scale) || pts.empty())
            return;
        // A dry run over the weights as stored: nothing changes, only the outcome is read.
        if (mHoverFor.get() != w)
        {
            mHoverTable = WeightTable::from(w, mBvhMesh ? mBvhMesh->pointCapacity() : 0);
            mHoverFor = std::get<scene::DataPtr>(scene::Graph::current().storedValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1}));
        }
        const auto report = skinpaint::paintPoints(mHoverTable, pts, paintMode(b), mods & Qt::ControlModifier, weightPaintSettings().value,
                                                   float(weightPaintSettings().stroke.strength), scene::skin::lockMask(mInfluences), paintOptions(), true);
        mHoverRefusal = report.mainRefusal();
    }

    scene::skin::PaintOptions paintOptions(int64_t influence = -2, int64_t receiver = -2) const
    {
        const WeightPaintSettings& s = weightPaintSettings();
        scene::skin::PaintOptions o;
        o.influence = uint32_t(std::max<int64_t>(0, influence == -2 ? s.influence : influence));
        o.receiver = receiver == -2 ? s.receiver : receiver;
        o.strict = strict();
        o.maxInfluences = mSkin != kNoNode && scene::Graph::current().node(mSkin)
                              ? scene::skin::maxInfluencesOf(scene::Graph::current(), mSkin)
                              : 4;
        return o;
    }

    void beginStroke()
    {
        scene::Graph& g = scene::Graph::current();
        ensureBvh();
        mStroking = true;
        mDabRefusal = PaintOutcome::Painted;
        mStrokeReport = {};
        if (mapMode())
        {
            mMapStart = scene::deformWeights::read(g, mMap);
            mMapWork = mMapStart;
            if (weightPaintSettings().stroke.mirror)
                buildSymmetry();
            return;
        }
        mStart = std::get<scene::DataPtr>(g.storedValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1}));
        const int layers = skinAttr(mSkin, &scene::skin::WeightHost::layers);
        mLayersStart = layers >= 0 ? std::get<scene::DataPtr>(g.storedValue({mSkin, layers, -1})) : nullptr;
        mInfluences = scene::skin::influences(g, mSkin);
        mRest = scene::skin::inputMesh(g, mSkin);
        if (maskMode())
        {
            auto l = scene::skin::layers(g, mSkin);
            mMaskLayer = l->active;
            mMapWork = l->layers[size_t(mMaskLayer)].mask;
            mMapWork.resize(mRest ? mRest->pointCapacity() : mMapWork.size(), 0.0f);
        }
        else
        {
            mEdit = std::make_unique<scene::skin::WeightEdit>(g, mSkin);
            mLive = scene::skin::table(g, mSkin);
            mLive.automatic.resize(mLive.points.size());
        }
        if (weightPaintSettings().stroke.mirror)
            buildSymmetry();
    }

    void buildSymmetry()
    {
        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr input = mapMode() ? mMap.mesh : scene::skin::inputMesh(g, mSkin);
        if (!input || (input == mSymmetryFor && mSymmetryAxis == weightPaintSettings().mirrorAxis))
            return;
        geo::Mesh world = *input;
        glm::dmat4 geom = mapMode() ? mMap.world : g.node(mSkin)->as<glm::dmat4>(skinAttr(mSkin, &scene::skin::WeightHost::geomMatrix));
        for (geo::Index p = 0; p < world.pointCapacity(); ++p)
            world.setPosition(p, glm::dvec3(geom * glm::dvec4(input->position(p), 1.0)));
        geo::SymmetryPlane plane;
        plane.normal = glm::dvec3(0.0);
        plane.normal[weightPaintSettings().mirrorAxis] = 1.0;
        mSymmetry = geo::SymmetryMap::build(world, plane, 0.1);
        mSymmetryFor = input;
        mSymmetryAxis = weightPaintSettings().mirrorAxis;
        mPartner.clear();
        for (const auto& i : mInfluences)
        {
            std::string other = scene::rig::mirrorName(i.name, "_l", "_r");
            if (other == i.name)
                other = scene::rig::mirrorName(i.name, "_r", "_l");
            int64_t j = scene::skin::indexOf(mInfluences, other);
            mPartner[i.index] = j >= 0 ? uint32_t(j) : i.index;
        }
    }

    int64_t partnerOf(int64_t inf) const
    {
        if (inf < 0)
            return inf;
        auto it = mPartner.find(uint32_t(inf));
        return it != mPartner.end() ? int64_t(it->second) : inf;
    }

    void apply(const std::vector<std::pair<uint32_t, float>>& pts, Brush b, bool invert, int64_t inf, int64_t receiver, float amount)
    {
        const WeightPaintSettings& s = weightPaintSettings();
        std::vector<std::pair<uint32_t, float>> scaled;
        scaled.reserve(pts.size());
        for (auto [p, f] : pts)
            scaled.push_back({p, f * amount});
        if (mapMode() || maskMode())
        {
            const geo::Mesh& surface = mapMode() ? *mMap.mesh : *mRest;
            if (b == Brush::Relax || b == Brush::Settle)
            {
                scene::deformWeights::smooth(surface, mMapWork, scaled);
                return;
            }
            for (auto [p, f] : scaled)
                if (p < mMapWork.size())
                    scene::deformWeights::paint(mMapWork[p], paintMode(b), invert, s.value, f);
            return;
        }
        WeightTable& t = mEdit->table();
        switch (b)
        {
        case Brush::Relax: scene::skin::relaxPoints(*mRest, t, scaled, mEdit->locked(), mEdit->maxInfluences(), 3); break;
        case Brush::Settle:
            skinpaint::settlePoints(scene::Graph::current(), mSkin, t, scaled, mEdit->locked(), mEdit->maxInfluences(), {});
            break;
        default:
        {
            const auto report = skinpaint::paintPoints(t, pts, paintMode(b), invert, s.value, amount, mEdit->locked(), paintOptions(inf, receiver));
            if (report.refusedCount())
                mDabRefusal = report.mainRefusal();
            mStrokeReport.merge(report);
        }
        }
        for (auto [p, f] : pts)
            mEdit->compositeInto(p, mLive);
    }

    /** The shared stroke's view of the skinned surface under a pixel, in the mesh's object space. */
    bool surfaceQuery(QPointF px, const ViewInfo& view, brush::SurfaceHit& out)
    {
        geo::BvhHit hit;
        if (!raycast(px, view, hit))
            return false;
        const glm::dmat4 w = meshWorld();
        const glm::dvec3 c(w * glm::dvec4(hit.point, 1.0));
        const double scale = std::max(1e-12, glm::length(glm::dvec3(w[0])));
        glm::dvec3 o, d;
        view.ray(px, o, d);
        out.center = hit.point;
        out.normal = triangleNormal(hit.triangle);
        out.viewDir = glm::normalize(glm::dvec3(glm::inverse(w) * glm::dvec4(d, 0.0)));
        out.right = brush::ui::screenRight(view, w);
        out.pixelSize = view.unitsPerPixel(c) / scale;
        out.worldToLocal = 1.0 / scale;
        return true;
    }

    void dab(const brush::Dab& d) override
    {
        LILA_ZONE();

        const WeightPaintSettings& s = weightPaintSettings();
        const Brush b = brushFor(mMods);
        const bool invert = mMods & Qt::ControlModifier;
        const float amount = float(s.stroke.strength * d.opacity * d.flow);
        const brush::Falloff falloff = s.stroke.resolveFalloff();
        std::vector<std::pair<uint32_t, float>> pts;
        for (brush::Dab copy : brush::radialDabs(d, s.stroke.radial))
        {
            geo::Index tri = geo::kInvalid;
            if (brush::SurfaceWalk::snap(mBvh, copy, tri))
                for (auto [p, w] : mWalk.gather(*mBvhMesh, mBvh, tri, copy, falloff))
                    pts.push_back({p, w});
        }
        if (s.stroke.radial.any())
            brush::mergeHits(pts);
        mDabRefusal = PaintOutcome::Painted;
        apply(pts, b, invert, s.influence, s.receiver, amount);
        if (s.stroke.mirror && !mSymmetry.empty())
        {
            std::vector<std::pair<uint32_t, float>> mirrored;
            for (auto [p, f] : pts)
                if (geo::Index q = mSymmetry.mirrorPoint(p); q != geo::kInvalid && q != p)
                    mirrored.push_back({q, f});
            apply(mirrored, b, invert, partnerOf(s.influence), partnerOf(s.receiver), amount);
        }
        pushLive();
    }

    /** Shows the stroke so far without recording undo; endStroke records the whole stroke once. */
    void pushLive()
    {
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        bool was = u.enabled();
        u.setEnabled(false);
        if (mapMode())
            scene::deformWeights::write(g, mMap, mMapWork);
        else if (maskMode())
            skinpaint::setLayerMask(g, mSkin, mMaskLayer, mMapWork);
        else
            g.setValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1}, scene::DataPtr(mLive.pack()));
        u.setEnabled(was);
        viewportTools::requestRedraw();
    }

    void endStroke()
    {
        mStroke.end();
        mStroking = false;
        mDabRefusal = PaintOutcome::Painted;
        scene::Graph& g = scene::Graph::current();
        UndoStack& u = UndoStack::get();
        if (mapMode())
        {
            if (!g.node(mMap.node))
                return;
            bool was = u.enabled();
            u.setEnabled(false);
            scene::deformWeights::write(g, mMap, mMapStart);
            u.setEnabled(was);
            UndoChunk chunk("paintDeformerWeights");
            scene::deformWeights::write(g, mMap, mMapWork);
            return;
        }
        if (!g.node(mSkin))
            return;
        bool was = u.enabled();
        u.setEnabled(false);
        g.setValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1}, mStart);
        if (const int layers = skinAttr(mSkin, &scene::skin::WeightHost::layers); layers >= 0)
            g.setValue({mSkin, layers, -1}, mLayersStart);
        u.setEnabled(was);
        if (mEdit)
        {
            UndoChunk chunk("paintSkinWeights");
            mEdit->commit();
        }
        else
        {
            UndoChunk chunk("paintSkinLayerMask");
            skinpaint::setLayerMask(g, mSkin, mMaskLayer, mMapWork);
        }
        mEdit.reset();
        mStart.reset();
        mLayersStart.reset();
        if (mStrokeReport.refusedCount())
        {
            mFlash = mStrokeReport.refused;
            mFlashClock.start();
            mFlashCloud.reset();
            log::info("Paint Weights: {} vertices refused: {}", mStrokeReport.refusedCount(), skinpaint::refusalHint(mStrokeReport.mainRefusal()));
        }
        scene::skin::reportAutomatic(g);
        countAutomatic();
    }

    /** The painted influence over the skinned surface through the weight colour ramp, maps and masks in gray. */
    void addWeightTriangles(render::FrameState& s)
    {
        LILA_ZONE();

        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr m = deformed();
        scene::DataPtr w = mapMode() ? mapKey() : std::get<scene::DataPtr>(g.storedValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1}));
        scene::DataPtr layers = mapMode() ? nullptr : mapKey();
        glm::dmat4 world = meshWorld();
        const bool mask = maskMode();
        bool same = m == mDrawnMesh && w == mDrawnWeights && layers == mDrawnLayers && weightPaintSettings().influence == mDrawnInfluence &&
                    mask == mDrawnMask && world == mDrawnWorld && glm::dvec3(s.eye) == mDrawnEye;
        if (!same && m && mBvhMesh && m->pointCapacity() == mBvhMesh->pointCapacity())
        {
            mDrawnMesh = m;
            mDrawnWeights = w;
            mDrawnLayers = layers;
            mDrawnMask = mask;
            mDrawnInfluence = weightPaintSettings().influence;
            mDrawnWorld = world;
            mDrawnEye = s.eye;
            const auto* sw = dynamic_cast<const scene::SkinWeights*>(w.get());
            const bool gray = mapMode() || mask || viewportDisplay::grayscaleWeights();
            std::vector<glm::vec4> ramp(m->pointCapacity(), viewportDisplay::weightColor(0.0f, gray));
            if (mapMode() || mask)
            {
                std::vector<float> values;
                if (mStroking)
                    values = mMapWork;
                else if (mapMode())
                    values = scene::deformWeights::read(g, mMap);
                else if (auto l = scene::skin::layers(g, mSkin))
                    values = l->layers[size_t(l->active)].mask;
                for (size_t p = 0; p < std::min(values.size(), ramp.size()); ++p)
                    ramp[p] = viewportDisplay::weightColor(values[p], true);
            }
            else if (sw)
                for (uint32_t p = 0; p < std::min<uint32_t>(sw->pointCount(), uint32_t(ramp.size())); ++p)
                    for (uint32_t k = sw->offsets[p]; k < sw->offsets[p + 1]; ++k)
                        if (int64_t(sw->influences[k]) == weightPaintSettings().influence)
                            ramp[p] = viewportDisplay::weightColor(sw->weights[k], gray);
            const auto& tris = mBvh.triangles();
            size_t count = tris.faces.size();
            mTris.resize(count * 3);
            glm::dmat3 nrm = glm::transpose(glm::inverse(glm::dmat3(world)));
            tbb::parallel_for(size_t(0), count, [&](size_t t) {
                glm::dvec3 pw[3];
                geo::Index pid[3];
                for (int k = 0; k < 3; ++k)
                {
                    pid[k] = m->cornerPoint(tris.corners[t * 3 + k]);
                    pw[k] = glm::dvec3(world * glm::dvec4(m->position(pid[k]), 1.0));
                }
                glm::dvec3 n = nrm * glm::cross(pw[1] - pw[0], pw[2] - pw[0]);
                glm::dvec3 toEye = glm::dvec3(s.eye) - pw[0];
                double facing = glm::length(n) > 1e-18 && glm::length(toEye) > 1e-18
                                    ? std::abs(glm::dot(glm::normalize(n), glm::normalize(toEye)))
                                    : 1.0;
                // A little headlight shading so the form still reads through the ramp.
                float shade = float(0.7 + 0.3 * facing);
                for (int k = 0; k < 3; ++k)
                    mTris[t * 3 + k] = {glm::vec3(pw[k]), glm::vec4(glm::vec3(ramp[pid[k]]) * shade, 1.0f)};
            });
            mAutomaticCloud.reset();
        }
        s.tris3d.insert(s.tris3d.end(), mTris.begin(), mTris.end());
    }

    /** Dots on the automatic vertices (Show Automatic) and, briefly, on the ones the last stroke refused. */
    void addPointDots(render::FrameState& s)
    {
        scene::Graph& g = scene::Graph::current();
        scene::MeshPtr m = mDrawnMesh;
        if (!m || mapMode())
            return;
        const glm::dmat4 world = meshWorld();
        auto cloudOf = [&](const std::vector<uint32_t>& pts) {
            auto c = std::make_shared<render::PointCloud>();
            c->origin = glm::dvec3(world[3]);
            for (uint32_t p : pts)
                if (p < m->pointCapacity() && m->pointValid(p))
                    c->points.push_back({glm::vec3(glm::dvec3(world * glm::dvec4(m->position(p), 1.0)) - c->origin), render::PointDraw::Normal});
            return c;
        };
        const glm::vec4 warn = themeLinear("issueWarning", {0.89f, 0.53f, 0.08f, 1.0f});
        if (showAutomatic() && !mask())
        {
            if (!mAutomaticCloud)
            {
                const auto* sw = dynamic_cast<const scene::SkinWeights*>(std::get<scene::DataPtr>(g.storedValue({mSkin, skinAttr(mSkin, &scene::skin::WeightHost::weights), -1})).get());
                std::vector<uint32_t> pts;
                if (sw && !sw->automatic.empty())
                    for (uint32_t p = 0; p < sw->pointCount(); ++p)
                        for (uint32_t k = sw->offsets[p]; k < sw->offsets[p + 1]; ++k)
                            if (sw->automatic[k])
                            {
                                pts.push_back(p);
                                break;
                            }
                mAutomaticCloud = cloudOf(pts);
            }
            if (!mAutomaticCloud->points.empty())
            {
                render::PointDraw d;
                d.cloud = mAutomaticCloud;
                d.colors[0] = glm::vec4(glm::vec3(warn) * 0.8f, 1.0f);
                d.size = 3.0f;
                s.points.push_back(d);
            }
        }
        if (!mFlash.empty() && mFlashClock.isValid() && mFlashClock.elapsed() < kFlashMs)
        {
            if (!mFlashCloud)
                mFlashCloud = cloudOf(mFlash);
            render::PointDraw d;
            d.cloud = mFlashCloud;
            const float fade = 1.0f - float(mFlashClock.elapsed()) / float(kFlashMs);
            d.colors[0] = glm::vec4(glm::vec3(warn), fade);
            d.size = 7.0f;
            s.points.push_back(d);
        }
        else if (!mFlash.empty() && mFlashClock.isValid())
        {
            mFlash.clear();
            mFlashCloud.reset();
        }
    }

    bool mask() const { return maskMode(); }

    std::shared_ptr<bool> mAlive;
    int mListener = 0;
    bool mRefreshQueued = false;

    NodeId mMesh = kNoNode;
    NodeId mSkin = kNoNode;
    std::vector<scene::skin::Influence> mInfluences;
    scene::deformWeights::MapTarget mMap;
    std::vector<scene::deformWeights::MapTarget> mMaps;
    std::vector<float> mMapStart, mMapWork;
    int mMaskLayer = 0;

    scene::MeshPtr mBvhMesh, mRest;
    geo::MeshBvh mBvh;
    brush::SurfaceWalk mWalk;

    scene::MeshPtr mSymmetryFor;
    int mSymmetryAxis = -1;
    geo::SymmetryMap mSymmetry;
    std::unordered_map<uint32_t, uint32_t> mPartner;

    bool mStroking = false;
    scene::DataPtr mStart, mLayersStart;
    std::unique_ptr<scene::skin::WeightEdit> mEdit;
    WeightTable mLive;
    skinpaint::PaintReport mStrokeReport;
    PaintOutcome mDabRefusal = PaintOutcome::Painted, mHoverRefusal = PaintOutcome::Painted;
    std::unordered_set<uint32_t> mUnderBrush;
    WeightTable mHoverTable;
    scene::DataPtr mHoverFor;
    size_t mAutomaticCount = 0;
    std::vector<uint32_t> mFlash;
    QElapsedTimer mFlashClock;
    std::shared_ptr<render::PointCloud> mFlashCloud, mAutomaticCloud;

    QPointF mHover;
    Qt::KeyboardModifiers mMods;
    brush::Stroke mStroke;
    brush::ui::BrushControls mControls{[]() -> brush::BrushSettings& { return weightPaintSettings().stroke; },
                                       [] {
                                           syncPalette();
                                           viewportTools::requestRedraw();
                                       },
                                       [] { saveWeightPaintSettings(); }};

    scene::MeshPtr mDrawnMesh;
    scene::DataPtr mDrawnWeights, mDrawnLayers;
    bool mDrawnMask = false;
    int64_t mDrawnInfluence = -2;
    glm::dmat4 mDrawnWorld{0.0};
    glm::dvec3 mDrawnEye{0.0};
    std::vector<render::LineVertex> mTris;
};

PaletteState paletteState() { return gWeightTool ? gWeightTool->state() : PaletteState{.target = "Select a skinned or deformed mesh"}; }

void WeightPaintPalette::runOnTarget(const char* command, nlohmann::json call)
{
    if (!gWeightTool || gWeightTool->mesh() == kNoNode || !scene::Graph::current().node(gWeightTool->mesh()))
        return;
    call["args"] = {scene::Graph::current().node(gWeightTool->mesh())->path()};
    gWeightTool->runCommand(command, call);
}

/** The mesh the weight actions work on: the paint tool's while it is up, else the selection's. */
nlohmann::json targetArgs()
{
    if (gWeightTool && gWeightTool->mesh() != kNoNode && scene::Graph::current().node(gWeightTool->mesh()))
        return {{"args", {scene::Graph::current().node(gWeightTool->mesh())->path()}}};
    return nlohmann::json::object();
}

void runWeightCommand(const char* command, nlohmann::json extra = nlohmann::json::object())
{
    nlohmann::json call = targetArgs();
    for (auto& [k, v] : extra.items())
        call[k] = v;
    // With the option box's remembered values, so its settings apply from the menu and the palette alike.
    actions::runWithOptions(command, call);
    viewportTools::requestRedraw();
}

/** Gives a weight action its command's option box while keeping its own run, which aims at the painted mesh. */
void optionBoxFor(const char* id, const char* command, nlohmann::json call = nlohmann::json::object())
{
    if (Action* a = actions::registry().find(id))
    {
        a->command = command;
        a->call = std::move(call);
    }
}
} // namespace

void startWeightPaint()
{
    viewportTools::set(std::make_unique<WeightPaintTool>());
}

void registerWeightPaint()
{
    panels::add({.id = kWeightPaletteId,
                 .title = "Paint Weights",
                 .create = [](QWidget* parent) -> QWidget* { return gWeightPalette = new WeightPaintPalette(parent); },
                 .area = Qt::LeftDockWidgetArea,
                 .icon = "brush",
                 .open = false,
                 .place = [](QDockWidget* d) { d->setProperty(panels::kOwnSizeProperty, true); },
                 .floatFirst = true,
                 .toolPalette = true,
                 .docs = "weight-painting"});
    auto add = [](const char* id, const char* label, std::function<void()> fn, const char* python, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Rig", .run = std::move(fn), .doc = doc, .python = python});
    };
    add("rig.floodWeights", "Flood Weights", [] {
        if (gWeightTool)
            gWeightTool->flood();
    }, "floodSkinWeights(influence=\"joint\")",
        "Applies the Paint Weights brush to the selected vertices, or the whole mesh, at once.");
    add("rig.acceptWeights", "Accept Automatic Weights", [] { runWeightCommand("acceptSkinWeights"); }, "acceptSkinWeights()",
        "Marks the automatic weights on the selected vertices (or all) as yours, so they stop warning.");
    add("rig.selectAutomaticWeights", "Select Automatic Weights", [] { runWeightCommand("selectAutomaticWeights"); }, "selectAutomaticWeights()",
        "Selects the vertices whose weights Lila split rather than you set.");
    add("rig.relaxWeights", "Relax Weights", [] { runWeightCommand("relaxSkinWeights"); }, "relaxSkinWeights()",
        "Settles the automatic weights between the ones you painted, over the selected vertices or all.");
    add("rig.settleWeights", "Settle Weights", [] { runWeightCommand("settleSkinWeights"); }, "settleSkinWeights()",
        "Fits the weights so the current pose loses its pinches and collapses, over the selected vertices or all.");
    add("rig.settleWeightsSimulate", "Settle Weights (Simulate)", [] { runWeightCommand("settleSkinWeights", {{"target", "simulate"}}); },
        "settleSkinWeights(target=\"simulate\")",
        "Relaxes the posed mesh toward its rest shape, keeping folds apart, then fits the weights to it.");
    add("rig.flattenWeightLayers", "Flatten Weight Layers", [] { runWeightCommand("flattenSkinLayers"); }, "flattenSkinLayers()",
        "Merges the weight layers into plain weights.");
    add("rig.createWeightProxy", "Create Weight Proxy", [] { runWeightCommand("createWeightProxy"); }, "createWeightProxy()",
        "Makes a light, single-sided mesh over the selected meshes to paint weights on, with edge loops at the joints. Push "
        "its weights back when done.");
    add("rig.pushProxyWeights", "Push Proxy Weights", [] { runWeightCommand("pushProxyWeights"); }, "pushProxyWeights()",
        "Copies the weight proxy's weights onto its render meshes.");
    add("rig.pullProxyWeights", "Pull Proxy Weights", [] { runWeightCommand("pullProxyWeights"); }, "pullProxyWeights()",
        "Copies the render meshes' weights onto their weight proxy.");
    // Words people search for, including other apps' names for the same tools.
    for (auto& [id, words] : std::initializer_list<std::pair<const char*, std::vector<std::string>>>{
             {"rig.paintWeights", {"weight paint", "skin weights", "paint skin", "artisan"}},
             {"rig.floodWeights", {"flood", "fill weights"}},
             {"rig.relaxWeights", {"smooth weights", "blur weights"}},
             {"rig.settleWeights", {"hammer", "fix collapse", "candy wrapper", "delta mush", "smooth skin"}},
             {"rig.settleWeightsSimulate", {"hammer", "simulate weights", "physics weights"}},
             {"rig.acceptWeights", {"automatic weights", "unauthored weights"}},
             {"rig.selectAutomaticWeights", {"automatic weights", "unauthored weights"}},
             {"rig.flattenWeightLayers", {"merge layers", "weight layers"}},
             {"rig.createWeightProxy", {"proxy", "cage", "weight cage", "transfer weights", "copy weights"}},
             {"rig.pushProxyWeights", {"transfer weights", "copy weights", "proxy"}},
             {"rig.pullProxyWeights", {"transfer weights", "copy weights", "proxy"}}})
        if (Action* act = actions::registry().find(id))
            for (const std::string& w : words)
                if (std::find(act->keywords.begin(), act->keywords.end(), w) == act->keywords.end())
                    act->keywords.push_back(w);
    optionBoxFor("rig.relaxWeights", "relaxSkinWeights");
    optionBoxFor("rig.settleWeights", "settleSkinWeights");
    optionBoxFor("rig.settleWeightsSimulate", "settleSkinWeights", {{"target", "simulate"}});
    optionBoxFor("rig.createWeightProxy", "createWeightProxy");
    actions::setEnabledWhen("rig.floodWeights", [] { return gWeightTool != nullptr; });
}
} // namespace lila::ui
