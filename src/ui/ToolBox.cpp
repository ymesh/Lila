#include "ui/ToolBox.h"

#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/DisplayAids.h"
#include "ui/PaintSelect.h"
#include "ui/Shelf.h"
#include "ui/Tools.h"
#include "ui/ViewportTool.h"

#include <QMainWindow>
#include <QMenu>
#include <QToolBar>

namespace lila::ui::toolBox
{
namespace
{
bool shown()
{
    return PrefStore::get().value<bool>("general", kShownPref, true);
}

/** Shows or hides the tool box as the pref says. */
class Keeper : public QObject
{
public:
    Keeper(QMainWindow* window, QToolBar* bar) : QObject(window), mBar(bar)
    {
        mPrefListener = PrefStore::get().onChanged([this](const std::string& file) {
            if (file == "general")
                QMetaObject::invokeMethod(this, [this] { sync(); });
        });
    }
    ~Keeper() override { PrefStore::get().removeOnChanged(mPrefListener); }

    void sync()
    {
        if (mBar->isVisible() != shown())
            mBar->setVisible(shown());
    }

private:
    QToolBar* mBar;
    int mPrefListener = 0;
};
} // namespace

void install(QMainWindow* window, iconBars::StateFn state)
{
    auto* bar = new QToolBar("Tool Box", window);
    bar->setObjectName("toolBox");
    bar->setMovable(false);
    bar->setFloatable(false);
    bar->setOrientation(Qt::Vertical);
    bar->toggleViewAction()->setVisible(false);
    bar->setContextMenuPolicy(Qt::PreventContextMenu);
    bar->setStyleSheet("QToolBar { background: #383838; border: none; border-right: 1px solid #262626; padding: 0; spacing: 0; }");
    auto* icons = new iconBars::IconBar(iconBars::kToolBox, Qt::Vertical, kIconSize, std::move(state), bar);
    icons->setWrap(true);
    bar->addWidget(icons);
    window->addToolBar(Qt::LeftToolBarArea, bar);
    window->addToolBarBreak(Qt::LeftToolBarArea);
    auto* keeper = new Keeper(window, bar);
    keeper->sync();
}

void place(QMainWindow* window)
{
    auto* bar = window->findChild<QToolBar*>("toolBox", Qt::FindDirectChildrenOnly);
    auto* strip = window->findChild<QToolBar*>("sidebarLeft", Qt::FindDirectChildrenOnly);
    if (!bar)
        return;
    if (!strip || window->toolBarArea(strip) != Qt::LeftToolBarArea)
    {
        if (window->toolBarArea(bar) != Qt::LeftToolBarArea)
            window->addToolBar(Qt::LeftToolBarArea, bar);
        return;
    }
    // Inserted before the collapsed-panel strip with a break between, so each keeps a column of its own.
    window->insertToolBar(strip, bar);
    if (!window->toolBarBreak(strip))
        window->insertToolBarBreak(strip);
}

void registerItems(std::function<ViewportWidget*()> active)
{
    actions::add({.id = "window.toggleToolBox", .label = "Tool Box", .category = "Windows",
                  .run = [] { PrefStore::get().set("general", kShownPref, !shown()); },
                  .doc = "The column of selection tools and display aids down the window's left edge.",
                  .python = "lila.prefs.set(\"general\", \"/ui/showToolBox\", not lila.prefs.get(\"general\", \"/ui/showToolBox\", True))"});
    auto selectTool = [](bool lasso) {
        PrefStore::get().set("general", paintSelect::kLassoPref, lasso);
        actions::run(nlohmann::json("tool.select"));
    };
    actions::add({.id = "select.marqueeTool", .label = "Select Tool (Marquee)", .category = "Select",
                  .run = [selectTool] { selectTool(false); },
                  .doc = "The select tool, dragging a rectangle to select.",
                  .python = "lila.prefs.set(\"general\", \"/select/lasso\", False)\nlila.ui.set_tool(\"select\")"});
    actions::add({.id = "select.lassoTool", .label = "Lasso Select Tool", .category = "Select",
                  .run = [selectTool] { selectTool(true); },
                  .doc = "The select tool, dragging a freehand lasso to select what falls inside it.",
                  .python = "lila.prefs.set(\"general\", \"/select/lasso\", True)\nlila.ui.set_tool(\"select\")"});

    using iconBars::Kind;
    const std::string bar = iconBars::kToolBox;
    auto add = [&](iconBars::Item item) {
        item.bar = bar;
        iconBars::add(std::move(item));
    };
    add({.id = "toolBox/select", .kind = Kind::Menu, .action = "select.marqueeTool", .icon = "pointer",
         .menuActions = {"#Drag Selects With", "select.marqueeTool", "select.lassoTool", "#Options", "select.toggleCameraBased",
                         "select.toggleBackfaceCulling", "viewport.togglePreselectObjects"}});
    add({.id = "toolBox/lasso", .action = "select.lassoTool", .icon = "lasso"});
    add({.id = "toolBox/paint", .kind = Kind::Menu, .action = "select.paintTool", .icon = "brush",
         .menuActions = {"select.paintRadiusSmaller", "select.paintRadiusLarger"}});
    // Component modes, soft select and selection masks live on the viewport header; the column below shows what each
    // view draws, by object type.
    add({.id = "toolBox/-show", .kind = Kind::Separator});
    add({.id = "toolBox/showMesh", .action = "viewport.show.mesh", .icon = "box"});
    add({.id = "toolBox/showCurve", .action = "viewport.show.curve", .icon = "vector-spline"});
    add({.id = "toolBox/showJoint", .action = "viewport.show.joint", .icon = "bone"});
    add({.id = "toolBox/showLight", .action = "viewport.show.light", .icon = "bulb"});
    add({.id = "toolBox/showLocator", .action = "viewport.show.locator", .icon = "crosshair"});
    add({.id = "toolBox/-display", .kind = Kind::Separator});
    add({.id = "toolBox/backfaceTint", .action = "viewport.aid.backfaceTint", .icon = "arrows-exchange"});
    add({.id = "toolBox/display", .kind = Kind::Menu, .icon = "eye", .label = "Mesh Display (active view)",
         .fill = [active](QMenu* m) { displayAids::fillMenu(m, active()); }});
    add({.id = "toolBox/-windows", .kind = Kind::Separator});
    add({.id = "toolBox/floatingShelf", .action = "shelf.floatingOpen", .icon = "layout-grid",
         .checked = [] { return FloatingShelf::isOpen(); }});
}
} // namespace lila::ui::toolBox
