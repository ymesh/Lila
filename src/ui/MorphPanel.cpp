#include "ui/MorphPanel.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Log.h"
#include "scene/Drivers.h"
#include "scene/Graph.h"
#include "scene/Modifiers.h"
#include "scene/Morphs.h"
#include "scene/Transform.h"
#include "ui/Actions.h"
#include "ui/Controls.h"
#include "ui/Deletion.h"
#include "ui/Hotkeys.h"
#include "ui/NodeLookup.h"
#include "ui/Icons.h"
#include "ui/Panels.h"
#include "ui/SpinScrub.h"
#include "ui/Theme.h"
#include "ui/UiCommands.h"
#include "ui/UiCommon.h"
#include "ui/ValueReset.h"
#include "ui/ViewportHooks.h"
#include "ui/Overlay.h"

#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QSlider>
#include <QSplitter>
#include <QTableWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>

namespace lila::ui
{
namespace
{
using scene::Graph;

constexpr int kTargetRole = Qt::UserRole;
constexpr int kWeightSliderSteps = 1000;

std::string gCurrentTarget;
/** The current target's paths are drawn in the viewport while the panel shows. */
bool gShowPaths = true;
QPointer<QWidget> gMorphPanel;

/** The morph of the lead selection: a mesh's, a target's, or the morph itself. */
std::string selectedMorph()
{
    Graph& g = Graph::current();
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
    {
        scene::Node* n = g.node(*it);
        scene::NodeId m = scene::kNoNode;
        if (n->type().isA("morph"))
            m = n->id();
        else if (scene::morphs::isTarget(*n))
            m = scene::morphs::morphOf(g, n->id());
        else if (n->type().isA("mesh"))
            m = scene::morphs::find(g, n->id());
        if (m != scene::kNoNode)
            return g.node(m)->path();
    }
    return {};
}

/** Runs a target command on the current target, with its option box values. */
void onTarget(const char* command, json call = json::object())
{
    std::string t = currentMorphTarget();
    if (t.empty())
    {
        log::warn("{}: pick a morph target in the Morph Targets panel first", command);
        return;
    }
    json args = json::array({t});
    if (call.contains("args"))
        for (const json& a : call["args"])
            args.push_back(a);
    call["args"] = args;
    actions::runWithOptions(command, call);
}

/** Puts the target's pivot at the last selected node that isn't the target. */
void pivotFromSelected(const std::string& target)
{
    json sel = selectionNames();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
        if (it->get<std::string>() != target)
        {
            runUi("morphPath", {{"args", {target}}, {"pivot", *it}});
            return;
        }
    log::warn("Pivot From Selected: select the node to swing about, such as a locator at the eye's centre");
}

uint64_t gPathsVersion = 0;

/** The current target's point paths (only the farthest moving points), and its pivot in Pivot mode, while the panel shows. */
void drawPaths(render::FrameState& f, OverlayPainter&, const ViewInfo&)
{
    if (!gShowPaths || !gMorphPanel || !gMorphPanel->isVisible())
        return;
    const std::string t = currentMorphTarget();
    static std::string cachedFor;
    static uint64_t cachedVersion = ~uint64_t(0);
    static std::vector<render::LineVertex> cached;
    if (t.empty())
        return;
    if (t != cachedFor || gPathsVersion != cachedVersion)
    {
        cachedFor = t;
        cachedVersion = gPathsVersion;
        cached.clear();
        try
        {
            Graph& g = Graph::current();
            scene::Node* n = findNode(t);
            const scene::NodeId morph = n ? scene::morphs::morphOf(g, n->id()) : scene::kNoNode;
            const scene::NodeId mesh = morph != scene::kNoNode ? scene::modifiers::meshOf(g, morph) : scene::kNoNode;
            if (mesh == scene::kNoNode)
                return;
            const glm::dmat4 world = g.node(mesh)->as<glm::dmat4>(scene::Transform::kWorldMatrix);
            auto at = [&](const glm::dvec3& p) { return glm::vec3(world * glm::dvec4(p, 1.0)); };
            const glm::vec4 color = f.wirePreselect * glm::vec4(1.0f, 1.0f, 1.0f, 0.7f);
            double reach = 0.0;
            for (const std::vector<glm::dvec3>& path : scene::morphs::paths(g, n->id(), 24, 300))
            {
                for (size_t k = 1; k < path.size(); ++k)
                {
                    cached.push_back({at(path[k - 1]), color});
                    cached.push_back({at(path[k]), color});
                }
                reach = std::max(reach, glm::length(path.back() - path.front()));
            }
            if (scene::morphs::Interpolation(n->as<int64_t>(scene::morphs::kInterpolation)) == scene::morphs::Interpolation::Pivot)
            {
                const glm::dvec3 c = n->as<glm::dvec3>(scene::morphs::kPivot);
                const double r = std::max(reach * 0.1, 0.5);
                for (int axis = 0; axis < 3; ++axis)
                {
                    glm::dvec3 d(0.0);
                    d[axis] = r;
                    cached.push_back({at(c - d), f.wireSelected});
                    cached.push_back({at(c + d), f.wireSelected});
                }
            }
        }
        catch (const std::exception& e)
        {
            log::warn("Morph paths: {}", e.what());
        }
    }
    f.linesXray.insert(f.linesXray.end(), cached.begin(), cached.end());
}

/** Live preview while a slider drags, kept out of undo; the release commits one setAttr. */
void previewWeight(const std::string& target, double w)
{
    Graph& g = Graph::current();
    scene::Node* n = findNode(target);
    if (!n)
        return;
    UndoStack& u = UndoStack::get();
    const bool was = u.enabled();
    u.setEnabled(false);
    try
    {
        g.setValue({n->id(), scene::morphs::kWeight, -1}, w);
    }
    catch (const std::exception&)
    {
    }
    u.setEnabled(was);
}

/** A small read-only plot of a driven channel's curve over its keys' range. */
class CurvePlot : public QWidget
{
public:
    explicit CurvePlot(QWidget* parent) : QWidget(parent) { setMinimumHeight(90); }
    void setCurve(scene::anim::Curve c, double input)
    {
        mCurve = std::move(c);
        mInput = input;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPalette pal = palette();
        p.fillRect(rect(), pal.color(QPalette::Base));
        if (mCurve.keys.empty())
            return;
        double x0 = mCurve.keys.front().time, x1 = mCurve.keys.back().time;
        if (x1 - x0 < 1e-9)
        {
            x0 -= 1.0;
            x1 += 1.0;
        }
        const double pad = (x1 - x0) * 0.1;
        x0 -= pad;
        x1 += pad;
        double y0 = 1e300, y1 = -1e300;
        const int samples = std::max(2, width());
        std::vector<QPointF> pts;
        for (int i = 0; i < samples; ++i)
        {
            double x = x0 + (x1 - x0) * double(i) / double(samples - 1);
            double y = mCurve.evaluate(x);
            y0 = std::min(y0, y);
            y1 = std::max(y1, y);
            pts.push_back({x, y});
        }
        if (y1 - y0 < 1e-9)
        {
            y0 -= 1.0;
            y1 += 1.0;
        }
        const QRectF r = QRectF(rect()).adjusted(6, 6, -6, -6);
        auto map = [&](double x, double y) { return QPointF(r.left() + (x - x0) / (x1 - x0) * r.width(), r.bottom() - (y - y0) / (y1 - y0) * r.height()); };
        p.setPen(QPen(pal.color(QPalette::Mid), 1.0));
        p.drawRect(r);
        QPainterPath path;
        for (size_t i = 0; i < pts.size(); ++i)
            i == 0 ? path.moveTo(map(pts[i].x(), pts[i].y())) : path.lineTo(map(pts[i].x(), pts[i].y()));
        p.setPen(QPen(pal.color(QPalette::Text), 1.5));
        p.drawPath(path);
        p.setBrush(pal.color(QPalette::Text));
        for (const scene::anim::Key& k : mCurve.keys)
            p.drawEllipse(map(k.time, k.value), 3.0, 3.0);
        if (mInput >= x0 && mInput <= x1)
        {
            p.setPen(QPen(pal.color(QPalette::BrightText), 1.0, Qt::DashLine));
            QPointF top = map(mInput, y1), bottom = map(mInput, y0);
            p.drawLine(top, bottom);
        }
    }

private:
    scene::anim::Curve mCurve;
    double mInput = 0.0;
};

/** A middle click (not a drag) on a weight slider; the slider never sees the middle button, which would jump it. */
class MiddleClickReset : public QObject
{
public:
    MiddleClickReset(QWidget* target, std::function<void()> reset) : QObject(target), mReset(std::move(reset)) {}

protected:
    bool eventFilter(QObject* o, QEvent* e) override
    {
        if (e->type() != QEvent::MouseButtonPress && e->type() != QEvent::MouseButtonRelease && e->type() != QEvent::MouseMove)
            return false;
        auto* m = static_cast<QMouseEvent*>(e);
        const QPoint at = m->globalPosition().toPoint();
        if (e->type() == QEvent::MouseMove)
        {
            mClick.move(at.x(), at.y());
            return mClick.down();
        }
        if (m->button() != Qt::MiddleButton)
            return false;
        if (e->type() == QEvent::MouseButtonPress)
            mClick.press(at.x(), at.y(), mClock.elapsed());
        else if (mClick.release(at.x(), at.y(), mClock.elapsed()) && static_cast<QWidget*>(o)->isEnabled())
        {
            mReset();
            flashReset(static_cast<QWidget*>(o));
        }
        return true;
    }

private:
    std::function<void()> mReset;
    valueReset::ClickTracker mClick;
    QElapsedTimer mClock = [] {
        QElapsedTimer t;
        t.start();
        return t;
    }();
};

/** Both ends of a target's weight slider, one undo step. */
void setSliderRange(const std::string& target, double lo, double hi)
{
    if (!(hi > lo))
    {
        log::warn("Morph slider range: the maximum must be above the minimum");
        return;
    }
    UndoStack& undo = UndoStack::get();
    undo.openChunk("Set Morph Slider Range");
    runUi("setAttr", {{"args", {target + ".sliderMin", lo}}});
    runUi("setAttr", {{"args", {target + ".sliderMax", hi}}});
    undo.closeChunk();
}

/** Asks for a custom slider range; false when cancelled. */
bool askSliderRange(QWidget* parent, double& lo, double& hi)
{
    QDialog d(parent);
    d.setWindowTitle("Slider Range");
    auto* form = new QFormLayout(&d);
    auto spin = [&](double v) {
        auto* s = new QDoubleSpinBox(&d);
        s->setRange(-100.0, 100.0);
        s->setDecimals(3);
        s->setSingleStep(0.1);
        s->setValue(v);
        return s;
    };
    QDoubleSpinBox* min = spin(lo);
    QDoubleSpinBox* max = spin(hi);
    form->addRow("Min", min);
    form->addRow("Max", max);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &d);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &d, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    form->addRow(buttons);
    if (d.exec() != QDialog::Accepted)
        return false;
    lo = min->value();
    hi = max->value();
    return true;
}

const QStringList kTangents = {"linear", "spline", "flat", "step", "auto", "clamped"};
} // namespace

