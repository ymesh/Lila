#pragma once

#include "core/Export.h"

#include <array>
#include <string>
#include <string_view>

/**
 * The scene's dependency graph as node-editor groups: each group is a view of real scene nodes around the selection,
 * edited through the sceneGraph* commands (sceneGraphInfo, sceneGraphNodeTypes, sceneGraphAddNode, sceneGraphDeleteNode,
 * sceneGraphConnect, sceneGraphDisconnect, sceneGraphMoveNodes, sceneGraphSetValue, sceneGraphSetNodeFlags,
 * sceneGraphSetFrames, sceneGraphSetTies, sceneGraphSetBookmarks, sceneGraphRenameNode, sceneGraphCopyNodes,
 * sceneGraphPasteNodes, sceneGraphDuplicateNodes, sceneGraphGroupNodes, sceneGraphUngroup).
 *
 * Node groups exist only in the node editor: a named set of scene nodes kept with the scene, drawn as one node of
 * kGroupNodeType (named "@" and the group's name) wherever its members show, and opened as a group of its own.
 */
namespace lila::scene::sceneGraph
{
/** The views, in the order the group picker lists them. */
inline constexpr std::array<const char*, 5> kViews = {"Selection", "History", "Hierarchy", "Upstream", "Downstream"};

/** Node type of the node standing for a node group; its "group" prop names the group. */
inline constexpr const char* kGroupNodeType = "sceneNodeGroup";

/** Whether name is one of kViews. */
LILA_SCENE_API bool isView(std::string_view name);
/** Whether the scene has a node group of that name. */
LILA_SCENE_API bool hasGroup(const std::string& name);
/** The node group a node group is nested in; empty for a top-level one or none. */
LILA_SCENE_API std::string parentGroup(const std::string& name);

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
