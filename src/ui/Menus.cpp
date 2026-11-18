#include "ui/Menus.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "ui/Actions.h"
#include "ui/Arrange.h"
#include "ui/DocsUi.h"
#include "ui/FeatureStatus.h"
#include "ui/Icons.h"
#include "ui/Shelf.h"

#include <QAction>
#include <QMenu>
#include <QPointer>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <set>

namespace lila::ui::menus
{
namespace
{
constexpr const char* kBuilt = "lilaMenuBuilt";

std::string uniqueItemId(std::string base)
{
    if (!itemRegistry().contains(base))
        return base;
    for (int n = 2;; ++n)
        if (std::string id = base + "#" + std::to_string(n); !itemRegistry().contains(id))
            return id;
}

std::string defaultId(const Item& item)
{
    switch (item.kind)
    {
    case Kind::Action: return item.menu + "/" + item.action;
    case Kind::Submenu: return item.menu + "/" + item.label + "/";
    case Kind::Separator: return item.menu + "/-";
    case Kind::Section: return item.menu + "/#" + item.label;
    case Kind::Custom: return item.menu + "/*";
    }
    return item.menu;
}
} // namespace

Registry<Menu>& menuRegistry()
{
    static Registry<Menu> r;
    return r;
}

Registry<Item>& itemRegistry()
{
    static Registry<Item> r;
    return r;
}

void addMenu(Menu m, std::string owner)
{
    if (m.title.empty())
        m.title = m.id;
    if (m.before.empty() && owner != kLilaOwner && m.area == Area::MenuBar)
        m.before = "Help";
    if (!m.docs.empty())
        docs::pageAction(m.docs);
    std::string id = m.id;
    menuRegistry().put(std::move(id), std::move(m), std::move(owner));
}

std::string addItem(Item item, std::string owner)
{
    if (item.id.empty())
        item.id = uniqueItemId(defaultId(item));
    std::string id = item.id;
    itemRegistry().put(id, std::move(item), std::move(owner));
    return id;
}

void addActions(const std::string& menu, std::initializer_list<const char*> actionIds, std::string owner)
{
    for (const char* id : actionIds)
    {
        if (*id == '#')
            addItem({.menu = menu, .kind = Kind::Section, .label = id + 1}, owner);
        else
            addItem({.menu = menu, .kind = *id ? Kind::Action : Kind::Separator, .action = id}, owner);
    }
}

std::string addSubmenu(const std::string& menu, const std::string& label, std::function<void(QMenu*)> setup, std::string owner)
{
    return addItem({.menu = menu, .kind = Kind::Submenu, .label = label, .setup = std::move(setup)}, std::move(owner));
}

uint64_t addMenu(ext::Context& ctx, Menu m)
{
    if (menuRegistry().contains(m.id))
    {
        ctx.log(LogLevel::Error, "menu '" + m.id + "' already exists");
        return 0;
    }
    std::string id = m.id;
    addMenu(std::move(m), ctx.owner());
    return ctx.track("menu:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = menuRegistry().ownerOf(id); o && *o == own)
            removeMenu(id);
    });
}

uint64_t addItem(ext::Context& ctx, Item item)
{
    if (!item.id.empty() && itemRegistry().contains(item.id))
    {
        ctx.log(LogLevel::Error, "menu item '" + item.id + "' already exists");
        return 0;
    }
    std::string id = addItem(std::move(item), ctx.owner());
    return ctx.track("menuItem:" + id, [id, own = ctx.owner()] {
        if (const std::string* o = itemRegistry().ownerOf(id); o && *o == own)
            itemRegistry().remove(id);
    });
}

bool removeMenu(const std::string& id)
{
    if (!menuRegistry().contains(id))
        return false;
    std::vector<std::string> under;
    for (Item* item : itemRegistry().all())
        if (item->menu == id || item->menu.starts_with(id + "/"))
            under.push_back(item->id);
    for (const std::string& i : under)
        itemRegistry().remove(i);
    return menuRegistry().remove(id);
}

bool removeItem(const std::string& id)
{
    return itemRegistry().remove(id);
}

/** Lila's own first, so an extension that registered before the main window built its menus still comes after them. */
template <class T> void lilaFirst(std::vector<T*>& v, const Registry<T>& registry)
{
    std::stable_partition(v.begin(), v.end(), [&](T* t) {
        const std::string* owner = registry.ownerOf(t->id);
        return owner && *owner == kLilaOwner;
    });
}

