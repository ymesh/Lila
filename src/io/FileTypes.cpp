#include "io/FileTypes.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "io/FileTranslators.h"
#include "ops/Command.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <set>
#include <tuple>

#ifdef _WIN32
#include <windows.h>
#include <shlobj.h>
#endif

namespace lila::io::fileTypes
{
using json = nlohmann::json;

namespace
{
/** The ext key value holding the default Lila replaced, to hand back on unregister. */
constexpr const char* kPrevious = "Lila.PreviousDefault";

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string normalizedExt(std::string ext)
{
    ext = lower(std::move(ext));
    return ext.empty() || ext[0] == '.' ? ext : "." + ext;
}

/** HKCU\Software\Classes\Applications\<exe name>: what lists Lila under "Open with". */
std::string appKey(const fs::path& exe)
{
    return "Applications\\" + paths::toUtf8(exe.filename());
}

/** Sets a value unless it already holds it. */
bool put(Registry& r, const std::string& key, const std::string& name, const std::string& value)
{
    if (r.get(key, name) == value)
        return false;
    r.set(key, name, value);
    return true;
}

/** Native separators, as Explorer expects in commands and icon paths. */
std::string nativeUtf8(const fs::path& p)
{
    std::u8string s = fs::path(p).make_preferred().lexically_normal().u8string();
    return std::string(s.begin(), s.end());
}

bool drop(Registry& r, const std::string& key, const std::string& name)
{
    if (!r.get(key, name))
        return false;
    r.removeValue(key, name);
    return true;
}

#ifdef _WIN32
std::wstring wide(const std::string& s)
{
    return paths::fromUtf8(s).wstring();
}

class UserClasses : public Registry
{
public:
    std::optional<std::string> get(const std::string& key, const std::string& name) const override
    {
        DWORD bytes = 0;
        const std::wstring k = wide(kRoot + key), n = wide(name);
        if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), n.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_NONE, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
            return std::nullopt;
        std::wstring data(bytes / sizeof(wchar_t) + 1, L'\0');
        if (RegGetValueW(HKEY_CURRENT_USER, k.c_str(), n.c_str(), RRF_RT_REG_SZ | RRF_RT_REG_NONE, nullptr, data.data(), &bytes) != ERROR_SUCCESS)
            return std::nullopt;
        data.resize(wcsnlen(data.c_str(), data.size()));
        // Native form: these values hold commands and icon paths, which must read back exactly as they were written.
        const std::u8string utf8 = fs::path(data).u8string();
        return std::string(utf8.begin(), utf8.end());
    }

    void set(const std::string& key, const std::string& name, const std::string& value) override
    {
        HKEY h = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, wide(kRoot + key).c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &h, nullptr) != ERROR_SUCCESS)
            throw std::runtime_error("can't write HKCU\\" + kRoot + key);
        const std::wstring v = wide(value);
        const LONG rc = RegSetValueExW(h, wide(name).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), DWORD((v.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(h);
        if (rc != ERROR_SUCCESS)
            throw std::runtime_error("can't write HKCU\\" + kRoot + key);
    }

    void removeValue(const std::string& key, const std::string& name) override
    {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, wide(kRoot + key).c_str(), wide(name).c_str());
    }

    void removeKey(const std::string& key) override { RegDeleteTreeW(HKEY_CURRENT_USER, wide(kRoot + key).c_str()); }

private:
    inline static const std::string kRoot = "Software\\Classes\\";
};
#endif
} // namespace

std::vector<Format> formats()
{
    std::vector<Format> out = {{".lila", "Lila scene", true}, {".lilaa", "Lila text scene", true}};
    std::set<std::string> seen = {".lila", ".lilaa"};
    for (const translators::FileTranslator* t : translators::supporting(translators::Op::Open))
        for (const std::string& ext : t->extensions)
            if (seen.insert(lower(ext)).second)
                out.push_back({lower(ext), t->label, false});
    return out;
}

std::string progId(const std::string& ext)
{
    return "Lila" + normalizedExt(ext);
}

