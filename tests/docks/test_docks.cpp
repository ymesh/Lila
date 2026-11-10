#include <doctest/doctest.h>

#include "ui/DockSidebar.h"
#include "ui/Panels.h"

#include <QApplication>
#include <QDockWidget>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QToolBar>
#include <QTabBar>
#include <QMouseEvent>

#include <map>

using namespace lila::ui;

namespace
{
void click(QWidget* w, Qt::MouseButton button, QPoint at)
{
    QPointF local(at), global(w->mapToGlobal(at));
    QMouseEvent press(QEvent::MouseButtonPress, local, global, button, button, Qt::NoModifier);
    QApplication::sendEvent(w, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, button, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

void settle()
{
    for (int i = 0; i < 5; ++i)
    {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

/** The main window's dock arrangement in miniature: a left column, a right tab group and a bottom tab group. */
struct Window
{
    QMainWindow win;
    DockSidebars* sidebars = nullptr;
    QWidget* central = nullptr;
    std::map<QString, QDockWidget*> docks;
    QMenuBar* menu = nullptr;

    QDockWidget* add(const QString& name, Qt::DockWidgetArea area)
    {
        auto* d = new QDockWidget(name, &win);
        d->setObjectName(name);
        d->setWidget(new QLabel(name));
        win.addDockWidget(area, d);
        sidebars->manage(d);
        docks[name] = d;
        return d;
    }

    explicit Window(bool show = true)
    {
        win.setDockOptions(QMainWindow::AnimatedDocks | QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks | QMainWindow::GroupedDragging);
        central = new QWidget;
        win.setCentralWidget(central);
        sidebars = new DockSidebars(&win);
        add("shelf", Qt::TopDockWidgetArea);
        add("outliner", Qt::LeftDockWidgetArea);
        QDockWidget* channels = add("channelBox", Qt::RightDockWidgetArea);
        QDockWidget* attrs = add("attributeEditor", Qt::RightDockWidgetArea);
        win.tabifyDockWidget(channels, attrs);
        QDockWidget* output = add("scriptOutput", Qt::BottomDockWidgetArea);
        QDockWidget* editor = add("scriptEditor", Qt::BottomDockWidgetArea);
        QDockWidget* term = add("terminal", Qt::BottomDockWidgetArea);
        // MainWindow's order: the slider goes in before the hidden editors that share the tab group.
        QDockWidget* slider = add("timeSlider", Qt::BottomDockWidgetArea);
        slider->setTitleBarWidget(new QWidget(slider));
        slider->widget()->setMinimumHeight(56);
        slider->widget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        for (const char* hidden : {"graphEditor", "nodeEditor"})
            add(hidden, Qt::BottomDockWidgetArea)->hide();
        arrangeBottomDocks(&win, {output, editor, term, docks["graphEditor"], docks["nodeEditor"]}, editor, slider);
        channels->raise();
        win.resize(1600, 960);
        if (!show)
            return;
        win.show();
        settle();
        win.resizeDocks({output}, {360}, Qt::Vertical);
        settle();
    }

    QTabBar* tabBarFor(QDockWidget* d)
    {
        for (QTabBar* bar : win.findChildren<QTabBar*>())
            if (bar->parentWidget() == &win && bar->isVisible())
                for (int i = 0; i < bar->count(); ++i)
                    if (bar->tabText(i) == d->windowTitle())
                        return bar;
        return nullptr;
    }
};
} // namespace

TEST_CASE("collapsing every dock gives the whole window to the central widget")
{
    Window w;
    // Collapse in the order a user clicks: whichever dock is showing, until none are.
    for (int guard = 0; guard < 40; ++guard)
    {
        QDockWidget* next = nullptr;
        for (auto& [name, d] : w.docks)
            if (d->isVisible() && !d->isFloating())
                next = d;
        if (!next)
            break;
        w.sidebars->collapse(next);
        settle();
    }
    for (auto& [name, d] : w.docks)
        CHECK_MESSAGE(!d->isVisible(), name.toStdString());
    QSize free = w.win.size();
    INFO("central " << w.central->width() << "x" << w.central->height() << " in " << free.width() << "x" << free.height());
    CHECK(w.central->height() > free.height() - 120);
    CHECK(w.central->width() > free.width() - 120);
}

/** Within the sidebar strips and any docks still showing (allowance) of the whole window. */
bool centralFills(Window& w, int allowance = 0)
{
    QSize free = w.win.size() - QSize(0, allowance);
    INFO("central " << w.central->width() << "x" << w.central->height() << " in " << free.width() << "x" << free.height());
    CHECK(w.central->height() > free.height() - 120);
    CHECK(w.central->width() > free.width() - 120);
    return w.central->height() > free.height() - 120;
}

TEST_CASE("closing some bottom docks and collapsing the rest frees the bottom area")
{
    Window w;
    for (const char* name : {"scriptOutput", "timeSlider"})
        w.sidebars->close(w.docks[name]);
    settle();
    for (const char* name : {"scriptEditor", "terminal", "channelBox", "attributeEditor", "outliner", "shelf"})
    {
        w.sidebars->collapse(w.docks[name]);
        settle();
    }
    centralFills(w);

    SUBCASE("and after a save and restore")
    {
        QByteArray state = w.win.saveState();
        QStringList collapsed = w.sidebars->collapsedNames();
        w.sidebars->expandAll();
        settle();
        w.win.restoreState(state);
        w.sidebars->restoreCollapsed(collapsed);
        settle();
        centralFills(w);
    }
}

TEST_CASE("collapsing the bottom tab group leaves the time slider its own height")
{
    Window w;
    w.sidebars->close(w.docks["scriptOutput"]);
    settle();
    for (const char* name : {"scriptEditor", "terminal", "channelBox", "attributeEditor", "outliner", "shelf"})
    {
        w.sidebars->collapse(w.docks[name]);
        settle();
    }
    INFO("time slider height " << w.docks["timeSlider"]->height());
    CHECK(w.docks["timeSlider"]->height() < 120);
    centralFills(w, w.docks["timeSlider"]->height());
}

TEST_CASE("a layout restored before the window shows keeps collapsed docks out of the way")
{
    QByteArray state;
    QStringList collapsed;
    {
        Window w;
        w.sidebars->close(w.docks["scriptOutput"]);
        settle();
        for (const char* name : {"scriptEditor", "terminal", "channelBox", "attributeEditor", "outliner", "shelf"})
        {
            w.sidebars->collapse(w.docks[name]);
            settle();
        }
        state = w.win.saveState();
        collapsed = w.sidebars->collapsedNames();
    }
    Window w(false);
    w.win.restoreState(state);
    w.sidebars->restoreCollapsed(collapsed);
    w.win.show();
    settle();
    CHECK(w.sidebars->collapsedNames().size() == collapsed.size());
    CHECK(w.docks["timeSlider"]->height() < 120);
    centralFills(w, w.docks["timeSlider"]->height());
}

TEST_CASE("switching tabs keeps the tab group's size")
{
    Window w;
    QDockWidget* channels = w.docks["channelBox"];
    QDockWidget* attrs = w.docks["attributeEditor"];
    w.win.resizeDocks({channels}, {300}, Qt::Horizontal);
    settle();
    const int narrow = channels->width();

    QTabBar* bar = w.tabBarFor(channels);
    REQUIRE(bar);
    int attrsTab = -1, channelsTab = -1;
    for (int i = 0; i < bar->count(); ++i)
        (bar->tabText(i) == attrs->windowTitle() ? attrsTab : channelsTab) = i;
    REQUIRE(attrsTab >= 0);

    click(bar, Qt::LeftButton, bar->tabRect(attrsTab).center());
    settle();
    CHECK(attrs->isVisible());
    CHECK(std::abs(attrs->width() - narrow) < 4);
    w.win.resizeDocks({attrs}, {560}, Qt::Horizontal);
    settle();
    const int wide = attrs->width();
    CHECK(wide > narrow + 100);

    click(bar, Qt::LeftButton, bar->tabRect(channelsTab).center());
    settle();
    CHECK(channels->isVisible());
    CHECK(std::abs(channels->width() - wide) < 4);
}

TEST_CASE("toggling panels keeps every other panel's size")
{
    Window w;
    QDockWidget* channels = w.docks["channelBox"];
    QDockWidget* outliner = w.docks["outliner"];
    QDockWidget* top = w.add("rightTop", Qt::RightDockWidgetArea);
    QDockWidget* material = w.add("materialEditor", Qt::RightDockWidgetArea);
    w.win.tabifyDockWidget(channels, material);
    QDockWidget* uv = w.add("uvEditor", Qt::RightDockWidgetArea);
    keepOwnColumn(uv);
    uv->setProperty(kDefaultTabExtent, 400);
    uv->hide();
    channels->raise();
    settle();
    w.win.resizeDocks({outliner, channels}, {280, 330}, Qt::Horizontal);
    w.win.resizeDocks({top, channels}, {250, 300}, Qt::Vertical);
    settle();

    auto shape = [&] {
        return std::vector<int>{outliner->width(), top->width(), top->height(), w.docks["scriptEditor"]->height()};
    };
    const std::vector<int> before = shape();
    for (QDockWidget* d : {uv, material, material, material, material, uv})
    {
        toggleDock(d);
        settle();
        settle();
        const std::vector<int> now = shape();
        CAPTURE(d->objectName().toStdString());
        for (size_t i = 0; i < now.size(); ++i)
            CHECK(std::abs(now[i] - before[i]) < 3);
    }
    CHECK(dockShowing(channels));
}

TEST_CASE("middle-click closes a dock tab and a collapsed sidebar tab")
{
    Window w;
    QDockWidget* term = w.docks["terminal"];
    QTabBar* bar = w.tabBarFor(term);
    REQUIRE(bar);
    int tab = -1;
    for (int i = 0; i < bar->count(); ++i)
        if (bar->tabText(i) == term->windowTitle())
            tab = i;
    REQUIRE(tab >= 0);
    click(bar, Qt::MiddleButton, bar->tabRect(tab).center());
    settle();
    CHECK(term->isHidden());
    CHECK_FALSE(term->toggleViewAction()->isChecked());

    QDockWidget* outliner = w.docks["outliner"];
    w.sidebars->collapse(outliner);
    settle();
    REQUIRE(w.sidebars->isCollapsed(outliner));
    QWidget* side = nullptr;
    for (QWidget* c : w.win.findChildren<QWidget*>())
        if (c->inherits("QToolButton") && c->toolTip() == "Expand " + outliner->windowTitle())
            side = c;
    REQUIRE(side);
    click(side, Qt::MiddleButton, side->rect().center());
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
    CHECK(outliner->isHidden());
}

TEST_CASE("the time slider is a slim full-width row under the script editor")
{
    Window w;
    QDockWidget* slider = w.docks["timeSlider"];
    QDockWidget* editor = w.docks["scriptEditor"];
    INFO("slider " << slider->x() << "," << slider->y() << " " << slider->width() << "x" << slider->height());
    CHECK(slider->width() == w.win.width());
    CHECK(slider->height() < 90);
    CHECK(slider->y() >= editor->y() + editor->height());
    CHECK(editor->isVisible());
}

namespace
{
QWidget* sideTabFor(Window& w, QDockWidget* d)
{
    for (QWidget* c : w.win.findChildren<QWidget*>())
        if (c->inherits("QToolButton") && c->toolTip() == "Expand " + d->windowTitle())
            return c;
    return nullptr;
}
} // namespace

TEST_CASE("a hidden dock collapsed before the window shows gets its tab on its own edge's strip")
{
    Window w(false);
    QDockWidget* uv = w.add("uvEditor", Qt::RightDockWidgetArea);
    w.win.splitDockWidget(uv, w.docks["channelBox"], Qt::Horizontal);
    uv->hide();
    w.menu = w.win.menuBar();
    w.menu->addMenu("File");
    w.sidebars->restoreCollapsed({"uvEditor"});
    w.win.show();
    settle();
    QWidget* tab = sideTabFor(w, uv);
    REQUIRE(tab);
    CHECK(tab->isVisible());
    QPoint at = tab->mapTo(&w.win, QPoint(0, 0));
    INFO("tab at " << at.x() << "," << at.y() << " " << tab->width() << "x" << tab->height());
    CHECK(at.x() > w.win.width() / 2);
    CHECK(at.y() >= w.menu->height());
    CHECK(w.win.toolBarArea(qobject_cast<QToolBar*>(tab->parentWidget())) == Qt::RightToolBarArea);
}

TEST_CASE("a dock that isn't in a dock area isn't collapsed")
{
    Window w;
    auto* loose = new QDockWidget("Loose", &w.win);
    loose->setObjectName("loose");
    w.sidebars->manage(loose);
    w.sidebars->collapse(loose);
    CHECK_FALSE(w.sidebars->isCollapsed(loose));
    QDockWidget* outliner = w.docks["outliner"];
    outliner->setFloating(true);
    w.sidebars->collapse(outliner);
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
}

TEST_CASE("closing a collapsed dock removes its tab")
{
    Window w;
    QDockWidget* outliner = w.docks["outliner"];
    w.sidebars->collapse(outliner);
    settle();
    REQUIRE(sideTabFor(w, outliner));
    w.sidebars->close(outliner);
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
    CHECK(sideTabFor(w, outliner) == nullptr);
}

TEST_CASE("restoring a state saved before the strips existed keeps the strips in their toolbar areas")
{
    Window w;
    QByteArray before = w.win.saveState();
    QDockWidget* channels = w.docks["channelBox"];
    w.sidebars->collapse(channels);
    settle();
    w.sidebars->expandAll();
    w.win.restoreState(before);
    w.sidebars->restoreCollapsed({"channelBox"});
    settle();
    QWidget* tab = sideTabFor(w, channels);
    REQUIRE(tab);
    auto* strip = qobject_cast<QToolBar*>(tab->parentWidget());
    REQUIRE(strip);
    QPoint at = tab->mapTo(&w.win, QPoint(0, 0));
    INFO("tab at " << at.x() << "," << at.y());
    CHECK(w.win.toolBarArea(strip) == Qt::RightToolBarArea);
    CHECK(at.x() > w.win.width() / 2);
}

namespace
{
/** The bottom area is one tab group with the script editor on top, and the slider is a slim full-width row under it. */
void checkBottomArrangement(Window& w)
{
    QDockWidget* slider = w.docks["timeSlider"];
    QDockWidget* editor = w.docks["scriptEditor"];
    for (const char* name : {"scriptOutput", "scriptEditor", "terminal", "graphEditor", "nodeEditor", "timeSlider"})
        CHECK_MESSAGE(w.win.dockWidgetArea(w.docks[name]) == Qt::BottomDockWidgetArea, name);
    QList<QDockWidget*> group = w.win.tabifiedDockWidgets(editor);
    for (const char* name : {"scriptOutput", "terminal"})
        CHECK_MESSAGE(group.contains(w.docks[name]), name);
    CHECK_FALSE(group.contains(slider));
    CHECK(w.win.tabifiedDockWidgets(slider).isEmpty());

    QTabBar* bar = w.tabBarFor(editor);
    REQUIRE(bar);
    QStringList tabs;
    for (int i = 0; i < bar->count(); ++i)
        tabs << bar->tabText(i);
    INFO("tabs " << tabs.join(" | ").toStdString());
    CHECK(tabs == QStringList{"scriptOutput", "scriptEditor", "terminal"});
    CHECK(bar->tabText(bar->currentIndex()) == "scriptEditor");

    QRect inside = w.win.rect();
    CHECK(inside.contains(editor->geometry()));
    for (const char* name : {"scriptOutput", "terminal"})
        CHECK_MESSAGE(!inside.intersects(w.docks[name]->geometry()), name);
    CHECK(w.docks["graphEditor"]->isHidden());
    CHECK(w.docks["nodeEditor"]->isHidden());

    INFO("slider " << slider->x() << "," << slider->y() << " " << slider->width() << "x" << slider->height() << ", editor " << editor->x()
                   << "," << editor->y() << " " << editor->width() << "x" << editor->height());
    CHECK(slider->isVisible());
    CHECK(slider->width() == w.win.width());
    CHECK(slider->height() < 90);
    CHECK(slider->y() >= editor->y() + editor->height());
    CHECK(editor->width() == w.win.width());
}
} // namespace

TEST_CASE("the default bottom area survives a reset and a saved layout")
{
    QByteArray saved;
    {
        Window w(false);
        QByteArray defaults = w.win.saveState();
        w.win.show();
        settle();
        checkBottomArrangement(w);

        SUBCASE("opening the graph and node editors adds them to the group")
        {
            for (const char* name : {"graphEditor", "nodeEditor"})
                w.docks[name]->show();
            settle();
            QList<QDockWidget*> group = w.win.tabifiedDockWidgets(w.docks["scriptEditor"]);
            for (const char* name : {"graphEditor", "nodeEditor"})
                CHECK_MESSAGE(group.contains(w.docks[name]), name);
            CHECK(w.docks["timeSlider"]->width() == w.win.width());
            for (const char* name : {"graphEditor", "nodeEditor"})
                w.docks[name]->hide();
            w.docks["scriptEditor"]->raise();
            settle();
        }
        SUBCASE("reset after rearranging")
        {
            w.win.splitDockWidget(w.docks["scriptEditor"], w.docks["scriptOutput"], Qt::Horizontal);
            w.sidebars->collapse(w.docks["terminal"]);
            settle();
            w.sidebars->expandAll();
            w.win.restoreState(defaults);
            settle();
            checkBottomArrangement(w);
        }
        saved = w.win.saveState();
    }
    Window w(false);
    w.win.restoreState(saved);
    w.win.show();
    settle();
    checkBottomArrangement(w);
}


namespace
{
void doubleClick(QWidget* w, QPoint at)
{
    click(w, Qt::LeftButton, at);
    QPointF local(at), global(w->mapToGlobal(at));
    QMouseEvent dbl(QEvent::MouseButtonDblClick, local, global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(w, &dbl);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(w, &release);
}

int tabIndex(QTabBar* bar, QDockWidget* d)
{
    for (int i = 0; i < bar->count(); ++i)
        if (bar->tabText(i) == d->windowTitle())
            return i;
    return -1;
}
} // namespace

TEST_CASE("double-clicking the showing tab collapses it; double-clicking another tab only switches")
{
    Window w;
    QDockWidget* editor = w.docks["scriptEditor"];
    QDockWidget* output = w.docks["scriptOutput"];
    QTabBar* bar = w.tabBarFor(editor);
    REQUIRE(bar);
    int other = tabIndex(bar, output);
    REQUIRE(other >= 0);
    REQUIRE(other != bar->currentIndex());
    doubleClick(bar, bar->tabRect(other).center());
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(output));
    CHECK(output->isVisible());

    bar = w.tabBarFor(output);
    REQUIRE(bar);
    int current = tabIndex(bar, output);
    REQUIRE(current == bar->currentIndex());
    doubleClick(bar, bar->tabRect(current).center());
    settle();
    CHECK(w.sidebars->isCollapsed(output));
    CHECK_FALSE(output->isVisible());
}

TEST_CASE("revealing a panel whose tab group is collapsed gives it room and keeps it expanded")
{
    Window w;
    for (const char* name : {"scriptOutput", "scriptEditor", "terminal"})
        w.sidebars->collapse(w.docks[name]);
    settle();
    QDockWidget* graph = w.docks["graphEditor"];
    revealDock(graph);
    settle();
    INFO("graph " << graph->width() << "x" << graph->height());
    CHECK(graph->isVisible());
    CHECK_FALSE(w.sidebars->isCollapsed(graph));
    CHECK(graph->height() >= 120);
}

TEST_CASE("revealing a hidden panel in a showing tab group raises its tab")
{
    Window w;
    QDockWidget* graph = w.docks["graphEditor"];
    revealDock(graph);
    settle();
    QTabBar* bar = w.tabBarFor(graph);
    REQUIRE(bar);
    CHECK(bar->tabText(bar->currentIndex()) == graph->windowTitle());
    CHECK(graph->height() >= 120);
}

TEST_CASE("revealing a collapsed panel expands it")
{
    Window w;
    QDockWidget* outliner = w.docks["outliner"];
    w.sidebars->collapse(outliner);
    settle();
    REQUIRE(w.sidebars->isCollapsed(outliner));
    revealDock(outliner);
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
    CHECK(outliner->isVisible());
}

TEST_CASE("toggling a panel reveals it, then tucks it into its sidebar")
{
    Window w;
    QDockWidget* outliner = w.docks["outliner"];
    w.sidebars->collapse(outliner);
    settle();
    toggleDock(outliner);
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
    CHECK(outliner->isVisible());
    toggleDock(outliner);
    settle();
    CHECK(w.sidebars->isCollapsed(outliner));
    toggleDock(outliner);
    settle();
    CHECK_FALSE(w.sidebars->isCollapsed(outliner));
}

TEST_CASE("toggling a tabbed panel with opaque contents reveals it, then tucks it away")
{
    Window w;
    QDockWidget* graph = w.docks["graphEditor"];
    graph->widget()->setAttribute(Qt::WA_OpaquePaintEvent);
    graph->setTitleBarWidget(new QWidget(graph));
    graph->titleBarWidget()->setAutoFillBackground(true);
    toggleDock(graph);
    settle();
    REQUIRE(dockShowing(graph));
    toggleDock(graph);
    settle();
    CHECK_FALSE(dockShowing(graph));
    toggleDock(graph);
    settle();
    CHECK(dockShowing(graph));
    // A tab behind another one is not showing, so toggling it brings it forward instead of hiding it.
    revealDock(w.docks["terminal"]);
    settle();
    CHECK_FALSE(dockShowing(graph));
    toggleDock(graph);
    settle();
    CHECK(dockShowing(graph));
}

TEST_CASE("a dock split under the channel box before its tabs join sits under the whole tab group")
{
    QMainWindow win;
    win.setDockOptions(QMainWindow::AllowNestedDocks | QMainWindow::AllowTabbedDocks);
    win.setCentralWidget(new QWidget);
    auto add = [&](const QString& name) {
        auto* d = new QDockWidget(name, &win);
        d->setObjectName(name);
        d->setWidget(new QLabel(name));
        win.addDockWidget(Qt::RightDockWidgetArea, d);
        return d;
    };
    QDockWidget* channels = add("channelBox");
    QDockWidget* layers = add("displayLayers");
    win.splitDockWidget(channels, layers, Qt::Vertical);
    QDockWidget* attrs = add("attributeEditor");
    win.tabifyDockWidget(channels, attrs);
    QDockWidget* toolkit = add("modelingToolkit");
    win.tabifyDockWidget(attrs, toolkit);
    win.resize(1600, 960);
    win.show();
    settle();
    toolkit->raise();
    settle();
    CHECK(toolkit->isVisible());
    CHECK(layers->isVisible());
    CHECK(layers->geometry().top() > toolkit->geometry().bottom());
    CHECK(layers->geometry().left() == toolkit->geometry().left());
}

TEST_CASE("showing collapsed docks directly and then restoring a state never touches a deleted sidebar tab")
{
    Window w;
    QByteArray before = w.win.saveState();
    for (const char* name : {"outliner", "channelBox", "scriptEditor", "terminal"})
        w.sidebars->collapse(w.docks[name]);
    settle();
    for (int round = 0; round < 3; ++round)
    {
        for (auto& [name, d] : w.docks)
            d->setVisible(false);
        for (auto& [name, d] : w.docks)
            d->setVisible(true);
        w.sidebars->expandAll();
        w.win.restoreState(before);
        w.sidebars->restoreCollapsed({"outliner", "scriptEditor"});
        settle();
    }
    CHECK(w.sidebars->collapsedNames().size() == 2);
    for (QWidget* c : w.win.findChildren<QWidget*>())
        if (c->inherits("QToolButton") && c->toolTip().startsWith("Expand "))
            CHECK(c->parentWidget()->isVisible());
}

TEST_CASE("revealing a floating dock that was never sized centres it at a usable size")
{
    QMainWindow window;
    window.setCentralWidget(new QLabel("centre"));
    window.resize(1200, 800);
    window.show();
    auto* dock = new QDockWidget("Docs", &window);
    dock->setObjectName("misplacedDock");
    dock->setWidget(new QLabel("page"));
    window.addDockWidget(Qt::RightDockWidgetArea, dock);
    dock->setFloating(true);
    dock->setGeometry(0, 0, 60, 40);
    QApplication::processEvents();
    CHECK(panels::floatingMisplaced(dock));

    revealDock(dock);
    QApplication::processEvents();
    CHECK(dock->isFloating());
    CHECK_FALSE(panels::floatingMisplaced(dock));
    CHECK(dock->width() >= 200);
}

TEST_CASE("collapsing and revealing a tabbed dock keeps its width")
{
    Window w;
    QDockWidget* attrs = w.docks["attributeEditor"];
    attrs->raise();
    settle();
    w.win.resizeDocks({attrs}, {520}, Qt::Horizontal);
    settle();
    const int width = attrs->width();
    for (int i = 0; i < 3; ++i)
    {
        w.sidebars->collapse(attrs);
        settle();
        w.sidebars->reveal(attrs);
        settle();
        CAPTURE(i);
        CHECK(std::abs(attrs->width() - width) < 4);
    }
}

TEST_CASE("a tab hidden behind another doesn't hold its group wider; shown, it gets its own minimum back")
{
    Window w;
    QDockWidget* channels = w.docks["channelBox"];
    QDockWidget* attrs = w.docks["attributeEditor"];
    attrs->raise();
    settle();
    attrs->widget()->setMinimumWidth(420);
    channels->raise();
    settle();
    w.win.resizeDocks({channels}, {60}, Qt::Horizontal);
    settle();
    CHECK(channels->width() < 200);
    attrs->raise();
    settle();
    CHECK(attrs->width() >= 420);
    CHECK(attrs->widget()->minimumWidth() == 420);
}

TEST_CASE("dragging a tab out of its group keeps the tab bar carrying the drag")
{
    Window w;
    QDockWidget* attrs = w.docks["attributeEditor"];
    QTabBar* bar = w.tabBarFor(attrs);
    REQUIRE(bar);
    int tab = -1;
    for (int i = 0; i < bar->count(); ++i)
        if (bar->tabText(i) == attrs->windowTitle())
            tab = i;
    REQUIRE(tab >= 0);
    auto send = [&](QEvent::Type type, QPoint at, Qt::MouseButton button, Qt::MouseButtons held) {
        QMouseEvent e(type, QPointF(at), QPointF(bar->mapToGlobal(at)), button, held, Qt::NoModifier);
        QApplication::sendEvent(bar, &e);
    };
    QPoint at = bar->tabRect(tab).center();
    send(QEvent::MouseButtonPress, at, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 8; ++step)
    {
        send(QEvent::MouseMove, at + QPoint(-step * 20, step * 40), Qt::NoButton, Qt::LeftButton);
        settle();
    }
    CHECK(attrs->isFloating());
    CHECK(bar->isVisible());
    send(QEvent::MouseButtonRelease, at + QPoint(-160, 320), Qt::LeftButton, Qt::NoButton);
    settle();
    CHECK(attrs->isFloating());

    // A layout restore after the drag must find the main window's layout whole.
    QByteArray state = w.win.saveState();
    w.win.restoreState(state);
    settle();
    CHECK(w.docks["channelBox"]->isVisible());
}
