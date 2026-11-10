#pragma once

#include "core/Export.h"
#include "core/Registry.h"
#include "ui/Owner.h"

#include <QObject>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

class QAction;
class QMenu;
class QWidget;

/**
 * The menu bar and the hotbox's panel menus, as registered menus and items (Maya's menu/menuItem). Lila's own are
 * registered with owner kLilaOwner; an extension adds items into any menu by path, or menus of its own, and they go
 * when it unloads. The main window builds QMenus from the registry and rebuilds them whenever it changes.
 */
namespace lila::ui::menus
{
enum class Area
{
    MenuBar,
    Hotbox, // a viewport panel menu: Lila's views have no menu bar of their own, so these live in the hotbox
};

struct Menu
{
    std::string id;     // "Mesh": the first part of its items' paths
    std::string title;  // shown, with an & accelerator if wanted; the id when empty
    Area area = Area::MenuBar;
    std::string before; // a menu id to sit in front of; empty goes last
    /** Run once on the QMenu when it is created: show-time fills, check marks. */
    std::function<void(QMenu*)> setup;
    /** A docs page ("curves") whose "Open <Page> Docs" row ends the menu; empty for none. */
    std::string docs;
    /**
     * The menu sets ("Model", "Rig", "Animate", "Level"; menuContext::sets() lists them all) whose menu bar shows this menu.
     * Empty shows it in every set. The hotbox and menu search always reach every menu.
     */
    std::vector<std::string> sets;
};

enum class Kind
{
    Action,
    Separator,
    Section, // a titled separator heading the rows after it; label is the title
    Submenu, // a cascade; items under it use the path "<menu>/<label>"
    Custom,  // rows the item adds itself, each time its menu is built
};

struct Item
{
    std::string id;   // unique; addItem makes one from the path and action or label when empty
    std::string menu; // "Mesh", or "Mesh/Symmetry" for a submenu's items
    Kind kind = Kind::Action;
    std::string action; // Kind::Action: the action id
    std::string label;  // Kind::Submenu or Kind::Section: its title
    /** An item id in the same menu to follow or precede; empty appends. */
    std::string after;
    std::string before;
    /** Kind::Submenu: run once when its QMenu is created. Kind::Custom: adds rows to the menu being built. */
    std::function<void(QMenu*)> setup;
};

/** Items under this path show in the time slider's context menu while a range is selected on it. */
inline constexpr const char* kTimeSliderRange = "TimeSliderRange";

LILA_UI_API Registry<Menu>& menuRegistry();
LILA_UI_API Registry<Item>& itemRegistry();

LILA_UI_API void addMenu(Menu m, std::string owner = kLilaOwner);
/** Returns the item's id. */
LILA_UI_API std::string addItem(Item item, std::string owner = kLilaOwner);
/** Action items in order; "" adds a separator and "#Title" a section. */
LILA_UI_API void addActions(const std::string& menu, std::initializer_list<const char*> actionIds, std::string owner = kLilaOwner);
LILA_UI_API std::string addSubmenu(const std::string& menu, const std::string& label, std::function<void(QMenu*)> setup = {},
                                   std::string owner = kLilaOwner);

/** Owned by the extension and removed when it unloads. 0 when the id is taken. */
LILA_UI_API uint64_t addMenu(ext::Context& ctx, Menu m);
LILA_UI_API uint64_t addItem(ext::Context& ctx, Item item);

/** Removes the menu and every item under it. */
LILA_UI_API bool removeMenu(const std::string& id);
LILA_UI_API bool removeItem(const std::string& id);

/** The menus of an area in display order. */
LILA_UI_API std::vector<const Menu*> menusIn(Area area);
/** The items directly in a menu path, in display order (after and before applied). */
LILA_UI_API std::vector<const Item*> itemsIn(const std::string& path);

/** A row that runs the action, labelled and tooltipped from it. */
LILA_UI_API QAction* addActionRow(QMenu* menu, const std::string& actionId);

/**
 * Keeps QMenus in step with the registry. The QMenu of a menu or submenu path lives as long as the path is registered,
 * so tear-offs and the hotbox keep working across rebuilds; only its rows are rebuilt.
 */
class LILA_UI_API MenuBuilder : public QObject
{
    Q_OBJECT

public:
    /** decorate runs once per new QMenu (topLevel for a menu, false for a submenu). */
    MenuBuilder(QWidget* parent, std::function<void(QMenu*, bool topLevel)> decorate);
    ~MenuBuilder() override;

    /** The top-level QMenus of an area in order, building them if needed. */
    std::vector<QMenu*> menus(Area area);
    QMenu* menu(const std::string& path) const;

    void rebuild();
    /** Rebuilds once the event loop comes round, however many changes arrive first. */
    void scheduleRebuild();

Q_SIGNALS:
    void rebuilt();

private:
    void fill(QMenu* menu, const std::string& path, std::vector<std::string>& live);

    QWidget* mParent;
    std::function<void(QMenu*, bool)> mDecorate;
    std::map<std::string, QMenu*> mMenus; // by path
    int mMenuListener = 0;
    int mItemListener = 0;
    bool mScheduled = false;
};
} // namespace lila::ui::menus
