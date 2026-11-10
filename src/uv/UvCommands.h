#pragma once

#include "core/Export.h"
#include "scene/Plug.h"
#include "uv/Project.h"

#include <glm/glm.hpp>

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::uv
{
/** Registers the uv* commands (uvUnwrap, uvProject, uvAuto, uvPack, ...). */
LILA_SCENE_API void registerCommands();

/** Registers the live solver commands (uvLiveRelax, uvPin, uvCut, uvSew, ...); called by registerCommands. */
LILA_SCENE_API void registerLiveCommands();

/** Registers BFF, optimise, mosaic, symmetric unwrap and edge straighten/align commands; called by registerCommands. */
LILA_SCENE_API void registerOptimizeCommands();

/** Every mesh at or under the roots (each root, then its descendants), each once, in that order. */
LILA_SCENE_API std::vector<scene::NodeId> meshesUnder(const scene::Graph& g, std::span<const scene::NodeId> roots);

/**
 * Meshes the uv editor shows and whole-mesh uv tools work on: those at or under the object selection (only the
 * selected meshes themselves when includeChildren is off). With nothing selected, the meshes with components selected
 * in the current component mode.
 */
LILA_SCENE_API std::vector<scene::NodeId> shownMeshes();

/** Whether meshes under the selected objects count as selected for uv display and tools. A pref, on by default. */
LILA_SCENE_API bool includeChildren();
LILA_SCENE_API void setIncludeChildren(bool on);

/** A mesh's frame from the last uvProject, for the projection manipulator. */
struct ProjectionRecord
{
    scene::NodeId node = scene::kNoNode;
    Projection type = Projection::Planar;
    ProjectFrame frame; // object space
    glm::dmat4 world{1.0};
    glm::dvec2 scale{1.0};
    glm::dvec2 offset{0.0};
};

/** One per mesh the last uvProject ran on, in the order they were projected. */
LILA_SCENE_API const std::vector<ProjectionRecord>& lastProjections();

/** World direction the active view looks along; uvProject reads it for the seam when no viewDir is given. Set by the UI. */
LILA_SCENE_API std::function<std::optional<glm::dvec3>()>& projectionViewProvider();

/** World to clip (Vulkan) of the active view; uvProject type=camera uses it when given no camera or matrix. Set by the UI. */
LILA_SCENE_API std::function<std::optional<glm::dmat4>()>& projectionCameraProvider();
} // namespace lila::uv
