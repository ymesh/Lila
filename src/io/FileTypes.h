#pragma once

#include "core/Export.h"

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/**
 * File type registration: Explorer's icon, double-click and "Open with" for Lila's scenes and, when ticked, the formats
 * Lila imports. Per user (HKCU\Software\Classes), so it needs no admin rights and works for installer, Steam and source
 * builds alike. Scenes are Lila's by default; an import format only becomes Lila's when the user ticks it, and giving it
 * up again hands the extension back to whatever opened it before.
 */
namespace lila::io::fileTypes
{
namespace fs = std::filesystem;

/** Whether .lila and .lilaa open with Lila. Unset, on for installer and Steam builds, off for source builds. */
inline constexpr const char* kScenesPref = "/files/registerScenes";
/** Import formats ticked to open with Lila, as extensions with the dot: [".fbx", ".obj"]. */
inline constexpr const char* kImportsPref = "/files/registerImports";

struct Format
{
    std::string ext; // ".fbx"
    std::string description;
    bool scene = false;
};
/** .lila and .lilaa, then every other extension File > Open reads, from the translators. */
LILA_SCENE_API std::vector<Format> formats();

/** The ProgID Lila registers ext under: "Lila.fbx". */
LILA_SCENE_API std::string progId(const std::string& ext);

/** Registry access below one root key. Keys are '\\' separated and relative to it; "" names a key's default value. */
class Registry
{
public:
    virtual ~Registry() = default;
    virtual std::optional<std::string> get(const std::string& key, const std::string& name) const = 0;
    virtual void set(const std::string& key, const std::string& name, const std::string& value) = 0;
    virtual void removeValue(const std::string& key, const std::string& name) = 0;
    /** The key and everything under it. */
    virtual void removeKey(const std::string& key) = 0;
};

/** HKCU\Software\Classes; null off Windows. */
LILA_SCENE_API std::unique_ptr<Registry> userClasses();

/** A registry in memory, for tests and dry runs. Keys compare case-insensitively, as Windows' do. */
class LILA_SCENE_API MemoryRegistry : public Registry
{
public:
    std::optional<std::string> get(const std::string& key, const std::string& name) const override;
    void set(const std::string& key, const std::string& name, const std::string& value) override;
    void removeValue(const std::string& key, const std::string& name) override;
    void removeKey(const std::string& key) override;

    /** (lowercased key, value name) -> data. */
    std::map<std::pair<std::string, std::string>, std::string> values;
};

/**
 * Makes ext open with exe: its ProgID with the icon and open command, the extension's "Open with" entry and, so a
 * double-click opens Lila, the extension's default (the previous one is kept to hand back). True when anything changed.
 */
LILA_SCENE_API bool registerType(Registry& r, const fs::path& exe, const Format& f);
/** Removes what registerType wrote and restores the previous default if Lila still holds it. True when anything changed. */
LILA_SCENE_API bool unregisterType(Registry& r, const fs::path& exe, const std::string& ext);
/** Whether ext's default opens Lila's ProgID. */
LILA_SCENE_API bool isRegistered(const Registry& r, const std::string& ext);

/**
 * Registers the scene formats when scenes is set and the ticked imports, and unregisters every other format Lila
 * registered before. True when anything changed (Explorer then needs telling, see notifyShell).
 */
LILA_SCENE_API bool sync(Registry& r, const fs::path& exe, bool scenes, const std::vector<std::string>& imports);

/** Whether this build registers scenes when the pref is unset: installer and Steam builds yes, source builds no. */
LILA_SCENE_API bool scenesByDefault(const fs::path& exeDir);

/**
 * Applies the prefs to HKCU for this executable. Does nothing off Windows or with LILA_NO_FILE_TYPES set (test
 * instances). Called at GUI startup and when the prefs change.
 */
LILA_SCENE_API void applyPrefs(bool force = false);

/** Tells Explorer associations changed, so icons refresh. */
LILA_SCENE_API void notifyShell();

/** fileTypes, registerFileTypes, unregisterFileTypes. */
LILA_SCENE_API void registerCommands();
} // namespace lila::io::fileTypes
