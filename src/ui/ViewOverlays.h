#pragma once

#include "core/Export.h"
#include "core/ExtensionContext.h"

#include <string>
#include <string_view>
#include <vector>

/**
 * Named overlays a viewport draws through viewport drawers (zones, a world remix's regions...), each with a per-view
 * show toggle in the viewport's Overlays > Show section and the action "viewport.overlay.<id>". Hidden views are
 * remembered in the general pref /viewport/overlays/<id>/hidden, a list of view names, like image planes.
 */
namespace lila::ui::viewOverlays
{
struct Overlay
{
    std::string id;
    /** The noun shown after "Show" ("Zones"). */
    std::string label;
    std::string doc;
    /** The viewport drawer ids (viewportHooks::addDrawer) this overlay draws with; skipped in views that hide it. */
    std::vector<std::string> drawers;
    std::vector<std::string> keywords;
};

/** Registers the overlay and its toggle action, both owned by ctx. 0, with a log message, when the id is taken. */
LILA_UI_API ext::Context::Handle add(ext::Context& ctx, Overlay overlay);
/** For Lila's own overlays; adding an id again replaces it. */
LILA_UI_API void add(Overlay overlay);
LILA_UI_API bool remove(std::string_view id);

/** Every overlay, in the order added. */
LILA_UI_API std::vector<Overlay> all();
LILA_UI_API std::string actionId(std::string_view id);

LILA_UI_API bool shown(std::string_view id, const std::string& view);
LILA_UI_API void setShown(std::string_view id, const std::string& view, bool on);
/** False when the drawer belongs to an overlay hidden in view. */
LILA_UI_API bool drawerShown(std::string_view drawer, const std::string& view);
} // namespace lila::ui::viewOverlays
