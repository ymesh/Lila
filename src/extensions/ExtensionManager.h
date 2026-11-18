#pragma once

#include "core/ExtensionManifest.h"
#include "core/Export.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lila::ext
{
class Context;
}

namespace lila::extensions
{
using json = nlohmann::json;
namespace fs = std::filesystem;

struct ExtensionError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

enum class ExtensionKind : uint8_t
{
    Native, // a package with a library exporting lilaExtensionInit (and maybe a Python module too)
    Python, // a package with only a Python module, or a bare .py file or package defining initialize_extension()
    Resources, // a package with only resources (materials, nodes, presets) that Lila's libraries scan
};

struct LILA_SCENE_API ExtensionInfo
{
    std::string name; // the id
    fs::path path;    // the package folder, or the bare Python file or folder
    ExtensionKind kind = ExtensionKind::Native;
    bool loaded = false;
    bool autoload = false;
    /** From extension.json; empty for a bare Python extension. */
    std::optional<ext::Manifest> manifest;
    /** Why the manifest could not be read, or why the last load failed. */
    std::string error;

    json toJson() const;
};

/** How Lila follows edits to a watched extension's package. */
struct WatchOptions
{
    /** Reload it after the event; false only sends the event. */
    bool reload = true;
    /** How long the package must be quiet before one event covers everything written. */
    int settleMs = 500;
};

/**
 * Finds, loads and unloads extensions. Main thread only.
 *
 * An extension is a package folder <Id>.lilaext with an extension.json (see ext::Manifest), or a bare Python file or
 * package. Search order, first match per id wins: addSearchPath() dirs, LILA_EXTENSION_PATH, general.json
 * /extensions/paths, <workspace>/.lila/extensions, <user config>/extensions, <exe dir>/extensions.
 *
 * Lila checks a package's SDK version (its manifest, then what its library was built against) before running any of
 * its code, and loads its dependencies first.
 *
 * A package outside Lila's install loads its native libraries from a copy in the temp folder, so a build can write
 * into the package while it is loaded.
 */
class LILA_SCENE_API ExtensionManager
{
public:
    static ExtensionManager& get();

    ExtensionManager();
    ~ExtensionManager();
    ExtensionManager(const ExtensionManager&) = delete;
    ExtensionManager& operator=(const ExtensionManager&) = delete;

    std::vector<fs::path> searchPaths() const;
    void addSearchPath(const fs::path& dir);

    /** Everything on the search paths, plus loaded extensions that came from elsewhere. */
    std::vector<ExtensionInfo> available() const;

    /** An id on the search paths or a path to a package (or its extension.json). Loading a loaded extension does nothing. Returns the id. */
    std::string load(const std::string& nameOrPath);
    /** Refuses while scene nodes use the extension's node types or a loaded extension depends on it. */
    void unload(const std::string& name);
    std::string reload(const std::string& name);
    void unloadAll();

    bool isLoaded(const std::string& name) const;
    /** In load order. */
    std::vector<std::string> loaded() const;
    /** Info plus what the extension registered. Throws for unknown names. */
    json describe(const std::string& name) const;
    /** The last load failure, empty when there was none. */
    std::string lastError(const std::string& name) const;

    /** general.json /extensions/autoload: extensions the user turned on. */
    std::vector<std::string> autoloadList() const;
    /**
     * Whether the extension loads at startup: turned on by the user, or enabledByDefault and not turned off. One the
     * roadmap marks not ready for use (core/Features.h) loads only once the user turns it on.
     */
    bool autoloads(const ExtensionInfo& info) const;
    void setAutoload(const std::string& name, bool on);
    /** Loads every extension that autoloads, dependencies first, logging failures. */
    void loadAutoload();

    /**
     * Follow edits to an extension's package: once writes settle, send extensionChanged with the changed files, then
     * reload it when the options say so. Kept in general.json /extensions/watch.
     */
    void watch(const std::string& name, WatchOptions options);
    void unwatch(const std::string& name);
    std::map<std::string, WatchOptions> watched() const;
    /** Resumes the watches kept in prefs. */
    void loadWatches();
    /** Stops the watcher thread; watches resume on the next watch() or loadWatches(). */
    void stopWatching();

private:
    struct Loaded;
    struct Watch;

    std::optional<ExtensionInfo> find(const std::string& nameOrPath) const;
    std::vector<std::string> disabledList() const;
    void loadInfo(const ExtensionInfo& info, std::set<std::string>& visiting);
    void loadNative(Loaded& rec);
    void loadPython(Loaded& rec, const fs::path& module);
    void removeOwned(Loaded& rec);
    void checkUnloadable(const std::string& name) const;
    void addWatch(const std::string& name, WatchOptions options);
    void startWatcher();
    void watchLoop();
    void packageChanged(const std::string& name, std::vector<fs::path> paths);
    void saveWatches() const;
    /** Rebuilds lila.ops after native registrations change, or marks it pending while a batch of loads or unloads runs. */
    void commandsChanged();
    void flushCommands();

    std::map<std::string, std::unique_ptr<Loaded>> mLoaded;
    std::vector<std::string> mOrder; // load order
    std::map<std::string, std::string> mErrors;
    std::vector<fs::path> mExtraPaths;
    int mBatchDepth = 0;
    bool mCommandsPending = false;

    mutable std::mutex mWatchMutex;
    std::map<std::string, std::unique_ptr<Watch>> mWatches;
    std::condition_variable mWatchWake;
    std::thread mWatcher;
    bool mStopWatcher = false;
    /** The extension whose extensionChanged handlers are running; reloading it from one of them is deferred. */
    std::string mDispatching;
};

/** The context of an extension that is loaded or loading, for registrations made on its behalf (from its Python code). */
LILA_SCENE_API ext::Context* contextOf(std::string_view name);

/** loadExtension, unloadExtension, reloadExtension, listExtensions, extensionInfo. */
LILA_SCENE_API void registerCommands();

/** Startup: load the extensions that autoload. After python::init so Python extensions can load. */
LILA_SCENE_API void autoload();
} // namespace lila::extensions
