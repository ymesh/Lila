// Stagehand: set dressing with physics and a prop library. Node types, commands, the Push tool and the Library panel.

#include "Stagehand.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    lila::physics::initJolt();
    if (!stagehand::registerNodes(ctx) || !stagehand::registerLibraryNodes(ctx) || !stagehand::registerArrangeNodes(ctx) ||
        !stagehand::registerWearNodes(ctx) || !stagehand::registerKitNodes(ctx) || !stagehand::registerUnrealNodes(ctx))
        return 1;
    stagehand::registerCommands(ctx);
    stagehand::registerLibraryCommands(ctx);
    stagehand::registerArrangeCommands(ctx);
    stagehand::registerPresetCommands(ctx);
    stagehand::registerKitCommands(ctx);
    stagehand::registerRealizeCommands(ctx);
    stagehand::registerUnrealExport(ctx);
    stagehand::registerUi(ctx);
    stagehand::registerLibraryUi(ctx);
    stagehand::registerKitUi(ctx);
    stagehand::registerPenetrationCheck(ctx);
    return 0;
}
