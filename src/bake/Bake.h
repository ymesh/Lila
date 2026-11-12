#pragma once

#include "core/Export.h"

#include "bake/BakeTypes.h"
#include "scene/Node.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace lila::scene
{
class Graph;
}

/**
 * Running bakes. bake() snapshots the scene (BakeInputs), then runBake() does the work with a Backend: for every group,
 * low and UDIM tile, bands of texel rows go raster -> projection (traced once) -> every map's kernel -> accumulate into
 * the texture each map's file name resolves to; then each texture is resolved, finalized, dilated and written, files
 * encoding in parallel. Maps the backend keeps resident (a GPU backend) do all of that on the device; the rest run
 * bands in parallel on the CPU side. Textures bake in batches whose accumulators fit the device (or host memory) at
 * once, a low's texture set kept together, so memory stays bounded however many textures a bake writes. Groups only
 * ever trace their own highs. Progress goes through a cancellable
 * progress::Scope, and BakeResult::timings says where the time went.
 *
 * Commands (registerCommands): createBakeProject, bakeGroup, autoGroupBake, setBakeMap, setBakeOutput, bake, bakeStatus,
 * bakeForTarget, bakeTargets, bakeMaps.
 */
namespace lila::bake
{
struct BakeInputs;
struct TextureBuffer;
class Backend;

struct LILA_SCENE_API OutputFile
{
    std::filesystem::path file;
    std::string map;
    std::string suffix;
    int udim = 1001;
    int width = 0;
    int height = 0;
    int bits = 8;
    std::string format;
    /** The layer of a layered PSD (file) this map went into; empty for a file of its own. */
    std::string layer;
    /** srgb for colour maps, linear for data. */
    std::string colorSpace = "linear";
    /** Tangent normal maps: directx or opengl (green up); empty otherwise. */
    std::string normalFormat;
    std::vector<std::string> groups;
    std::vector<std::string> meshes;
    std::vector<std::string> materials;
    /** Decode data (height range, position bounds, ID legend). */
    json meta = json::object();

    json toJson() const;
};

struct LILA_SCENE_API GroupStats
{
    std::string group;
    uint64_t samples = 0;
    uint64_t hits = 0;
    uint64_t misses = 0;
    /** Rays whose nearest surface faced away (included in misses when backfaces are ignored). */
    uint64_t backfaces = 0;
    /** Rays that met the far side of a thin part and were re-aimed along the low's face normal. */
    uint64_t reaimed = 0;
    /** Samples no ray along the normal reached, taken from the nearest high facing the low's way. */
    uint64_t probed = 0;
    uint64_t highTriangles = 0;
    uint64_t lowTriangles = 0;
    /** The frontal and rear distances the group baked with, cm; Auto ones are measured when measured is set. */
    double frontal = 0.0;
    double rear = 0.0;
    bool frontalAuto = false;
    bool rearAuto = false;
    bool measured = false;

    json toJson() const;
};

struct LILA_SCENE_API BakeResult
{
    std::vector<OutputFile> outputs;
    std::vector<GroupStats> groups;
    std::vector<std::string> warnings;
    /** The JSON manifest written next to the textures ({project}_bake.json), empty on a dry run. */
    std::filesystem::path manifest;
    bool dryRun = false;
    double seconds = 0.0;
    /** The inputs' content hash (see inputHash in BakePipeline.h); empty when the bake covered only some groups. */
    std::string inputHash;
    /** The bakeLint report bake() ran on the same inputs first (its warnings also lead warnings); null from runBake. */
    json lint;
    /**
     * Seconds per stage: inputs, modifiers, accel, lint, inputHash, plan, raster, trace, map:<id>, resolve, finalize, dilate,
     * write, ... Stages that run on many threads at once report thread seconds summed, so they can exceed the total.
     */
    json timings = json::object();
    /** The backend that ran the bake and how it traced (Backend::mode): cpu, rt or compute. */
    std::string backend;
    std::string backendMode;