std::string currentMorphTarget()
{
    Graph& g = Graph::current();
    if (scene::Node* n = findNode(gCurrentTarget); n && scene::morphs::isTarget(*n))
        return gCurrentTarget;
    for (auto it = g.selection().rbegin(); it != g.selection().rend(); ++it)
        if (scene::morphs::isTarget(*g.node(*it)))
            return g.node(*it)->path();
    return {};
}

// ------------------------------------------------------------------------------------------------ morph targets

MorphTargetsPanel::MorphTargetsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    auto add = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        controls::setRole(b, "push");
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
    };
    add("Add Targets", "Add Targets\nAdd the selected meshes as targets of the last selected mesh (a morph is made if needed)", [] {
        json sel = selectionNames();
        if (sel.size() < 2)
        {
            log::warn("Add Morph Target: select the target meshes, then the base mesh last");
            return;
        }
        json args = json::array({sel.back()});
        for (size_t i = 0; i + 1 < sel.size(); ++i)
            args.push_back(sel[i]);
        runUi("addMorphTarget", {{"args", args}});
    });
    add("New Empty", "New Empty Target\nAdd an empty target to sculpt with Edit", [this] {
        if (mMorph.empty())
            return;
        bool ok = false;
        QString name = QInputDialog::getText(this, "New Morph Target", "Name", QLineEdit::Normal, "Target", &ok);
        if (ok && !name.isEmpty())
            runUi("addMorphTarget", {{"args", {mMorph}}, {"name", name.toStdString()}});
    });
    bar->addWidget(controls::push("deform.editMorphTarget", this, "Edit"));
    bar->addWidget(controls::overflow({"deform.addInBetween", "deform.flipMorphTarget", "deform.symmetrizeMorphTarget", "deform.extractMorphTarget",
                                       "deform.setMorphTarget", "deform.sculptCorrective", "deform.addCorrective"},
                                      this));
    bar->addStretch(1);
    QToolButton* trash = toolButton(this, "trash", QString::fromStdString(actions::tooltip("deform.removeMorphTarget")));
    connect(trash, &QToolButton::clicked, this, [] { actions::run(json("deform.removeMorphTarget")); });
    bar->addWidget(trash);
    v->addLayout(bar);

    mHeader = new QLabel(this);
    mHeader->setContentsMargins(4, 2, 4, 2);
    mHeader->setForegroundRole(QPalette::PlaceholderText);
    v->addWidget(mHeader);

    mTree = new QTreeWidget(this);
    deletion::claim(mTree, [] {
        if (std::string t = currentMorphTarget(); !t.empty())
            runUi("removeMorphTarget", {{"args", {t}}});
    });
    mTree->setColumnCount(5);
    mTree->setHeaderLabels({"Target", "Weight", "", "", ""});
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Hotkeys::passThrough(mTree);
    QHeaderView* header = mTree->header();
    header->setStretchLastSection(true);
    header->setSectionResizeMode(kName, QHeaderView::Interactive);
    header->resizeSection(kName, 120);
    header->setSectionResizeMode(kSlider, QHeaderView::Interactive);
    header->resizeSection(kSlider, 110);
    header->setSectionResizeMode(kValue, QHeaderView::Fixed);
    header->resizeSection(kValue, 52);
    header->setSectionResizeMode(kEdit, QHeaderView::Fixed);
    header->resizeSection(kEdit, 26);
    setEmptyHint(mTree, "No morph targets. Select the target meshes, then the base mesh, and click Add Targets.");
    v->addWidget(mTree, 1);

    connect(mTree, &QTreeWidget::currentItemChanged, this, [](QTreeWidgetItem* item, QTreeWidgetItem*) {
        gCurrentTarget = item ? item->data(kName, kTargetRole).toString().toStdString() : std::string();
    });
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int column) {
        if (column != kName)
            return;
        const std::string t = item->data(kName, kTargetRole).toString().toStdString();
        bool ok = false;
        QString name = QInputDialog::getText(this, "Rename Morph Target", "Name", QLineEdit::Normal, item->text(kName), &ok);
        if (ok && !name.isEmpty() && name != item->text(kName))
            if (json r = runUi("renameMorphTarget", {{"args", {t, name.toStdString()}}}); r.is_string())
                gCurrentTarget = r.get<std::string>();
    });

    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTree, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QTreeWidgetItem* item = mTree->itemAt(at);
        if (!item)
            return;
        mTree->setCurrentItem(item);
        QMenu menu;
        fillTargetMenu(&menu, item);
        menu.exec(mTree->viewport()->mapToGlobal(at));
    });

    gMorphPanel = this;
    QPointer<MorphTargetsPanel> self(this);
    mGraphListener = Graph::current().addListener([self](scene::Change, scene::NodeId) {
        ++gPathsVersion;
        if (self)
            self->scheduleRefresh();
    });
    refresh();
}

