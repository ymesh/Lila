#pragma once

#include "UsdCommon.h"
#include "UsdMaterials.h"

#include <lila/Graph.h>

#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdSkel/cache.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::scene
{
struct MeshCacheSamples;
}

namespace lila::usd
{
struct ImportOptions
{
    /** Parent the imported roots under this node. */
    scene::NodeId parent = scene::kNoNode;
    /** Only the prim at this path and below; empty imports the whole stage. */
    std::string prim;
    /** Skeleton bindings become skinClusters; off imports skinned meshes in their bind shape, unskinned. */
    bool skins = true;
    /** Time samples become keys. */
    bool animation = true;
    /** Set the scene fps and playback range from the stage. */
    bool setPlayback = true;
    bool materials = true;
    /** Load payloads; off leaves them unloaded and reports them. */
    bool loadPayloads = true;
    /** Place the roots by the georeference the stage carries (Lila's lilaGeoreference layer data, or a Cesium georeference prim). */
    bool georeference = true;
    /** Variant selections: set name to variant for every prim with that set... */
    std::map<std::string, std::string> variants;
    /** ...or per prim path. */
    std::map<std::string, std::map<std::string, std::string>> primVariants;
    /** Take to import from a Lila export's take variants; empty keeps the file's selection. */
    std::string take;
    /** Prim purposes to import; render-only files import fine with the default, proxies and guides are skipped. */
    std::set<std::string> purposes{"default", "render"};

    /** Existing nodes by prim path, taken over (moved under the new parent, reset and filled again) instead of new ones. */
    std::unordered_map<pxr::SdfPath, scene::NodeId, pxr::SdfPath::Hash>* reuse = nullptr;
    /** Build prim alone, leaving its children's nodes as they are. */
    bool shallow = false;
    /** More prims built alone in the same pass, each under its parent: meshes skinned to joints the pass builds again. */
    std::vector<std::pair<std::string, scene::NodeId>> alsoBuild;
    /**
     * Reused meshes fed by a stack (modifiers, deformers, a skin) whose composed shape is Lila's own written result, by
     * link path: their base mesh stays as it is, so the stack isn't applied to its own output.
     */
    const std::set<pxr::SdfPath>* keepShape = nullptr;
    /** A live stage: variant switches made while building are taken back out of the session layer, leaving no opinion. */
    bool live = false;
    /** Light-link collections baked into lighting channels (never on a live stage). */
    bool lightingChannels = true;
};

struct ImportResult
{
    bool ok = false;
    std::string error;
    std::vector<scene::NodeId> roots;
    std::vector<scene::NodeId> nodes; // every node created, parents first
    std::vector<scene::NodeId> joints;
    std::vector<scene::NodeId> skins;
    std::vector<scene::NodeId> cameras;
    std::vector<scene::NodeId> lights;
    std::vector<scene::NodeId> curves;
    /** meshCache deformers made for meshes with time-sampled points. */
    std::vector<scene::NodeId> meshCaches;
    std::vector<std::string> materials;
    std::vector<std::string> skipped;
    std::vector<std::string> warnings;
    std::string upAxis;
    double metersPerUnit = 0.01;
    double fps = 0.0;
    double start = 0.0, end = 0.0; // seconds
    std::string take;
    std::vector<std::string> takes;
    int keyedCurves = 0;
    int instances = 0;
    /** Lighting channels the import set on its lights and objects. */
    std::vector<std::string> lightingChannels;
    /**
     * The node built for each prim, by its link path: its path in the composed stage, or for a prim inside an LOD level
     * the path through that level's variant (/Chair{LOD=LOD1}Mesh), which tells levels with the same names apart.
     */
    std::unordered_map<pxr::SdfPath, scene::NodeId, pxr::SdfPath::Hash> nodeOf;
    /** Every joint built, with the Skeleton prim it is a joint of. */
    std::vector<std::pair<scene::NodeId, pxr::SdfPath>> jointSkeletons;
    /** Point instance copies: every node built from a prototype prim, with that prim's path. */
    std::vector<std::pair<scene::NodeId, pxr::SdfPath>> copies;
    /** The transform each point instance sits under, with its instancer's path and index. */
    struct PointInstance
    {
        scene::NodeId holder = scene::kNoNode;
        pxr::SdfPath instancer;
        size_t index = 0;
    };
    std::vector<PointInstance> pointInstances;
    /** The Lila slot each USD material became. */
    std::map<pxr::SdfPath, std::string> materialSlots;
};

/** Main thread. Opens the stage, resolves its composition and builds the composed result in the scene, undoably. */
ImportResult importUsd(scene::Graph& graph, const fs::path& path, const ImportOptions& opts);

/**
 * Builds an open stage's composed prims (or opts.prim) into the scene, filling r. file is where its textures resolve
 * from. Takes and variant selections are left to the caller.
 */
void importStage(scene::Graph& graph, const pxr::UsdStageRefPtr& stage, const fs::path& file, const ImportOptions& opts, ImportResult& r);

/** Unreal's LOD convention, built as an LOD group: a variant set named LOD whose variants LOD0, LOD1, ... each hold a level. In level order; empty for any other prim. */
std::vector<std::string> lodLevels(const pxr::UsdPrim& prim);

/** Deletes the node and the nodes upstream of it that feed nothing else (its curves, skin and modifiers), undoably. */
void deleteWithInputs(scene::Graph& graph, scene::NodeId id);
/** Takes a modifier, deformer or skin out of its mesh's stack, joining what fed it to what it fed, and deletes it. */
void removeFromStack(scene::Graph& graph, scene::NodeId modifier);

/** What the import passes between its hierarchy, mesh and skeleton parts. */
struct Importer
{
    scene::Graph& g;
    const ImportOptions& opts;
    ImportResult& r;
    pxr::UsdStageRefPtr stage;
    Axes axes;
    double tcps = 24.0;
    /** Where rest values are read: the stage's start time, or the earliest sample when it has no range. */
    pxr::UsdTimeCode restTime = pxr::UsdTimeCode::EarliestTime();
    bool held = false; // the stage interpolates held, not linearly
    std::unique_ptr<MaterialReader> materials;
    /** Every time sample keyed, for a playback range when the stage declares none. */
    double firstKey = 1e300, lastKey = -1e300;

