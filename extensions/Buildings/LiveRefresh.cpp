// Buildings follow their settlement: after a command edits a settlement whose buildings were made by the buildings route,
// the stale groups are rebuilt (buildingRefresh) as an undo step of their own, so the town never shows old buildings.

#include "BuildingsScene.h"

#include <lila/Command.h>
#include <lila/Log.h>
#include <lila/Undo.h>

#include <set>
#include <string>

using namespace lila;
using json = nlohmann::json;

namespace buildings
{
namespace
{
int gListener = -1;

/** Settlement commands that read, export or make the buildings themselves: nothing to follow after them. */
bool editsSettlement(std::string_view name)
{
    static const std::set<std::string, std::less<>> kSkip = {"settlementBuildings", "settlementCreate", "settlementFromOsm", "settlementDelete",
                                                             "settlementSendToUnreal", "settlementExport", "settlementRoadProfileFromUnreal",
                                                             "settlementRoadProfileToUnreal"};
    if (!name.starts_with("settlement") || kSkip.contains(name))
        return false;
    const CommandDef* def = ops::find(name);
    return def && def->undoable;
}

void followSettlement(const ExecuteEvent& e)
{
    static bool busy = false;
    if (busy || !e.done || !e.ok || ops::previewing() || UndoStack::get().replaying() || !editsSettlement(e.name))
        return;
    busy = true;
    struct Done
    {
        ~Done() { busy = false; }
    } done;
    try
    {
        json call = json::object();
        if (e.call.contains("args") && e.call["args"].is_array() && !e.call["args"].empty() && e.call["args"][0].is_string())
            call["args"] = json::array({e.call["args"][0]});
        json dry = call;
        dry["dryRun"] = true;
        if (ops::execute("buildingRefresh", dry)["stale"].empty())
            return;
        const json r = ops::execute("buildingRefresh", call);
        int n = 0;
        for (const json& g : r["refreshed"])
            n += g.value("buildings", 0);
        log::info("Buildings: {} building(s) rebuilt after {} (undo once to see the old ones)", n, e.name);
    }
    catch (const std::exception& ex)
    {
        log::warn("Buildings: not rebuilt after {} ({}); run Refresh Buildings", e.name, ex.what());
    }
}
} // namespace

void registerLiveRefresh(lila::ext::Context&)
{
    unregisterLiveRefresh();
    gListener = ops::addExecuteListener(followSettlement);
}

void unregisterLiveRefresh()
{
    if (gListener >= 0)
        ops::removeExecuteListener(gListener);
    gListener = -1;
}
} // namespace buildings
