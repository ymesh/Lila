#include "ui/Panels.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"
#include "ui/Actions.h"
#include "ui/DisplayLayersPanel.h"
#include "ui/UiCommon.h"
#include "ui/UndoHistory.h"

#include <QApplication>
#include <QCursor>
#include <QDockWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QTimer>

#include <algorithm>

namespace lila::ui
{
namespace
{
std::function<void(const char*)>& showDockHook()
{
    static std::function<void(const char*)> hook;
    return hook;
}

constexpr const char* kDockedByUserPref = "/ui/dockedByUser";

bool dockedByUser(const std::string& id)
{
    for (const auto& v : PrefStore::get().value("general", kDockedByUserPref, nlohmann::json::array()))
        if (v.is_string() && v.get<std::string>() == id)
            return true;
    return false;
}

std::function<void(const char*)>& toggleDockHook()
{
    static std::function<void(const char*)> hook;
    return hook;
}

void toggleDockByName(const char* dock)
{
    if (toggleDockHook())
        toggleDockHook()(dock);
}
} // namespace

Registry<panels::Panel>& panels::registry()
{
    static Registry<Panel> r;
    return r;
}

void panels::add(Panel p, std::string owner)
{
    std::string id = p.id;
    if (!p.toggleAction.empty() && !actions::find(p.toggleAction))
        actions::add({.id = p.toggleAction, .label = p.title, .category = "Windows", .run = [id] { panels::toggle(id); },
                      .doc = "Shows the " + p.title + " panel, or tucks it away when it is showing.",
                      .python = "lila.ui.show_panel(\"" + id + "\")", .keywords = p.keywords},
                     owner);
    registry().put(id, std::move(p), std::move(owner));
}

uint64_t panels::add(ext::Context& ctx, Panel p)
{
    if (const std::string* owner = registry().ownerOf(p.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "panel '" + p.id + "' already belongs to " + *owner);
        return 0;
    }
    if (p.toggleAction.empty())
        p.toggleAction = "window." + p.id;
    std::string id = p.id, action = p.toggleAction;
    bool ownsAction = !actions::find(action);
    add(std::move(p), ctx.owner());
    return ctx.track("panel:" + id, [id, action, ownsAction, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            registry().remove(id);
        if (const std::string* o = actions::registry().ownerOf(action); ownsAction && o && *o == own)
            actions::remove(action);
    });
}

bool panels::remove(const std::string& id)
{
    return registry().remove(id);
}

void panels::show(const std::string& id)
{
    if (showDockHook())
        showDockHook()(id.c_str());
}

void panels::toggle(const std::string& id)
{
    lila::ui::toggleDockByName(id.c_str());
}

void panels::floatUnplaced(QDockWidget* dock)
{
    const std::string id = dock ? dock->objectName().toStdString() : std::string();
    const Panel* p = dock ? registry().find(id) : nullptr;
    if (!p || !p->floatFirst)
        return;
    if (!dock->property("lilaWatchesDocking").toBool())
    {
        dock->setProperty("lilaWatchesDocking", true);
        QObject::connect(dock, &QDockWidget::topLevelChanged, dock, [dock, id](bool floating) {
            // Only the user docks a dock while the window is up: a saved layout is restored before anything shows.
            if (floating || !dock->isVisible() || !dock->window()->isVisible() || dockedByUser(id))
                return;
            // Deferred: general pref listeners rebuild menus, which must not run inside Qt's dock plug.
            QTimer::singleShot(0, dock, [id] {
                if (dockedByUser(id))
                    return;
                nlohmann::json list = PrefStore::get().value("general", kDockedByUserPref, nlohmann::json::array());
                if (!list.is_array())
                    list = nlohmann::json::array();
                list.push_back(id);
                PrefStore::get().set("general", kDockedByUserPref, list);
            });
        });
    }
    if (dock->isFloating() || dockedByUser(id))
        return;
    placeFloating(dock);
}

bool panels::floatingMisplaced(const QDockWidget* dock)
{
    if (!dock || !dock->isFloating())
        return false;
    const QRect g = dock->frameGeometry();
    // A floating dock that was never sized comes up at the screen corner at its minimum size.
    if (!dock->property(kOwnSizeProperty).toBool() && (g.width() < 200 || g.height() < 120))
        return true;
    for (QScreen* s : QGuiApplication::screens())
        if (s->availableGeometry().contains(g.topLeft() + QPoint(g.width() / 2, 10)))
            return false;
    return true;
}

void panels::placeFloating(QDockWidget* dock)
{
    QWidget* window = dock->parentWidget() ? dock->parentWidget()->window() : nullptr;
    QScreen* screen = window && window->screen() ? window->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();
    const Panel* p = registry().find(dock->objectName().toStdString());
    QRect r(0, 0, std::min(p ? p->floatWidth : 1000, avail.width() * 9 / 10), std::min(p ? p->floatHeight : 750, avail.height() * 9 / 10));
    r.moveCenter(window && window->isVisible() ? window->frameGeometry().center() : avail.center());
    r.moveLeft(std::clamp(r.left(), avail.left(), avail.right() - r.width()));
    r.moveTop(std::clamp(r.top(), avail.top(), avail.bottom() - r.height()));
    dock->setFloating(true);
    dock->setGeometry(r);
}

std::vector<std::string> panels::shownToolPalettes(QWidget* window)
{
    std::vector<std::string> out;
    for (const Panel* p : registry().allInOrder())
        if (p->toolPalette)
            if (auto* d = window->findChild<QDockWidget*>(QString::fromStdString(p->id)); d && d->isVisible() && d->isFloating())
                out.push_back(p->id);
    return out;
}

void panels::hideToolPalettes(QWidget* window, const std::vector<std::string>& keep)
{
    for (const Panel* p : registry().allInOrder())
        if (p->toolPalette)
            if (auto* d = window->findChild<QDockWidget*>(QString::fromStdString(p->id)); d && d->isFloating())
                d->setVisible(std::ranges::find(keep, p->id) != keep.end());
}

void panels::placeToolPalette(QDockWidget* dock)
{
    if (!dock || !dock->isFloating() || (dock->property("lilaPalettePlaced").toBool() && !floatingMisplaced(dock)))
        return;
    dock->setProperty("lilaPalettePlaced", true);
    dock->resize(dock->sizeHint());
    const QPoint cursor = QCursor::pos();
    QScreen* screen = QGuiApplication::screenAt(cursor);
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return;
    const QRect avail = screen->availableGeometry();
    const QSize size = dock->frameGeometry().size();
    // Beside the cursor rather than under it, so the click that opened the tool does not land on the palette.
    QPoint at = cursor + QPoint(32, -size.height() / 3);
    at.setX(std::clamp(at.x(), avail.left(), std::max(avail.left(), avail.right() - size.width())));
    at.setY(std::clamp(at.y(), avail.top(), std::max(avail.top(), avail.bottom() - size.height())));
    dock->move(at);
}

void setShowDockHook(std::function<void(const char* dockName)> showDock)
{
    showDockHook() = std::move(showDock);
}

void setToggleDockHook(std::function<void(const char* dockName)> toggleDock)
{
    toggleDockHook() = std::move(toggleDock);
}

void registerPanelActions()
{
    using namespace ops;
    if (!find("showPanel"))
        registerCommand({
            .name = "showPanel",
            .doc = "Show a panel by its id, or hide it with hide. With no id, returns every panel id.",
            .positionalDoc = "panel",
            .flags = {{.name = "hide", .shortName = "h", .type = ArgType::Bool, .doc = "Hide the panel instead."}},
            .undoable = false,
            .run = [](const Args& a) -> json {
                if (a.positional.empty())
                    return panels::registry().idsInOrder();
                const std::string id = a.positional[0].is_string() ? a.positional[0].get<std::string>() : std::string();
                if (!panels::registry().contains(id))
                    throw CommandError("showPanel: no panel '" + id + "'");
                if (a.get<bool>("hide"))
                {
                    if (const QDockWidget* dock = findDock(QString::fromStdString(id)); dock && dock->isVisible())
                        panels::toggle(id);
                }
                else
                    panels::show(id);
                return nullptr;
            },
        });
    actions::add({.id = "window.apiReference", .label = "API Reference", .category = "Windows", .run = [] { toggleDockByName(kApiReferenceDock); },
                  .doc = "Searchable reference for every command and the lila Python package.",
                  .python = "lila.ui.run_action(\"window.apiReference\")"});
    actions::add({.id = "window.scriptEditor", .label = "Script Editor", .category = "Windows", .run = [] { toggleDockByName(kScriptEditorDock); },
                  .doc = "Python editor with tabs, run selection and the API reference a key away.",
                  .python = "lila.ui.run_action(\"window.scriptEditor\")"});
    actions::add({.id = "window.terminal", .label = "Terminal", .category = "Windows", .run = [] { toggleDockByName(kTerminalDock); },
                  .doc = "Shell in the workspace folder, with Lila's Python on PATH.",
                  .python = "lila.ui.run_action(\"window.terminal\")"});
    actions::add({.id = "window.referenceEditor", .label = "Reference Editor", .category = "Windows", .run = [] { toggleDockByName(kReferenceEditorDock); },
                  .doc = "Lists the scene's references: load, unload, reload, import or remove them. Again to tuck it away.",
                  .python = "lila.ui.run_action(\"window.referenceEditor\")"});
    actions::add({.id = "window.displayLayers", .label = "Display Layers", .category = "Windows", .run = [] { toggleDockByName(kDisplayLayersDock); },
                  .doc = "Display layer editor: show, hide, template or reference groups of objects together.",
                  .python = "lila.ui.run_action(\"window.displayLayers\")"});
    registerDisplayLayerActions();
    actions::add({.id = "window.undoHistory", .label = "Undo History", .category = "Windows", .run = [] { toggleDockByName(kUndoHistoryDock); },
                  .doc = "Lists the undo steps; click one to undo or redo to it.",
                  .python = "lila.ui.run_action(\"window.undoHistory\")"});
}
} // namespace lila::ui
