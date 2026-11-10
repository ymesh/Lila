#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "geo/Mesh.h"
#include "math/MathCore.h"
#include "scene/AnimCurve.h"
#include "scene/Light.h"
#include "scene/Plug.h"
#include "scene/Transform.h"

#include <glm/glm.hpp>

#include <array>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace lila::scene
{
class Graph;
}

namespace lila::ext
{
class Context;
}

/**
 * FBX import (through ufbx) and export (our own 7.4 binary writer).
 *
 * Axes. FBX files are right-handed. Export bakes one of three profiles into the data (no root rotation is left
 * behind) and declares it in GlobalSettings:
 *
 *   profile   file axes                         Lila semantic axes in the file
 *   ue5       Z up, front -Y, coord +X           +X forward, -Y right, +Z up
 *   ue6       Y up, front +Z, coord +X           +Z forward, -X right, +Y up
 *   generic   ue5's axes in a Z-up scene, ue6's in a Y-up scene, so Lila's own coordinates go out unchanged
 *
 * ue5 is exactly what Unreal's FBX importer expects: it reads Z-up right-handed and negates Y, so Lila forward,
 * right and up land on Unreal +X, +Y, +Z (math::toUnreal). It is also what Unreal's own exporter writes. ue6
 * assumes UE6 is left-handed Y-up with +Z forward and +X right (Unity's convention) and mirrors on import by
 * negating X, the way Unity treats Maya files; the file stays a standard right-handed Y-up FBX. If UE6 lands
 * differently only the ue6 row of the profile table needs to change.
 *
 * Import inverts the same mapping: a Z-up file is read as ue5 space and anything else as ue6 space. Files that
 * declare another front or right axis are first rotated onto those (as ufbx would), so Maya, Max, Blender and
 * Unreal files arrive without a stray rotation on their roots. Units are converted to cm.
 */
