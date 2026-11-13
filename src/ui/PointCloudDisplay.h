#pragma once

#include "core/Export.h"
#include "render/FrameState.h"
#include "scene/Plug.h"
#include "ui/ViewInfo.h"

#include <QPointF>

#include <optional>

namespace lila::scene
{
class Graph;
}

/**
 * Point clouds in the viewport: per view, the octree nodes a cloud needs at this distance (largest on screen first, until
 * the cloud's point budget), loaded from the cache off the main thread and kept in memory while they are drawn.
 */
namespace lila::ui::pointCloudDisplay
{
/**
 * Adds the frame's point cloud draws. heightPx is the view's height in device pixels. complete waits (up to 30 s) for every
 * node the view needs, for renders and captures; a live view draws what has loaded and redraws as the rest arrives.
 */
LILA_UI_API void draw(render::FrameState& s, const ViewInfo& view, double heightPx, scene::Graph& g, const std::function<bool(scene::NodeId)>& shown,
                      const std::function<glm::vec3(scene::NodeId)>& tint, bool complete = false);

/** The loaded point of the cloud nearest px on screen within radiusPx, world space. */
LILA_UI_API std::optional<glm::dvec3> nearestPoint(scene::Graph& g, scene::NodeId cloud, const ViewInfo& view, QPointF px, double radiusPx);

/** Nodes loading now and bytes held in memory, for the HUD and tests. */
LILA_UI_API std::pair<size_t, size_t> streaming();

/** The pointCloudClip box drawing and picking. */
LILA_UI_API void registerUi();
} // namespace lila::ui::pointCloudDisplay
