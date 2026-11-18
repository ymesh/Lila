#include <doctest/doctest.h>

#include "ui/Actions.h"
#include "ui/HotkeyBindings.h"
#include "core/PrefStore.h"
#include "ui/Hotkeys.h"
#include "ui/HotkeyCheatSheet.h"
#include "ui/Shelf.h"

#include <QApplication>
#include <QFrame>
#include <QLabel>
#include <QPointer>
#include <QLineEdit>
#include <QMainWindow>
#include <QElapsedTimer>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QWidget>

#include <string>
#include <vector>

using namespace lila::ui;

TEST_CASE("hotkey text spelled with a shifted symbol parses to Shift plus the key under it")
{
    auto grow = hotkeys::parse("Shift+.");
    REQUIRE(grow);
    CHECK(grow->key() == Qt::Key_Period);
    CHECK(grow->keyboardModifiers() == Qt::ShiftModifier);
    CHECK(hotkeys::parse("Shift+>") == grow);
    CHECK(hotkeys::parse(">") == grow);
    CHECK(hotkeys::parse("Shift+<") == hotkeys::parse("Shift+,"));
    auto open = hotkeys::parse("Ctrl+Shift+R");
    REQUIRE(open);
    CHECK(open->keyboardModifiers() == (Qt::ControlModifier | Qt::ShiftModifier));
    CHECK(open->key() == Qt::Key_R);
    auto undo = hotkeys::parse("[");
    REQUIRE(undo);
    CHECK(undo->key() == Qt::Key_BracketLeft);
    CHECK(undo->keyboardModifiers() == Qt::NoModifier);
    CHECK(hotkeys::parse("]")->key() == Qt::Key_BracketRight);
}

namespace
{
std::vector<std::string>& ranActions()
{
    static std::vector<std::string> ran;
    return ran;
}

void pressAndRelease(QWidget* w, Qt::Key key, Qt::KeyboardModifiers mods, quint32 scan)
{
    QKeyEvent down(QEvent::KeyPress, key, mods, scan, 0, 0);
    QApplication::sendEvent(w, &down);
    QKeyEvent up(QEvent::KeyRelease, key, mods, scan, 0, 0);
    QApplication::sendEvent(w, &up);
}
} // namespace

TEST_CASE("Shift+backtick and Shift+1 pick selection modes without the floating shelf on backtick")
{
    for (const char* id : {"select.objectMode", "select.vertexMode", "shelf.floatingOpen", "shelf.floatingRelease"})
        actions::add({.id = id, .label = id, .category = "Test", .run = [id] { ranActions().push_back(id); }, .doc = "Test stand-in."});
    QWidget w;
    w.show();
    // The filter acts on an event's first receiver only; a grab makes that this widget without window activation.
    w.grabKeyboard();
    Hotkeys::get().install();

    // Windows reports Shift+` as Key_AsciiTilde with the backtick's scancode; elsewhere only the Qt key is known.
    for (quint32 scan : {0x29u, 0u})
    {
        ranActions().clear();
        pressAndRelease(&w, Qt::Key_AsciiTilde, Qt::ShiftModifier, scan);
        CHECK(ranActions() == std::vector<std::string>{"select.objectMode"});
        ranActions().clear();
        pressAndRelease(&w, Qt::Key_QuoteLeft, Qt::NoModifier, scan);
        CHECK(ranActions() == std::vector<std::string>{"shelf.floatingOpen", "shelf.floatingRelease"});
    }
    ranActions().clear();
    pressAndRelease(&w, Qt::Key_Exclam, Qt::ShiftModifier, 0x02);
    CHECK(ranActions() == std::vector<std::string>{"select.vertexMode"});
    w.releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}

TEST_CASE("the floating shelf introduces its hotkey once")
{
    lila::PrefStore::get().set("general", "/floatingShelf/introduced", false);
    Hotkeys::get().install();
    qApp->removeEventFilter(&Hotkeys::get());
    FloatingShelf::introduce();
    QPointer<FloatingShelf> shelf = FloatingShelf::active();
    REQUIRE(shelf);
    CHECK(FloatingShelf::isOpen());
    auto* intro = shelf->findChild<QFrame*>("floatingShelfIntro");
    REQUIRE(intro);
    QLabel* text = intro->findChild<QLabel*>();
    REQUIRE(text);
    CHECK(text->text().contains("<b>`</b> (left of 1)"));
    shelf->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(!shelf);
    CHECK(!FloatingShelf::isOpen());

    FloatingShelf::introduce();
    CHECK(!FloatingShelf::active());
}