MorphTargetsPanel::~MorphTargetsPanel() { Graph::current().removeListener(mGraphListener); }

void MorphTargetsPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void MorphTargetsPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(30, this, [this] {
        mPending = false;
        refresh();
    });
}

void MorphTargetsPanel::refresh()
{
    // A slider or value being dragged keeps its row; the release refreshes.
    for (QSlider* s : mTree->findChildren<QSlider*>())
        if (s->isSliderDown())
            return;
    if (QApplication::mouseButtons() != Qt::NoButton && mTree->underMouse())
    {
        scheduleRefresh();
        return;
    }
    if (std::string m = selectedMorph(); !m.empty())
        mMorph = m;
    Graph& g = Graph::current();
    if (!mMorph.empty() && !findNode(mMorph))
        mMorph.clear();
    json list = mMorph.empty() ? json::array() : runUi("morphTargets", {{"args", {mMorph}}});
    if (!list.is_array())
        list = json::array();
    std::string meshName;
    if (scene::Node* m = findNode(mMorph))
        if (scene::NodeId mesh = scene::modifiers::meshOf(g, m->id()); mesh != scene::kNoNode)
            meshName = g.node(mesh)->path();
    mHeader->setText(QString::fromStdString(meshName + "  /  " + mMorph));
    mHeader->setVisible(!mMorph.empty());

    const std::string keep = gCurrentTarget;
    mTree->setUpdatesEnabled(false);
    mTree->clear();
    const QColor dim = palette().color(QPalette::Disabled, QPalette::Text);
    for (const json& t : list)
    {
        const std::string node = t["node"].get<std::string>();
        auto* item = new QTreeWidgetItem(mTree);
        item->setData(kName, kTargetRole, QString::fromStdString(node));
        item->setText(kName, QString::fromStdString(t["name"].get<std::string>()));
        item->setToolTip(kName, QString::fromStdString(node + "\nDouble-click to rename."));
        const bool driven = t["driver"].is_string();
        QStringList info;
        if (driven)
            info << QString::fromStdString("driven by " + t["driver"].get<std::string>());
        if (!t["inBetweens"].empty())
        {
            QStringList ws;
            for (const json& w : t["inBetweens"])
                ws << QString::number(w.get<double>());
            info << "in-betweens " + ws.join(", ");
        }
        if (t["live"].get<bool>())
            info << "live";
        if (t["interpolation"] != "linear")
            info << QString::fromStdString(t["interpolation"].get<std::string>()) + " path";
        if (t["response"] != "linear")
            info << niceName(t["response"].get<std::string>()).toLower();
        item->setText(kInfo, info.join(" | "));
        item->setToolTip(kInfo, item->text(kInfo));

        const double weight = t["weight"].get<double>();
        const double lo = t["sliderMin"].get<double>(), hi = std::max(t["sliderMax"].get<double>(), lo + 1e-3);
        auto* slider = new QSlider(Qt::Horizontal, mTree);
        slider->setRange(int(std::lround(lo * kWeightSliderSteps)), int(std::lround(hi * kWeightSliderSteps)));
        slider->setValue(int(std::lround(std::clamp(weight, lo, hi) * kWeightSliderSteps)));
        slider->setEnabled(!driven);
        slider->setToolTip(hintedTip(QString("%1 to %2").arg(lo).arg(hi), resetHint("0")));
        slider->installEventFilter(new MiddleClickReset(slider, [node] { runUi("setAttr", {{"args", {node + ".weight", 0.0}}}); }));
        auto* value = new QDoubleSpinBox(mTree);
        value->setRange(std::min(-10.0, lo), std::max(10.0, hi));
        value->setKeyboardTracking(false);
        setSpinDefault(value, 0.0);
        value->setDecimals(3);
        value->setSingleStep(0.05);
        value->setFrame(false);
        value->setButtonSymbols(QAbstractSpinBox::NoButtons);
        value->setValue(weight);
        value->setEnabled(!driven);
        if (driven)
            item->setForeground(kName, dim);
        auto start = std::make_shared<double>(weight);
        connect(slider, &QSlider::sliderPressed, this, [start, value] { *start = value->value(); });
        connect(slider, &QSlider::valueChanged, this, [slider, value, node](int v) {
            const double w = double(v) / kWeightSliderSteps;
            value->blockSignals(true);
            value->setValue(w);
            value->blockSignals(false);
            if (slider->isSliderDown())
                previewWeight(node, w);
        });
        connect(slider, &QSlider::sliderReleased, this, [slider, start, node] {
            const double w = double(slider->value()) / kWeightSliderSteps;
            previewWeight(node, *start);
            runUi("setAttr", {{"args", {node + ".weight", w}}});
        });
        connect(value, &QDoubleSpinBox::valueChanged, this, [node](double w) { runUi("setAttr", {{"args", {node + ".weight", w}}}); });
        mTree->setItemWidget(item, kSlider, slider);
        mTree->setItemWidget(item, kValue, value);

        auto* edit = toolButton(mTree, "pencil", "Edit Target\nSculpt this target on an edit mesh; click again to bake it back");
        edit->setCheckable(true);
        edit->setChecked(t["editing"].is_string());
        connect(edit, &QToolButton::clicked, this, [node](bool on) { runUi("editMorphTarget", {{"args", {node}}, {"on", on}}); });
        mTree->setItemWidget(item, kEdit, edit);
        if (node == keep)
            mTree->setCurrentItem(item);
    }
    mTree->setUpdatesEnabled(true);
}