namespace lila::fbx
{
namespace fs = std::filesystem;

enum class AxisProfile
{
    Ue5,
    Ue6,
    Generic,
};

/** "ue5", "ue6", "generic" (case-insensitive). False for anything else. */
LILA_SCENE_API bool parseProfile(std::string_view name, AxisProfile* out);
LILA_SCENE_API const char* profileName(AxisProfile p);

/** Lila space to file space for a profile and scene up axis: a rotation (signed axis permutation). */
LILA_SCENE_API glm::dmat3 profileBasis(AxisProfile p, math::UpAxis up);
/**
 * The quarter turn about up that characters in Z-up (Unreal) files take on import: Unreal builds skeletal meshes facing
 * its +Y, which profileBasis puts on Lila's right, and Lila characters face forward. Skeletal and animation exports to a
 * Z-up profile turn them back by its inverse.
 */
LILA_SCENE_API glm::dmat3 characterTurn(math::UpAxis up);

/**
 * What an export is for. Static writes meshes and transforms. Skeletal adds joints as LimbNodes, skins (Skin and
 * Cluster deformers, BindPose) with skinned meshes in their bind pose, and the animation over the bake range.
 * Animation writes only the skeleton and its curves, no meshes, as Unreal wants for an anim sequence.
 * Sequence writes cameras and transforms with their animation and no meshes, for Unreal Sequencer's FBX import:
 * cameras arrive as CineCameras with their focal length keyed.
 */
enum class ExportKind
{
    Static,
    Skeletal,
    Animation,
    Sequence,
};

/** "static", "skeletal", "animation", "sequence" (case-insensitive). False for anything else. */
LILA_SCENE_API bool parseKind(std::string_view name, ExportKind* out);
LILA_SCENE_API const char* kindName(ExportKind k);

struct ExportOptions
{
    AxisProfile profile = AxisProfile::Ue5;
    ExportKind kind = ExportKind::Static;
    /** Skin influences per vertex: the heaviest are kept and renormalised. 8 suits desktop, 4 mobile. */
    int maxInfluences = 8;
    /** Write the take (skeletal and animation kinds only). */
    bool animation = true;
    /**
     * Sample every frame of the range. False writes the curves' own keys for nodes whose channels map one to one onto
     * FBX cubic keys (plain translate/rotate/scale curves, constant infinity); the rest are still sampled.
     */
    bool bakeAnimation = true;
    /** Bake range in seconds; NaN means the playback range. */
    double start = std::numeric_limits<double>::quiet_NaN();
    double end = std::numeric_limits<double>::quiet_NaN();
    /** AnimStack name; empty uses the active take's name, else "Take 001". */
    std::string takeName;
    /**
     * Scene takes to write, one AnimStack each, sampled over each take's own range and fps: names, or just "all".
     * Empty writes the animation connected now as one stack.
     */
    std::vector<std::string> takes;
    /** Triangles exactly as geo::triangulate makes them (the viewport's), so Unreal shades what Lila shows. */
    bool triangulate = true;
    /** MikkTSpace tangents and binormals from uv0. */
    bool tangents = true;
    /** Bake world matrices into the meshes and write every node at the top level. */
    bool worldSpace = false;
    /** Put the given roots at the origin: their own transform is dropped, their children stay relative to them. */
    bool atOrigin = false;
    /** The roots' transforms relative to this node rather than to the world (a USD stage's nodes, to its stage node). */
    scene::NodeId relativeTo = scene::kNoNode;
    /** Export the descendants of the given nodes too. */
    bool children = true;
    /** Roots written without their descendants, whatever children says. */
    std::set<scene::NodeId> childless;
    /**
     * Roots below a node other than relativeTo keep their own local transforms, as they would under their exported
     * parent, so a node written alone matches what a write of its whole hierarchy gives it.
     */
    bool localRoots = false;
    /** Write each edge's smoothing (hard or soft) as a smoothing layer. */
    bool smoothingGroups = true;
    /** Centimetres per file unit (100 writes metres): lengths are scaled and UnitScaleFactor declares it. */
    double unitScale = 1.0;
    /**
     * Skeletal and animation kinds: write deform joints only (their deform flag), closing the hierarchy over the rest,
     * so IK helpers and rig-only joints stay behind. A non-deform joint that carries skin weights still goes out.
     */
    bool deformOnly = true;
    /** Write the split normals; off leaves them for the importer to compute. */
    bool normals = true;
    /** Deflate the larger arrays (zlib, as the FBX SDK writes them); off writes every array raw. */
    bool compress = true;
    /**
     * Write the materials' colors and base color, normal and emissive maps as FBX file textures (with their wrap
     * modes). Off writes plain named materials, which Unreal binds to its own assets by name.
     */
    bool textures = false;
    /**
     * Static kind: write each mesh's simple collision from the collision sources (collisionSources) as UCX_<mesh>_NN
     * meshes beside it, which Unreal's importer turns into the static mesh's collision.
     */
    bool collision = false;
};

/** A material texture as an FBX file texture: its file and wrap modes (FBX has only repeat and clamp). */
struct ExportTexture
{
    std::string path; // absolute
    bool clampU = false, clampV = false;
};

/** What ExportOptions::textures writes of a material slot. */
struct ExportMaterial
{
    glm::dvec3 baseColor{0.5};
    glm::dvec3 emissive{0.0};
    std::optional<ExportTexture> baseColorMap, normalMap, emissiveMap;
};

/** A skin on an exported mesh, already pruned to the influence limit. Matrices are export space, Lila axes. */
struct ExportSkin
{
    struct Cluster
    {
        int joint = -1; // index into ExportScene::nodes
        glm::dmat4 bindWorld{1.0};
        std::vector<int32_t> vertices; // FBX control point indices
        std::vector<double> weights;
    };
    std::vector<Cluster> clusters;
    glm::dmat4 meshBindWorld{1.0};
    bool dualQuaternion = false;
};

/** Morph targets of an exported mesh (one BlendShape with a channel per target). Deltas are Lila space, per FBX vertex. */
struct ExportMorphs
{
    struct Target
    {
        std::string name;
        std::vector<int32_t> vertices; // FBX control point indices
        std::vector<glm::dvec3> deltas;
        double weight = 0.0; // current weight, 0 to 1
    };
    std::vector<Target> targets;
};

/** A camera's lens, film back and clipping. Lengths in cm, film back in inches, focal length in mm. */
struct ExportCamera
{
    double focalLength = 35.0;
    double filmWidth = 1.41732, filmHeight = 0.94488;
    double nearClip = 0.1, farClip = 10000.0;
    bool ortho = false;
    double orthoWidth = 30.0;
    int filmFit = 1; // scene::FilmFit

