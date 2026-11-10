#pragma once

#include "core/Export.h"

#include <QString>

#include <functional>

class QMenuBar;
class QWidget;

/**
 * Searching menus by typing. In any open QMenu, typing filters its rows (and submenus with a match inside) as you
 * type; Backspace edits, Esc clears, Enter runs the highlighted row. The search box finds any item in the main menu
 * bar by name or by its menu path, or any scene node.
 */
namespace lila::ui::menuSearch
{
/** A QMenu property: when true, the menu search leaves the menu and its submenus out. */
inline constexpr const char* kNoSearchProperty = "lilaNoSearch";
/** A QMenu property: text the menu search appends to the menu's checkable rows, such as " Layout". */
inline constexpr const char* kSearchSuffixProperty = "lilaSearchSuffix";

/** Type-to-filter for every QMenu in the app. */
LILA_UI_API void install();

/**
 * Opens the search box over the window, listing every item reachable from the menu bar. Shift+Enter, or the square
 * at a row's right, opens the item's option box. With the box already open in scene mode it switches to menus in
 * place; already showing menus, it closes.
 */
LILA_UI_API void openBox(QMenuBar* bar, QWidget* over);

/**
 * The same box listing every node the outliner shows, by full path, matched as the outliner filter does plus fuzzy
 * letters in order. Enter selects, Shift adds, Ctrl toggles, and the outliners reveal it. Switches an open box the
 * same way openBox does.
 */
LILA_UI_API void openSceneFinder(QMenuBar* bar, QWidget* over);

/**
 * The same box listing every menu and titled menu section reachable from the menu bar. Enter tears the pick off under
 * the cursor, and the next click drops it (tearoff::tearOffToPlace). Switches an open box the same way openBox does.
 */
LILA_UI_API void openTearOffSearch(QMenuBar* bar, QWidget* over);

/** Where the scripting API mode sends its picks: the script editor that opened it. */
struct ApiTarget
{
    /** Source to insert at the cursor; ends in "(" for a call. */
    std::function<void(const QString& code)> insert;
    /** Full docs for an ApiSymbol key. */
    std::function<void(const QString& key)> showDocs;
};

/**
 * The same box listing the scripting API (apiSymbols): commands, their flags and the lila Python package, matched on
 * name and summary. Enter inserts the call, Shift+Enter or the book at a row's right shows its docs. The box's tabs
 * switch to menus or the scene and back.
 */
LILA_UI_API void openApiSearch(QMenuBar* bar, QWidget* over, ApiTarget target);
} // namespace lila::ui::menuSearch
