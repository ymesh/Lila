#include "ui/ModelingToolkit.h"

#include "core/PrefStore.h"
#include "scene/Graph.h"
#include "ui/Actions.h"
#include "ui/AdjustLastOp.h"
#include "ui/Controls.h"
#include "ui/DockSidebar.h"
#include "ui/OptionBox.h"
#include "ui/ToolkitLayout.h"
#include "ui/Tools.h"
#include "ui/UiCommon.h"

#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QScrollArea>
#include <QSignalBlocker>
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

    add("Move", moveSettings(body), false);

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
    mToolsListener = tools::addListener([this] { mRefresh->start(); });
    mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
        if (file == "general")
            QMetaObject::invokeMethod(mRefresh, qOverload<>(&QTimer::start), Qt::QueuedConnection);
    });
}

ModelingToolkit::~ModelingToolkit()
{
    scene::Graph::current().removeListener(mGraphListener);
    tools::removeListener(mToolsListener);
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

QWidget* ModelingToolkit::moveSettings(QWidget* parent)
{
    auto* box = new QWidget(parent);
    auto* grid = new QGridLayout(box);
    grid->setContentsMargins(0, 0, 0, 4);
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(2);
    grid->setColumnStretch(1, 1);

    mMoveAxis = new QComboBox(box);
    mMoveAxis->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    mMoveAxis->setMinimumContentsLength(6);
    for (auto [label, space] : {std::pair{"World", "world"}, {"Object", "object"}, {"Parent", "parent"}, {"Component", "component"},
                                {"Normal", "normal"}, {"Custom", "custom"}})
        mMoveAxis->addItem(label, space);
    mMoveAxis->setToolTip("The move manipulator's axes. Also on the move tool's marking menu.");
    connect(mMoveAxis, &QComboBox::activated, this,
            [this](int i) { PrefStore::get().set("general", "/manipulator/moveSpace", mMoveAxis->itemData(i).toString().toStdString()); });
    grid->addWidget(new QLabel("Axis", box), 0, 0);
    grid->addWidget(mMoveAxis, 0, 1);

    mCustomAxis = controls::vectorField(mCustomAngles, -360.0, 360.0, 3, QStringLiteral("°"), box);
    mCustomAxis->setToolTip("Custom axis angles, in degrees. Used by the move and rotate manipulators when their axis is Custom.");
    for (QDoubleSpinBox* s : mCustomAngles)
        connect(s, &QDoubleSpinBox::valueChanged, this, [this] {
            PrefStore::get().set("general", "/manipulator/customAxis",
                                 json::array({mCustomAngles[0]->value(), mCustomAngles[1]->value(), mCustomAngles[2]->value()}));
        });
    grid->addWidget(new QLabel("Angles", box), 1, 0);
    grid->addWidget(mCustomAxis, 1, 1);

    std::vector<QWidget*> buttons;
    if (QToolButton* edit = push("tool.pivotEditToggle", "Edit Pivot", box))
    {
        edit->setCheckable(true);
        buttons.push_back(edit);
    }
    if (QToolButton* reset = push("modify.resetPivot", "Reset Pivot", box))
        buttons.push_back(reset);
    grid->addWidget(controls::grid(buttons, kModelingGroupColumns, box), 2, 0, 1, 2);
    return box;
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
    if (mMoveAxis)
    {
        const std::string space = PrefStore::get().value<std::string>("general", "/manipulator/moveSpace", "world");
        mMoveAxis->setCurrentIndex(std::max(0, mMoveAxis->findData(QString::fromStdString(space))));
        const bool custom = space == "custom" || PrefStore::get().value<std::string>("general", "/manipulator/rotateSpace", "world") == "custom";
        mCustomAxis->setEnabled(custom);
        const json angles = PrefStore::get().value("general", "/manipulator/customAxis", json::array({0.0, 0.0, 0.0}));
        for (size_t i = 0; i < mCustomAngles.size(); ++i)
            if (!mCustomAngles[i]->hasFocus())
            {
                QSignalBlocker block(mCustomAngles[i]);
                mCustomAngles[i]->setValue(angles.is_array() && angles.size() == 3 && angles[i].is_number() ? angles[i].get<double>() : 0.0);
            }
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
