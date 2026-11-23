#pragma once

#include "core/Export.h"

/**
 * Texture graphs as files. A `.lilamat` is one texture set (its graph and settings):
 *   { "format": "lilamat", "version": 1, "name", "graph": {...}, "settings": {...} }
 * A `.lilatexnodes` file is a node library (NodeDef.h): the set's node groups, each as a kind "graph" node.
 */
namespace lila::tex
{
/** texExportGraph and texImportGraph, plus the node group commands. */
LILA_SCENE_API void registerFileCommands();
/** texGroupNodes, texUngroup, texSaveGroup and the group interface commands (texAddInput, texRenameInput, ...). */
LILA_SCENE_API void registerGroupCommands();
} // namespace lila::tex
