#pragma once

#include "core/Export.h"
#include "core/Registry.h"

#include "link/RpcClient.h"
#include "scene/Node.h"
#include "seq/MeshSequence.h"
#include "seq/Vat.h"

#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::link
{
namespace fs = std::filesystem;

/** First port LilaLink tries in the editor; it walks up from here when that one is taken. */
constexpr uint16_t kDefaultPort = 8741;
constexpr int kPortSearchCount = 8;

/** The PrefStore file the Unreal link settings live in. */
inline constexpr const char* kUnrealPrefFile = "unreal";

/** A running editor with LilaLink, found through its port file or by probing. */
struct Instance
{
    uint16_t port = 0;
    int64_t pid = 0;
    std::string project;
    fs::path projectDir;
    std::string engineVersion;
    std::string source; // "portFile" or "probe"
};

LILA_SCENE_API json toJson(const Instance& i);

/** <projectDir>/Saved/LilaLink/port.json for each dir (a .uproject path works too). Missing files are skipped. */
LILA_SCENE_API std::vector<Instance> readPortFiles(const std::vector<fs::path>& projectDirs);

/** Project dirs listed under "projects" in the "unreal" pref file. */
LILA_SCENE_API std::vector<fs::path> configuredProjects();

/** Port files of configured projects, then any default port that accepts a connection and wasn't already found. */
LILA_SCENE_API std::vector<Instance> discover(bool probe = true, const std::string& host = "127.0.0.1");

/**
 * The scene's mappings, {node (full path), contentPath, kind, skeleton, ...import options}. Each lives on its node, so it
 * saves with the scene, follows renames and goes with deletes; duplicates don't inherit it.
 */
LILA_SCENE_API json mappings();
/** One node's mapping with its shortest path as "node", or null. */
LILA_SCENE_API json mappingOf(const std::string& node);
/** Adds or replaces the mapping on node m["node"]. Undoable. Throws std::invalid_argument for a missing node. */
LILA_SCENE_API void setMapping(const json& m);
LILA_SCENE_API bool removeMapping(const std::string& node);
/**
 * Moves mappings kept in the workspace's "unreal" pref file onto the nodes of the open scene they resolve in (not
 * undoable, marks the scene modified); saving the scene then drops them from the file. Static and skeletal ones with no
 * mesh under them are left out.
 */
LILA_SCENE_API void adoptWorkspaceMappings();

/**
 * importAsset params for a file and a mapping (or the flags of an unrealImport call shaped the same way). An array
 * `materials` (from materialParams, even empty) goes along as params.materials: it tells the plugin to match the slots
 * to Unreal materials by name and never make one for the "Default" slot. Without it Interchange handles the materials.
 * A mapping `components` array (attrComponents::unrealPayload, empty to clear them) goes along as params.components.
 */
LILA_SCENE_API json importParams(const fs::path& file, const json& mapping, const json& materials = json());

/**
 * The materials the meshes under `roots` use, in slot order, for the plugin to find or create by name:
 * [{name, baseColor, metallic, ..., baseColorMap (absolute), ...}]. Faces on the default slot are left out; a slot
 * with no material node gives just {name}.
 */
LILA_SCENE_API json materialParams(scene::Graph& g, const std::vector<scene::NodeId>& roots);

/** One slot's entry of materialParams: {name} plus the material node's values when there is one. */
LILA_SCENE_API json materialInputs(scene::Graph& g, const std::string& slot);

/**
 * Timeouts for importAsset: the "unreal" prefs "/importTimeoutMs" (overall, default 10 min) and "/importIdleTimeoutMs"
 * (without any reply or progress from the editor, default 60 s).
 */
LILA_SCENE_API CallOptions importCallOptions();

/** Asset names of a meshSequence mapping's frames: contentPath /Game/FX/SM_Spike gives /Game/FX/SM_Spike_000, ... */
struct SequenceAssets
{
    std::string folder; // /Game/FX
    std::string base;   // SM_Spike
    std::vector<std::string> contentPaths;
    std::vector<std::string> objectPaths;
};
LILA_SCENE_API SequenceAssets sequenceAssets(const json& mapping, int count);

/** From a listAssets reply, the frames <base>_<n> with n >= count: what is left over when a sequence gets shorter. */
LILA_SCENE_API std::vector<std::string> surplusFrames(const json& listed, const std::string& base, int count);

/**
 * Sampling for a meshSequence (fbx per frame) or geometryCache (one abc) mapping: its "sequence" object (the
 * exportMeshSequence flags: start, end, step, fps, scale, padding, worldSpace, recenter, ...; the playback range without
 * start and end), written to `folder` with the ue5 axes.
 */
LILA_SCENE_API seq::SequenceOptions sequenceOptions(const json& mapping, const fs::path& folder);

/**
 * A vertexAnimation mapping's bake: sequenceOptions for the sampling plus its "vat" object (the exportVertexAnimation
 * flags format, mode, positions, normals, normalBits, restFrame, maxWidth), written to `folder` as `name`. Throws
 * std::invalid_argument.
 */
LILA_SCENE_API seq::VatOptions vertexAnimationOptions(const json& mapping, const fs::path& folder, const std::string& name);

/**
 * importVertexAnimation params for a bake: {contentPath, fbx, positions, normals?, metaFile, meta, materialParent?,
 * options {lodGroup, save, ...}}.
 */
LILA_SCENE_API json vertexAnimationParams(const seq::VatResult& bake, const json& mapping);

/** Where auto-sync writes exports: the "/cacheDir" pref, else <workspace>/.lila/cache/unreal, else the user dir. */
LILA_SCENE_API fs::path cacheDir();

/**
 * The level open in the connected editor: its package, its package's persistent GUID (a new one whenever a level is
 * made, so a level deleted and made again under the same path differs) and whether it is World Partition. level is
 * empty when not connected or the plugin has no levelInfo; guid is empty from a plugin that predates it.
 */
struct LevelIdentity
{
    std::string level, guid;
    bool partitioned = false;
};

/** levelInfo of the connected editor (see LevelIdentity). */
LILA_SCENE_API LevelIdentity openLevelIdentity();

/**
 * Whether a record of what Lila sent to a level ({"level", "levelGuid", ...}, see stampLevelRecord) still describes the
 * open one. Returns empty when it does, else why not: a different GUID (the level was deleted and made again, or
 * replaced), another level, or a record without a GUID. Not connected, or a plugin without GUIDs, trusts the record.
 */
LILA_SCENE_API std::string levelRecordMismatch(const json& record, const LevelIdentity& open);

/** Writes the open level's path and GUID into a record, for levelRecordMismatch. */
LILA_SCENE_API void stampLevelRecord(json& record, const LevelIdentity& open);

/** A file-name-safe form of a level package path, whole, so levels of the same name in different folders differ. */
LILA_SCENE_API std::string levelFileKey(const std::string& level);

/** The connection to one Unreal editor plus a worker thread for slow jobs (export + import). */
class LILA_SCENE_API Link
{
public:
    static Link& get();

    struct ConnectOptions
    {
        std::string host = "127.0.0.1";
        std::optional<uint16_t> port;
        std::string project; // name or directory; empty = first instance found
        int timeoutMs = 2000;
    };

    /** Connects and says hello. Returns the hello result; throws std::runtime_error with a readable message. */
    json connect(const ConnectOptions& opts);
    /** Connects with default options unless already connected. Throws like connect(). */
    void ensureConnected();
    void disconnect();
    bool connected() const;

    /** hello result plus port, or {"connected": false}. */
    json status() const;

    /** The live client; null when not connected. Shared so worker jobs keep it alive across a reconnect. */
    std::shared_ptr<RpcClient> client() const;

    /** Runs fn on the worker thread, in order. */
    void post(std::function<void()> fn);
    /** Blocks until everything posted so far has run; on the main thread it keeps running main-queue work meanwhile. */
    void waitIdle();
    /** Stops the worker and disconnects. Call before the app exits. */
    void shutdown();

    Link() = default;
    ~Link();

private:
    void workerLoop();

    mutable std::mutex mMutex;
    std::shared_ptr<RpcClient> mClient;
    json mHello;

    std::mutex mQueueMutex;
    std::condition_variable mQueueCv;
    std::deque<std::function<void()>> mQueue;
    bool mBusy = false;
    bool mQuit = false;
    std::thread mWorker;
};

/**
 * Auto-sync: for each mapping whose node exists, export it with the "fbx" exporter to cacheDir()/<node>.fbx on the
 * worker and send importAsset. A mapped object set exports its member objects together into one file, through the
 * "fbx:set:<kind>" exporter when one is registered. Results are logged. Main thread. Off when the "unreal" pref "/autoSync" is false.
 *
 * Animated kinds are sampled here on the main thread first. meshSequence writes an FBX per frame and imports each as
 * the static mesh <contentPath>_<index> (reimported in place, so references stay), then sets the Niagara system's mesh
 * renderer to them when the mapping names niagaraSystem, then deletes frames past the end. geometryCache writes one
 * .abc and imports it as a Geometry Cache.
 */
LILA_SCENE_API void onSceneSaved();

/**
 * While one is alive, slot rule, override and pin changes don't send the last slot roots to Unreal again: a caller that
 * rebuilds those roots (worldBuild) sends them itself once they are whole.
 */
struct LILA_SCENE_API SlotSyncPause
{
    SlotSyncPause();
    ~SlotSyncPause();
    SlotSyncPause(const SlotSyncPause&) = delete;
    SlotSyncPause& operator=(const SlotSyncPause&) = delete;
};

/**
 * What onSceneSaved does, minus the pref check when `force`, then syncTextureSets. Returns how many mappings and sets were
 * queued. Given nodes, only their mappings go, and no texture sets. Skeletal and animation exports refuse when a skeleton
 * fails the Unreal check, unless unrealCheck is false.
 */
LILA_SCENE_API int syncMappings(bool force, const std::vector<std::string>& only = {}, bool unrealCheck = true);

/**
 * Before a send: runs every validator (unless runValidators is false), then checks the issues on what the send carries
 * (nodes and their descendants) and on the scene itself. An issue that blocks `command`, "unreal" or "*" throws
 * CommandError naming each; the rest come back as [{target, severity, message, blocks?}].
 */
LILA_SCENE_API json preflight(scene::Graph& g, const std::vector<scene::NodeId>& nodes, const std::string& command, bool runValidators = true);

/** How unrealPlaceInstances sends instance groups to the open level. */
struct PlacementOptions
{
    std::string folder = "/Game/Lila/Instances";
    std::string mode = "auto"; // actors | ism | hism | auto (actors up to threshold members, then hism)
    int threshold = 50;
    /** Give members without a link id (or sharing one with an older node) a new one, undoably. Off, they are keyed by node id. */
    bool assignIds = false;
    /** Each placement and light carries its Unreal Lighting Channels (lightingChannels::unrealBits). */
    bool lightingChannels = true;
    /**
     * The assets' collision: none, box, convex, complexAsSimple, ucx (the importAsset collision option), or auto: ucx for a
     * source with UCX_ children or a collision source's hulls (a Stagehand body), complexAsSimple for one whose body asks
     * for it or has none.
     */
    std::string collision = "auto";
    bool nanite = false;
};

/**
 * The instance groups of the meshes under `roots`, one asset per instance root:
 * {groups: [{key, source (full path), sourceId, contentPath, objectPath, mode, collision (resolved), ucxChildren, nanite, placements: [...]}]}, each placement
 * {id, name, location, rotation (quat xyzw), scale, customData: [stable random 0..1], components} in Unreal space, components
 * being the member's attrComponents::unrealPayload, and lightingChannels [bool x3] when o.lightingChannels; hidden members
 * are left out. Lights under the roots go in plan.lights, one light actor each: {id, name, kind, location, forward, up,
 * color, intensity, units, ...the light's settings, lightingChannels?}, directions in Unreal space; domes are skipped
 * (plan.skipped). plan.lightingChannels is lightingChannels::report with the Unreal slots and what was dropped. Groups and placements are keyed by the nodes'
 * linkId, which survives renames and reopening the scene. A group lists its root and every instance of it, wherever
 * they sit. Throws CommandError for an unknown mode.
 *
 * A source whose geometry nodes end in instances gets a group per mesh they reference, keyed "<source key>:gn:<mesh
 * hash>" with an instance's placements keyed "<member key>:<instance id>" (the "id" instance attribute, else its index),
 * and a gnMesh index into the meshes sent. Its own group then holds only the geometry beside the instances (gnMesh), or
 * has remove set when there is none. plan.gnSources lists {prefix, keep} per such source, so Unreal can drop the meshes
 * it no longer instances.
 */
LILA_SCENE_API json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o);

