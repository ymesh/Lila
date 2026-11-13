#pragma once

#include "core/Export.h"

/**
 * 3D Tiles import (1.0 and 1.1, local folders): tileset.json and the tilesets it links, with b3dm, i3dm, pnts, cmpt, glb
 * and glTF content. Tiles are picked by geometric error and an optional radius, placed from the tileset's Earth-centred
 * frame into the scene's georeference (set from the tileset when the scene has none); meshes come in through the glTF
 * importer, point tiles as point clouds.
 */
namespace lila::tiles
{
/** import3dTiles, and the file translator for single tiles. */
LILA_SCENE_API void registerCommands();
} // namespace lila::tiles