    /** Per node: Lila local = before * (file local in Lila axes) * after, turning cameras between file and Lila frames. */
    struct Frame
    {
        glm::dmat4 before{1.0}, after{1.0};
    };
    std::unordered_map<scene::NodeId, Frame> frames;

    /** Node made for each prim (by its path in the composed stage). */
    std::unordered_map<pxr::SdfPath, scene::NodeId, pxr::SdfPath::Hash> nodeOf;
    /** Prims built alone (opts.shallow, opts.alsoBuild). */
    std::set<pxr::SdfPath> shallowRoots;
    /** Reused meshes a stack feeds: the base under the stack is replaced and the stack kept. */
    std::set<scene::NodeId> stacked;
    /** Stacked meshes whose base was kept (opts.keepShape). */
    std::set<scene::NodeId> keptShape;
    /** The LOD levels being built, innermost last: the LOD prim's composed path and the link path of its variant. */
    std::vector<std::pair<pxr::SdfPath, pxr::SdfPath>> lodScope;
    /** Above zero while a point instance's prototype is built. */
    int inCopy = 0;

    /** The path a prim's node is linked by (ImportResult::nodeOf). */
    pxr::SdfPath linkPath(const pxr::SdfPath& composed) const;
    /** The plug holding a mesh node's base mesh: its inMesh, or the stored source under its stack. */
    scene::PlugRef basePlug(scene::NodeId mesh);
    /** Meshes shared between instances of one prototype prim. */
    std::unordered_map<pxr::SdfPath, scene::MeshPtr, pxr::SdfPath::Hash> meshCache;
    /** The point cache samples read for each rest mesh, shared by the instances of a prototype. */
    std::unordered_map<const geo::Mesh*, std::shared_ptr<const scene::MeshCacheSamples>> pointCaches;
    /** Lila point per USD point of each mesh prim built, kInvalid for dropped ones. */
    std::unordered_map<pxr::SdfPath, std::vector<uint32_t>, pxr::SdfPath::Hash> pointMaps;

    struct Skel
    {
        scene::NodeId parent = scene::kNoNode; // node the root joints hang under
        std::vector<scene::NodeId> joints;     // per skeleton joint, in its order
        bool synthetic = false;                // added by Lila's exporter to carry blend shapes
    };
    std::unordered_map<pxr::SdfPath, Skel, pxr::SdfPath::Hash> skeletons;
    /**
     * Skinned or blend-shaped mesh prims and their nodes, bound after the hierarchy exists; and animated xformables,
     * keyed after that. By path: switching an LOD group's variants recomposes its prims.
     */
    std::vector<std::pair<pxr::SdfPath, scene::NodeId>> bound;
    std::vector<std::pair<pxr::SdfPath, scene::NodeId>> animated;
    /** Blend shape targets per mesh node, by the channel name the animation drives them with. */
    struct Shapes
    {
        pxr::SdfPath skeleton;
        std::vector<std::pair<std::string, scene::NodeId>> targets;
    };
    std::vector<Shapes> shapes;
    pxr::UsdSkelCache skelCache;

    /** Seconds of a time code. */
    double seconds(double timeCode) const { return timeCode / tcps; }

    // UsdImportSkel.cpp
    void buildSkeleton(const pxr::UsdPrim& prim, scene::NodeId node);
    void bindAll();
    void keySkeletons();
};

/**
 * Shared by the import parts: curves for the translate/rotate/scale channels that move from samples (seconds), linear
 * or held between the keys, keeping only the samples that interpolation doesn't already reproduce. Joints, which key
 * rotation curves, get one. recorded holds the curves Lila's exporter wrote beside the samples (lila:curves on xformOpOrder); a
 * channel whose recorded curve still reproduces its samples gets that curve back, keys and tangents as they were.
 */
void keyChannels(Importer& im, scene::NodeId node, const std::vector<double>& times, const std::vector<glm::dvec3>& t,
                 const std::vector<glm::dvec3>& r, const std::vector<glm::dvec3>& s, const nlohmann::json* recorded = nullptr);
/** A curve on one scalar plug from samples (seconds), thinned the same way. */
void keyPlug(Importer& im, const scene::PlugRef& plug, const std::vector<double>& times, const std::vector<double>& values);
} // namespace lila::usd
