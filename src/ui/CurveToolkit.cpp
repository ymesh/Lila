#include "ui/CurveToolkit.h"

#include "core/PrefStore.h"
#include "curve/CurveNodes.h"
#include "curve/Tessellate.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/Controls.h"
#include "ui/DockSidebar.h"
#include "ui/OptionBox.h"
#include "ui/SpinScrub.h"
#include "ui/ToolkitLayout.h"

#include <QApplication>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace lila::ui
{
namespace
{
constexpr const char* kCurveCollapsedPref = "/curveToolkit/collapsed";
constexpr const char* kCurveSymmetryIds[] = {"curve.symmetryOff", "curve.symmetryX", "curve.symmetryY", "curve.symmetryZ"};

QDockWidget* findCurveDock()
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (auto* d = w->findChild<QDockWidget*>(kCurveToolkitDock))
            return d;
    return nullptr;
}

/** The grid of a group's buttons, skipping actions that aren't registered, with its overflow menu after them. */
QWidget* curveGroup(std::vector<QToolButton*> buttons, const std::vector<std::string>& more, int columns, QWidget* parent)
{
    std::vector<QWidget*> items;
    for (QToolButton* b : buttons)
        if (b)
            items.push_back(b);
    QToolButton* overflow = more.empty() ? nullptr : controls::overflow(more, parent);
    // In a tile grid the overflow gets its own row, so it never reads as a tile.
    if (overflow && columns == 2)
        items.push_back(overflow);
    QWidget* grid = controls::grid(items, columns, parent);
    if (!overflow || columns == 2)
        return grid;
    auto* box = new QWidget(parent);
    auto* v = new QVBoxLayout(box);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(2);
    grid->setParent(box);
    overflow->setParent(box);
    v->addWidget(grid);
    v->addWidget(overflow);
    return box;
}

QFormLayout* curveForm(QWidget* parent)
{
    auto* form = new QFormLayout(parent);
    form->setContentsMargins(0, 0, 0, 4);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setHorizontalSpacing(8);
    form->setVerticalSpacing(4);
    return form;
}

controls::Segmented* curveAxes(const QString& offTip, const QString& axisTip, QWidget* parent)
{
    auto* s = new controls::Segmented(parent);
    s->add("Off", {}, offTip, 0);
    for (int i = 1; i <= 3; ++i)
    {
        const QString axis = QString(QChar('W' + i));
        s->add(axis, {}, axisTip.arg(axis), i);
    }
    return s;
}
} // namespace

