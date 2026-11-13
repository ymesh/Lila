#pragma once

#include "core/Export.h"

/**
 * IFC import (web-ifc): each building element a mesh (its colours in colorSet), the spatial structure (site, building,
 * storeys, spaces) as groups, every property set value as an attribute, and IfcMapConversion as the georeference.
 */
namespace lila::ifc
{
/** importIfc, and the .ifc file translator. */
LILA_SCENE_API void registerCommands();
} // namespace lila::ifc
