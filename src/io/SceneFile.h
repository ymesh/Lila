#pragma once

#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace lila::geo
{
class Mesh;
}

namespace lila::ext
{
class Context;
}

namespace lila::io
{
/**
 * Scenes save as binary .lila or text .lilaa (see SceneFormats.h); both carry this document:
 *   { "lila": 1, "upAxis": "y", "units": "cm", "time": 0.0,
 *     "nodes": [ { "name", "type", "parent", "attrs": { name: value } } ],   parents before children
 *     "connections": [ ["src.attr", "dst.attr"] ],
 *     "selection": [ names ],
 *     "references": [ { "namespace", "path", "loaded", "edits": [ { "node", "attrs", "parent" } ] } ],
 *     "data": { key: value },   scene::data, values tools and scripts keep with the scene
 *     "ui": { section: value } }
 * Only storable attrs that differ from their defaults are written.
 */
/**
 * `sceneDir` is where the file will live, so reference paths can be written relative to it. packed leaves meshes' big
 * arrays as packed:: binary values, which only encodeBinary takes; packed::unpackAll turns them back into plain json.
 */
LILA_SCENE_API nlohmann::json serialize(const std::filesystem::path& sceneDir = {}, bool packed = false);

/** A mesh attr serialize left as null for fillMeshes to write later, off the main thread. */
struct DeferredMesh
{
    size_t node = 0; // index into the document's "nodes"
    std::string attr;
    std::shared_ptr<const geo::Mesh> mesh;
};
/**
 * serialize with packed meshes left for later: the scene is read on the main thread, but its meshes (immutable
 * snapshots) are converted by fillMeshes on any thread, so a dense scene's autosave doesn't stall the UI.
 */
LILA_SCENE_API nlohmann::json serializeDeferred(std::vector<DeferredMesh>& meshes);
/** Converts the meshes serializeDeferred left out, in parallel, and writes them into doc. */
LILA_SCENE_API void fillMeshes(nlohmann::json& doc, std::vector<DeferredMesh>& meshes);
/**
 * Replaces the current scene. Throws std::runtime_error with a readable message. ownBytes weighs the document's own
 * nodes against its references' file sizes on the progress bar (the scene file's size, typically).
 */
LILA_SCENE_API void deserialize(const nlohmann::json& doc, const std::filesystem::path& sceneDir = {}, double ownBytes = 0.0);

/**
 * The scene format, written as "lila". Raised only when the document changes in a way older Lilas would misread; new
 * node types and attributes don't need it, they are what the writer version tells apart.
 */
LILA_SCENE_API int formatVersion();

/** Which Lila wrote a scene document, against this one. */
struct SceneOrigin
{
    enum class Age
    {
        Same,
        Older, // opens as is; saving writes it in the current format
        Newer, // opens best effort; saving here can drop what the newer Lila added
    };
    Age age = Age::Same;
    int format = 0;
    std::string version; // "major.minor.patch"; empty in files from before writers were recorded
    std::string build;
};
/** Compares doc's format and writer version with `running` (this Lila's version when empty). */
LILA_SCENE_API SceneOrigin sceneOrigin(const nlohmann::json& doc, std::string_view running = {});
/** Who wrote the scene last opened (load, loadVersion); Same after a new scene or a save. */
LILA_SCENE_API const SceneOrigin& openedOrigin();

/**
 * A part of the scene document owned by a layer above the scene, such as the viewports' look-through cameras. It is
 * written as "ui"/key from save() (null leaves it out) and handed to load() after a scene opens, null when the file has
 * none. Sections nobody registered are kept and written back, so batch mode saves them unchanged.
 */
LILA_SCENE_API void registerUiSection(const std::string& key, std::function<nlohmann::json()> save, std::function<void(const nlohmann::json&)> load);
/** For extensions: owned by ctx and removed when it unloads. False, with a log message, if the key is taken. */
LILA_SCENE_API bool registerUiSection(ext::Context& ctx, const std::string& key, std::function<nlohmann::json()> save,
                                      std::function<void(const nlohmann::json&)> load);
/** The section's current value stays in the scene document, written back like any unregistered section. */
LILA_SCENE_API bool unregisterUiSection(const std::string& key);

LILA_SCENE_API void save(const std::filesystem::path& path);
/** Writes the scene to path without making it the current file, marking it clean or taking a backup. */
LILA_SCENE_API void saveCopy(const std::filesystem::path& path);
/** Can be cancelled (progress::Cancelled) at any point, which leaves the current scene as it was (see SceneSnapshot). */
LILA_SCENE_API void load(const std::filesystem::path& path);

/**
 * The open scene held in memory, for an open that replaces it to put it back when aborted: its nodes, references,
 * selection, UI sections, file, title and unsaved state. The undo history can't come back, since restored nodes get
 * new ids; it is left empty.
 */
class LILA_SCENE_API SceneSnapshot
{
public:
    SceneSnapshot();
    /** Replaces the scene with the captured one, even inside a cancelled progress scope. */
    void restore();

private:
    nlohmann::json mDoc;
    std::filesystem::path mPath;
    std::filesystem::path mDir; // what the document's relative paths resolve against
    std::string mTitle;
    SceneOrigin mOrigin;
    bool mModified = false;
};

/** What a scene load shows instead of "Loading": one of a dozen verbs, picked at random ("Unfurling"). */
LILA_SCENE_API std::string loadingVerb();

/** The file the scene was last saved to or opened from; empty for an untitled scene. */
LILA_SCENE_API const std::filesystem::path& currentPath();
LILA_SCENE_API void setCurrentPath(const std::filesystem::path& path);
/** True when the scene changed since the last save/open/new. */
LILA_SCENE_API bool modified();
/** True while a scene file is read into the graph, including the opened-scene notifications after it. */
LILA_SCENE_API bool loading();
/**
 * On: a loaded scene computes all of itself in one parallel pass before anything reads it, instead of node by node as
 * the first readers ask. For views that draw everything right after a load; headless loads stay lazy.
 */
LILA_SCENE_API void setEvaluateOnLoad(bool on);
/** The file a load is reading while loading(); empty otherwise (it becomes currentPath() only once the load finishes). */
LILA_SCENE_API const std::filesystem::path& loadingPath();
/** True when saving to path would write what is already there: it is the current file, it exists and nothing changed. */
LILA_SCENE_API bool upToDate(const std::filesystem::path& path);
LILA_SCENE_API void markClean();
LILA_SCENE_API void onStateChanged(std::function<void()> fn);
/** Called after every successful save (not autosaves). */
LILA_SCENE_API void onSaved(std::function<void(const std::filesystem::path&)> fn);

/** Commands: saveScene, openScene, sceneInfo, plus autosave scheduling. */
LILA_SCENE_API void registerCommands();

/**
 * Writes a timestamped autosave into history::autosaveFolder() if the scene changed since the last one, keeping the
 * newest /autosave/keep (default 8). Never touches the scene file or the modified flag. Called on a timer by the host.
 */
LILA_SCENE_API void autosave();

/** Marks the scene as changed, so Save is offered and the next autosave writes. */
LILA_SCENE_API void markModified();

/**
 * A UI section saved with the scene (cameras) changed. The scene counts as modified while the section differs from
 * what was last saved or opened, so moving a camera back clears it again.
 */
LILA_SCENE_API void uiChanged(const std::string& key);
/** Takes the section's current state as the saved one, for automatic changes such as framing a scene just opened. */
LILA_SCENE_API void acceptUi(const std::string& key);

/**
 * Replaces the scene with a history file (autosave, backup, revision copy) whose references resolve against scene's
 * folder. keepPath: it becomes scene, modified, so Save writes the real file. Otherwise it opens untitled and
 * modified, shown as title.
 */
LILA_SCENE_API void loadVersion(const std::filesystem::path& file, const std::filesystem::path& scene, bool keepPath,
                 const std::string& title = {});

/** What the window title shows: the version title from loadVersion, the file name, or "untitled". */
LILA_SCENE_API std::string displayName();

/** Serializes the scene marked as an emergency save into its autosave folder. Never throws; empty on failure. */
LILA_SCENE_API std::filesystem::path emergencySave();
} // namespace lila::io