void MorphTargetsPanel::fillTargetMenu(QMenu* menu, QTreeWidgetItem* item)
{
    const std::string node = item->data(kName, kTargetRole).toString().toStdString();
    scene::Node* n = findNode(node);
    if (!n)
        return;
    const double lo = n->as<double>(scene::morphs::kSliderMin), hi = n->as<double>(scene::morphs::kSliderMax);
    auto* slider = qobject_cast<QSlider*>(mTree->itemWidget(item, kSlider));
    menu->addAction("Reset Weight", this, [node] { runUi("setAttr", {{"args", {node + ".weight", 0.0}}}); })
        ->setEnabled(slider && slider->isEnabled());
    QMenu* range = menu->addMenu("Slider Range");
    bool preset = false;
    for (auto [a, b] : {std::pair{0.0, 1.0}, std::pair{-1.0, 1.0}, std::pair{0.0, 2.0}})
    {
        QAction* act = range->addAction(QString("%1 to %2").arg(a).arg(b), this, [node, a, b] { setSliderRange(node, a, b); });
        act->setCheckable(true);
        act->setChecked(a == lo && b == hi);
        preset |= act->isChecked();
    }
    range->addSeparator();
    QAction* custom = range->addAction(preset ? QString("Custom...") : QString("Custom (%1 to %2)...").arg(lo).arg(hi), this, [this, node, lo, hi] {
        double a = lo, b = hi;
        if (askSliderRange(this, a, b))
            setSliderRange(node, a, b);
    });
    custom->setCheckable(true);
    custom->setChecked(!preset);

    menu->addSeparator();
    Graph& g = Graph::current();
    auto choice = [&](QMenu* sub, const std::string& label, bool checked, auto fn) {
        QAction* a = sub->addAction(QString::fromStdString(label), this, fn);
        a->setCheckable(true);
        a->setChecked(checked);
    };
    const auto mode = scene::morphs::Interpolation(n->as<int64_t>(scene::morphs::kInterpolation));
    QMenu* path = menu->addMenu("Path");
    const std::vector<std::string>& modes = g.attr({n->id(), scene::morphs::kInterpolation, -1}).enumNames;
    for (size_t i = 0; i < modes.size(); ++i)
        choice(path, niceName(modes[i]).toStdString(), size_t(mode) == i,
               [node, m = modes[i]] { runUi("morphPath", {{"args", {node}}, {"interpolation", m}}); });
    path->addSeparator();
    path->addAction("Pivot From Selected", this, [node] { pivotFromSelected(node); })
        ->setEnabled(mode == scene::morphs::Interpolation::Pivot);
    QAction* show = path->addAction("Show Paths", this, [](bool on) { gShowPaths = on; });
    show->setCheckable(true);
    show->setChecked(gShowPaths);

    // Control Rig's easing names come in families (quadraticEaseIn, quadraticEaseOut ...): one submenu per family.
    QMenu* response = menu->addMenu("Response");
    const std::vector<std::string>& eases = g.attr({n->id(), scene::morphs::kResponse, -1}).enumNames;
    const int64_t current = n->as<int64_t>(scene::morphs::kResponse);
    std::map<std::string, QMenu*> families;
    for (size_t i = 0; i < eases.size(); ++i)
    {
        auto set = [node, e = eases[i]] { runUi("morphPath", {{"args", {node}}, {"response", e}}); };
        const size_t cut = eases[i].find("Ease");
        if (cut == std::string::npos)
        {
            choice(response, niceName(eases[i]).toStdString(), current == int64_t(i), set);
            continue;
        }
        const std::string family = eases[i].substr(0, cut);
        QMenu*& sub = families[family];
        if (!sub)
            sub = response->addMenu(niceName(family));
        if (current == int64_t(i))
        {
            sub->menuAction()->setCheckable(true);
            sub->menuAction()->setChecked(true);
        }
        choice(sub, niceName(eases[i].substr(cut + 4)).toStdString(), current == int64_t(i), set);
    }
}