TEST_CASE("Ctrl+backtick opens the command palette")
{
    actions::add({.id = "window.commandPalette", .label = "Command Palette", .category = "Test",
                  .run = [] { ranActions().push_back("window.commandPalette"); }, .doc = "Test stand-in."});
    for (const char* id : {"shelf.floatingOpen", "shelf.floatingRelease"})
        actions::add({.id = id, .label = id, .category = "Test", .run = [id] { ranActions().push_back(id); }, .doc = "Test stand-in."});
    QWidget w;
    w.show();
    w.grabKeyboard();
    Hotkeys::get().install();
    for (quint32 scan : {0x29u, 0u})
    {
        ranActions().clear();
        pressAndRelease(&w, Qt::Key_QuoteLeft, Qt::ControlModifier, scan);
        CHECK(ranActions() == std::vector<std::string>{"window.commandPalette"});
    }
    // What Windows actually reports for Ctrl+` on a US layout.
    ranActions().clear();
    pressAndRelease(&w, Qt::Key_At, Qt::ControlModifier, 0x29);
    CHECK(ranActions() == std::vector<std::string>{"window.commandPalette"});
    w.releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}

TEST_CASE("held undo repeats on the pref cadence, ignoring key autorepeat, and a tap undoes once")
{
    int undos = 0;
    actions::add({.id = "edit.undo", .label = "Undo", .category = "Test", .run = [&undos] { ++undos; }, .doc = "Test stand-in."});
    lila::PrefStore::get().set("general", "/undo/repeatDelayMs", 100);
    lila::PrefStore::get().set("general", "/undo/repeatIntervalMs", 50);
    QWidget w;
    w.show();
    w.grabKeyboard();
    Hotkeys::get().install();
    auto pump = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QApplication::processEvents(QEventLoop::AllEvents, 5);
    };

    pressAndRelease(&w, Qt::Key_Z, Qt::ControlModifier, 0x2C);
    pump(250);
    CHECK(undos == 1);

    undos = 0;
    QKeyEvent down(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, 0x2C, 0, 0);
    QApplication::sendEvent(&w, &down);
    QElapsedTimer held;
    held.start();
    while (held.elapsed() < 380)
    {
        QKeyEvent again(QEvent::KeyPress, Qt::Key_Z, Qt::NoModifier, 0x2C, 0, 0, QString(), true);
        QApplication::sendEvent(&w, &again);
        pump(10);
    }
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_Z, Qt::NoModifier, 0x2C, 0, 0);
    QApplication::sendEvent(&w, &up);
    int atRelease = undos;
    pump(200);
    // One on press, then repeats from 100 ms every 50 ms until 380 ms: about 6, never the ~38 autorepeats.
    CHECK(atRelease >= 4);
    CHECK(atRelease <= 8);
    CHECK(undos == atRelease);

    w.releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}

TEST_CASE("Shift+F1 to Shift+F5 switch to the built-in layouts in order")
{
    const char* ids[] = {"layout.apply.Model", "layout.apply.UV", "layout.apply.Rig", "layout.apply.Animate", "layout.apply.Script"};
    for (const char* id : ids)
        actions::add({.id = id, .label = id, .category = "Test", .run = [id] { ranActions().push_back(id); }, .doc = "Test stand-in."});
    QWidget w;
    w.show();
    w.grabKeyboard();
    Hotkeys::get().install();
    for (int i = 0; i < 5; ++i)
    {
        ranActions().clear();
        pressAndRelease(&w, Qt::Key(Qt::Key_F1 + i), Qt::ShiftModifier, 0);
        CHECK(ranActions() == std::vector<std::string>{ids[i]});
    }
    w.releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}

TEST_CASE("F10 resets the layout and F1 opens the cheat sheet on press, closing it on release")
{
    for (const char* id : {"window.resetLayout", "select.edgeMode", "help.hotkeyCheatSheet", "help.hotkeyCheatSheetClose"})
        actions::add({.id = id, .label = id, .category = "Test", .run = [id] { ranActions().push_back(id); }, .doc = "Test stand-in."});
    QWidget w;
    w.show();
    w.grabKeyboard();
    Hotkeys::get().install();
    ranActions().clear();
    pressAndRelease(&w, Qt::Key_F10, Qt::NoModifier, 0);
    CHECK(ranActions() == std::vector<std::string>{"window.resetLayout"});

    ranActions().clear();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier, 0, 0, 0);
    QApplication::sendEvent(&w, &down);
    CHECK(ranActions() == std::vector<std::string>{"help.hotkeyCheatSheet"});
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_F1, Qt::NoModifier, 0, 0, 0);
    QApplication::sendEvent(&w, &up);
    CHECK(ranActions() == std::vector<std::string>{"help.hotkeyCheatSheet", "help.hotkeyCheatSheetClose"});
    w.releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}

TEST_CASE("a disabled action is skipped by run and reported by isEnabled")
{
    bool on = false;
    int runs = 0;
    actions::add({.id = "test.gated", .label = "Gated", .category = "Test", .run = [&runs] { ++runs; }, .doc = "Test stand-in."});
    CHECK(actions::isEnabled(nlohmann::json("test.gated")));
    actions::setEnabledWhen("test.gated", [&on] { return on; });
    CHECK_FALSE(actions::isEnabled(nlohmann::json("test.gated")));
    CHECK_FALSE(actions::run(nlohmann::json("test.gated")));
    CHECK(runs == 0);
    on = true;
    CHECK(actions::isEnabled(nlohmann::json("test.gated")));
    CHECK(actions::run(nlohmann::json("test.gated")));
    CHECK(runs == 1);
    CHECK(actions::isEnabled(nlohmann::json{{"command", "anything"}}));
    actions::setEnabledWhen("test.gated", nullptr);
}

