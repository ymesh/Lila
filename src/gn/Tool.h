#pragma once

#include "core/Export.h"

#include "geo/Mesh.h"
#include "gn/Group.h"
#include "scene/Graph.h"

#include <glm/glm.hpp>

#include <array>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * Node tools and gizmos. A group flagged as a tool (Group::tool) runs on the selected meshes as a one-off edit, reading the
 * component selection, the pointer and the view through its tool nodes (Selection, Mouse Position, Viewport Transform,
 * Active Element) and writing the selection back with Set Selection. Gizmo nodes (Linear, Dial, Transform Gizmo) put
 * handles in the viewport that drive the group input linked into their value, on a modifier or a running tool.
 */
namespace lila::gn
{
/** What a group running as a tool reads about the view and pointer. */
struct ToolContext
{
    /** The pointer in viewport pixels from the top-left, and the viewport's size. */
    glm::dvec2 mouse{0.0};
    glm::dvec2 region{0.0};
    /** The pointer's ray in the target's own space. */
    glm::dvec3 rayOrigin{0.0};
    glm::dvec3 rayDirection{0.0, 0.0, -1.0};
    /** World to camera, and camera to clip. */
    glm::dmat4 view{1.0};
    glm::dmat4 projection{1.0};
    bool orthographic = false;
    /** The last picked point, edge and face of the target; -1 for none. */
    std::array<int64_t, 3> active{-1, -1, -1};
};

/** One handle a gizmo node recorded while its group ran. Positions and directions are in the group's own (object) space. */
struct GizmoSpec
{
    enum class Kind : uint8_t
    {
        Linear,
        Dial,
        Transform,
    };
    Kind kind = Kind::Linear;
    std::string node;
    /** The group input linked into the gizmo's value, which a drag edits. */
    std::string param;
    SocketType paramType = SocketType::Float;
    /** The input's subtype ("angle" keeps degrees on the modifier while the graph reads radians). */
    std::string subtype;
    /** The value as the graph read it (radians for angles), for Linear and Dial. */
    double value = 0.0;
    /** Transform's current matrix (a vector input is its translation). */
    glm::dmat4 matrix{1.0};
    glm::dvec3 position{0.0};
    /** Linear: the axis a drag moves along; one unit of value per unit of its length. Dial: the ring's axis. */
    glm::dvec3 direction{0.0, 1.0, 0.0};
    /** Linear: arrow, box or cross. */
    std::string style = "arrow";
    /** Dial: the ring's size, 1 being the default screen size. */
    double radius = 1.0;
    /** Transform: which handles it shows. */
    bool translate = true, rotate = true, scale = false;
};
using GizmoList = std::vector<GizmoSpec>;

LILA_SCENE_API bool isGizmoType(std::string_view type);

/** The gizmos a gn modifier's group recorded on its last evaluation; null when none. */
LILA_SCENE_API std::shared_ptr<const GizmoList> modifierGizmos(scene::NodeId modifier);
LILA_SCENE_API void setModifierGizmos(scene::NodeId modifier, std::shared_ptr<const GizmoList> gizmos);

/** Bool attributes carrying the component selection into a tool (Selection) and back out of it (Set Selection), per domain. */
LILA_SCENE_API const char* toolSelectionAttr(int domain);
LILA_SCENE_API const char* toolSetSelectionAttr(int domain);

/** What a tool made of one mesh. */
struct ToolResult
{
    geo::Mesh mesh;
    /** Per point, edge and face: the selection Set Selection wrote, or nullopt to keep the old one. */
    std::array<std::optional<std::vector<uint32_t>>, 3> selection;
    GizmoList gizmos;
};

/**
 * Runs a group as a tool on a mesh. selection holds the selected point, edge and face ids (null in object mode, where every
 * element counts as selected); params override the group inputs' defaults by name, in the values a modifier attr holds.
 * Throws ExecError.
 */
LILA_SCENE_API ToolResult runTool(const Group& g, const geo::Mesh& mesh, const std::array<std::vector<uint32_t>, 3>* selection, const ToolContext& ctx,
                                  const std::map<std::string, json>& params);

/** A scene mesh as a tool sees it: its base mesh (the cage), the selection in the current component mode, its context. */
struct ToolTarget
{
    scene::NodeId node = scene::kNoNode;
    geo::Mesh mesh;
    /** Point, edge and face ids; nullopt in object mode. */
    std::optional<std::array<std::vector<uint32_t>, 3>> selection;
    ToolContext ctx;
};

/** Fills a target for a mesh node: ctx is the view's context, with the world-space ray moved into the mesh's space and its active elements set. */
LILA_SCENE_API ToolTarget toolTarget(scene::Graph& g, scene::NodeId mesh, const ToolContext& ctx, const glm::dvec3& rayOrigin, const glm::dvec3& rayDirection);

/** Whether a tool reads the pointer (a Mouse Position node), so a preview follows it. */
LILA_SCENE_API bool readsPointer(const Group& g);

/** Groups in the scene flagged as tools. */
LILA_SCENE_API std::vector<std::string> toolGroups(scene::Graph& g);

/** gnSetTool, gnRunTool. */
void registerToolCommands();
} // namespace lila::gn
