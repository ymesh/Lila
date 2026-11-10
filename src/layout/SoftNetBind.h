#pragma once

#include "core/Export.h"

#include "deform/SoftNet.h"
#include "layout/SoftNet.h"
#include "scene/Plug.h"

#include <map>
#include <span>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
class Node;
}

/** Soft Net internals shared by its command files: the stored bindings and the net's node lookups. */
namespace lila::layout
{
/** The knots group's bind: each knot's rig on the body. */
struct LILA_SCENE_API NetBind
{
    std::vector<deform::softnet::KnotRig> knots;
    nlohmann::json toJson() const;
    static NetBind fromJson(const std::string& text);
};

/** One bound mesh's bind: its samples, the lines (runs) they lie on, and each tangent handle's knot and rest place in its knot's frame. */
struct LILA_SCENE_API MeshBind
{
    size_t points = 0;
    size_t knots = 0;
    double edge = 1.0;
    double autoReach = 1.0;
    std::vector<deform::softnet::Sample> samples;
    std::vector<deform::softnet::Run> runs;
    std::vector<int> tangentKnot;
    std::vector<glm::dvec3> tangentRest;
    nlohmann::json toJson() const;
    static MeshBind fromJson(const std::string& text);
};

/**
 * The degrees of freedom's moves in a mesh's object space: each knot's origin carried by its control, each handle's
 * place carried by its knot's control and its own offset. frames are in the body's object space; toMesh carries them
 * into this mesh's. knots and tangents are the controls' local matrices by index (missing is identity).
 */
LILA_SCENE_API std::vector<glm::dvec3> dofMoves(const MeshBind& b, std::span<const glm::dmat4> frames, const std::map<int, glm::dmat4>& knots,
                                                const std::map<int, glm::dmat4>& tangents, const glm::dmat4& toMesh);

/** The net's deformers, body first. */
LILA_SCENE_API std::vector<scene::NodeId> netDeformers(scene::Graph& g, scene::NodeId knotsNode);
/** The meshes the net binds, body first. */
LILA_SCENE_API std::vector<scene::NodeId> netMeshes(scene::Graph& g, scene::NodeId knotsNode);
/** The knots group of the net a node belongs to (the group, a knot, a handle, a deformer, or a mesh it deforms), or kNoNode. */
LILA_SCENE_API scene::NodeId netOf(scene::Graph& g, scene::NodeId node);

/** Draws a closed loop and spokes out from it on a Panel Layout (panelLayoutLoopSpokes, the Layout Pencil's Loop with Spokes). */
LILA_SCENE_API nlohmann::json loopWithSpokes(scene::Graph& g, scene::Node& layoutNode, const std::vector<glm::dvec3>& loop, int count, double spacing,
                                             double length, double cornerBias, double tolerance);

void registerSoftNetBindCommands();
void registerSoftNetPoseCommands();
void registerSoftNetLayoutsCommands();
void registerSoftNetExportCommands();
} // namespace lila::layout