// ------------------------------------------------------------------------------------------------ driven channels

DrivenChannelsPanel::DrivenChannelsPanel(QWidget* parent) : QWidget(parent)
{
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(2, 2, 2, 2);
    v->setSpacing(2);

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(0, 0, 0, 0);
    bar->setSpacing(2);
    auto add = [&](const QString& text, const QString& tip, auto fn) {
        auto* b = new QToolButton(this);
        b->setText(text);
        b->setToolTip(tip);
        controls::setRole(b, "push");
        connect(b, &QToolButton::clicked, this, fn);
        bar->addWidget(b);
    };
    add("Set Key", "Set Driven Key\nKey the current channel at the driver's value now (asks for the driven value)", [this] {
        std::string c = current();
        if (c.empty())
            return;
        bool ok = false;
        double v = QInputDialog::getDouble(this, "Set Driven Key", "Driven value at the driver's current value", 0.0, -1e9, 1e9, 4, &ok);
        if (ok)
            runUi("setDrivenKey", {{"args", {c}}, {"value", v}});
    });
    add("Curve Name", "Curve Name\nThe Unreal curve the channel bakes into on export", [this] {
        std::string c = current();
        if (c.empty())
            return;
        bool ok = false;
        QString name = QInputDialog::getText(this, "Curve Name", "Unreal curve (empty for none)", QLineEdit::Normal, "", &ok);
        if (ok)
            runUi("editDrivenChannel", {{"args", {c}}, {"curveName", name.toStdString()}});
    });
    bar->addStretch(1);
    QToolButton* trash = toolButton(this, "trash", "Delete Channel\nDelete the current channel; the driven plug keeps its value");
    connect(trash, &QToolButton::clicked, this, [this] {
        if (std::string c = current(); !c.empty())
            runUi("removeDrivenChannel", {{"args", {c}}});
    });
    bar->addWidget(trash);
    v->addLayout(bar);

    auto* split = new QSplitter(Qt::Vertical, this);
    mTree = new QTreeWidget(split);
    mTree->setColumnCount(3);
    mTree->setHeaderLabels({"Driver", "Driven", "Curve"});
    deletion::claim(mTree, [this] {
        if (std::string c = current(); !c.empty())
            runUi("removeDrivenChannel", {{"args", {c}}});
    });
    mTree->setRootIsDecorated(false);
    mTree->setIndentation(0);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::SingleSelection);
    mTree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    Hotkeys::passThrough(mTree);
    setEmptyHint(mTree, "No driven channels. In the Channel Box, use Set as Driver, then Drive From on the driven channel.");

    auto* lower = new QWidget(split);
    auto* lv = new QVBoxLayout(lower);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(2);
    mPlot = new CurvePlot(lower);
    lv->addWidget(mPlot);
    mKeys = new QTableWidget(lower);
    mKeys->setColumnCount(3);
    mKeys->setHorizontalHeaderLabels({"Driver", "Driven", "Interpolation"});
    mKeys->verticalHeader()->setVisible(false);
    mKeys->horizontalHeader()->setStretchLastSection(true);
    lv->addWidget(mKeys, 1);
    auto* keyBar = new QHBoxLayout;
    auto* addKey = toolButton(lower, "plus", "Add Key\nAdd a key after the last one");
    auto* removeKey = toolButton(lower, "minus", "Remove Key\nRemove the selected key");
    keyBar->addWidget(addKey);
    keyBar->addWidget(removeKey);
    keyBar->addStretch(1);
    lv->addLayout(keyBar);
    split->addWidget(mTree);
    split->addWidget(lower);
    v->addWidget(split, 1);

    connect(mTree, &QTreeWidget::currentItemChanged, this, [this] { refreshKeys(); });
    connect(mKeys, &QTableWidget::itemChanged, this, [this] {
        if (!mFilling)
            commitKeys();
    });
    connect(addKey, &QToolButton::clicked, this, [this] {
        std::string c = current();
        if (c.empty())
            return;
        json info = runUi("drivenChannels", {{"args", {c}}});
        if (!info.is_array() || info.empty())
            return;
        json keys = info[0]["keys"];
        double in = keys.empty() ? 0.0 : keys.back()[0].get<double>() + 1.0;
        double out = keys.empty() ? 0.0 : keys.back()[1].get<double>();
        runUi("setDrivenKey", {{"args", {c}}, {"input", in}, {"value", out}});
    });
    auto removeDrivenKey = [this] {
        std::string c = current();
        if (!c.empty() && mKeys->currentRow() >= 0)
            runUi("removeDrivenKey", {{"args", {c}}, {"index", mKeys->currentRow()}});
    };
    connect(removeKey, &QToolButton::clicked, this, removeDrivenKey);
    deletion::claim(mKeys, removeDrivenKey);

    QPointer<DrivenChannelsPanel> self(this);
    mGraphListener = Graph::current().addListener([self](scene::Change c, scene::NodeId) {
        if (self && c != scene::Change::Selection)
            self->scheduleRefresh();
    });
    refresh();
}

