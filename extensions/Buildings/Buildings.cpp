// Buildings: the extension's entry point. Slot types, the style library, the Buildings geometry node, commands, check,
// UI, the agent guide and the Unreal side.

#include "BuildingsScene.h"

#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Slots.h>

namespace buildings
{
namespace
{
void registerSlotTypes(lila::ext::Context& ctx)
{
    for (const json& row : slotTypeRows())
    {
        lila::slots::SlotType t;
        t.id = row[0].get<std::string>();
        if (lila::slots::types().find(t.id))
            continue;
        t.label = row[1].get<std::string>();
        t.footprint = {row[2][0].get<double>(), row[2][1].get<double>(), row[2][2].get<double>()};
        t.attach = lila::slots::parseAttach(row[3].get<std::string>()).value_or(lila::slots::Attach::Wall);
        t.placeholder = row[4].get<std::string>();
        t.tags = row[5].get<std::vector<std::string>>();
        t.doc = row[6].get<std::string>();
        lila::slots::registerType(ctx, std::move(t));
    }
}
} // namespace
} // namespace buildings

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    buildings::registerSlotTypes(ctx);
    buildings::loadStyles(ctx.resources() / "styles");
    if (buildings::allStyles().empty())
        lila::log::warn("Buildings: no styles found in {}", (ctx.resources() / "styles").generic_string());
    buildings::setShippedTrimSheet(ctx.resources() / "trims" / "LilaBuilding.lilatrim");
    buildings::registerNode(ctx);
    buildings::registerCache(ctx);
    buildings::registerCommands(ctx);
    buildings::registerCheck(ctx);
    buildings::registerInterior(ctx);
    buildings::registerUi(ctx);
    buildings::registerAgent(ctx);
    buildings::registerUnreal(ctx);
    buildings::registerLiveRefresh(ctx);
    buildings::registerViewLod(ctx);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&) { buildings::unregisterLiveRefresh(); }