/** placementPlan, also handing back the meshes the groups' gnMesh index (geometry nodes parts and bases). */
LILA_SCENE_API json placementPlan(scene::Graph& g, const std::vector<scene::NodeId>& roots, const PlacementOptions& o, std::vector<scene::MeshPtr>& meshes);

/** Writes a mesh alone at the origin as an FBX for Unreal. Any thread. */
LILA_SCENE_API bool writeMeshFbx(const scene::MeshPtr& mesh, const std::string& name, const fs::path& path, std::string* err);

/**
 * More of what a placement carries: an extension returns a JSON object whose fields are merged into the placement of the
 * mesh (Stagehand's "physics"), or null for none. A placement whose physics.simulate is true needs an actor of its own,
 * so mode auto never puts its group into an (H)ISM. Main thread.
 */
using PlacementExtra = std::function<json(scene::Graph& g, scene::NodeId mesh)>;
LILA_SCENE_API Registry<PlacementExtra>& placementExtras();
/** For extensions: owned by ctx and removed when it unloads. False when the id is taken. */
LILA_SCENE_API bool addPlacementExtra(ext::Context& ctx, const std::string& id, PlacementExtra extra);

/**
 * What an instance source's placements carry as custom primitive data, in slot order: "random" is a stable 0 to 1 per
 * placement, any other name a number, bool or vector attr on each member (a vector fills 3 slots) or, for geometry nodes
 * instances, the instance attribute of that name (as wide as its type), else the member's attr. Saved on the source;
 * ["random"] when none is set.
 */
LILA_SCENE_API std::vector<std::string> customDataNames(const scene::Node& source);
/** Sets them, undoably. An empty list sends no custom data. */
LILA_SCENE_API void setCustomDataNames(scene::Graph& g, scene::NodeId source, const std::vector<std::string>& names);

/** RPC timeout for a placement call carrying count placements: 30 s plus 20 ms each, at most 30 minutes. */
LILA_SCENE_API int placementTimeoutMs(size_t count);

/** unrealPlaceInstances and unrealRemovePlacements. */
LILA_SCENE_API void registerPlacementCommands();

/** Registers unrealConnect, unrealStatus, unrealImport, unrealMap, unrealUnmap, unrealMappings, unrealSync and the placement commands. */
LILA_SCENE_API void registerCommands();
} // namespace lila::link
