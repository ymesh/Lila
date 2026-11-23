#pragma once

#include "core/Export.h"

#include "scene/Node.h"
#include "vcolor/Paint.h"

#include <string>

/**
 * Vertex color commands on scene meshes. Every edit lands in the mesh's editable base (under its modifiers) and is one
 * undo step that stores only the corners it changed.
 *
 * Commands: polyColorSet, vertexColorStroke, vertexColorFill, vertexColorGradient, vertexColorAdjust, vertexColorBlur,
 * vertexColorBake, vertexColorSample, getVertexColorData, setVertexColorData.
 */
namespace lila::vcolor
{
/** The set a command works on: the named one, else the current one; created white (and made current) when missing. */
LILA_SCENE_API std::string resolveSet(geo::Mesh& mesh, const std::string& requested, bool* created = nullptr);

/** The mesh node's symmetry (its symmetry setting, matched by position), empty when off. */
LILA_SCENE_API geo::SymmetryMap symmetryOf(const scene::Node& meshNode, const geo::Mesh& mesh);

/**
 * Stores a mesh whose color set was edited as the node's base and records the delta as one undo step labelled label.
 * A set the edit created makes the step a whole-mesh one, so undo removes the set again. Returns the stored mesh.
 */
LILA_SCENE_API scene::MeshPtr commit(scene::NodeId mesh, std::shared_ptr<geo::Mesh> edited, const std::string& set, ColorDelta delta, bool created,
                      const std::string& label);

/** Writes colors into the base without undo, for live previews under modifiers. The final commit records the step. */
LILA_SCENE_API scene::MeshPtr preview(scene::NodeId mesh, const std::string& set, std::span<const glm::vec4> colors);

/** A stroke the viewport tool already applied, adopted by the vertexColorStroke call that passes the returned token. */
LILA_SCENE_API uint64_t stashLiveStroke(scene::NodeId mesh, std::string set, ColorDelta delta);

/** Hands a per-face-vertex buffer (Python's numpy array) to the setVertexColorData call that passes the returned token. */
LILA_SCENE_API uint64_t stashValues(std::vector<glm::vec4> values);

/** Face-vertex order used by the data commands and Python: faces in order, each face's corners around it. */
LILA_SCENE_API std::vector<geo::Index> cornerOrder(const geo::Mesh& mesh);

LILA_SCENE_API void registerCommands();
/** vertexColorMasks; registerCommands calls it. */
void registerMaskCommands();
} // namespace lila::vcolor