DrivenChannelsPanel::~DrivenChannelsPanel() { Graph::current().removeListener(mGraphListener); }

void DrivenChannelsPanel::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refresh();
}

void DrivenChannelsPanel::scheduleRefresh()
{
    if (mPending || !isVisible())
        return;
    mPending = true;
    QTimer::singleShot(30, this, [this] {
        mPending = false;
        refresh();
    });
}

std::string DrivenChannelsPanel::current() const
{
    if (QTreeWidgetItem* it = mTree->currentItem())
        return it->data(0, kTargetRole).toString().toStdString();
    return {};
}

void DrivenChannelsPanel::refresh()
{
    if (QWidget* f = QApplication::focusWidget(); f && mKeys->isAncestorOf(f))
        return;
    const std::string keep = current();
    json list = runUi("drivenChannels", json::object());
    mTree->blockSignals(true);
    mTree->clear();
    for (const json& c : list.is_array() ? list : json::array())
    {
        auto* item = new QTreeWidgetItem(mTree);
        const std::string node = c["node"].get<std::string>();
        item->setData(0, kTargetRole, QString::fromStdString(node));
        item->setText(0, c["driver"].is_string() ? QString::fromStdString(c["driver"].get<std::string>()) : QString("(input)"));
        QStringList driven;
        for (const json& d : c["driven"])
            driven << QString::fromStdString(d.get<std::string>());
        item->setText(1, driven.isEmpty() ? QString("(curve only)") : driven.join(", "));
        item->setText(2, QString::fromStdString(c["curveName"].get<std::string>()));
        item->setToolTip(0, QString::fromStdString(node));
        if (node == keep)
            mTree->setCurrentItem(item);
    }
    mTree->blockSignals(false);
    refreshKeys();
}

