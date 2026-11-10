#pragma once

#include "core/Export.h"

#include <array>
#include <string_view>

/**
 * The scene's dependency graph as node-editor groups: each group is a view of real scene nodes around the selection,
 * edited through the sceneGraph* commands (sceneGraphInfo, sceneGraphNodeTypes, sceneGraphAddNode, sceneGraphDeleteNode,
 * sceneGraphConnect, sceneGraphDisconnect, sceneGraphMoveNodes, sceneGraphSetValue, sceneGraphSetNodeFlags).
 */
namespace lila::scene::sceneGraph
{
/** The views, in the order the group picker lists them. */
inline constexpr std::array<const char*, 5> kViews = {"Selection", "History", "Hierarchy", "Upstream", "Downstream"};

/** Socket type of the hierarchy sockets the Hierarchy view adds: "@parent" in, "@children" out. */
inline constexpr const char* kDagSocketType = "dag";

/**
 * Whether an output of socket type `from` may feed an input of type `to`: numbers (and Vec3 components, which are floats)
 * feed each other, vectors and colours each other, everything else only its own kind. An array input ("float[]") takes
 * its element type.
 */
LILA_SCENE_API bool linkable(std::string_view from, std::string_view to);

LILA_SCENE_API void registerCommands();
} // namespace lila::scene::sceneGraph
