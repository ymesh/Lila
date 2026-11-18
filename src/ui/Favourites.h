#pragma once

#include "core/Export.h"

#include <functional>
#include <string>
#include <vector>

class QAbstractItemView;
class QModelIndex;

/**
 * Favourites: items a user stars in a list (brush presets, smart materials, alphas, textures, picker rows), kept in prefs
 * per list and sorted to the top where they are listed.
 */
namespace lila::ui::favourites
{
LILA_UI_API bool has(const std::string& list, const std::string& id);
LILA_UI_API void set(const std::string& list, const std::string& id, bool on);
/** The list's favourites, in the order they were starred. */
LILA_UI_API std::vector<std::string> all(const std::string& list);

/**
 * A star on the view's items: filled on favourites, an outline on the hovered item, at a tile's top-right corner in icon
 * mode or a row's right end. Clicking it toggles the favourite and runs changed. listOf gives an item's list and id;
 * an empty list leaves the item without a star.
 */
LILA_UI_API void attach(QAbstractItemView* view, std::function<std::pair<std::string, std::string>(const QModelIndex&)> listOf,
                        std::function<void()> changed);
} // namespace lila::ui::favourites