void DrivenChannelsPanel::refreshKeys()
{
    mFilling = true;
    mKeys->setRowCount(0);
    std::string c = current();
    Graph& g = Graph::current();
    scene::Node* n = c.empty() ? nullptr : findNode(c);
    if (!n || !scene::drivers::isChannel(*n))
    {
        static_cast<CurvePlot*>(mPlot)->setCurve({}, 0.0);
        mFilling = false;
        return;
    }
    scene::anim::Curve curve = scene::drivers::channelCurve(*n);
    mKeys->setRowCount(int(curve.keys.size()));
    for (int r = 0; r < int(curve.keys.size()); ++r)
    {
        const scene::anim::Key& k = curve.keys[size_t(r)];
        mKeys->setItem(r, 0, new QTableWidgetItem(QString::number(k.time, 'g', 6)));
        mKeys->setItem(r, 1, new QTableWidgetItem(QString::number(k.value, 'g', 6)));
        auto* combo = new QComboBox(mKeys);
        for (const QString& t : kTangents)
            combo->addItem(niceName(t.toStdString()), t);
        combo->setCurrentIndex(std::max(0, combo->findData(QString::fromUtf8(scene::anim::tangentName(k.out)))));
        connect(combo, &QComboBox::currentIndexChanged, this, [this] {
            if (!mFilling)
                commitKeys();
        });
        mKeys->setCellWidget(r, 2, combo);
    }
    g.evaluate();
    static_cast<CurvePlot*>(mPlot)->setCurve(curve, std::get<double>(g.value({n->id(), scene::drivers::kInput, -1})));
    mFilling = false;
}

void DrivenChannelsPanel::commitKeys()
{
    std::string c = current();
    if (c.empty())
        return;
    json keys = json::array();
    for (int r = 0; r < mKeys->rowCount(); ++r)
    {
        bool okIn = false, okOut = false;
        double in = mKeys->item(r, 0) ? mKeys->item(r, 0)->text().toDouble(&okIn) : 0.0;
        double out = mKeys->item(r, 1) ? mKeys->item(r, 1)->text().toDouble(&okOut) : 0.0;
        if (!okIn || !okOut)
            return;
        auto* combo = qobject_cast<QComboBox*>(mKeys->cellWidget(r, 2));
        keys.push_back({in, out, combo ? combo->currentData().toString().toStdString() : std::string("linear")});
    }
    runUi("editDrivenChannel", {{"args", {c}}, {"keys", keys}});
}

// ------------------------------------------------------------------------------------------------ actions

