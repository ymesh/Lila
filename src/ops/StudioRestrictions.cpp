#include "ops/StudioRestrictions.h"

#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PrefStore.h"
#include "ops/Command.h"

#include <algorithm>
#include <stdexcept>

namespace lila::restrictions
{
using json = nlohmann::json;

namespace
{
void validate(const std::string& id, const Restriction& r)
{
    if (id.empty() || r.label.empty())
        throw std::invalid_argument("restriction '" + id + "' needs an id and a label");
    if (r.file.empty() || !r.key.starts_with('/'))
        throw std::invalid_argument("restriction '" + id + "' needs a pref file and a key starting with '/'");
}
} // namespace

Registry<Restriction>& registry()
{
    static Registry<Restriction> r;
    return r;
}

void add(std::string id, Restriction r, std::string owner)
{
    validate(id, r);
    registry().put(std::move(id), std::move(r), std::move(owner));
}

bool add(ext::Context& ctx, std::string id, Restriction r)
{
    try
    {
        validate(id, r);
        ctx.add(registry(), "studioRestriction", std::move(id), std::move(r));
        return true;
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: studio restriction: {}", ctx.owner(), e.what());
        return false;
    }
}

bool locked(std::string_view id)
{
    const Restriction* r = registry().find(id);
    return r && PrefStore::get().isLocked(r->file, r->key);
}

json value(std::string_view id)
{
    const Restriction* r = registry().find(id);
    return r ? PrefStore::get().value(r->file, r->key) : json();
}

json describe()
{
    std::vector<std::string> ids = registry().ids();
    std::ranges::sort(ids);
    json out = json::array();
    for (const std::string& id : ids)
    {
        const Restriction& r = *registry().find(id);
        const std::string* owner = registry().ownerOf(id);
        out.push_back({{"id", id},
                       {"label", r.label},
                       {"doc", r.doc},
                       {"file", r.file},
                       {"key", r.key},
                       {"owner", owner ? *owner : std::string()},
                       {"locked", locked(id)},
                       {"value", value(id)}});
    }
    return out;
}

void registerBuiltins()
{
    const std::string lila = "lila";
    add("extensions.disabled", {"Disabled extensions", "Extensions that stay off: users can't turn them back on.", "general", "/extensions/disabled"}, lila);
    add("extensions.autoload", {"Loaded extensions", "Extensions loaded at startup.", "general", "/extensions/autoload"}, lila);
    add("extensions.paths", {"Extension folders", "Where Lila looks for extensions beyond its own.", "general", "/extensions/paths"}, lila);
    add("telemetry.usage", {"Usage statistics", "Whether anonymous usage statistics are sent. Lock to false to keep them off.", "general", "/telemetry/usage"}, lila);
    add("telemetry.crashSend",
        {"Crash reports", "\"auto\" sends them, \"ask\" offers Send in the crash dialog, \"off\" never sends. Lock to \"off\" to keep them off.",
         "general", "/telemetry/crashSend"},
        lila);
    add("network.updateCheck", {"Update check", "Whether Lila checks GitHub for a newer version at startup.", "general", "/update/check"}, lila);
    add("network.docsUpdate", {"Docs download", "Whether Lila downloads docs updates at startup.", "general", "/docs/autoUpdate"}, lila);
    add("network.news", {"Welcome news", "Whether the welcome dialog downloads news from the Lila site.", "general", "/startup/news"}, lila);
    add("files.registerScenes", {"Scene file types", "Whether .lila and .lilaa open with Lila from Explorer.", "general", "/files/registerScenes"}, lila);
    add("files.registerImports", {"Import file types", "Import formats that open with Lila from Explorer.", "general", "/files/registerImports"}, lila);

    using T = ArgType;
    ops::registerCommand({
        .name = "studioRestrictions",
        .doc = "The prefs a studio can lock, from Lila and its extensions: id, label, doc, the pref (file and key) a studio "
               "folder's \"$locked\" list names, the owner, whether it is locked now and the value that applies. Any pref "
               "can be locked; these are the ones documented for it. With an id, just that one.",
        .positionalDoc = "[id]",
        .flags = {{"lockedOnly", "lo", T::Bool, false, "Only the restrictions a studio folder locks now."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json all = describe();
            if (!a.positional.empty() && a.positional[0].is_string())
            {
                for (const json& r : all)
                    if (r["id"] == a.positional[0])
                        return r;
                throw CommandError("studioRestrictions: no restriction '" + a.positional[0].get<std::string>() + "'");
            }
            if (a.get<bool>("lockedOnly", false))
            {
                json out = json::array();
                for (const json& r : all)
                    if (r["locked"].get<bool>())
                        out.push_back(r);
                return out;
            }
            return all;
        },
    });
}
} // namespace lila::restrictions
