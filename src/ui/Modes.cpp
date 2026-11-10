#include "ui/Modes.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ui/Actions.h"
#include "ui/Gizmo.h"
#include "ui/Hotkeys.h"
#include "ui/Layouts.h"
#include "ui/MarkingMenu.h"
#include "ui/SelectionHandlers.h"
#include "ui/Shelf.h"

#include <QComboBox>
#include <QPointer>

namespace lila::ui::modes
{
namespace
{
std::string& currentId()
{
    static std::string id;
    return id;
}

/** The layout in use before entering a mode that has its own, restored when switching to a mode without one. */
std::string& layoutBeforeModes()
{
    static std::string name;
    return name;
}

void applyLayout(const std::string& mode, const std::string& name)
{
    try
    {
        ops::execute("applyLayout", {{"name", name}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("mode {}: {}", mode, e.what());
    }
}

void call(const std::function<void()>& fn, const std::string& id, const char* what)
{
    if (!fn)
        return;
    try
    {
        fn();
    }
    catch (const std::exception& e)
    {
        log::error("mode {}: {} failed: {}", id, what, e.what());
    }
}

std::string actionId(const std::string& id)
{
    return "mode." + id;
}

std::string layerId(const std::string& id)
{
    return "mode." + id;
}

/** Steps through the modes in registration order. */
void step(int by)
{
    std::vector<std::string> ids = registry().idsInOrder();
    if (ids.empty())
        return;
    auto it = std::find(ids.begin(), ids.end(), current());
    int at = it == ids.end() ? 0 : int(it - ids.begin());
    int n = int(ids.size());
    activate(ids[size_t(((at + by) % n + n) % n)]);
}
} // namespace

Registry<Mode>& registry()
{
    static Registry<Mode> r;
    static int listener = r.addListener([](RegistryChange change, const std::string& id, const std::string&) {
        // A mode removed without going through remove() (an owner sweep) can't run its onExit any more.
        if (change == RegistryChange::Removed && id == currentId() && id != kDefaultMode)
        {
            currentId().clear();
            if (r.contains(kDefaultMode))
                activate(kDefaultMode, true);
        }
    });
    (void)listener;
    return r;
}

void add(Mode mode, std::string owner)
{
    if (mode.label.empty())
        mode.label = mode.id;
    std::string id = mode.id;
    actions::add({.id = actionId(id),
                  .label = mode.label + " Mode",
                  .category = "Modes",
                  .run = [id] { activate(id); },
                  .doc = mode.doc.empty() ? "Switches to the " + mode.label + " mode." : mode.doc,
                  .python = "lila.ui.set_mode(\"" + id + "\")"},
                 owner);
    if (mode.hotkeys.is_object() && !mode.hotkeys.empty())
        hotkeyLayers::add({layerId(id), mode.hotkeys, id}, owner);
    else
        hotkeyLayers::remove(layerId(id));
    registry().put(id, std::move(mode), std::move(owner));
}

uint64_t add(ext::Context& ctx, Mode mode)
{
    if (const std::string* owner = registry().ownerOf(mode.id); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "mode '" + mode.id + "' already belongs to " + *owner);
        return 0;
    }
    std::string id = mode.id;
    add(std::move(mode), ctx.owner());
    return ctx.track("mode:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = registry().ownerOf(id); o && *o == own)
            remove(id);
    });
}

bool remove(const std::string& id)
{
    if (!registry().contains(id))
        return false;
    if (current() == id && id != kDefaultMode)
        activate(kDefaultMode);
    const std::string* owner = registry().ownerOf(id);
    std::string own = owner ? *owner : std::string();
    if (const std::string* o = actions::registry().ownerOf(actionId(id)); o && *o == own)
        actions::remove(actionId(id));
    if (const std::string* o = hotkeyLayers::registry().ownerOf(layerId(id)); o && *o == own)
        hotkeyLayers::remove(layerId(id));
    return registry().remove(id);
}

bool activate(const std::string& id, bool restoring)
{
    const Mode* next = registry().find(id);
    if (!next)
        return false;
    if (id == currentId())
        return true;
    std::string oldGizmo, oldHandler, oldLayout;
    if (const Mode* old = registry().find(currentId()))
    {
        oldLayout = old->layout;
        oldGizmo = old->gizmoProvider;
        oldHandler = old->selectionHandler;
        call(old->onExit, old->id, "onExit");
    }
    next = registry().find(id);
    if (!next)
        return false;
    currentId() = id;
    // A mode without its own gizmo or handler only undoes the previous mode's, so a pref-enabled handler stays on.
    if (!next->gizmoProvider.empty())
        gizmos::setActive(next->gizmoProvider);
    else if (!oldGizmo.empty())
        gizmos::setActive("");
    if (!next->selectionHandler.empty())
        selectionHandlers::setActive(next->selectionHandler);
    else if (!oldHandler.empty() && selectionHandlers::activeId() == oldHandler)
        selectionHandlers::setActive("");
    PrefStore::get().set("general", "/ui/mode", id);
    Hotkeys::get().setMode(id);
    markingMenus::setActiveSet(next->markingMenuSet);
    if (!restoring)
    {
        if (!next->layout.empty())
        {
            if (oldLayout.empty())
                layoutBeforeModes() = layouts::current();
            applyLayout(id, next->layout);
        }
        // Only while the old mode's layout is still up: leaving because another layout was picked keeps that one.
        else if (!oldLayout.empty() && !layoutBeforeModes().empty() && layouts::current() == oldLayout)
            applyLayout(id, layoutBeforeModes());
    }
    if (!next->shelf.empty())
        shelves::setCurrent(next->shelf);
    // Copied: onEnter may re-register the mode, replacing what next points at.
    std::function<void()> enter = next->onEnter;
    call(enter, id, "onEnter");
    events::send({.type = events::Event::ModeChanged, .name = id});
    return true;
}

const std::string& current()
{
    return currentId();
}

const Mode* currentMode()
{
    return registry().find(currentId());
}

void registerBuiltins()
{
    static bool done = false;
    if (done)
        return;
    done = true;
    owners::OwnerScope lila({});
    if (!registry().contains(kDefaultMode))
        add({.id = kDefaultMode, .label = "Lila", .doc = "Switches to Lila's own mode: the default hotkeys, marking menus and gizmos."});
    actions::add({.id = "mode.next", .label = "Next Mode", .category = "Modes", .run = [] { step(1); },
                  .doc = "Switches to the next mode in the mode list.", .python = "lila.ui.run_action(\"mode.next\")"});
    actions::add({.id = "mode.previous", .label = "Previous Mode", .category = "Modes", .run = [] { step(-1); },
                  .doc = "Switches to the previous mode in the mode list.", .python = "lila.ui.run_action(\"mode.previous\")"});
    ops::registerCommand({
        .name = "mode",
        .doc = "Switch to a mode (a bundle of layout, hotkeys, marking menus, shelf and gizmos), or with no argument return the "
               "current one. list returns every mode as {id, label, doc}.",
        .positionalDoc = "mode",
        .flags = {{.name = "list", .shortName = "l", .type = ArgType::Bool, .doc = "Return every mode instead."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("list", false))
            {
                json out = json::array();
                for (const Mode* m : registry().allInOrder())
                    out.push_back({{"id", m->id}, {"label", m->label}, {"doc", m->doc}});
                return out;
            }
            if (a.positional.empty())
                return current();
            if (!a.positional[0].is_string())
                throw CommandError("mode: give a mode id");
            std::string id = a.positional[0].get<std::string>();
            if (!activate(id))
                throw CommandError("mode: no mode named '" + id + "'");
            return current();
        },
    });
}

void restore()
{
    std::string id = PrefStore::get().value<std::string>("general", "/ui/mode", kDefaultMode);
    if (!activate(id, true))
        activate(kDefaultMode, true);
}

QWidget* createSwitcher(QWidget* parent)
{
    auto* box = new QComboBox(parent);
    box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    box->setToolTip("Mode: the layout, hotkeys, marking menus and gizmos for a kind of work");
    box->setFocusPolicy(Qt::NoFocus);
    QPointer<QComboBox> self(box);
    auto refresh = [self] {
        if (!self)
            return;
        QSignalBlocker block(self);
        self->clear();
        for (const Mode* m : registry().allInOrder())
            self->addItem(QString::fromStdString(m->label), QString::fromStdString(m->id));
        self->setCurrentIndex(self->findData(QString::fromStdString(current())));
        // One mode has nothing to switch to.
        self->setVisible(self->count() > 1);
    };
    refresh();
    QObject::connect(box, &QComboBox::activated, box, [box](int index) {
        actions::run(json(actionId(box->itemData(index).toString().toStdString())));
    });
    int listener = registry().addListener([refresh](RegistryChange, const std::string&, const std::string&) { refresh(); });
    events::SubscriptionId sub = events::subscribe(events::Event::ModeChanged, [refresh](const events::EventData&) { refresh(); });
    QObject::connect(box, &QObject::destroyed, [listener, sub] {
        registry().removeListener(listener);
        events::unsubscribe(sub);
    });
    return box;
}
} // namespace lila::ui::modes
