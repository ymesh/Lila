#include "ui/ModelingToolkit.h"

#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/Controls.h"
#include "ui/DockSidebar.h"
#include "ui/OptionBox.h"
#include "ui/ToolkitLayout.h"
#include "ui/UiCommon.h"

#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace lila::ui
{
namespace
{
constexpr const char* kModelingCollapsed = "/modelingToolkit/collapsed";
constexpr int kModelingGroupColumns = 2;

/** The grid of a group's buttons, skipping actions that aren't registered, with its overflow menu in the last cell. */
QWidget* modelingGroup(std::vector<QToolButton*> buttons, const std::vector<std::string>& more, int columns, QWidget* parent)
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

ModelingToolkit::ModelingToolkit(QWidget* parent) : QWidget(parent)
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
        root->addWidget(collapsibleSection(title, content, body, kModelingCollapsed, open));
    };

    add("Edit", modelingGroup({tile("mesh.extrude", "cube-plus", "Extrude", body), tile("mesh.bevel", "vector-bezier", "Bevel", body),
                               tile("mesh.inset", "box-model-2", "Inset", body), tile("mesh.bridge", "arrows-join", "Bridge", body),
                               tile("mesh.insertLoop", "separator-vertical", "Insert Loop", body), tile("mesh.knife", "slice", "Knife", body),
                               tile("mesh.merge", "arrow-merge", "Merge", body), tile("mesh.fillHole", "square-plus", "Fill Hole", body)},
                              {}, kToolkitColumns, body));

    QToolButton* boolean = nullptr;
    if (actions::find("mesh.booleanUnion"))
        boolean = track(controls::splitRow("mesh.booleanUnion", {"mesh.booleanUnion", "mesh.booleanDifference", "mesh.booleanIntersection"}, "layers-subtract", body, "Boolean"),
                        "mesh.booleanUnion");
    add("Mesh", modelingGroup({push("mesh.combine", "Combine", body), push("mesh.separate", "Separate", body), push("mesh.extract", "Extract", body),
                               push("mesh.mirror", "Mirror", body), push("mesh.subdivide", "Subdivide", body), boolean},
                              {"mesh.triangulate", "mesh.quadrangulate", "mesh.transferAttributes", "mesh.repair", "mesh.relax", "mesh.bakeModifierStack"},
                              kModelingGroupColumns, body));

    add("Components", modelingGroup({push("mesh.collapse", "Collapse", body), push("mesh.dissolve", "Dissolve", body), push("mesh.poke", "Poke", body),
                                     push("mesh.spin", "Spin Edge", body), push("mesh.circularize", "Circularize", body),
                                     push("mesh.straighten", "Straighten", body)},
                                    {"mesh.connect", "mesh.mergeCenter", "mesh.targetWeld", "mesh.gridFill", "mesh.splitRings", "mesh.offsetLoop", "edit.delete"},
                                    kModelingGroupColumns, body));

    // A menu of conversions, so the main part opens the menu instead of running one of them.
    QToolButton* convert = nullptr;
    if (actions::find("select.toVertices"))
    {
        convert = controls::splitRow("select.toVertices", {"select.toVertices", "select.toEdges", "select.toFaces", "select.containedFaces"}, "transform", body, "Convert To");
        convert->setPopupMode(QToolButton::InstantPopup);
        convert->setToolTip("Convert To: turn the selection into vertices, edges or faces");
    }
    add("Selection", modelingGroup({push("select.grow", "Grow", body), push("select.shrink", "Shrink", body), push("select.loop", "Loop", body),
                                    push("select.ring", "Ring", body), push("select.border", "Border", body), push("edit.invertSelection", "Invert", body),
                                    convert},
                                   {"select.allComponents", "select.paintTool", "select.byAngle"}, kModelingGroupColumns, body));

    add("Normals",
        modelingGroup({push("mesh.softenEdges", "Soften", body), push("mesh.hardenEdges", "Harden", body), push("mesh.setNormalAngle", "By Angle", body),
                       push("mesh.flip", "Flip", body), push("mesh.conform", "Conform", body), push("mesh.softFoliageNormals", "Foliage", body)},
                      {}, kModelingGroupColumns, body),
        false);
    add("Shape",
        modelingGroup({push("mesh.editEdgeFlow", "Edge Flow", body), push("mesh.spherize", "Spherize", body), push("mesh.flatten", "Flatten", body),
                       push("mesh.conformShape", "To Shape", body), push("mesh.conformToObject", "To Object", body)},
                      {}, kModelingGroupColumns, body),
        false);
    add("Retopology",
        modelingGroup({push("retopo.draw", "Retopo Draw", body), push("mesh.quadRemesh", "Quad Remesh", body), push("mesh.voxelRemesh", "Voxel Remesh", body),
                       push("modifier.decimate", "Decimate", body)},
                      {}, kModelingGroupColumns, body),
        false);
    add("Auto Bevel",
        modelingGroup({push("modifier.autoBevel", "Auto Bevel", body), push("select.foldEdges", "Fold Edges", body), push("bevel.showEdges", "Show Edges", body),
                       push("bevel.editEdges", "Edit Edges", body), push("bevel.applyEdges", "Apply", body), push("bevel.cancelEdges", "Cancel", body)},
                      {"bevel.measureThin", "bevel.clearOverrides"}, kModelingGroupColumns, body),
        false);
    add("Curves to Mesh",
        modelingGroup({push("curve.extrude", "Sweep", body), push("curve.revolve", "Revolve", body), push("curve.loft", "Loft", body),
                       push("curve.pipeTool", "Pipe", body), push("curve.textMesh", "Text Mesh", body), push("curve.bevelPlus", "Bevel+", body)},
                      {}, kModelingGroupColumns, body),
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
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
}

ModelingToolkit::~ModelingToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    PrefStore::get().removeOnChanged(mPrefListener);
}

QToolButton* ModelingToolkit::track(QToolButton* b, const std::string& id)
{
    b->setProperty("lilaAction", QString::fromStdString(id));
    connect(b, &QToolButton::clicked, this, [this] { mRefresh->start(); });
    mButtons.push_back(b);
    return b;
}

QToolButton* ModelingToolkit::tile(const std::string& id, const char* iconName, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::row(id, iconName, parent, label), id) : nullptr;
}

QToolButton* ModelingToolkit::push(const std::string& id, const QString& label, QWidget* parent)
{
    return actions::find(id) ? track(controls::row(id, nullptr, parent, label), id) : nullptr;
}

void ModelingToolkit::showEvent(QShowEvent* e)
{
    QWidget::showEvent(e);
    refreshState();
}

void ModelingToolkit::refreshState()
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
        if (b->popupMode() != QToolButton::InstantPopup)
            b->setToolTip(tip);
        if (a->checked && b->isCheckable())
            b->setChecked(a->checked());
    }
}

void registerModelingToolkitActions()
{
    actions::add({.id = "window.modelingToolkit", .label = "Modeling Toolkit", .category = "Windows", .run = [] {
                      toggleDock(findDock(kModelingToolkitDock));
                  },
                  .doc = "The daily modeling tools, mesh, component and selection commands, and the last operation's live settings in one panel.",
                  .python = "lila.ui.run_action(\"window.modelingToolkit\")"});
}
} // namespace lila::ui
