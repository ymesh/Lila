#pragma once

#include "core/Export.h"

/** The bendBones deformer and bendBoneCV node (deform/BendBones.h does the maths), with their commands. */
namespace lila::scene::bendbones
{
LILA_SCENE_API void registerTypes();
LILA_SCENE_API void registerCommands();
} // namespace lila::scene::bendbones
