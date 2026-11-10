// Plating: plates and fasteners built on Panel Layouts. Node types and commands; the Layout Pencil is their UI.

#include "Plating.h"

#include <lila/Extension.h>

LILA_EXTENSION_EXPORT int lilaExtensionInit(lila::ext::Context& ctx)
{
    if (!plating::registerNodes(ctx) || !plating::registerFastenerNodes(ctx) || !plating::registerPatternNodes(ctx))
        return 1;
    plating::registerCommands(ctx);
    plating::registerFastenerCommands(ctx);
    plating::registerPatternCommands(ctx);
    return 0;
}
