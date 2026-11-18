// Terrain: heightfield terrain as geometry nodes, with commands that build and read it and their UI.

#include "CliffScene.h"
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
    terrain::registerUi(ctx);
    terrain::registerSurfaceUi(ctx);
    if (!terrain::registerRoads(ctx))
        return 1;
    terrain::cliffs::registerCliffCommands(ctx);
    terrain::cliffs::registerCliffUi(ctx);
    terrain::registerRegionRefs(ctx);
    terrain::registerSpec(ctx);
    terrain::registerWedge(ctx);
    return 0;
}

LILA_EXTENSION_EXPORT void lilaExtensionUninit(lila::ext::Context&)
{
    terrain::httpShutdown();
}