    bool operator==(const ExportCamera&) const = default;
};

struct ExportNode
{
    std::string name;
    int parent = -1; // index into ExportScene::nodes
    glm::dmat4 local{1.0}; // Lila space, relative to the parent
    scene::MeshPtr mesh;   // evaluated outMesh (a skinned mesh's bind shape in skeletal exports); null for an empty
    glm::dmat4 bake{1.0};  // applied to the mesh points (worldSpace)

    scene::NodeId source = scene::kNoNode;
    bool joint = false;
    /** jointOrient as a rotation, Lila space. Written as PreRotation, the way Maya writes joints. */
    glm::dmat4 pre{1.0};
    /** rotateAxis as a rotation, Lila space: applied before rotate, so it is written as the inverse PostRotation. */
    glm::dmat4 post{1.0};
    scene::RotateOrder rotateOrder = scene::RotateOrder::XYZ;
    /**
     * local is exactly T * pre * R(rotateOrder) * S of these channel values (no pivots, no segment scale
     * compensation), so the channels can be written one to one instead of decomposing the matrix.
     */
    bool plainTrs = false;
    glm::dvec3 translate{0.0}, rotate{0.0}, scale{1.0};
    /** World at bind time, export space; what BindPose and the clusters hold. */
    glm::dmat4 bindWorld{1.0};
    std::shared_ptr<const ExportSkin> skin;
    std::shared_ptr<const ExportMorphs> morphs;
    /** Written as an FBX camera, which looks down its local +X with +Y up where a Lila camera looks down -Z. */
    std::optional<ExportCamera> camera;
    /** Written as an FBX light, which shines down its local -Y with -Z up where a Lila light shines down -Z. */
    std::optional<scene::lights::Settings> light;
    /** An LOD group: written as an FbxLODGroup whose children are the levels. */
    bool lodGroup = false;
    /** Its Unreal screen size per level, written as screen percentage thresholds; empty leaves them to the importer. */
    std::vector<double> lodScreenSizes;
    /** A pinned prop's joint to attach to (scene/Pin.h), written as the string user property UnrealSocket. */
    std::string unrealSocket;
    /** The node's added attrs (attrComponents::exportProperties): FBX user properties, glTF node extras. */
    std::vector<std::pair<std::string, nlohmann::json>> properties;
};

/** Sampled (or keyed) animation of the exported nodes over one range. */
struct ExportTake
{
    std::string name;
    double fps = 30.0;
    double start = 0.0, end = 0.0; // seconds
    std::vector<double> times;
    /** [node][frame] local matrix, Lila space; empty for a node written from its curves or not animated. */
    std::vector<std::vector<glm::dmat4>> locals;
    /** [node] translate, rotate or scale has an input (keys, a constraint, a driver), even if it holds still. */
    std::vector<char> keyed;
    /** [node] translate xyz, rotate xyz, scale xyz curves in Lila channel space, for nodes written from their keys. */
    std::vector<std::optional<std::array<scene::anim::Curve, 9>>> curves;
    /** [node][frame] camera settings; empty for a node that is not a camera or whose settings don't change. */
    std::vector<std::vector<ExportCamera>> cameras;
    /** [node][morph target][frame] weight 0 to 1; empty for a node whose morph weights don't change. */
    std::vector<std::vector<std::vector<double>>> morphWeights;
    /** [curve][frame] values of ExportScene::curveNames. */
    std::vector<std::vector<double>> namedCurves;
    /**
     * [node][frame] points of a mesh a meshCache drives, laid out like its ExportNode::mesh; empty for other nodes. FBX
     * leaves them out; USD writes them as the points' time samples.
     */
    std::vector<std::vector<std::vector<glm::dvec3>>> meshPoints;
};

/** What an export needs from the scene, taken on the main thread so the file can be written anywhere. */
struct ExportScene
{
    std::vector<ExportNode> nodes;
    math::UpAxis upAxis = math::UpAxis::Y;
    double fps = 30.0;
    std::optional<ExportTake> take;
    /** Further takes after take, each its own AnimStack. */
    std::vector<ExportTake> extraTakes;
    /**
     * Curves Unreal imports by name onto the animation: driven channels' curveNames, and in animation exports the morph
     * weights of meshes skinned to the skeleton. Written as animated user properties on node curveHolder (the root joint).
     */
    std::vector<std::string> curveNames;
    int curveHolder = -1;
    /** ExportOptions::textures: the material of every slot the meshes use that has a material node, by slot. */
    std::map<std::string, ExportMaterial> materials;
    std::vector<std::string> warnings;
};

/** Main thread: evaluates the graph and snapshots the nodes (and, with children, their descendants). */
LILA_SCENE_API ExportScene gatherExport(scene::Graph& graph, const std::vector<scene::NodeId>& roots, const ExportOptions& opts);

/** Any thread. The FBX bytes of a snapshot. */
LILA_SCENE_API std::string encodeFbx(const ExportScene& scene, const ExportOptions& opts);
LILA_SCENE_API bool writeFbx(const ExportScene& scene, const fs::path& path, const ExportOptions& opts, std::string* error);

/**
 * For the Unreal link: the evaluated meshes of `nodes` and their children, triangulated with tangents, roots at the
 * origin unless atOrigin is false (an object set's members, exported as one asset), axes per `profile` ("ue5"/"ue6").
 * `kind` is the mapping's static, skeletal or animation. Safe from any thread: the scene is read on the main thread (waiting for it when called elsewhere) and the file is written on the
 * caller's. A take name writes only that scene take, over its own range. children false writes the given nodes alone.
 */
LILA_SCENE_API bool exportForUnreal(const std::vector<scene::NodeId>& nodes, const fs::path& path, const std::string& profile, std::string* error,
                     const std::string& kind = "static", bool atOrigin = true, const std::string& take = {}, bool children = true);

/**
 * Main thread. The Unreal check (scene/UnrealCheck.h) of what a skeletal or animation export of ids writes, over the
 * export's range; throws CommandError naming the joint Unreal can't match. Other kinds pass.
 */
LILA_SCENE_API void unrealCheck(scene::Graph& g, const std::vector<scene::NodeId>& ids, const ExportOptions& opts, const std::string& command);

/**
 * A mesh node's simple collision as convex meshes in its object space, empty when it has none: how an extension (a
 * physics body) gives exported meshes their UCX_ collision. Main thread.
 */
using CollisionSource = std::function<std::vector<geo::Mesh>(scene::Graph& g, scene::NodeId mesh)>;
LILA_SCENE_API Registry<CollisionSource>& collisionSources();
/** For extensions: owned by ctx and removed when it unloads. False when the id is taken. */
LILA_SCENE_API bool addCollisionSource(ext::Context& ctx, const std::string& id, CollisionSource source);

struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Skins become skinClusters on joints; off imports the meshes unskinned. */
    bool skins = true;
    /** Bake the chosen take onto the nodes' translate/rotate/scale as one key per frame. */
    bool animation = true;
    /** AnimStack to import; empty takes the file's active one, else the first. */
    std::string take;
    /** Set the scene fps and playback range from the take. */
    bool setPlayback = true;
    /** Nodes sharing one geometry become instances of the first; off gives each its own mesh (sharing the snapshot). */
    bool instances = true;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every node created, parents first
    /** Things in the file that were not imported, one line each (lights, curves). */
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    /** What the file declared, for the report. */
    std::string sourceAxes;
    double sourceUnitMeters = 0.01;
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins; // skinCluster nodes
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> lights;
    std::string take; // imported AnimStack, empty for none
    double fps = 0.0;
    double start = 0.0, end = 0.0; // seconds
    int curves = 0;
};

/** Main thread. Creates transforms and meshes for the file's node hierarchy, undoably. */
LILA_SCENE_API ImportResult importFbx(scene::Graph& graph, const fs::path& path, const ImportOptions& opts = {});

struct ImportAxes
{
    /** The rotation importFbx conjugates every local transform by: file axes to Lila axes, no unit scale. */
    glm::dmat3 toLila{1.0};
    /** File axes to the canonical declaration for its up axis (Z up: right +X, front -Y; Y up: right +X, front +Z). */
    glm::dmat3 toCanonical{1.0};
    bool zUp = false;
};

/** The file's axis conversions, read from its settings alone. Nullopt when the file can't be read. */
LILA_SCENE_API std::optional<ImportAxes> importAxes(const fs::path& path);

/** Every mesh instance triangulated in world space, Y up, three corners per triangle. Any thread, no scene; for previews. */
LILA_SCENE_API std::vector<glm::vec3> previewTriangles(const fs::path& path);

/** importFbx and exportFbx. */
LILA_SCENE_API void registerCommands();
} // namespace lila::fbx