TEST_CASE("the cheat sheet filters by action, key and category, sorted with Other last")
{
    actions::add({.id = "window.resetLayout", .label = "Reset Layout", .category = "Windows", .run = [] {}, .doc = "Test stand-in."});
    QWidget host;
    host.resize(1400, 900);
    HotkeyCheatSheet sheet(&host, nullptr);
    const int all = sheet.shownRows();
    REQUIRE(all > 1);
    QStringList categories = sheet.shownCategories();
    if (categories.contains("Other"))
        CHECK(categories.back() == "Other");
    categories.removeAll("Other");
    QStringList sorted = categories;
    sorted.sort();
    CHECK(categories == sorted);

    sheet.filter()->setText("f10");
    CHECK(sheet.shownRows() == 1);
    sheet.filter()->setText("reset layout");
    CHECK(sheet.shownRows() >= 1);
    CHECK(sheet.shownRows() < all);
    sheet.filter()->setText("windows");
    CHECK(sheet.shownCategories().contains("Windows"));
    sheet.filter()->setText("zzqx nothing");
    CHECK(sheet.shownRows() == 0);
    CHECK(sheet.shownCategories().isEmpty());
    sheet.filter()->clear();
    CHECK(sheet.shownRows() == all);
    CHECK_FALSE(sheet.pinned());
    sheet.pin();
    CHECK(sheet.pinned());
}

TEST_CASE("the cheat sheet keeps readable rows and scrolls when they do not fit")
{
    QWidget host;
    host.resize(1400, 900);
    HotkeyCheatSheet sheet(&host, nullptr);
    sheet.resize(host.size());
    REQUIRE(sheet.shownRows() > 1);
    CHECK(sheet.lineHeight() >= 22);
    CHECK(sheet.columns() >= 3);

    // An unshown widget only gets its resize event when shown.
    sheet.resize(520, 360);
    QResizeEvent resized(sheet.size(), host.size());
    QApplication::sendEvent(&sheet, &resized);
    CHECK(sheet.lineHeight() == 22);
    CHECK(sheet.columns() == 1);
    REQUIRE(sheet.maxScroll() > 0);
    sheet.scrollTo(1e9);
    CHECK(sheet.scroll() == sheet.maxScroll());
    sheet.filter()->setText("f10");
    CHECK(sheet.scroll() == 0);
    CHECK(sheet.maxScroll() == 0);
}

TEST_CASE("the cheat sheet peeks only while F1 is held; a tap, the menu or a script leaves it open to filter")
{
    actions::add({.id = "help.hotkeyCheatSheet", .label = "Hotkey Cheat Sheet", .category = "Help", .run = [] { HotkeyCheatSheet::open(); },
                  .doc = "Test stand-in."});
    actions::add({.id = "help.hotkeyCheatSheetClose", .label = "Close Hotkey Cheat Sheet", .category = "Help",
                  .run = [] { HotkeyCheatSheet::released(); }, .doc = "Test stand-in."});
    QMainWindow main;
    auto* w = new QWidget(&main);
    main.setCentralWidget(w);
    main.resize(1200, 800);
    main.show();
    w->setFocusPolicy(Qt::StrongFocus);
    w->setFocus();
    auto settle = [](int ms) {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < ms)
            QApplication::processEvents();
    };

    actions::run(nlohmann::json("help.hotkeyCheatSheet"));
    REQUIRE(HotkeyCheatSheet::active());
    CHECK(HotkeyCheatSheet::active()->pinned());
    actions::run(nlohmann::json("help.hotkeyCheatSheet"));
    CHECK_FALSE(HotkeyCheatSheet::active());
    settle(20);

    Hotkeys::get().install();
    w->grabKeyboard();
    QKeyEvent down(QEvent::KeyPress, Qt::Key_F1, Qt::NoModifier, 0, 0, 0);
    QApplication::sendEvent(w, &down);
    REQUIRE(HotkeyCheatSheet::active());
    CHECK_FALSE(HotkeyCheatSheet::active()->pinned());
    settle(400);
    QKeyEvent up(QEvent::KeyRelease, Qt::Key_F1, Qt::NoModifier, 0, 0, 0);
    QApplication::sendEvent(w, &up);
    CHECK_FALSE(HotkeyCheatSheet::active());
    settle(20);

    QApplication::sendEvent(w, &down);
    QApplication::sendEvent(w, &up);
    REQUIRE(HotkeyCheatSheet::active());
    CHECK(HotkeyCheatSheet::active()->pinned());
    HotkeyCheatSheet::dismiss();
    settle(20);
    w->releaseKeyboard();
    qApp->removeEventFilter(&Hotkeys::get());
}
