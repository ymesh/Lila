// Terrain: heightfield terrain as geometry nodes, with commands that build and read it and their UI.

#include "Analysis.h"
#include "CliffScene.h"
#include "Coast.h"
#include "ExportEngines.h"
#include "Http.h"
#include "Roads.h"
#include "LandformCommands.h"
#include "MaterialNodes.h"
#include "Query.h"
#include "Spec.h"
#include "Wedge.h"
#include "Terrain.h"
#include "Volume.h"

#include <lila/Events.h>
#include <lila/Extension.h>
#include <lila/ExtensionScope.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    lila::ui::scope(ctx);
    if (!terrain::registerNodes(ctx) || !terrain::registerVolume(ctx) || !terrain::registerLandformNodes(ctx) || !terrain::registerInputs(ctx)
        || !terrain::registerSurfaceNodes(ctx) || !terrain::registerQuery(ctx) || !terrain::cliffs::registerCliffNodes(ctx))
        return 1;
    terrain::registerCommands(ctx);
    terrain::registerSurfaceCommands(ctx);
    terrain::registerExportEngines(ctx);
    terrain::registerWater(ctx);
    terrain::registerCoast(ctx);
    terrain::registerUi(ctx);
    terrain::registerCoastUi(ctx);
    terrain::registerSurfaceUi(ctx);
    if (!terrain::registerRoads(ctx))
        return 1;
    terrain::cliffs::registerCliffCommands(ctx);
    terrain::cliffs::registerCliffUi(ctx);
    terrain::registerRegionRefs(ctx);
    terrain::registerSpec(ctx);
    terrain::registerWedge(ctx);
    // What the replaced scene's modifiers published (heightfields, volumes) goes with it.
    for (const lila::events::Event e : {lila::events::Event::SceneNew, lila::events::Event::SceneOpened})
        lila::events::subscribe(
            e,
            [](const lila::events::EventData&) {
                terrain::cliffs::forgetGoneCliffs();
                terrain::forgetGoneVolumeResults();
                terrain::forgetAnalyses();
            },
            "Terrain");
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&)
{
    terrain::httpShutdown();
}