void registerMorphUi()
{
    static bool done = false;
    if (std::exchange(done, true))
        return;
    auto cmd = [](const char* id, const char* label, const char* command, json call, const char* python, const char* doc) {
        actions::add({.id = id, .label = label, .category = "Deform", .doc = doc, .python = python, .command = command, .call = std::move(call)});
    };
    auto onCurrent = [](const char* id, const char* label, const char* command, json call, const char* doc) {
        actions::add({.id = id,
                      .label = label,
                      .category = "Deform",
                      .run = [command, call] { onTarget(command, call); },
                      .enabled = [] { return !currentMorphTarget().empty(); },
                      .doc = doc});
    };
    cmd("deform.morph", "Morph", "morph", json::object(), "morph()",
        "Select the target meshes, then the base last: the base gets a morph target per target, exported to Unreal as morph targets.");
    cmd("deform.addMorphTarget", "Add Morph Target", "addMorphTarget", json::object(), "addMorphTarget()",
        "Select the base mesh (or its morph) first, then the new targets; with just the base, adds an empty target to sculpt.");
    onCurrent("deform.editMorphTarget", "Edit Morph Target", "editMorphTarget", json::object(),
              "Toggle edit mode on the current morph target: sculpt its shape on an edit mesh, run again to bake it back.");
    onCurrent("deform.extractMorphTarget", "Extract Morph Target", "extractMorphTarget", json::object(),
              "Builds a mesh in the current morph target's shape.");
    actions::add({.id = "deform.setMorphTarget",
                  .label = "Set Morph Target From Selected",
                  .category = "Deform",
                  .run =
                      [] {
                          json sel = selectionNames();
                          if (sel.empty())
                              return;
                          onTarget("setMorphTarget", {{"args", {sel.back()}}});
                      },
                  .enabled = [] { return !currentMorphTarget().empty(); },
                  .doc = "Replaces the current morph target's shape with the selected mesh's."});
    onCurrent("deform.addInBetween", "Add In-Between", "addInBetween", json::object(),
              "Adds an in-between shape to the current morph target at a weight (option box), ready to edit.");
    actions::add({.id = "deform.morphPivotFromSelected",
                  .label = "Morph Pivot From Selected",
                  .category = "Deform",
                  .run =
                      [] {
                          if (std::string t = currentMorphTarget(); !t.empty())
                              pivotFromSelected(t);
                      },
                  .enabled = [] { return !currentMorphTarget().empty(); },
                  .doc = "Puts the current morph target's pivot at the selected node, for the Pivot path: a locator at the eye's centre "
                         "swings a lid over the eye."});
    viewportHooks::addDrawer("lila.morphPaths", drawPaths);
    onCurrent("deform.flipMorphTarget", "Flip Morph Target", "mirrorMorphTarget", {{"mode", "flip"}},
              "Mirrors the current morph target to the other side as a new target, left and right swapped in its name.");
    onCurrent("deform.symmetrizeMorphTarget", "Symmetrize Morph Target", "mirrorMorphTarget", {{"mode", "symmetrize"}},
              "Copies one side of the current morph target onto the other.");
    actions::add({.id = "deform.removeMorphTarget",
                  .label = "Remove Morph Target",
                  .category = "Deform",
                  .run =
                      [] {
                          if (std::string t = currentMorphTarget(); !t.empty())
                              runUi("removeMorphTarget", {{"args", {t}}});
                      },
                  .enabled = [] { return !currentMorphTarget().empty(); },
                  .doc = "Removes the current morph target from its morph."});
    cmd("deform.sculptCorrective", "Sculpt Corrective", "sculptCorrective", json::object(), "sculptCorrective()",
        "Pose the rig and select the skinned mesh: makes a copy of the posed mesh to sculpt the fix on.");
    cmd("deform.addCorrective", "Add Corrective", "addCorrective", json::object(), "addCorrective()",
        "Select the corrective sculpt: it becomes a morph target applied before skinning, driven by the posed joint so it shows at this pose.");
    cmd("deform.poseDriver", "Pose Driver", "poseDriver", json::object(), "poseDriver()",
        "Makes a pose driver for the selected joint: 1 at its pose now, 0 at rest. Connect its output to drive anything.");
    cmd("deform.setPoseDriverTarget", "Set Pose Driver Target", "setPoseDriverTarget", json::object(), "setPoseDriverTarget()",
        "Makes the selected pose driver's joint pose now its target.");
    actions::add({.id = "deform.driveChannel",
                  .label = "Drive Channel...",
                  .category = "Deform",
                  .run =
                      [] {
                          bool ok = false;
                          QString driver = QInputDialog::getText(nullptr, "Drive Channel", "Driver plug (node.attr)", QLineEdit::Normal, "", &ok);
                          if (!ok || driver.isEmpty())
                              return;
                          QString driven = QInputDialog::getText(nullptr, "Drive Channel", "Driven plug (node.attr)", QLineEdit::Normal, "", &ok);
                          if (!ok || driven.isEmpty())
                              return;
                          actions::runWithOptions("driveChannel", {{"args", {driven.toStdString()}}, {"driver", driver.toStdString()}});
                      },
                  .doc = "Drives one plug from another through a curve of driver and driven key pairs. In the channel box: Set as Driver, "
                         "then Drive From on the driven channel."});

    panels::add({.id = kMorphTargetsDock,
                 .title = "Morph Targets",
                 .create = [](QWidget* p) -> QWidget* { return new MorphTargetsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "mood-smile",
                 .open = false,
                 .toggleAction = "window.morphTargets"});
    panels::add({.id = kDrivenChannelsDock,
                 .title = "Driven Channels",
                 .create = [](QWidget* p) -> QWidget* { return new DrivenChannelsPanel(p); },
                 .area = Qt::RightDockWidgetArea,
                 .icon = "link",
                 .open = false,
                 .toggleAction = "window.drivenChannels"});

    // The common industry name, so users who know morph targets by it find them.
    for (const char* id : {"deform.morph", "deform.addMorphTarget", "deform.editMorphTarget", "deform.extractMorphTarget", "deform.removeMorphTarget", "window.morphTargets"})
        if (Action* a = actions::registry().find(id))
            a->keywords = {"blendshape", "blend shape"};
}
} // namespace lila::ui
