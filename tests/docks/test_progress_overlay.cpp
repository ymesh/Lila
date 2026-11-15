#include <doctest/doctest.h>

#include "TestUtil.h"

#include "ops/Command.h"
#include "core/MainQueue.h"
#include "core/Progress.h"
#include "io/SceneFile.h"
#include "maya/Maya.h"
#include "scene/Node.h"
#include "scene/SceneCommands.h"
#include "ui/Outliner.h"
#include "ui/ProgressOverlay.h"
#include "ui/ReferenceEditor.h"

#include <QApplication>
#include <QCloseEvent>
#include <QDockWidget>
#include <QKeyEvent>
#include <QMainWindow>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>

using namespace lila;
using json = nlohmann::json;
using namespace dockstest;

namespace
{
/** The overlay card: the visible top-level window other than the main one. */
QWidget* card(QMainWindow& win)
{
    for (QWidget* w : QApplication::topLevelWidgets())
        if (w != &win && w->isVisible() && w->parentWidget() == &win)
            return w;
    return nullptr;
}

void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

QPushButton* cardButton(QMainWindow& win, const char* text)
{
    if (QWidget* c = card(win))
        for (QPushButton* b : c->findChildren<QPushButton*>())
            if (b->text() == text && b->isVisible())
                return b;
    return nullptr;
}

void pressKey(QWidget* target, Qt::Key key)
{
    QKeyEvent e(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(target, &e);
}

void setupProgress()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    mainq::init();
    ops::registerBuiltins();
    scene::types::registerBuiltins();
    scene::registerCommands();
    io::registerCommands();
    maya::registerCommands();
}
} // namespace

TEST_CASE("progress overlay: shown after 300 ms, pumps with input blocked, gone at the end")
{
    setupProgress();
    QMainWindow win;
    auto* button = new QPushButton("work", &win);
    win.setCentralWidget(button);
    int clicks = 0;
    QObject::connect(button, &QPushButton::clicked, [&] { ++clicks; });
    win.resize(800, 600);
    win.show();
    ui::progressui::install(&win);
    QApplication::processEvents();

    int timers = 0;
    QTimer::singleShot(0, [&] { ++timers; });
    {
        progress::Scope root("Unfurling...", 100.0, progress::Cancellable);
        root.setStatus("scene.lila");
        root.step();
        CHECK_FALSE(card(win)); // quick operations never flash a card
        CHECK(timers == 0);

        sleepMs(350);
        root.step();
        REQUIRE(card(win));
        CHECK(card(win)->geometry() == win.geometry());
        CHECK(timers == 1); // the event loop ran inside the step

        QPointF at(10, 10);
        QMouseEvent press(QEvent::MouseButtonPress, at, button->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, at, button->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(button, &press);
        QApplication::sendEvent(button, &release);
        CHECK(clicks == 0);

        QCloseEvent close;
        QApplication::sendEvent(&win, &close);
        CHECK_FALSE(close.isAccepted());

        // Abort asks first, from the button or Esc; Esc again or Continue backs out. Nothing takes the keyboard.
        QPushButton* abort = cardButton(win, "Abort");
        REQUIRE(abort);
        CHECK(abort->focusPolicy() == Qt::NoFocus);
        CHECK_FALSE(cardButton(win, "Continue"));
        pressKey(button, Qt::Key_Escape);
        REQUIRE(cardButton(win, "Continue"));
        CHECK(cardButton(win, "Continue")->focusPolicy() == Qt::NoFocus);
        pressKey(button, Qt::Key_Escape);
        CHECK_FALSE(cardButton(win, "Continue"));
        abort->click();
        pressKey(button, Qt::Key_Space);
        pressKey(button, Qt::Key_Return);
        CHECK_FALSE(root.cancelled());
        cardButton(win, "Continue")->click();
        CHECK_FALSE(cardButton(win, "Continue"));
        CHECK_FALSE(root.cancelled());
        abort->click();
        abort->click();
        CHECK(root.cancelled());

        // Until the operation notices, the card says the abort is on its way and offers nothing to press.
        REQUIRE(card(win));
        CHECK(card(win)->accessibleDescription() == "Aborting once the current step ends");
        CHECK_FALSE(cardButton(win, "Abort"));
        CHECK_FALSE(cardButton(win, "Continue"));
    }
    CHECK_FALSE(card(win));

    // Input flows again once the operation is over.
    QPointF at(10, 10);
    QMouseEvent press(QEvent::MouseButtonPress, at, button->mapToGlobal(at), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, at, button->mapToGlobal(at), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(button, &press);
    QApplication::sendEvent(button, &release);
    CHECK(clicks == 1);
}

TEST_CASE("progress overlay: a scene open with the editors up stays quick")
{
    // LILA_PROGRESS_SCENE names a big .lila or Maya file to time by hand; otherwise a small generated scene.
    setupProgress();
    std::string path;
    if (const char* env = std::getenv("LILA_PROGRESS_SCENE"))
        path = env;
    else
    {
        run("newScene");
        for (int i = 0; i < 60; ++i)
            run("polySphere", {{"name", "Ball" + std::to_string(i)}, {"subdivisionsAxis", 48}, {"subdivisionsHeight", 48}, {"generator", false}});
        path = (std::filesystem::temp_directory_path() / "lila_dock_tests" / "progress_scene.lila").generic_string();
        run("saveScene", {{"args", {path}}});
        run("newScene");
    }
    const bool maya = path.ends_with(".ma") || path.ends_with(".mb");
    // The quicker of two opens, so one stall on a loaded machine doesn't decide the result.
    auto timeOpen = [&] {
        double best = 1e9;
        for (int i = 0; i < 2; ++i)
        {
            const auto start = std::chrono::steady_clock::now();
            if (maya)
                run("mayaImport", {{"args", {path}}});
            else
                run("openScene", {{"args", {path}}, {"force", true}});
            QApplication::processEvents();
            best = std::min(best, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
        }
        return best;
    };
    const double bare = timeOpen();

    QMainWindow win;
    auto* outliner = new QDockWidget("Outliner", &win);
    auto* tree = new ui::Outliner(outliner);
    outliner->setWidget(tree);
    win.addDockWidget(Qt::LeftDockWidgetArea, outliner);
    auto* refs = new QDockWidget("Reference Editor", &win);
    refs->setWidget(new ui::ReferenceEditor(refs));
    win.addDockWidget(Qt::LeftDockWidgetArea, refs);
    win.resize(1200, 800);
    win.show();
    ui::progressui::install(&win);
    QApplication::processEvents();

    const double withEditors = timeOpen();
    MESSAGE("opened " << path << " in " << bare << " s bare, " << withEditors << " s with the outliner and reference editor up");
    CHECK(tree->topLevelItemCount() > 0);
    // Against the same open without the editors, so load slows both alike: the editors may add a little, never a multiple.
    CHECK(withEditors < 2.0 * bare + 0.25);
    // A card that came up on a slow open goes once the open ends.
    for (int i = 0; i < 50 && card(win); ++i)
    {
        sleepMs(10);
        QApplication::processEvents();
    }
    CHECK_FALSE(card(win));
}
