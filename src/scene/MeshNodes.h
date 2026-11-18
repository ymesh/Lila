#pragma once

#include "core/Export.h"

#include "geo/Symmetry.h"
#include "scene/Transform.h"

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

namespace lila
{
struct Args;
}

namespace lila::scene
{
/**
 * A polygon mesh in the hierarchy. `inMesh` holds the base mesh (or is fed by a generator such as polyCube);
 * `outMesh` is what gets drawn and exported once modifiers run.
 */
class LILA_SCENE_API MeshNode : public Transform
{
public:
    enum Attr
    {
        kInMesh = Transform::kCount,
        kOutMesh,
        kSymmetry,
        kSmoothPreview,
        kRetopoDrawReference,
        kRetopoDrawDots,
        kSymmetryPlane,
        kSymmetryOffset,
        kRadialCount,
        kRadialAxis,
        kRadialDirection,
        kRadialMirror,
        kRadialPhase,
        kRadialCenter,
        kRetopoGuides,
        kRetopoConstructs,
        kDrawOffset,
        kCount
    };

    void compute(const EvalContext& ctx) override;
    static NodeType makeType(const NodeType& base);
};

/** Procedural source: keyable parameters in, a mesh out. One class serves every primitive type. */
class LILA_SCENE_API GeneratorNode : public Node
{
public:
    using Build = std::function<geo::Mesh(const Node&)>;

    void compute(const EvalContext& ctx) override;

    Build build;
    int outAttr = -1;
};

/** The `symmetry` attribute value for radial symmetry, after off, x, y and z. */
inline constexpr int64_t kSymmetryRadial = 4;

/** The `symmetry` attribute across a set of nodes, for toggles: how many are meshes, how many have it on, and on which axis. */
struct SymmetryState
{
    int meshes = 0;
    int on = 0;
    int axis = -1; // 0-2 (mirror) or kRadialState when every mesh that has it on agrees, else -1
    int radialCount = 0; // with axis kRadialState: the count when they all agree, else 0
    bool radialMirror = false;

    static constexpr int kRadialState = 3;
};
LILA_SCENE_API SymmetryState symmetryState(const Graph& g, const std::vector<NodeId>& nodes);

/** The mesh's drawOffset (cm toward the camera, display only), 0 for nodes that aren't meshes. */
inline float drawOffset(const Node& n)
{
    return n.type().isA("mesh") ? float(std::max(0.0, n.as<double>(MeshNode::kDrawOffset))) : 0.0f;
}

/**
 * Where a mesh's symmetry plane for axis (0-2) lies along that axis, in object space: through the object's pivot, or
 * at symmetryOffset when symmetryPlane is custom (set from an edge selection).
 */
LILA_SCENE_API double symmetryOffset(const Node& mesh, int axis);

/**
 * A mesh's symmetry setting in object space, nullopt when off: the mirror plane symmetry x/y/z sets, or the radial group
 * symmetry radial sets (radialCount turns about radialAxis through the pivot, or through radialCenter when
 * symmetryPlane is custom, with a mirror plane per wedge when radialMirror is on).
 */
LILA_SCENE_API std::optional<geo::SymmetrySpec> symmetrySpec(const Node& mesh);

/** symmetrySpec(), or with symmetry off the x plane through the pivot: where a brush's own mirror reflects. */
LILA_SCENE_API geo::SymmetrySpec paintSymmetrySpec(const Node& mesh);

/** The mesh's persistent symmetry map, stored onto mesh if it has none yet; nullopt when symmetry is off. */
LILA_SCENE_API std::optional<geo::SymmetryMap> symmetryMapFor(const Node& node, geo::Mesh& mesh, geo::SymmetrySync* sync = nullptr);

/** "x", "y", "z", or for radial symmetry e.g. "radial 6" / "radial 6 + mirror": for messages. */
LILA_SCENE_API std::string symmetryLabel(const geo::SymmetrySpec& spec);

/** World up as the scene's up axis pref sets it: +Y, or +Z in a Z-up scene. */
LILA_SCENE_API glm::dvec3 sceneUp();

/**
 * Stands a mesh on a surface point: its object space scene up (sceneUp) turned to normal (left alone when normal is
 * zero) and its origin on position, or, for a sphere primitive cut short by its fraction, its open base on position.
 */
LILA_SCENE_API void placePrimitive(Graph& g, NodeId mesh, const glm::dvec3& position, const glm::dvec3& normal);

/**
 * The meshes a whole-object command acts on: the positional objects (or the selection), each mesh as itself and each
 * other node (a group) as the meshes under it, without repeats. Selected non-meshes with no meshes under them are
 * skipped; a named one throws, as does an empty result.
 */
LILA_SCENE_API std::vector<std::string> meshObjects(const Args& a, const char* cmd);

/** Hands a point buffer (Python's numpy array) to the setMeshPoints call that passes the returned token. */
LILA_SCENE_API uint64_t stashPoints(std::vector<glm::dvec3> points);

LILA_SCENE_API void registerMeshTypes();
LILA_SCENE_API void registerMeshCommands();
/** importStl, exportStl, importPly, exportPly. */
LILA_SCENE_API void registerMeshFileCommands();
} // namespace lila::scene