std::vector<const Menu*> menusIn(Area area)
{
    std::vector<Menu*> all;
    for (Menu* m : menuRegistry().allInOrder())
        if (m->area == area)
            all.push_back(m);
    lilaFirst(all, menuRegistry());
    std::vector<Menu*> ordered = arrange(all, [](Menu* m) { return m->id; },
                                         [](Menu* m) { return std::pair<std::string, std::string>{{}, m->before}; });
    return {ordered.begin(), ordered.end()};
}

std::vector<const Item*> itemsIn(const std::string& path)
{
    std::vector<Item*> all;
    for (Item* i : itemRegistry().allInOrder())
        if (i->menu == path)
            all.push_back(i);
    lilaFirst(all, itemRegistry());
    std::vector<Item*> ordered = arrange(all, [](Item* i) { return i->id; },
                                         [](Item* i) { return std::pair<std::string, std::string>{i->after, i->before}; });
    return {ordered.begin(), ordered.end()};
}

/** "Edit Mesh > Bevel": the titles of the menus the row sits in, then its label. */
std::string menuPath(QMenu* menu, const std::string& id)
{
    QStringList parts;
    for (QWidget* w = menu; w; w = w->parentWidget())
        if (auto* m = qobject_cast<QMenu*>(w); m && !m->title().isEmpty())
            parts.prepend(QString(m->title()).remove('&'));
    parts << QString::fromStdString(actions::displayLabel(id));
    return parts.join(" > ").toStdString();
}

namespace
{
/** The status every action row directly in a menu shows, or Ready when they differ or there are none. */
features::Status sharedStatus(const std::vector<const Item*>& items, const std::string& path)
{
    features::Status shared = features::Status::Ready;
    bool any = false;
    for (const Item* item : items)
    {
        if (item->kind != Kind::Action)
            continue;
        const features::Status s = featureStatus::rowStatus(item->action, path);
        if (any && s != shared)
            return features::Status::Ready;
        shared = s;
        any = true;
    }
    return shared;
}

QString statusTip(features::Status s)
{
    return s == features::Status::NotReady ? "Not ready for use yet: expect broken tools and lost work. Help > Roadmap shows what is ready."
                                           : "Usable, with bugs to expect. Help > Roadmap shows what is ready.";
}

/** "  (in development)", after a row's label. */
QString statusSuffix(features::Status s)
{
    return "  (" + QString(features::label(s)).toLower() + ")";
}
} // namespace

QAction* addActionRow(QMenu* menu, const std::string& id)
{
    const Action* a = actions::find(id);
    QAction* qa = menu->addAction(QString::fromStdString(a ? a->label : id));
    qa->setData(QString::fromStdString(id));
    qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
    qa->setIcon(icon(QString::fromStdString(actions::iconFor(id))));
    qa->setStatusTip(QString::fromStdString(actions::statusLine(id)));
    // Hotkeys can be rebound while the menu exists, so the tooltip naming one is refreshed with the check state.
    menu->setToolTipsVisible(true);
    auto syncCheck = [qa, id] {
        qa->setToolTip(QString::fromStdString(actions::tooltip(id)));
        qa->setStatusTip(QString::fromStdString(actions::statusLine(id)));
        const Action* now = actions::find(id);
        qa->setCheckable(now && now->checked);
        if (qa->isCheckable())
            qa->setChecked(now->checked());
    };
    syncCheck();
    QObject::connect(menu, &QMenu::aboutToShow, qa, syncCheck);
    QObject::connect(qa, &QAction::triggered, menu, [id, menu = QPointer<QMenu>(menu)] {
        if (shelves::addModifierHeld())
            shelves::addToCurrent(id);
        else
        {
            actions::ViaScope via(actions::Via::Menu, menuPath(menu, id));
            actions::run(id);
        }
    });
    return qa;
}

MenuBuilder::MenuBuilder(QWidget* parent, std::function<void(QMenu*, bool)> decorate)
    : QObject(parent), mParent(parent), mDecorate(std::move(decorate))
{
    QPointer<MenuBuilder> self(this);
    auto changed = [self](RegistryChange, const std::string&, const std::string&) {
        if (self)
            self->scheduleRebuild();
    };
    mMenuListener = menuRegistry().addListener(changed);
    mItemListener = itemRegistry().addListener(changed);
}

MenuBuilder::~MenuBuilder()
{
    menuRegistry().removeListener(mMenuListener);
    itemRegistry().removeListener(mItemListener);
}

std::vector<QMenu*> MenuBuilder::menus(Area area)
{
    if (mMenus.empty())
        rebuild();
    std::vector<QMenu*> out;
    for (const Menu* m : menusIn(area))
        if (QMenu* q = menu(m->id))
            out.push_back(q);
    return out;
}

