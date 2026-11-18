#include <doctest/doctest.h>

#include "TestUtil.h"

#include "core/MainQueue.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/MaterialEditor.h"
#include "ui/SceneDialogs.h"

#include <QAction>
#include <QApplication>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QToolButton>
#include <QTreeWidget>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
void registerWelcomeUi()
{
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    ui::registerMaterialUi();
}

void pressOutside(QWidget* dialog)
{
    QWidget other;
    other.setGeometry(dialog->geometry().right() + 40, dialog->geometry().bottom() + 40, 50, 50);
    other.show();
    const QPointF global = QPointF(other.geometry().center());
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(10, 10), global, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&other, &press);
    pump();
}
} // namespace

TEST_CASE("welcome dialog shows whole example cards in one column and ignores clicks outside")
{
    QPointer<ui::StartupDialog> welcome = new ui::StartupDialog({}, true, nullptr);
    auto* examples = welcome->findChild<QListWidget*>("welcomeExamples");
    REQUIRE(examples);
    examples->clear();
    for (int i = 0; i < 12; ++i)
        new QListWidgetItem(QString("Example %1").arg(i), examples);
    welcome->present();
    pump();
    CHECK_FALSE(examples->horizontalScrollBar()->isVisible());
    CHECK(examples->verticalScrollBar()->isVisible());
    const QRect first = examples->visualItemRect(examples->item(0));
    CHECK(examples->visualItemRect(examples->item(1)).left() == first.left());
    CHECK(examples->visualItemRect(examples->item(1)).top() > first.top());
    const QRect view = examples->viewport()->rect();
    for (int i = 0; i < examples->count(); ++i)
    {
        const QRect card = examples->visualItemRect(examples->item(i));
        CAPTURE(i);
        CHECK((!card.intersects(view) || view.contains(card)));
    }

    pressOutside(welcome);
    REQUIRE(welcome);
    CHECK(welcome->isVisible());
    welcome->reject();
    pump();

    QPointer<ui::StartupDialog> recent = new ui::StartupDialog({}, false, nullptr);
    recent->present();
    pump();
    pressOutside(recent);
    CHECK((!recent || !recent->isVisible()));
    delete welcome.data();
    delete recent.data();
}

TEST_CASE("welcome dialog closes on Esc from the main window and from its close button")
{
    QWidget main;
    auto* field = new QLineEdit(&main);
    main.show();
    auto escTo = [](QWidget* w) {
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QApplication::sendEvent(w, &press);
        pump();
    };
    auto open = [&] {
        QPointer<ui::StartupDialog> welcome = new ui::StartupDialog({}, true, &main);
        welcome->present();
        pump();
        REQUIRE(welcome->isVisible());
        return welcome;
    };

    // The key goes to a widget of the main window, as when it holds the keyboard focus.
    QPointer<ui::StartupDialog> welcome = open();
    field->setFocus();
    escTo(field);
    CHECK((!welcome || !welcome->isVisible()));

    // A menu open over it takes Esc for itself.
    welcome = open();
    QMenu menu;
    menu.addAction("Item");
    menu.popup(main.geometry().center());
    pump();
    REQUIRE(QApplication::activePopupWidget() == &menu);
    escTo(&menu);
    REQUIRE(welcome);
    CHECK(welcome->isVisible());
    menu.close();
    pump();

    auto* close = welcome->findChild<QToolButton*>("welcomeClose");
    REQUIRE(close);
    CHECK(close->isVisible());
    CHECK(close->toolTip() == "Close (Esc)");
    // In the top right corner, inside the page margins.
    const QPoint corner = close->mapTo(welcome.data(), close->rect().topRight());
    CHECK(welcome->width() - corner.x() <= 24);
    CHECK(corner.y() <= 24);
    close->click();
    pump();
    CHECK((!welcome || !welcome->isVisible()));
    delete welcome.data();
}

TEST_CASE("material editor filters by name and shows icon-only buttons in order")
{
    registerWelcomeUi();
    ops::execute("newScene", json::object());
    for (const char* name : {"M_Brick", "M_Wood", "M_RedBRICK"})
        ops::execute("createMaterial", {{"args", {name}}});
    ui::MaterialEditor editor;
    editor.show();
    pump();
    auto* list = editor.findChild<QListWidget*>();
    QLineEdit* filter = nullptr;
    for (QLineEdit* e : editor.findChildren<QLineEdit*>())
        if (e->placeholderText() == "Filter")
            filter = e;
    REQUIRE(list);
    REQUIRE(filter);
    auto shown = [&] {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            if (!list->item(i)->isHidden())
                out << list->item(i)->text();
        return out;
    };
    const qsizetype all = shown().size();
    filter->setText("brick");
    CHECK(shown() == QStringList{"M_Brick", "M_RedBRICK"});
    ops::execute("createMaterial", {{"args", {"M_Stone"}}});
    pump();
    CHECK(shown() == QStringList{"M_Brick", "M_RedBRICK"});
    filter->setText("m_*brick");
    CHECK(shown() == QStringList{"M_Brick", "M_RedBRICK"});
    filter->setText("*_b?ick wood");
    CHECK(shown() == QStringList{"M_Brick", "M_Wood"});
    filter->clear();
    CHECK(shown().size() == all + 1);

    QStringList tips;
    for (QPushButton* b : editor.findChildren<QPushButton*>(Qt::FindDirectChildrenOnly))
        if (b->text().isEmpty() && !b->icon().isNull())
            tips << b->toolTip().section(' ', 0, 0).remove(':');
    CHECK(tips == QStringList{"Assign", "New", "Delete", "Show", "Select"});
}

