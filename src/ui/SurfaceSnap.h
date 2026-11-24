#pragma once

#include "core/Export.h"

#include "scene/Plug.h"
#include "ui/ViewInfo.h"

#include <glm/glm.hpp>

#include <QPointF>

#include <functional>
#include <optional>
#include <vector>

/**
 * Sliding things along mesh surfaces: raycasts and closest points against the visible meshes' evaluated geometry, one
 * cached BVH per mesh snapshot. The snap surface is a session designation of meshes that, while set, are the only ones
 * surface snapping hits and that moved components conform to.
 */
namespace lila::ui::surfaceSnap
{
struct Hit
{
    glm::dvec3 position{0.0};
    /** World space, unit length, facing the side the ray came from (for underCursor). */
    glm::dvec3 normal{0.0, 1.0, 0.0};
    scene::NodeId node = scene::kNoNode;
};

/** The visible mesh surface under the viewport pixel, world space. Meshes in exclude are skipped; if a snap surface is set (the persistent designation), only those meshes are hit. */
LILA_UI_API std::optional<Hit> underCursor(QPointF px, const ViewInfo& view, const std::vector<scene::NodeId>& exclude = {});
/** underCursor for a world-space ray. */
LILA_UI_API std::optional<Hit> raycast(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<scene::NodeId>& exclude = {});
/** The first visible mesh a world-space ray hits, ignoring the snap surface designation. */
LILA_UI_API std::optional<Hit> firstHit(const glm::dvec3& origin, const glm::dvec3& dir, const std::vector<scene::NodeId>& exclude = {});
/** Closest point on the snap surfaces (or, if none set, on the given fallback meshes) to a world point. */
LILA_UI_API std::optional<Hit> closest(const glm::dvec3& world, const std::vector<scene::NodeId>& fallback = {});
/** Closest point on exactly these meshes (visible ones only), whatever the snap surface. */
LILA_UI_API std::optional<Hit> closestAmong(const glm::dvec3& world, const std::vector<scene::NodeId>& meshes);

/** The designated snap surface meshes that still exist. */
LILA_UI_API std::vector<scene::NodeId> surfaces();
LILA_UI_API bool active();
LILA_UI_API bool isSurface(scene::NodeId id);
/** Replaces the designation; non-mesh ids are dropped. */
LILA_UI_API void setSurfaces(std::vector<scene::NodeId> ids);
LILA_UI_API void clear();
/** Called after the designation changes; returns an id for removeListener. */
LILA_UI_API int addListener(std::function<void()> fn);
LILA_UI_API void removeListener(int id);

/** /manipulator/snap/surface/alignToNormal: objects slid along a surface turn their up axis to its normal. */
LILA_UI_API bool alignToNormal();

/**
 * The rotation that turns up onto normal by the shortest arc, applied on top of rotation (a world rotation matrix),
 * for aligning an object's up axis to a surface. Both directions unit length.
 */
LILA_UI_API glm::dmat3 alignUp(const glm::dmat3& rotation, const glm::dvec3& up, const glm::dvec3& normal);

/** The setSnapSurface, clearSnapSurface and snapSurface commands and the snapSurface.* actions. Safe to call twice. */
LILA_UI_API void registerCommands();
} // namespace lila::ui::surfaceSnap
