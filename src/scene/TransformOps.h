#pragma once

#include "core/Export.h"

#include "scene/Transform.h"

#include <glm/gtc/quaternion.hpp>

#include <vector>

namespace lila::scene
{
class Graph;

enum class Space : uint8_t
{
    World,
    Object,
    Parent,
};

/** Transform edits shared by the manipulators and the move/rotate/scale commands. All record undo through Graph. */
namespace xformops
{
/** Drops ids whose ancestor is also in the list, so a selected parent and child move once. */
LILA_SCENE_API std::vector<NodeId> topLevel(Graph& g, const std::vector<NodeId>& ids);

/** World position of the rotate pivot. */
LILA_SCENE_API glm::dvec3 worldPivot(Graph& g, NodeId id);

/** Orthonormal world-space axes (columns) for a space. */
LILA_SCENE_API glm::dmat3 frame(Graph& g, NodeId id, Space space);

LILA_SCENE_API void moveBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& delta, Space space = Space::World);

/** Rotates each object about its own pivot. `rotation` is in `space` (world, or each object's own frame). */
LILA_SCENE_API void rotateBy(Graph& g, const std::vector<NodeId>& ids, const glm::dquat& rotation, Space space = Space::World);

/**
 * World axes (columns, one per channel) the rotate channels turn about, in the object's rotate order: the outermost is the
 * parent's own, each inner one carried round by the outer angles. Two columns go parallel at gimbal lock.
 */
LILA_SCENE_API glm::dmat3 gimbalFrame(Graph& g, NodeId id);

/** Adds `degrees` to each object's rotate channels, so a turn about one gimbal axis changes that channel only. */
LILA_SCENE_API void rotateChannelsBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& degrees);

/** The rotate channels rotateBy would give one object, before its locked channels are kept. */
LILA_SCENE_API glm::dvec3 rotatedEuler(Graph& g, NodeId id, const glm::dquat& rotation, Space space = Space::World);

/** Multiplies local scale; scaling is always along the object's own axes. */
LILA_SCENE_API void scaleBy(Graph& g, const std::vector<NodeId>& ids, const glm::dvec3& factor);

/** Moves the rotate and scale pivots to a world point without moving the object. */
LILA_SCENE_API void setWorldPivot(Graph& g, NodeId id, const glm::dvec3& world);

/** World axes (columns) of the pivot: the object's axes turned by its pivotOrient. */
LILA_SCENE_API glm::dmat3 pivotFrame(Graph& g, NodeId id);

/** Sets pivotOrient so the pivot's world axes become `world` (orthonormal columns), without moving the object. */
LILA_SCENE_API void setWorldPivotFrame(Graph& g, NodeId id, const glm::dmat3& world);

/**
 * Applies an object-space matrix to the node's mesh points (its edit plug, unshared first) or provided points, so the
 * geometry can stay put in the world while the transform changes.
 */
LILA_SCENE_API void bakeIntoGeometry(Graph& g, NodeId id, const glm::dmat4& bake);

/** Zeroes pivots, their compensation and the pivot orientation, keeping the object where it is. */
LILA_SCENE_API void resetPivot(Graph& g, NodeId id);

/** Euler angles for a rotation that are closest to `hint`, so keys don't flip by 360. */
LILA_SCENE_API glm::dvec3 closestEuler(const glm::dmat3& rotation, RotateOrder order, const glm::dvec3& hint);
} // namespace xformops
} // namespace lila::scene