TEST_CASE("material editor lists the read-only Default and filters unused and selection")
{
    registerWelcomeUi();
    ops::execute("newScene", json::object());
    PrefStore& prefs = PrefStore::get();
    for (const char* key : {"/materialEditor/hideReferenced", "/materialEditor/hideUnused", "/materialEditor/selectionOnly"})
        prefs.set("general", key, false);
    const std::string a = ops::execute("polyCube", json::object())[0], b = ops::execute("polyCube", json::object())[0];
    for (const char* name : {"M_Used", "M_Spare"})
        ops::execute("createMaterial", {{"args", {name}}});
    ops::execute("assignMaterial", {{"args", {"M_Used", a}}});
    ui::MaterialEditor editor;
    editor.show();
    pump();
    auto* list = editor.findChild<QListWidget*>();
    REQUIRE(list);
    auto shown = [&] {
        QStringList out;
        for (int i = 0; i < list->count(); ++i)
            if (!list->item(i)->isHidden())
                out << list->item(i)->text();
        return out;
    };
    CHECK(shown() == QStringList{"Default", "M_Spare", "M_Used"});
    CHECK(!(list->item(0)->flags() & Qt::ItemIsEditable));
    CHECK(list->currentItem()->text() == "M_Spare");

    QPushButton *assign = nullptr, *del = nullptr, *select = nullptr;
    for (QPushButton* p : editor.findChildren<QPushButton*>(Qt::FindDirectChildrenOnly))
    {
        const QString word = p->toolTip().section(' ', 0, 0).remove(':');
        (word == "Assign" ? assign : word == "Delete" ? del : word == "Select" ? select : p) = p;
    }
    REQUIRE((assign && del && select));
    list->setCurrentRow(0);
    pump();
    CHECK(assign->isEnabled());
    CHECK(select->isEnabled());
    CHECK(!del->isEnabled());
    select->click();
    pump();
    CHECK(ops::execute("ls", {{"selection", true}}) == json::array({b}));
    ops::execute("select", {{"args", {a}}});
    pump();
    CHECK(list->currentItem()->text() == "M_Used");
    list->setCurrentRow(0);
    assign->click();
    pump();
    CHECK(ops::execute("selectByMaterial", {{"args", {"M_Used"}}}).empty());

    ops::execute("assignMaterial", {{"args", {"M_Used", a}}});
    QAction *unused = nullptr, *selectionOnly = nullptr;
    for (QAction* act : editor.findChildren<QAction*>())
        (act->text() == "Hide Unused" ? unused : act->text() == "Selection Only" ? selectionOnly : act) = act;
    REQUIRE((unused && selectionOnly));
    unused->setChecked(true);
    CHECK(shown() == QStringList{"Default", "M_Used"});
    unused->setChecked(false);
    ops::execute("select", {{"args", {a}}});
    selectionOnly->setChecked(true);
    pump();
    CHECK(shown() == QStringList{"M_Used"});
    selectionOnly->setChecked(false);
    CHECK(shown().size() == 3);

    ops::execute("createMaterial", {{"args", {"M_Toon"}}, {"shadingModel", "toon"}, {"blendMode", "masked"}});
    pump();
    QAction *toon = nullptr, *opaque = nullptr;
    for (QAction* act : editor.findChildren<QAction*>())
        (act->text() == "Toon" ? toon : act->text() == "Opaque" ? opaque : act) = act;
    REQUIRE((toon && opaque));
    toon->setChecked(true);
    CHECK(shown() == QStringList{"M_Toon"});
    toon->setChecked(false);
    opaque->setChecked(true);
    CHECK(shown() == QStringList{"Default", "M_Spare", "M_Used"});
    opaque->setChecked(false);
    CHECK(shown().size() == 4);
}

TEST_CASE("display layers panel marks empty layers")
{
    registerWelcomeUi();
    ops::execute("newScene", json::object());
    ops::execute("createDisplayLayer", {{"empty", true}});
    ui::DisplayLayersPanel panel;
    panel.show();
    pump();
    auto* tree = panel.findChild<QTreeWidget*>();
    REQUIRE(tree);
    REQUIRE(tree->topLevelItemCount() == 1);
    const int last = tree->columnCount() - 1;
    CHECK_FALSE(tree->topLevelItem(0)->icon(last).isNull());
    CHECK(tree->topLevelItem(0)->text(1).isEmpty());
}