QMenu* MenuBuilder::menu(const std::string& path) const
{
    auto it = mMenus.find(path);
    return it == mMenus.end() ? nullptr : it->second;
}

void MenuBuilder::scheduleRebuild()
{
    if (mScheduled)
        return;
    mScheduled = true;
    QTimer::singleShot(0, this, [this] {
        mScheduled = false;
        rebuild();
    });
}

void MenuBuilder::rebuild()
{
    std::vector<std::string> live;
    for (const Menu* m : menuRegistry().allInOrder())
    {
        QMenu*& q = mMenus[m->id];
        if (!q)
        {
            q = new QMenu(QString::fromStdString(m->title), mParent);
            if (mDecorate)
                mDecorate(q, true);
            if (m->setup)
                m->setup(q);
        }
        q->setTitle(QString::fromStdString(m->title));
        live.push_back(m->id);
        fill(q, m->id, live, features::Status::Ready);
        if (!m->docs.empty())
        {
            q->addSeparator()->setProperty(kBuilt, true);
            addActionRow(q, docs::pageAction(m->docs))->setProperty(kBuilt, true);
        }
    }
    std::set<std::string> keep(live.begin(), live.end());
    for (auto it = mMenus.begin(); it != mMenus.end();)
    {
        if (keep.contains(it->first))
        {
            ++it;
            continue;
        }
        it->second->deleteLater();
        it = mMenus.erase(it);
    }
    Q_EMIT rebuilt();
}

void MenuBuilder::fill(QMenu* menu, const std::string& path, std::vector<std::string>& live, features::Status shownAbove)
{
    // Rows a submenu's setup added stay; only what the registry put here is rebuilt.
    for (QAction* a : menu->actions())
        if (a->property(kBuilt).toBool())
        {
            menu->removeAction(a);
            if (a->parent() == menu)
                delete a;
        }
    qsizetype before = menu->actions().size();
    std::vector<const Item*> items = itemsIn(path);
    bool sectioned = std::ranges::any_of(items, [](const Item* i) { return i->kind == Kind::Section; });
    const features::Status shared = sharedStatus(items, path);
    if (featureStatus::marked(shared) && shared != shownAbove)
    {
        QAction* note = menu->addAction(features::label(shared));
        note->setEnabled(false);
        note->setToolTip(statusTip(shared));
        menu->addSeparator();
    }
    bool lilaBefore = false;
    for (const Item* item : items)
    {
        const std::string* owner = itemRegistry().ownerOf(item->id);
        bool lila = owner && *owner == kLilaOwner;
        // Extension rows would otherwise read as part of whichever of Lila's sections ends the menu. Rows right under a
        // section heading were placed there on purpose, as in the Level menu, and so were user menus.
        bool placed = owner && *owner == kUserMenusOwner;
        if (sectioned && lilaBefore && !lila && !placed && item->kind != Kind::Section && item->kind != Kind::Separator)
            menu->addSection("Extensions");
        lilaBefore = lila && item->kind != Kind::Section;
        switch (item->kind)
        {
        case Kind::Action:
        {
            QAction* row = addActionRow(menu, item->action);
            // Only where the menu's rows differ: a menu whose rows share a status says it once, at the top.
            if (features::Status s = featureStatus::rowStatus(item->action, path); featureStatus::marked(s) && !featureStatus::marked(shared))
            {
                row->setProperty(kStatusSuffix, statusSuffix(s));
                row->setText(row->text() + statusSuffix(s));
            }
            break;
        }
        case Kind::Separator: menu->addSeparator(); break;
        case Kind::Section: menu->addSection(QString::fromStdString(item->label)); break;
        case Kind::Custom:
            if (item->setup)
                item->setup(menu);
            break;
        case Kind::Submenu:
        {
            std::string sub = path + "/" + item->label;
            QMenu*& q = mMenus[sub];
            if (!q)
            {
                q = new QMenu(QString::fromStdString(item->label), menu);
                if (mDecorate)
                    mDecorate(q, false);
                if (item->setup)
                    item->setup(q);
            }
            menu->addMenu(q);
            live.push_back(sub);
            fill(q, sub, live, featureStatus::marked(shared) ? shared : features::Status::Ready);
            break;
        }
        }
    }
    QList<QAction*> rows = menu->actions();
    for (qsizetype i = before; i < rows.size(); ++i)
        rows[i]->setProperty(kBuilt, true);
}
} // namespace lila::ui::menus
