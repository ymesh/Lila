#pragma once

#include "core/Export.h"

#include "geo/Heightfield.h"
#include "gn/Group.h"
#include "scene/Graph.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

/**
 * Geometry nodes: Blender-style node groups that run as modifiers in a mesh's stack.
 *
 * A group lives in the scene on a "gnGroup" node (its definition is a JSON string attr, so scenes save it and edits
 * undo). Each group registers a modifier node type "gn_<Group>" (versioned "gn_<Group>_v2"... when its interface
 * changes) with one keyable attr per exposed input. Instances get the definition through a connection from the
 * group node, so editing the group re-evaluates every mesh using it.
 */
namespace lila::gn
{
inline constexpr const char* kGroupNodeType = "gnGroup";

/** Node types (gnGroup) and the scene listener that registers modifier types as groups appear. Call after the scene's builtins. */
LILA_SCENE_API void registerTypes();
/** gnCreateGroup, gnAddNode, gnConnect, ... */
LILA_SCENE_API void registerCommands();

LILA_SCENE_API scene::Node* findGroupNode(scene::Graph& g, std::string_view group);
LILA_SCENE_API std::vector<std::string> groupNames(scene::Graph& g);
/** Throws std::invalid_argument when the definition is unreadable. */
LILA_SCENE_API Group readGroup(const scene::Node& groupNode);
/** Modifier node type name for the group's current interface version. */
LILA_SCENE_API std::string typeNameFor(const Group& g);
/** Modifiers using a group. */
LILA_SCENE_API std::vector<scene::NodeId> instancesOf(scene::Graph& g, scene::NodeId groupNode);
LILA_SCENE_API bool isGnModifier(const scene::Node& n);
/**
 * Drops the step results a gn modifier keeps between evaluations and marks it dirty, so every node runs again: for
 * nodes that read state no input carries (a style library, a tool's cutaway). False when it is not a gn modifier.
 */
LILA_SCENE_API bool rerunModifier(scene::Graph& g, scene::NodeId modifier);
/** The heightfield a gn modifier's group produced on its last evaluation; null when it made none or failed. */
LILA_SCENE_API std::shared_ptr<const geo::Heightfield> modifierHeightfield(scene::NodeId modifier);
/** Everything a gn modifier's group produced on its last evaluation (volumes, curves, instances unrealized); null when none. */
LILA_SCENE_API GeometryPtr modifierGeometry(scene::NodeId modifier);
/** modifierGeometry for every gn modifier holding one. */
LILA_SCENE_API std::vector<std::pair<scene::NodeId, GeometryPtr>> modifierGeometries();
/** The group node feeding a gn modifier, or null. */
LILA_SCENE_API scene::Node* groupOfModifier(scene::Graph& g, const scene::Node& modifier);

/**
 * Writes a changed definition back undoably: bumps the type version when the interface changed, drops links to
 * sockets that no longer exist, then migrates and reconnects the group's modifiers.
 */
LILA_SCENE_API void commitGroup(scene::Graph& g, scene::Node& groupNode, Group next);
/** commitGroup where renames maps old exposed-input names to new ones, so modifiers keep those values and animation. */
LILA_SCENE_API void commitGroup(scene::Graph& g, scene::Node& groupNode, Group next, const std::map<std::string, std::string>& renames);
/**
 * Renames a group: its node, its modifiers' type (keeping their values and connections) and the Group nodes in other
 * groups that run it. Undoable inside a command. Throws std::invalid_argument when the name is taken or invalid.
 */
LILA_SCENE_API void renameGroup(scene::Graph& g, scene::Node& groupNode, const std::string& to);

/** User library (prefs) and workspace library folders, user first. Missing folders are skipped. */
LILA_SCENE_API std::vector<std::filesystem::path> libraryDirs();
} // namespace lila::gn
