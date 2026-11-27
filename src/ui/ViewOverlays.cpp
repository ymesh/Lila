#include "ui/ViewOverlays.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "core/Registry.h"
#include "ui/Actions.h"
#include "ui/CameraBookmarks.h"
#include "ui/ViewportTool.h"
#include "ui/ViewportWidget.h"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace lila::ui::viewOverlays
{
using json = nlohmann::json;

namespace
{
Registry<Overlay>& registry()
{
    static Registry<Overlay> r;
    return r;
}

std::string hiddenKey(std::string_view id) { return "/viewport/overlays/" + std::string(id) + "/hidden"; }

Action actionFor(const Overlay& o)
{
    const std::string id = o.id;
    std::vector<std::string> keywords = o.keywords;
    keywords.push_back("overlay");
    return {.id = actionId(id),
            .label = "Show " + o.label,
            .category = "Display",
            .run =
                [id] {
                    if (ViewportWidget* v = cameraBookmarks::activeView())
                    {
                        setShown(id, v->viewName(), !shown(id, v->viewName()));
                        viewportTools::requestRedraw();
                    }
                },
            .doc = o.doc + " Per view: off hides it in the active view only.",
            .python = "lila.prefs.set(\"general\", \"" + hiddenKey(id) + "\", [\"persp\"])",
            .checked =
                [id] {
                    ViewportWidget* v = cameraBookmarks::activeView();
                    return !v || shown(id, v->viewName());
                },
            .keywords = std::move(keywords)};
}
} // namespace

std::string actionId(std::string_view id) { return "viewport.overlay." + std::string(id); }

ext::Context::Handle add(ext::Context& ctx, Overlay overlay)
{
    try
    {
        Action a = actionFor(overlay);
        std::string id = overlay.id;
        ext::Context::Handle h = ctx.add(registry(), "viewOverlay", id, std::move(overlay));
        actions::add(ctx, std::move(a));
        return h;
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: view overlay: {}", ctx.owner(), e.what());
        return 0;
    }
}

void add(Overlay overlay)
{
    actions::add(actionFor(overlay));
    std::string id = overlay.id;
    registry().put(std::move(id), std::move(overlay));
}

bool remove(std::string_view id)
{
    actions::remove(actionId(id));
    return registry().remove(id);
}

std::vector<Overlay> all()
{
    std::vector<Overlay> out;
    for (const std::string& id : registry().idsInOrder())
        out.push_back(*registry().find(id));
    return out;
}

bool shown(std::string_view id, const std::string& view)
{
    const json hidden = PrefStore::get().value("general", hiddenKey(id), json::array());
    return !hidden.is_array() || std::ranges::find(hidden, json(view)) == hidden.end();
}

void setShown(std::string_view id, const std::string& view, bool on)
{
    const json hidden = PrefStore::get().value("general", hiddenKey(id), json::array());
    json out = json::array();
    if (hidden.is_array())
        for (const json& v : hidden)
            if (v != json(view))
                out.push_back(v);
    if (!on)
        out.push_back(view);
    PrefStore::get().set("general", hiddenKey(id), out);
}

bool drawerShown(std::string_view drawer, const std::string& view)
{
    for (const std::string& id : registry().idsInOrder())
    {
        const Overlay* o = registry().find(id);
        if (std::ranges::find(o->drawers, drawer) != o->drawers.end() && !shown(id, view))
            return false;
    }
    return true;
}
} // namespace lila::ui::viewOverlays
