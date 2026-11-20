// Weathering: age, damage and overgrow anything, from the user's own pieces, in any style.

#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!weathering::registerMasks(ctx) || !weathering::registerEdges(ctx) || !weathering::registerIvy(ctx) ||
        !weathering::registerGenerators(ctx) || !weathering::registerGround(ctx) || !weathering::registerAge(ctx))
        return 1;
    weathering::registerPlaceholderCommands(ctx);
    return 0;
}
