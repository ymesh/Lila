// Blockwork: grid-first level blocking. Node types and commands; the Blockwork tool and panels are their UI.

#include "Blockwork.h"
#include "Entities.h"
#include "Tool.h"

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!blockwork::registerTypes(ctx))
        return 1;
    blockwork::registerCommands(ctx);
    blockwork::registerTextureCommands(ctx);
    blockwork::registerEntities(ctx);
    blockwork::registerOrganise(ctx);
    blockwork::registerUi(ctx);
    return 0;
}