std::optional<std::string> MemoryRegistry::get(const std::string& key, const std::string& name) const
{
    auto it = values.find({lower(key), name});
    return it == values.end() ? std::nullopt : std::optional(it->second);
}

void MemoryRegistry::set(const std::string& key, const std::string& name, const std::string& value)
{
    values[{lower(key), name}] = value;
}

void MemoryRegistry::removeValue(const std::string& key, const std::string& name)
{
    values.erase({lower(key), name});
}

void MemoryRegistry::removeKey(const std::string& key)
{
    const std::string k = lower(key);
    std::erase_if(values, [&](const auto& v) { return v.first.first == k || v.first.first.starts_with(k + "\\"); });
}

std::unique_ptr<Registry> userClasses()
{
#ifdef _WIN32
    return std::make_unique<UserClasses>();
#else
    return nullptr;
#endif
}

bool registerType(Registry& r, const fs::path& exe, const Format& f)
{
    const std::string ext = normalizedExt(f.ext), id = progId(ext), path = nativeUtf8(exe);
    const std::string command = "\"" + path + "\" \"%1\"";
    bool changed = false;
    changed |= put(r, id, "", f.description);
    changed |= put(r, id, "FriendlyTypeName", f.description);
    changed |= put(r, id + "\\DefaultIcon", "", path + ",0");
    changed |= put(r, id + "\\shell\\open\\command", "", command);
    changed |= put(r, ext + "\\OpenWithProgids", id, "");
    const std::string app = appKey(exe);
    changed |= put(r, app, "FriendlyAppName", "Lila");
    changed |= put(r, app + "\\shell\\open\\command", "", command);
    changed |= put(r, app + "\\SupportedTypes", ext, "");
    if (std::optional<std::string> was = r.get(ext, ""); was != id)
    {
        if (was && !was->empty())
            r.set(ext, kPrevious, *was);
        r.set(ext, "", id);
        changed = true;
    }
    return changed;
}

bool unregisterType(Registry& r, const fs::path& exe, const std::string& extension)
{
    const std::string ext = normalizedExt(extension), id = progId(ext);
    bool changed = false;
    if (r.get(ext, "") == id)
    {
        if (std::optional<std::string> previous = r.get(ext, kPrevious))
            r.set(ext, "", *previous);
        else
            r.removeValue(ext, "");
        changed = true;
    }
    changed |= drop(r, ext, kPrevious);
    changed |= drop(r, ext + "\\OpenWithProgids", id);
    changed |= drop(r, appKey(exe) + "\\SupportedTypes", ext);
    if (r.get(id, ""))
    {
        r.removeKey(id);
        changed = true;
    }
    return changed;
}

bool isRegistered(const Registry& r, const std::string& ext)
{
    return r.get(normalizedExt(ext), "") == progId(ext);
}

bool sync(Registry& r, const fs::path& exe, bool scenes, const std::vector<std::string>& imports)
{
    std::set<std::string> wanted;
    for (const std::string& e : imports)
        wanted.insert(normalizedExt(e));
    bool changed = false, any = false;
    for (const Format& f : formats())
        if (f.scene ? scenes : wanted.contains(f.ext))
        {
            changed |= registerType(r, exe, f);
            any = true;
        }
        else
            changed |= unregisterType(r, exe, f.ext);
    if (!any && r.get(appKey(exe), "FriendlyAppName"))
    {
        r.removeKey(appKey(exe));
        changed = true;
    }
    return changed;
}

bool scenesByDefault(const fs::path& exeDir)
{
    // A source build is a developer's own; registering it would point Explorer at a build folder.
    std::error_code ec;
    fs::path dir = exeDir;
    for (int up = 0; up < 4 && !dir.empty(); ++up, dir = dir.parent_path())
    {
        if (fs::exists(dir / "CMakeCache.txt", ec))
            return false;
        if (dir == dir.parent_path())
            break;
    }
    return true;
}

