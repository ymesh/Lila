#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Actions.h"
#include "ui/Outliner.h"
#include "ui/Theme.h"

#include <QApplication>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
QTreeWidgetItem* outlinerRow(ui::Outliner& o, const char* name)
{
    auto found = o.findItems(name, Qt::MatchExactly | Qt::MatchRecursive);
    return found.isEmpty() ? nullptr : found.front();
}

QString style(QTreeWidgetItem* item) { return item->data(0, Qt::UserRole + 2).toString(); }
} // namespace

TEST_CASE("outliner rows: icons, hidden dimming, reveal and kept collapse state")
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();

    run("newScene");
    run("group", {{"empty", true}, {"name", "grp"}});
    run("spaceLocator", {{"name", "loc"}});
    run("parent", {{"args", {"loc", "grp"}}});

    ui::Outliner o;
    QApplication::processEvents();
    // Rows are styled once they can be seen.
    o.expandEverything();
    QApplication::processEvents();
    REQUIRE(ui::actions::find("outliner.revealSelected"));
    QTreeWidgetItem* loc = outlinerRow(o, "loc");
    REQUIRE(loc);
    CHECK(style(loc) == "crosshair");
    CHECK(style(outlinerRow(o, "grp")) == "box-multiple");

    run("toggleVisibility", {{"args", {"grp"}}});
    QApplication::processEvents();
    loc = outlinerRow(o, "loc");
    CHECK(style(loc) == "crosshair/h");
    CHECK(loc->foreground(0).color() == ui::themeShade(100));
    run("undo");
    QApplication::processEvents();
    CHECK(style(outlinerRow(o, "loc")) == "crosshair");

    o.collapseEverything();
    CHECK_FALSE(outlinerRow(o, "grp")->isExpanded());
    run("spaceLocator", {{"name", "other"}});
    QApplication::processEvents();
    CHECK_FALSE(outlinerRow(o, "grp")->isExpanded());

    run("select", {{"args", {"loc"}}});
    QApplication::processEvents();
    o.revealSelected();
    CHECK(outlinerRow(o, "grp")->isExpanded());
    CHECK(outlinerRow(o, "loc")->isSelected());
    // The selection's ancestor chain is shaded, the selected row and unrelated rows are not.
    CHECK(outlinerRow(o, "grp")->data(0, Qt::BackgroundRole).isValid());
    CHECK_FALSE(outlinerRow(o, "loc")->data(0, Qt::BackgroundRole).isValid());
    CHECK_FALSE(outlinerRow(o, "other")->data(0, Qt::BackgroundRole).isValid());
    run("select", {{"args", {"other"}}});
    QApplication::processEvents();
    CHECK_FALSE(outlinerRow(o, "grp")->data(0, Qt::BackgroundRole).isValid());
}

TEST_CASE("outliner lists object sets after the hierarchy, members under them, click selects the members")
{
    run("newScene");
    run("spaceLocator", {{"name", "a"}});
    run("spaceLocator", {{"name", "b"}});
    run("createSet", {{"args", {"a"}}, {"name", "Props"}});

    ui::Outliner o;
    QApplication::processEvents();
    REQUIRE(o.topLevelItemCount() == 3);
    QTreeWidgetItem* set = o.topLevelItem(2);
    CHECK(set->text(0) == "Props");
    CHECK_FALSE(set->isExpanded());
    REQUIRE(set->childCount() == 1);
    CHECK(set->child(0)->text(0) == "a");

    run("addToSet", {{"args", {"Props", "b"}}});
    QApplication::processEvents();
    set = outlinerRow(o, "Props");
    REQUIRE(set);
    CHECK(set->childCount() == 2);

    run("select", {{"clear", true}});
    Q_EMIT o.itemClicked(set, 0);
    QApplication::processEvents();
    CHECK(run("ls", {{"selection", true}}) == json({"a", "b"}));
    CHECK_FALSE(set->isSelected());
}

TEST_CASE("outliner: selection changes inside a command show once the command finishes")
{
    run("newScene");
    run("spaceLocator", {{"name", "a"}});
    run("spaceLocator", {{"name", "b"}});
    ui::Outliner o;
    QApplication::processEvents();
    static ui::Outliner* probe = nullptr;
    static bool shownInside = true;
    probe = &o;
    if (!ops::find("outlinerSelectProbe"))
        ops::registerCommand({.name = "outlinerSelectProbe",
                              .doc = "Probe.",
                              .undoable = false,
                              .run = [](const Args&) -> json {
                                  scene::Graph& g = scene::Graph::current();
                                  for (int i = 0; i < 50; ++i)
                                      g.select({g.find(i % 2 ? "a" : "b")->id()});
                                  shownInside = outlinerRow(*probe, "a")->isSelected();
                                  return nullptr;
                              }});
    run("select", {{"clear", true}});
    run("outlinerSelectProbe");
    CHECK_FALSE(shownInside);
    CHECK(outlinerRow(o, "a")->isSelected());
    CHECK_FALSE(outlinerRow(o, "b")->isSelected());
    // Outside a command it follows at once.
    scene::Graph::current().select({scene::Graph::current().find("b")->id()});
    CHECK(outlinerRow(o, "b")->isSelected());
}
