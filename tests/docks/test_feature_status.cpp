#include <doctest/doctest.h>

#include "core/Features.h"
#include "ui/Actions.h"
#include "ui/Menus.h"

#include <QAction>
#include <QMenu>

using namespace lila;
using namespace lila::ui;
using json = nlohmann::json;

namespace
{
QStringList rowTexts(QMenu* menu)
{
    QStringList out;
    for (QAction* a : menu->actions())
        out << (a->isSeparator() ? QString("-") : a->text());
    return out;
}
} // namespace

TEST_CASE("menus state a feature's status once at the top when every row shares it, and per row when they differ")
{
    features::set({{"sections",
                    {{{"cards",
                       {{{"id", "wip"}, {"title", "Work"}, {"status", "dev"}},
                        {{"id", "held"}, {"title", "Held"}, {"status", "dev"}, {"note", "Not yet."}}}}}}}},
                  {{"wip", {{"menus", {"FsAll"}}, {"actions", {"fs.wip."}}}}, {"held", {{"actions", {"fs.held."}}}}});
    for (const char* id : {"fs.a", "fs.b", "fs.c", "fs.wip.d", "fs.held.e"})
        actions::add({id, id, "Test", [] {}, nullptr, "Does it."});
    menus::addMenu({.id = "FsAll", .title = "All"});
    menus::addActions("FsAll", {"fs.a", "fs.b"});
    menus::addSubmenu("FsAll", "Sub");
    menus::addActions("FsAll/Sub", {"fs.c"});
    menus::addMenu({.id = "FsMixed", .title = "Mixed"});
    menus::addActions("FsMixed", {"fs.a", "fs.wip.d", "fs.held.e"});

    {
        menus::MenuBuilder builder(nullptr, {});
        builder.rebuild();
        QMenu* all = builder.menu("FsAll");
        REQUIRE(all);
        const QStringList top = rowTexts(all);
        REQUIRE(top.size() >= 4);
        CHECK(top[0] == "In development");
        CHECK_FALSE(all->actions()[0]->isEnabled());
        CHECK(top[1] == "-");
        CHECK(top[2] == "fs.a");
        // The submenu shares the status its parent already states.
        CHECK(rowTexts(builder.menu("FsAll/Sub")) == QStringList{"fs.c"});
        CHECK(rowTexts(builder.menu("FsMixed")) == QStringList{"fs.a", "fs.wip.d  (in development)", "fs.held.e  (not ready for use)"});
        for (const char* path : {"FsAll", "FsMixed"})
            delete builder.menu(path);
    }

    menus::removeMenu("FsAll");
    menus::removeMenu("FsMixed");
    for (const char* id : {"fs.a", "fs.b", "fs.c", "fs.wip.d", "fs.held.e"})
        actions::remove(id);
    features::reset();
}
