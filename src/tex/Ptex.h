#pragma once

#include "core/Export.h"

/**
 * Ptex (.ptx) for texture sets on quad meshes: texExportPtex writes a set channel as per-face texels sampled through the
 * mesh's uvs, texImportPtex bakes a file's faces back through the uvs into a fill layer.
 */
namespace lila::tex
{
LILA_SCENE_API void registerPtexCommands();
} // namespace lila::tex