    /** {outputs, stats: {samples, hits, misses, backfaces, groups}, warnings, manifest, dryRun, seconds, inputHash, lint, timings}. */
    json toJson() const;
};

struct BakeOptions
{
    /** Map ids to bake; empty bakes every enabled map. */
    std::vector<std::string> maps;
    bool dryRun = false;
    /** Resolved output folder (absolute). */
    std::filesystem::path folder;
    /** Recorded in the manifest and sidecars. */
    std::string inputHash;
    /** Called with every file about to be written, before the first is (source control checkout). */
    std::function<void(const std::vector<std::filesystem::path>&)> prepareWrite;
    /** Called as each texture lands on disk, from the thread that wrote it. */
    std::function<void(const OutputFile&)> onWritten;
    /**
     * When set, every finished texture comes here instead of going to disk (previews): nothing is written and no manifest.
     * Called on the baking thread, one texture at a time.
     */
    std::function<void(const TextureBuffer&)> keepTexture;
    /**
     * Set by runBake for each pass of a bake split by OutputSettings::materials: bake only the texture sets named in
     * materials (all but them with excludeMaterials), at the settings given, and write no manifest.
     */
    bool materialPass = false;
    std::vector<std::string> materials;
    bool excludeMaterials = false;
};

/** The graph-free core: bakes prepared inputs with a backend. Throws progress::Cancelled or std::runtime_error. */
LILA_SCENE_API BakeResult runBake(const BakeInputs& inputs, const ProjectSettings& settings, const BakeOptions& options, Backend& backend);

struct BakeRequest
{
    scene::NodeId project = scene::kNoNode;
    /** bakeGroup nodes; empty bakes all of the project's. */
    std::vector<scene::NodeId> groups;
    std::vector<std::string> maps;
    bool dryRun = false;
    std::string backend = "cpu";
};

/** Snapshots the scene and bakes. Main thread. Throws std::runtime_error or progress::Cancelled. */
LILA_SCENE_API BakeResult bake(scene::Graph& g, const BakeRequest& request);

/**
 * Starts a bake that runs off the main thread: the scene is snapshot here, then tracing, maps and writing run on a
 * worker while the UI stays live (progress and cancel through progress::snapshot as "Baking <project>"). Listeners hear
 * Started, a Texture per written file and Finished, and lastResult holds the result once it lands. One bake runs at a
 * time. Main thread; throws std::runtime_error when a bake is running or the snapshot fails.
 */
LILA_SCENE_API void bakeAsync(scene::Graph& g, const BakeRequest& request);
/** True while a bakeAsync bake runs. */
LILA_SCENE_API bool bakeRunning();
/** Cancels the running bakeAsync bake, if any; wait joins its thread (shutdown). */
LILA_SCENE_API void cancelAsyncBake(bool wait);

/** What a running bake reports, on the main thread. */
struct BakeEvent
{
    enum class Kind
    {
        Started,
        /** data: the OutputFile just written (toJson). */
        Texture,
        /** data: the result (BakeResult::toJson), or {project, error} and cancelled true when it stopped short. */
        Finished,
    };
    Kind kind = Kind::Started;
    scene::NodeId project = scene::kNoNode;
    json data;
};
LILA_SCENE_API int addBakeListener(std::function<void(const BakeEvent&)> fn);
LILA_SCENE_API void removeBakeListener(int id);

/** The last result per project node this session (bakeStatus), null json when none. */
LILA_SCENE_API json lastResult(scene::NodeId project);

/**
 * The output folder as bake resolves it: relative paths under the workspace root, else the saved scene file's folder,
 * else (an unsaved scene) the current folder.
 */
LILA_SCENE_API std::filesystem::path resolveFolder(const std::string& folder);

/** One auto-grouping match: lows and highs that share a base name. */
struct AutoGroup
{
    std::string base;
    std::vector<std::string> lows;
    std::vector<std::string> highs;
};

/**
 * Pairs names by suffix, case blind: lows end in _low, _lo or _LP; highs in _high, _hi or _HP, optionally numbered
 * (_high1, _high_2) so a low can take several highs. Namespaces stay in the base. Groups need at least one low and one
 * high; *unmatched gets the rest.
 */
LILA_SCENE_API std::vector<AutoGroup> matchAutoGroups(const std::vector<std::string>& names, std::vector<std::string>* unmatched = nullptr);

LILA_SCENE_API void registerCommands();
} // namespace lila::bake