CurveToolkit::CurveToolkit(QWidget* parent) : QWidget(parent)
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
    root->setContentsMargins(10, 6, 10, 8);
    root->setSpacing(4);
    scroll->setWidget(body);
    controls::fitScrollWidth(scroll);

    auto add = [&](const char* title, QWidget* content, bool open = true) {
        root->addWidget(collapsibleSection(title, content, body, kCurveCollapsedPref, open));
    };

    mMode = new controls::Segmented(body);
    mMode->add("Object", {}, "Object mode: pick and move whole curves", 0);
    mMode->add("Points", {}, "Points mode: pick and move curve points", 1);
    connect(mMode, &controls::Segmented::picked, this, [this](int i) {
        actions::run(json(i ? "select.vertexMode" : "select.objectMode"));
        mRefresh->start();
    });
    root->addWidget(mMode);
    mInfo = new QLabel(body);
    mInfo->setContentsMargins(2, 2, 2, 2);
    controls::setRole(mInfo, "hint");
    root->addWidget(mInfo);

    add("Draw", curveGroup({tile("curve.ezCurve", "line", "EZ Curve", body), tile("curve.bezierPen", "vector-bezier", "Pen", body),
                            tile("curve.cvTool", "vector-bezier-2", "CV", body), tile("curve.epTool", "vector-spline", "EP", body),
                            tile("curve.pencil", "scribble", "Pencil", body), tile("curve.kappa", "ease-in-out", "Kappa", body),
                            tile("curve.circle", "circle", "Circle", body), tile("curve.fromEdges", "polygon", "From Edges", body)},
                           {"curve.catmullRom", "curve.sketchTool"}, kToolkitColumns, body));

    // Options the draw tools share.
    auto* drawBox = new QWidget(body);
    QFormLayout* draw = curveForm(drawBox);
    // Toggles inside a form row stay label-only buttons, so they fit beside the field label.
    mOnSurface = actions::find("curve.drawOnSurfaces") ? track(controls::push("curve.drawOnSurfaces", drawBox, "On Surfaces"), "curve.drawOnSurfaces") : nullptr;
    mKeepOnSurface =
        actions::find("curve.keepDrawnOnSurface") ? track(controls::push("curve.keepDrawnOnSurface", drawBox, "Keep On"), "curve.keepDrawnOnSurface") : nullptr;
    draw->addRow("Surface", curveGroup({mOnSurface, mKeepOnSurface}, {}, 2, drawBox));
    mOffset = controls::valueField(0.0, 10000.0, 2, "cm", drawBox);
    mOffset->setToolTip("Offset: how far drawn curves sit off the surface they are drawn on (Ctrl+scroll in a draw tool)");
    setSpinDefault(mOffset, 0.0);
    connect(mOffset, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", "/curveTools/surfaceOffset", v); });
    draw->addRow("Offset", mOffset);
    mDegree = new controls::SliderField(drawBox);
    mDegree->setRange(1, 7);
    mDegree->setDecimals(0);
    mDegree->setToolTip("Degree of curves the CV and EP tools draw");
    setSpinDefault(mDegree, 3);
    connect(mDegree, &QDoubleSpinBox::valueChanged, this, [](double v) { PrefStore::get().set("general", "/curveTools/degree", int(std::lround(v))); });
    draw->addRow("Degree", mDegree);
    add("Draw Options", drawBox);

    auto* symmetryBox = new QWidget(body);
    QFormLayout* symmetry = curveForm(symmetryBox);
    mDrawSymmetry = curveAxes("New curves are not mirrored", "New curves are mirrored across the world %1 plane (Y in a draw tool cycles it)", symmetryBox);
    connect(mDrawSymmetry, &controls::Segmented::picked, this, [](int i) { PrefStore::get().set("general", "/curveTools/symmetry", i); });
    symmetry->addRow("Drawing", mDrawSymmetry);
    mCurveSymmetry = curveAxes("Stop mirroring the selected curves", "Mirror the selected curves live across the world %1 plane", symmetryBox);
    connect(mCurveSymmetry, &controls::Segmented::picked, this, [this](int i) {
        actions::run(json(kCurveSymmetryIds[std::clamp(i, 0, 3)]));
        mRefresh->start();
    });
    symmetry->addRow("Selected", mCurveSymmetry);
    if (QToolButton* bake = push("curve.bakeSymmetry", "Bake", symmetryBox))
        symmetry->addRow(QString(), curveGroup({bake}, {}, 2, symmetryBox));
    add("Symmetry", symmetryBox);

    add("Edit", curveGroup({push("curve.curvePointMode", "Curve Point", body), push("curve.rebuild", "Rebuild", body), push("curve.attach", "Attach", body),
                            push("curve.detach", "Detach", body), push("curve.fillet", "Fillet", body), push("curve.offset", "Offset", body)},
                           {"curve.insertAtCurvePoints", "curve.deletePoints", "curve.close", "curve.open", "curve.reverse", "curve.evenSpacing", "curve.fit",
                            "curve.project"},
                           2, body));
    add("Surfaces", curveGroup({push("curve.extrude", "Sweep", body), push("curve.revolve", "Revolve", body), push("curve.loft", "Loft", body),
                                push("curve.patchFill", "Patch", body), push("curve.network", "Network", body), push("curve.borderBlend", "Border", body)},
                               {}, 2, body));
    add("Pipes and Straps",
        curveGroup({push("curve.pipeTool", "Pipe Tool", body), push("curve.cableTool", "Cable Tool", body), push("curve.strapTool", "Strap Tool", body),
                    push("curve.stitch", "Stitch", body), push("curve.pipeRouteTool", "Route Tool", body), push("curve.pipeSupports", "Supports", body)},
                   {"curve.pipe", "curve.cable", "curve.carveChannel", "curve.pipePresetPipe", "curve.pipePresetHose", "curve.pipePresetCable",
                    "curve.pipePresetConduit", "curve.strapPresetStrap", "curve.strapPresetRope", "curve.strapPresetTape", "curve.strapPresetMetalBand",
                    "curve.strapPresetZipTie"},
                   2, body));

    add("Deform",
        curveGroup({push("curve.bend", "Bend", body), push("curve.curl", "Curl", body), push("curve.scaleCurvature", "Curvature", body),
                    push("curve.smooth", "Smooth", body)},
                   {}, 2, body),
        false);
    add("Blend and Trim",
        curveGroup({push("curve.blendTool", "Blend Tool", body), push("curve.blend", "Blend", body), push("curve.match", "Match", body),
                    push("curve.fair", "Fair", body), push("curve.trimTool", "Trim", body), push("curve.extend", "Extend", body)},
                   {"curve.split"}, 2, body),
        false);
    add("Along and On Surface",
        curveGroup({push("curve.deformAlongCurve", "Deform Along", body), push("curve.arrayAlongCurve", "Array Along", body),
                    push("curve.instanceAlongCurve", "Instance Along", body), push("curve.flowAlongSurface", "Flow", body),
                    push("curve.keepOnSurface", "Keep On", body), push("curve.releaseFromSurface", "Release", body)},
                   {}, 2, body),
        false);
    QToolButton* planarBoolean = nullptr;
    if (actions::find("curve.booleanUnion"))
        planarBoolean = track(controls::splitRow("curve.booleanUnion", {"curve.booleanUnion", "curve.booleanSubtract", "curve.booleanIntersect", "curve.booleanXor"},
                                                 "layers-subtract", body, "Boolean"),
                              "curve.booleanUnion");
    add("Planar",
        curveGroup({push("curve.text", "Text", body), push("curve.textMesh", "Text Mesh", body), push("curve.bevelPlus", "Bevel+", body), planarBoolean,
                    push("curve.offsetRegion", "Offset Region", body), push("curve.insetSkeleton", "Skeleton", body)},
                   {"curve.centerLine", "curve.roof"}, 2, body),
        false);
    add("Sections and Groom",
        curveGroup({push("curve.contour", "Contour", body), push("curve.sectionTool", "Section", body), push("groom.create", "New Groom", body),
                    push("groom.drawHair", "Draw Hair", body), push("curve.sculptCurves", "Sculpt", body), push("groom.settle", "Settle", body)},
                   {"groom.shell", "curve.groomSurface", "curve.groomGuides", "curve.hairCards"}, 2, body),
        false);
    add("Handles",
        curveGroup({push("curve.handleAuto", "Auto", body), push("curve.handleVector", "Vector", body), push("curve.handleAligned", "Aligned", body),
                    push("curve.handleFree", "Free", body)},
                   {}, 2, body),
        false);
    add("Convert",
        curveGroup({push("curve.toBezier", "To Bezier", body), push("curve.toNurbs", "To NURBS", body), push("curve.toPoly", "To Poly", body),
                    push("curve.toggleComb", "Comb", body), push("curve.sendToUnreal", "Send to Unreal", body)},
                   {}, 2, body),
        false);

    // The in-view popup is the main place for these; the section serves those who turn it off.
    auto* optionsBox = new QWidget(body);
    auto* optionsLayout = new QVBoxLayout(optionsBox);
    optionsLayout->setContentsMargins(0, 0, 0, 4);
    optionsLayout->addWidget(new LastOpEditor(optionsBox));
    add("Tool Options", optionsBox, false);
    root->addStretch(1);

    mRefresh = new QTimer(this);
    mRefresh->setSingleShot(true);
    mRefresh->setInterval(30);
    connect(mRefresh, &QTimer::timeout, this, [this] { refreshState(); });
    mGraphListener = scene::Graph::current().addListener([this](scene::Change, scene::NodeId) { mRefresh->start(); });
    mComponentListener = scene::components::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
    refreshState();
}