namespace
{
bool scenesPref()
{
    const json v = PrefStore::get().value("general", kScenesPref);
    return v.is_boolean() ? v.get<bool>() : scenesByDefault(paths::executableDir());
}

std::vector<std::string> importsPref()
{
    std::vector<std::string> out;
    for (const json& e : PrefStore::get().value("general", kImportsPref, json::array()))
        if (e.is_string())
            out.push_back(e.get<std::string>());
    return out;
}

fs::path guiExe()
{
#ifdef _WIN32
    // lilabatch registers the GUI next to it, so double-clicking opens a window.
    return paths::executableDir() / "lila.exe";
#else
    return paths::executableDir() / "lila";
#endif
}

bool disabledByEnv()
{
    const char* v = std::getenv("LILA_NO_FILE_TYPES");
    return v && *v && std::string_view(v) != "0";
}
} // namespace

void applyPrefs(bool force)
{
    if (disabledByEnv())
        return;
    // Most "general" pref writes have nothing to do with file types; only a change to what is registered syncs.
    static std::optional<std::tuple<fs::path, bool, std::vector<std::string>>> applied;
    auto wanted = std::make_tuple(guiExe(), scenesPref(), importsPref());
    if (!force && applied == wanted)
        return;
    std::unique_ptr<Registry> r = userClasses();
    if (!r)
        return;
    try
    {
        const bool changed = sync(*r, std::get<0>(wanted), std::get<1>(wanted), std::get<2>(wanted));
        applied = std::move(wanted);
        if (changed)
        {
            notifyShell();
            log::info("file types updated");
        }
    }
    catch (const std::exception& e)
    {
        log::warn("file types: {}", e.what());
    }
}

void notifyShell()
{
#ifdef _WIN32
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#endif
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "fileTypes",
        .doc = "The file types Lila can open from Explorer: extension, description, whether it is a scene format, whether "
               "it is ticked in the prefs and whether Explorer opens it with Lila now.",
        .undoable = false,
        .run = [](const Args&) -> json {
            std::unique_ptr<Registry> r = userClasses();
            const bool scenes = scenesPref();
            const std::vector<std::string> imports = importsPref();
            json out = json::array();
            for (const Format& f : formats())
                out.push_back({{"extension", f.ext},
                               {"description", f.description},
                               {"scene", f.scene},
                               {"wanted", f.scene ? scenes : std::ranges::find(imports, f.ext) != imports.end()},
                               {"registered", r && isRegistered(*r, f.ext)}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "registerFileTypes",
        .doc = "Make Explorer open Lila's scenes (.lila, .lilaa) and the given import formats with this Lila, for the "
               "current user. Import formats left out are handed back to what opened them before. Saved in the prefs, "
               "so later starts keep it. dryRun returns the registry values instead of writing them.",
        .positionalDoc = "[extension...]",
        .flags = {{"scenes", "", T::Bool, true, "Register .lila and .lilaa."},
                  {"dryRun", "", T::Bool, false, "Return {key: {name: value}} for HKCU\\Software\\Classes without writing."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::vector<std::string> imports;
            for (const json& e : a.positional)
                if (e.is_string())
                    imports.push_back(normalizedExt(e.get<std::string>()));
            const bool scenes = a.get<bool>("scenes", true);
            if (a.get<bool>("dryRun", false))
            {
                MemoryRegistry m;
                sync(m, guiExe(), scenes, imports);
                json out = json::object();
                for (const auto& [where, value] : m.values)
                    out[where.first][where.second] = value;
                return out;
            }
            PrefStore::get().set("general", kScenesPref, scenes);
            PrefStore::get().set("general", kImportsPref, imports);
            if (disabledByEnv() || !userClasses())
                throw CommandError("registerFileTypes: not available here (LILA_NO_FILE_TYPES, or not Windows)");
            applyPrefs(true);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "unregisterFileTypes",
        .doc = "Stop Explorer opening any file type with Lila, for the current user, restoring the previous handlers. "
               "Saved in the prefs, so later starts don't register them again.",
        .undoable = false,
        .run = [](const Args&) -> json {
            PrefStore::get().set("general", kScenesPref, false);
            PrefStore::get().set("general", kImportsPref, json::array());
            applyPrefs(true);
            return nullptr;
        },
    });
}
} // namespace lila::io::fileTypes