CurveToolkit::~CurveToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    scene::components::removeListener(mComponentListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* CurveToolkit::track(QToolButton* b, const std::string& id)
{
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

QToolButton* CurveToolkit::tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::row(id, iconName, parent, label), id) : nullptr;
}

QToolButton* CurveToolkit::push(const std::string& id, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::row(id, nullptr, parent, label), id) : nullptr;
}

void CurveToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshState();
}

void CurveToolkit::refreshState()
{
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
        if (a->checked && b->isCheckable())
            b->setChecked(a->checked());
    }

    {
        QSignalBlocker block(mMode);
        mMode->setCurrent(scene::components::mode() != scene::ComponentMode::Object ? 1 : 0);
    }
    PrefStore& p = PrefStore::get();
    const bool onSurface = p.value<bool>("general", "/curveTools/onSurface", false);
    if (mKeepOnSurface)
        mKeepOnSurface->setEnabled(onSurface);
    mOffset->setEnabled(onSurface);
    if (!mOffset->hasFocus())
    {
        QSignalBlocker block(mOffset);
        mOffset->setValue(p.value<double>("general", "/curveTools/surfaceOffset", 0.0));
    }
    if (!mDegree->hasFocus())
    {
        QSignalBlocker block(mDegree);
        mDegree->setValue(std::clamp(p.value<int>("general", "/curveTools/degree", 3), 1, 7));
    }
    {
        QSignalBlocker block(mDrawSymmetry);
        mDrawSymmetry->setCurrent(std::clamp(p.value<int>("general", "/curveTools/symmetry", 0), 0, 3));
    }
    {
        QSignalBlocker block(mCurveSymmetry);
        mCurveSymmetry->setEnabled(actions::isEnabled(json(kCurveSymmetryIds[0])));
        for (int i = 0; i < 4; ++i)
            if (const Action* a = actions::find(kCurveSymmetryIds[i]); a && a->checked && a->checked())
                mCurveSymmetry->setCurrent(i);
    }

    scene::Graph& g = scene::Graph::current();
    int curves = 0;
    size_t pointCount = 0;
    double length = 0.0;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id); n && n->type().isA("curve"))
        {
            ++curves;
            for (const curve::Spline& s : curve::shapeSplines(g, id))
            {
                pointCount += s.size();
                length += curve::length(s);
            }
        }
    mInfo->setText(curves ? QString("%1 curve%2, %3 points, %4 cm long").arg(curves).arg(curves == 1 ? "" : "s").arg(pointCount).arg(length, 0, 'f', 1)
                          : QString("No curve selected"));
}

void registerCurveToolkitActions()
{
    actions::add({.id = "window.curveToolkit", .label = "Curve Toolkit", .category = "Windows", .run = [] { toggleDock(findCurveDock()); },
                  .doc = "Curve draw tools, their surface options, curve editing, deforming, surfacing and planar commands, and live tool options in one panel.",
                  .python = "lila.ui.run_action(\"window.curveToolkit\")"});
}
} // namespace lila::ui
