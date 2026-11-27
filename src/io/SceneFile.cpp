#include "io/SceneFile.h"
#include "core/Release.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "core/PackedArray.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "core/Version.h"
#include "core/Workspace.h"
#include "io/FileTranslators.h"
#include "io/References.h"
#include "io/SceneFormats.h"
#include "io/SceneHistory.h"
#include "scene/Anim.h"
#include "scene/Camera.h"
#include "scene/Graph.h"
#include "scene/MeshData.h"
#include "scene/Picker.h"
#include "scene/Poses.h"
#include "scene/RenderSettings.h"
#include "scene/SceneData.h"
#include "scene/SceneScripts.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <future>
#include <map>
#include <fstream>
#include <optional>
#include <random>

#include <tbb/parallel_for.h>
#include <sstream>
#include <thread>

namespace lila::io
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::NodeId;

namespace
{
constexpr int kVersion = 1;

/** .lilaa is ASCII; everything else is binary. */
void encodeFor(const fs::path& path, json doc, const std::function<void(std::string_view)>& sink)
{
    if (path.extension() != ".lilaa")
        return encodeBinary(std::move(doc), sink);
    // Attr values stay packed: the text writer formats them directly, the rest of the document has none.
    sink(encodeAscii(doc));
}

struct State
{
    fs::path path;
    bool modified = false;
    bool autosaveDirty = false;
    bool loading = false;
    bool evaluateOnLoad = false;
    int opening = 0; // load() calls running
    std::filesystem::path loadingPath;
    uint64_t cleanStateId = 0;
    bool untracked = false; // changed outside the undo stack, so no undo can return to the saved state
    std::optional<uint64_t> autosavedStateId; // undo state the last autosave wrote, while no untracked edit followed
    json cleanUi = json::object(); // UI sections as of the last save/open/new, so moving a camera back undoes the change
    bool uiDirty = false;
    SceneOrigin origin;

    int listener = 0;
    std::string title;
    std::vector<std::function<void()>> callbacks;
    std::vector<std::function<void(const fs::path&)>> savedCallbacks;

    struct UiSection
    {
        std::function<json()> save;
        std::function<void(const json&)> load;
    };
    std::map<std::string, UiSection> uiSections;
    json ui = json::object(); // the open scene's "ui" document, unregistered sections included
};

State& state()
{
    static State s;
    return s;
}

void notify()
{
    for (auto& fn : state().callbacks)
        fn();
    events::send({.type = events::Event::SceneStateChanged});
}

/** The import and export commands, to the event bus with the file they read or wrote. */
void forwardFileEvents(const ExecuteEvent& e)
{
    static const std::map<std::string_view, events::Event> kinds = {
        {"importFbx", events::Event::SceneImported},  {"importObj", events::Event::SceneImported},
        {"importGltf", events::Event::SceneImported},  {"importStl", events::Event::SceneImported},
        {"importPly", events::Event::SceneImported},  {"importAlembic", events::Event::SceneImported},
        {"mayaImport", events::Event::SceneImported}, {"blendImport", events::Event::SceneImported}, {"importReference", events::Event::SceneImported},
        {"exportFbx", events::Event::SceneExported},  {"exportObj", events::Event::SceneExported},
        {"exportGltf", events::Event::SceneExported},  {"exportStl", events::Event::SceneExported},
        {"exportPly", events::Event::SceneExported},
        {"gameExport", events::Event::SceneExported},
    };
    if (!e.done || !e.ok)
        return;
    auto it = kinds.find(e.name);
    if (it == kinds.end() || !events::hasSubscribers(it->second))
        return;
    events::EventData d{.type = it->second};
    if (const json& args = e.call.value("args", json::array()); args.is_array() && !args.empty() && args[0].is_string())
        d.path = fs::path(std::u8string(reinterpret_cast<const char8_t*>(args[0].get_ref<const std::string&>().c_str())));
    events::send(d);
}

json currentUiSection(const std::string& key)
{
    auto it = state().uiSections.find(key);
    return it == state().uiSections.end() ? json() : it->second.save();
}

void snapshotUi()
{
    State& s = state();
    s.cleanUi = json::object();
    for (auto& [key, section] : s.uiSections)
        s.cleanUi[key] = section.save();
    s.uiDirty = false;
}

void setModified(bool m)
{
    State& s = state();
    if (m)
        s.autosaveDirty = true;
    if (s.modified == m)
        return;
    s.modified = m;
    notify();
}

void ensureListener()
{
    State& s = state();
    if (s.listener)
        return;
    s.listener = Graph::current().addListener([](scene::Change c, NodeId) {
        if (state().loading || c == scene::Change::Selection)
            return;
        if (c == scene::Change::Cleared)
        {
            setCurrentPath({});
            state().origin = {};
            state().ui = json::object();
            refs::clear();
            scene::renderSettings().fromJson({});
            scene::renderSettingsChanged();
            scene::scripts::clear();
            scene::data::clear();
            scene::poses::clearScene();
            scene::picker::clearScene();
        }
        if (c == scene::Change::Cleared)
        {
            snapshotUi();
            return setModified(false);
        }
        UndoStack& u = UndoStack::get();
        if (u.replaying())
        {
            state().autosaveDirty = true;
            return;
        }
        if (!u.enabled())
            state().untracked = true;
        setModified(true);
    }, true);
    scene::addRenderSettingsListener([] {
        if (state().loading || UndoStack::get().replaying())
            return;
        state().untracked = true;
        setModified(true);
    });
    UndoStack::get().addListener([] {
        State& s = state();
        if (!s.loading)
            setModified(s.untracked || s.uiDirty || UndoStack::get().stateId() != s.cleanStateId);
    });
}

using Sink = std::function<void(std::string_view)>;

/**
 * Writes path.tmp through fill(sink), then swaps it in. beforeReplace runs between the two, for work that must finish
 * before the old file goes away.
 */
void writeAtomic(const fs::path& path, const std::function<void(const Sink&)>& fill, const std::function<void()>& beforeReplace = {})
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw std::runtime_error("can't write " + tmp.string());
        fill([&](std::string_view piece) {
            // In slices: MSVC's streams stop a single write at 2 GB.
            constexpr size_t kSlice = size_t(64) << 20;
            for (size_t at = 0; at < piece.size(); at += kSlice)
                if (!out.write(piece.data() + at, std::streamsize(std::min(kSlice, piece.size() - at))))
                    throw std::runtime_error("can't write " + tmp.string());
        });
        if (!out.flush())
            throw std::runtime_error("can't write " + tmp.string());
    }
    if (beforeReplace)
        beforeReplace();
    fs::rename(tmp, path, ec);
    if (ec)
        throw std::runtime_error("can't replace " + path.string() + ": " + ec.message());
}

void writeAtomic(const fs::path& path, const std::string& text)
{
    writeAtomic(path, [&](const Sink& sink) { sink(text); });
}

/** Depth-first so every parent is written before its children. */
void collectDag(Graph& g, NodeId id, std::vector<NodeId>& out)
{
    out.push_back(id);
    for (NodeId c : g.children(id))
        collectDag(g, c, out);
}
} // namespace

namespace
{
nlohmann::json serializeScene(const fs::path& sceneDir, bool packed, std::vector<DeferredMesh>* deferred)
{
    LILA_ZONE();

    Graph& g = Graph::current();
    std::vector<NodeId> order;
    for (NodeId r : g.roots())
        collectDag(g, r, order);
    for (NodeId id : g.nodes())
        if (!g.node(id)->type().dag)
            order.push_back(id);

    // Values first, all at once, so meshes convert in parallel.
    std::erase_if(order, [&](NodeId id) { return g.node(id)->referenceId() != 0; }); // saved as the reference plus its edits
    std::vector<const scene::PlugValue*> values;
    std::vector<std::vector<int>> stored(order.size());
    for (size_t k = 0; k < order.size(); ++k)
    {
        scene::Node* n = g.node(order[k]);
        const auto& defs = n->type().attrs;
        for (int i = 0; i < int(defs.size()); ++i)
        {
            const scene::AttrDef& a = defs[i];
            if (!(a.flags & scene::AttrStorable) || !(a.flags & scene::AttrInput) || (a.flags & scene::AttrArray))
                continue;
            if (g.source({order[k], i, -1}))
                continue;
            if (n->value(i) != a.storedDefault())
            {
                stored[k].push_back(i);
                values.push_back(&n->value(i));
            }
        }
    }
    // Deferred meshes stay out of the conversion; the caller's fillMeshes writes them.
    std::vector<scene::MeshPtr> later(values.size());
    std::vector<const scene::PlugValue*> now;
    for (size_t v = 0; v < values.size(); ++v)
    {
        const scene::MeshPtr* mesh = deferred ? std::get_if<scene::MeshPtr>(values[v]) : nullptr;
        if (mesh && *mesh)
            later[v] = *mesh;
        else
            now.push_back(values[v]);
    }
    std::vector<json> convertedNow = scene::toJsonAll(now, packed);
    std::vector<json> converted(values.size());
    for (size_t v = 0, c = 0; v < values.size(); ++v)
        if (!later[v])
            converted[v] = std::move(convertedNow[c++]);

    const fs::path dir = sceneDir.empty() ? paths::currentSceneDir() : sceneDir;
    json nodes = json::array();
    size_t next = 0;
    for (size_t k = 0; k < order.size(); ++k)
    {
        const NodeId id = order[k];
        scene::Node* n = g.node(id);
        json attrs = json::object();
        for (int i : stored[k])
        {
            const scene::AttrDef& a = n->type().attrs[i];
            if (scene::MeshPtr& mesh = later[next]; mesh)
                deferred->push_back({nodes.size(), a.name, std::move(mesh)});
            json& v = attrs[a.name] = std::move(converted[next++]);
            if ((a.flags & scene::AttrFile) && v.is_string())
                v = paths::storedPath(paths::fromUtf8(v.get<std::string>()), dir);
        }
        json node = {{"name", n->name()}, {"type", n->type().name}};
        if (NodeId p = g.parent(id); p != scene::kNoNode)
            node["parent"] = g.fullPath(p);
        json user = json::array();
        for (const scene::AttrDef& a : n->type().attrs)
            if (a.flags & scene::AttrUser)
                user.push_back(scene::attrDefToJson(a));
        if (!user.empty())
            node["userAttrs"] = std::move(user);
        for (int i = 0; i < int(n->type().attrs.size()); ++i)
            if (const scene::AttrDef& a = n->type().attrs[size_t(i)];
                (a.flags & scene::AttrArray) && (a.flags & scene::AttrStorable) && (a.flags & scene::AttrInput))
            {
                json els = json::object();
                for (const auto& [index, value] : n->elements(i))
                    if (!g.source({id, i, -1, index}))
                        els[std::to_string(index)] = scene::toJson(value);
                if (!els.empty())
                    attrs[a.name] = std::move(els);
            }
        if (!attrs.empty())
            node["attrs"] = std::move(attrs);
        json locked = json::array();
        for (int i = 0; i < int(n->type().attrs.size()); ++i)
            if (uint8_t mask = n->lockMask(i))
            {
                const scene::AttrDef& a = n->type().attrs[i];
                if (a.type != scene::PlugType::Vec3)
                    locked.push_back(a.name);
                else
                    for (int c = 0; c < 3; ++c)
                        if (mask & (1u << c))
                            locked.push_back(a.childNames[c]);
            }
        if (!locked.empty())
            node["locked"] = std::move(locked);
        nodes.push_back(std::move(node));
    }

    json connections = json::array();
    for (auto& [dst, src] : g.connections())
        if (std::string from = g.fullPlugName(src), to = g.fullPlugName(dst); !refs::isBaselineConnection(from, to))
            connections.push_back({from, to});

    json selection = json::array();
    for (NodeId id : g.selection())
        selection.push_back(g.fullPath(id));

    json doc = {
        {"lila", kVersion},
        {"writer", {{"version", version::number()}, {"build", version::buildId()}}},
        {"upAxis", PrefStore::get().value<std::string>("general", "/scene/upAxis", "y")},
        {"units", "cm"},
        {"time", g.time()},
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
        {"selection", std::move(selection)},
        {"playback", scene::anim::playback().toJson()},
        {"renderSettings", scene::renderSettings().toJson()},
        {"references", refs::serialize(dir)},
    };
    if (!scene::scripts::all().empty())
        doc["scripts"] = scene::scripts::toJson();
    if (!scene::data::all().empty())
        doc["data"] = scene::data::toJson();
    if (json poses = scene::poses::sceneToJson(); !poses.empty())
        doc["poses"] = std::move(poses);
    if (json pickers = scene::picker::sceneToJson(); !pickers.empty())
        doc["pickers"] = std::move(pickers);
    json ui = state().ui;
    for (auto& [key, section] : state().uiSections)
        if (json v = section.save(); v.is_null())
            ui.erase(key);
        else
            ui[key] = std::move(v);
    if (!ui.empty())
        doc["ui"] = std::move(ui);
    return doc;
}

} // namespace

nlohmann::json serialize(const fs::path& sceneDir, bool packed)
{
    return serializeScene(sceneDir, packed, nullptr);
}

nlohmann::json serializeDeferred(std::vector<DeferredMesh>& meshes)
{
    return serializeScene({}, true, &meshes);
}

void fillMeshes(nlohmann::json& doc, std::vector<DeferredMesh>& meshes)
{
    LILA_ZONE();

    std::vector<json> converted(meshes.size());
    tbb::parallel_for(size_t(0), meshes.size(), [&](size_t k) { converted[k] = scene::meshToJsonPacked(*meshes[k].mesh); });
    for (size_t k = 0; k < meshes.size(); ++k)
        doc["nodes"][meshes[k].node]["attrs"][meshes[k].attr] = std::move(converted[k]);
    meshes.clear();
}

void deserialize(const nlohmann::json& doc, const fs::path& sceneDir, double ownBytes)
{
    LILA_ZONE();

    if (!doc.is_object() || !doc.contains("lila") || !doc["lila"].is_number_integer())
        throw std::runtime_error("not a Lila scene");

    Graph& g = Graph::current();
    State& s = state();
    s.loading = true;
    // Nodes that evaluate while loading resolve their relative paths against this scene's folder.
    const fs::path previousDir = paths::currentSceneDir();
    paths::setCurrentSceneDir(sceneDir);
    std::vector<std::string> warnings;
    const json references = doc.value("references", json::array());
    const double refBytes = refs::loadedBytes(references, sceneDir);
    progress::Scope scope("", refBytes + (ownBytes > 0.0 ? ownBytes : 1e6));
    Graph::Batch batch(g);
    try
    {
        g.clear();
        UndoStack::get().setEnabled(false);
        // References first, so scene nodes parented under referenced ones find their parents.
        refs::deserialize(references, sceneDir, warnings, refBytes);
        {
            progress::Scope own("", 0.0, progress::None, ownBytes > 0.0 ? ownBytes : 1e6);
            instantiate(doc, "", 0, sceneDir, warnings);
        }
        refs::resolvePendingParents(warnings);

        LILA_ZONE_N("load.settings");
        std::vector<NodeId> sel;
        for (const json& n : doc.value("selection", json::array()))
            try
            {
                if (scene::Node* node = g.find(n.get<std::string>()))
                    sel.push_back(node->id());
            }
            catch (const std::exception& e)
            {
                warnings.push_back(std::string("selection: ") + e.what());
            }
        g.select(sel);
        g.setTime(doc.value("time", 0.0));
        if (doc.contains("playback"))
        {
            scene::anim::playback().fromJson(doc["playback"]);
            scene::anim::playbackChanged();
        }
        scene::renderSettings().fromJson(doc.value("renderSettings", json::object()));
        scene::camera::migrateNamedSettings(g, scene::renderSettings());
        scene::renderSettingsChanged();
        scene::scripts::fromJson(doc.value("scripts", json::array()));
        {
            LILA_ZONE_N("load.data");
            scene::data::fromJson(doc.value("data", json::object()));
        }
        scene::poses::sceneFromJson(doc.value("poses", json::array()));
        scene::picker::sceneFromJson(doc.value("pickers", json::array()));
        if (s.evaluateOnLoad)
        {
            LILA_ZONE_N("load.evaluate");
            g.evaluateUpstream(g.nodes());
        }
    }
    catch (...)
    {
        UndoStack::get().setEnabled(true);
        s.loading = false;
        paths::setCurrentSceneDir(previousDir);
        // Never part of a scene: an aborted load's caller puts the previous one back.
        if (progress::cancelRequested())
        {
            g.clear();
            UndoStack::get().clear();
        }
        throw;
    }
    UndoStack::get().setEnabled(true);
    UndoStack::get().clear();
    s.loading = false;

    json ui = doc.value("ui", json::object());
    s.ui = ui.is_object() ? ui : json::object();
    for (auto& [key, section] : s.uiSections)
        section.load(s.ui.value(key, json()));

    for (const std::string& w : warnings)
        log::warn("{}", w);
}

namespace
{
/** major.minor.patch as numbers; anything after the patch ("-dev", "+sha") is ignored. */
std::array<int, 3> versionParts(std::string_view v)
{
    std::array<int, 3> parts{};
    size_t at = 0;
    for (int& p : parts)
    {
        size_t end = at;
        while (end < v.size() && v[end] >= '0' && v[end] <= '9')
            ++end;
        std::from_chars(v.data() + at, v.data() + end, p);
        if (end >= v.size() || v[end] != '.')
            break;
        at = end + 1;
    }
    return parts;
}

/** Logs who wrote a scene being opened when it isn't this Lila's version. */
SceneOrigin noteOrigin(const json& doc, const std::string& name)
{
    const SceneOrigin o = sceneOrigin(doc);
    const std::string by = o.version.empty() ? "an earlier Lila" : "Lila " + o.version;
    if (o.age == SceneOrigin::Age::Newer)
        log::warn("{} was saved by {} (format {}), newer than this Lila {} (format {}). It opens as far as this version "
                  "understands it; saving it here can drop what the newer version added.",
                  name, by, o.format, version::number(), kVersion);
    else if (o.age == SceneOrigin::Age::Older)
        log::info("{} was saved by {}; saving updates it to Lila {}", name, by, version::number());
    return o;
}
} // namespace

SceneSnapshot::SceneSnapshot()
{
    const State& s = state();
    mPath = s.path;
    mTitle = s.title;
    mOrigin = s.origin;
    mModified = s.modified;
    mDir = paths::currentSceneDir();
    mDoc = serialize(mDir, true);
}

void SceneSnapshot::restore()
{
    // Out from under the cancelled operation's scope, whose cancel would stop the rebuild too.
    progress::Adopt detached(nullptr);
    deserialize(mDoc, mDir);
    State& s = state();
    setCurrentPath(mPath);
    s.title = mTitle;
    s.origin = mOrigin;
    markClean();
    if (mModified)
        markModified();
    log::info("open aborted; {} is back", displayName());
}

namespace
{
/** Runs build, which replaces the scene; when it is cancelled the scene as it was before comes back. */
void restoreIfCancelled(const std::function<void()>& build)
{
    SceneSnapshot previous;
    try
    {
        build();
    }
    catch (...)
    {
        if (progress::cancelRequested())
            previous.restore();
        throw;
    }
}
} // namespace

int formatVersion()
{
    return kVersion;
}

SceneOrigin sceneOrigin(const json& doc, std::string_view running)
{
    SceneOrigin o;
    o.format = doc.is_object() ? doc.value("lila", 0) : 0;
    if (const json* w = doc.is_object() && doc.contains("writer") ? &doc["writer"] : nullptr; w && w->is_object())
    {
        o.version = w->value("version", "");
        o.build = w->value("build", "");
    }
    const std::array<int, 3> mine = versionParts(running.empty() ? std::string_view(version::number()) : running);
    if (o.format > kVersion || (!o.version.empty() && versionParts(o.version) > mine))
        o.age = SceneOrigin::Age::Newer;
    else if (o.format < kVersion || o.version.empty() || versionParts(o.version) < mine)
        o.age = SceneOrigin::Age::Older;
    return o;
}

const SceneOrigin& openedOrigin()
{
    return state().origin;
}

void registerUiSection(const std::string& key, std::function<json()> save, std::function<void(const json&)> load)
{
    state().uiSections[key] = {std::move(save), std::move(load)};
}

bool registerUiSection(ext::Context& ctx, const std::string& key, std::function<json()> save, std::function<void(const json&)> load)
{
    if (state().uiSections.contains(key))
    {
        log::error("extension {}: scene ui section '{}' is already registered", ctx.owner(), key);
        return false;
    }
    registerUiSection(key, std::move(save), std::move(load));
    ctx.track("sceneUiSection:" + key, [key] { unregisterUiSection(key); });
    return true;
}

bool unregisterUiSection(const std::string& key)
{
    State& s = state();
    auto it = s.uiSections.find(key);
    if (it == s.uiSections.end())
        return false;
    // Kept as an unregistered section, so saving without its owner still writes what it last held.
    if (json v = it->second.save(); v.is_null())
        s.ui.erase(key);
    else
        s.ui[key] = std::move(v);
    s.uiSections.erase(it);
    return true;
}

void saveCopy(const fs::path& path)
{
    LILA_ZONE();

    json doc = serialize(path.parent_path(), true);
    progress::runOnWorker([&] { writeAtomic(path, [&](const Sink& sink) { encodeFor(path, std::move(doc), sink); }, [] {}); });
}

void save(const fs::path& path)
{
    LILA_ZONE();

    events::send({.type = events::Event::SceneBeforeSave, .path = path});
    progress::Scope scope(std::format("Saving {}...", path.filename().string()), 3.0);
    // The backup copies the file as it is now, so it runs alongside gathering and encoding the new one.
    const int keep = PrefStore::get().value<int>("general", "/backups/keep", 10);
    std::future<void> backup;
    if (keep > 0)
        backup = std::async(std::launch::async, [path, folder = history::backupFolder(path), keep] {
            history::backup(path, folder, keep);
        });
    json doc;
    std::vector<DeferredMesh> meshes;
    {
        progress::Scope s("Gathering the scene");
        doc = serializeScene(path.parent_path(), true, &meshes);
    }
    progress::Scope s("Writing " + path.filename().string(), 0.0, progress::None, 2.0);
    // Mesh conversion runs with the encode on the worker, so the progress card shows and the window repaints meanwhile.
    progress::runOnWorker([&] {
        fillMeshes(doc, meshes);
        writeAtomic(path, [&](const Sink& sink) { encodeFor(path, std::move(doc), sink); }, [&] {
            if (backup.valid())
                backup.get();
        });
    });
    setCurrentPath(path);
    state().origin = {};
    markClean();
    log::info("saved {}", path.string());
    for (auto& fn : state().savedCallbacks)
        fn(path);
    events::send({.type = events::Event::SceneSaved, .path = path});
}

void load(const fs::path& path)
{
    LILA_ZONE();

    struct Opening
    {
        explicit Opening(const fs::path& p) : was(std::exchange(state().loadingPath, p)) { ++state().opening; }
        ~Opening()
        {
            --state().opening;
            state().loadingPath = was;
        }
        fs::path was;
    } opening(path);
    const std::string name = path.filename().string();
    progress::Scope scope(loadingVerb() + "...", 10.0, progress::Cancellable);
    scope.setStatus(name);
    std::vector<std::string> warnings;
    json doc;
    {
        progress::Scope read("Reading " + name, 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { doc = readSceneFile(path, &warnings); });
    }
    scope.checkCancel();
    for (const std::string& w : warnings)
        log::warn("{}: {}", name, w);
    const SceneOrigin origin = noteOrigin(doc, name);
    {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        progress::Scope build("", 0.0, progress::None, 7.0);
        restoreIfCancelled([&] { deserialize(doc, path.parent_path(), ec ? 0.0 : double(size)); });
        // The document holds a copy of every mesh array, slow enough to free that it goes on its own thread.
        release::later(std::move(doc));
    }
    setCurrentPath(path);
    state().origin = origin;
    markClean();
    if (!Workspace::get().isOpen())
        if (auto ws = Workspace::detect(path))
            Workspace::get().open(*ws);
    log::info("opened {}", path.string());
    scene::scripts::sceneOpened();
    events::send({.type = events::Event::SceneOpened, .path = path});
}

std::string loadingVerb()
{
    static const char* const kVerbs[] = {
        "Perambulating", "Unfurling",          "Assembling",        "Summoning polygons", "Gathering vertices", "Untangling edges",
        "Rehydrating",   "Marshalling meshes", "Coaxing normals",   "Waking the scene",   "Stitching faces",    "Arranging things",
    };
    static std::mt19937 rng{std::random_device{}()};
    return kVerbs[std::uniform_int_distribution<size_t>(0, std::size(kVerbs) - 1)(rng)];
}

const fs::path& currentPath()
{
    return state().path;
}

void setCurrentPath(const fs::path& path)
{
    state().path = path;
    paths::setCurrentSceneDir(path.empty() ? fs::path() : path.parent_path());
    state().title.clear();
    notify();
}

bool loading()
{
    return state().loading || state().opening > 0;
}

void setEvaluateOnLoad(bool on) { state().evaluateOnLoad = on; }

const fs::path& loadingPath()
{
    return state().loadingPath;
}

bool modified()
{
    return state().modified;
}

bool upToDate(const fs::path& path)
{
    std::error_code ec;
    return !state().modified && !path.empty() && !state().path.empty() && fs::equivalent(path, state().path, ec);
}

void markClean()
{
    ensureListener();
    State& s = state();
    s.autosaveDirty = false;
    s.autosavedStateId.reset();
    s.cleanStateId = UndoStack::get().stateId();
    s.untracked = false;
    snapshotUi();
    setModified(false);
}

void onSaved(std::function<void(const fs::path&)> fn)
{
    state().savedCallbacks.push_back(std::move(fn));
}

void onStateChanged(std::function<void()> fn)
{
    ensureListener();
    state().callbacks.push_back(std::move(fn));
}

void autosave()
{
    LILA_ZONE();

    State& s = state();
    // A timer can fire while a long load pumps events; the scene is half built then.
    if (!s.autosaveDirty || progress::modal())
        return;
    s.autosaveDirty = false;
    // Unmodified means the scene matches its saved file, and an equal state id means undo/redo led back to what the
    // last autosave already holds.
    const uint64_t stateId = UndoStack::get().stateId();
    if (!s.modified || (!s.untracked && s.autosavedStateId == stateId))
        return;
    s.autosavedStateId = s.untracked ? std::nullopt : std::optional(stateId);

    fs::path folder = history::autosaveFolder(s.path);
    std::string stem = history::stemFor(s.path);
    int keep = std::max(1, PrefStore::get().value<int>("general", "/autosave/keep", 8));
    std::vector<DeferredMesh> meshes;
    json doc = serializeDeferred(meshes);
    doc["recovery"] = {{"kind", "autosave"}, {"scene", s.path.generic_string()}};
    // Mesh conversion, encoding and the disk write happen off the main thread.
    std::thread([folder, stem, keep, doc = std::move(doc), meshes = std::move(meshes)]() mutable {
        progress::Scope scope("Autosaving", 0.0, progress::Silent);
        try
        {
            fillMeshes(doc, meshes);
            std::string text = encodeBinary(std::move(doc));
            std::error_code ec;
            fs::create_directories(folder, ec);
            writeAtomic(history::uniqueName(folder, stem, history::timestamp(), "autosave"), text);
            history::prune(folder, stem, "autosave", keep);
        }
        catch (const std::exception& e)
        {
            log::warn("autosave failed: {}", e.what());
        }
    }).detach();
}

void uiChanged(const std::string& key)
{
    ensureListener();
    State& s = state();
    if (s.loading)
        return;
    s.uiDirty = currentUiSection(key) != (s.cleanUi.is_object() ? s.cleanUi.value(key, json()) : json());
    if (s.uiDirty)
        s.autosaveDirty = true;
    setModified(s.untracked || s.uiDirty || UndoStack::get().stateId() != s.cleanStateId);
}

void acceptUi(const std::string& key)
{
    State& s = state();
    s.cleanUi[key] = currentUiSection(key);
    uiChanged(key);
}

void markModified()
{
    ensureListener();
    state().untracked = true;
    setModified(true);
}

void loadVersion(const fs::path& file, const fs::path& scene, bool keepPath, const std::string& title)
{
    LILA_ZONE();

    progress::Scope scope(loadingVerb() + "...", 10.0, progress::Cancellable);
    scope.setStatus(file.filename().string());
    std::vector<std::string> warnings;
    json doc;
    {
        progress::Scope read("Reading " + file.filename().string(), 0.0, progress::None, 3.0);
        progress::runOnWorker([&] { doc = readSceneFile(file, &warnings); });
    }
    scope.checkCancel();
    for (const std::string& w : warnings)
        log::warn("{}: {}", file.filename().string(), w);
    const SceneOrigin origin = noteOrigin(doc, file.filename().string());
    {
        std::error_code ec;
        const auto size = fs::file_size(file, ec);
        progress::Scope build("", 0.0, progress::None, 7.0);
        restoreIfCancelled([&] { deserialize(doc, scene.empty() ? file.parent_path() : scene.parent_path(), ec ? 0.0 : double(size)); });
    }
    setCurrentPath(keepPath ? scene : fs::path());
    state().origin = origin;
    if (!keepPath)
    {
        state().title = title;
        // Paths in the version are stored for the scene it came from.
        if (!scene.empty())
            paths::setCurrentSceneDir(scene.parent_path());
    }
    markClean();
    markModified();
    if (!Workspace::get().isOpen() && !scene.empty())
        if (auto ws = Workspace::detect(scene))
            Workspace::get().open(*ws);
    log::info("opened {} as {}", file.string(), keepPath ? scene.string() : title);
    events::send({.type = events::Event::SceneOpened, .path = scene.empty() ? file : scene});
}

std::string displayName()
{
    const State& s = state();
    if (!s.title.empty())
        return s.title;
    return s.path.empty() ? std::string("untitled") : s.path.filename().string();
}

fs::path emergencySave()
{
    try
    {
        State& s = state();
        if (!s.modified)
            return {};
        fs::path folder = history::autosaveFolder(s.path);
        json doc = serialize({}, true);
        doc["recovery"] = {{"kind", "emergency"}, {"scene", s.path.generic_string()}};
        std::error_code ec;
        fs::create_directories(folder, ec);
        fs::path target = history::uniqueName(folder, history::stemFor(s.path), history::timestamp(), "emergency");
        writeAtomic(target, encodeBinary(std::move(doc)));
        return target;
    }
    catch (...)
    {
        return {};
    }
}

void registerCommands()
{
    using T = ArgType;
    ensureListener();
    refs::registerCommands();
    static int fileEvents = ops::addExecuteListener(forwardFileEvents);
    (void)fileEvents;

    ops::registerCommand({
        .name = "saveScene",
        .doc = "Save the scene. With no path, saves to the current file. .lila is binary, .lilaa is text (diffable and "
               "repairable by hand).",
        .positionalDoc = "[path]",
        .flags = {{"ifModified", "im", T::Bool, false, "Skip the write when the file is already up to date: the current "
                                                      "file, still on disk, with no changes since."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            fs::path path = a.positional.empty() ? currentPath() : fs::path(a.positional[0].get<std::string>());
            if (path.empty())
                throw CommandError("saveScene: the scene has no file yet; pass a path");
            if (path.extension().empty())
                path += PrefStore::get().value<std::string>("general", "/scene/defaultFormat", "binary") == "ascii" ? ".lilaa" : ".lila";
            if (a.get<bool>("ifModified") && upToDate(path))
            {
                log::info("No changes to save");
                return path.generic_string();
            }
            try
            {
                save(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return path.generic_string();
        },
    });

    ops::registerCommand({
        .name = "openScene",
        .doc = "Open a .lila or .lilaa scene, replacing the current one. Other formats File > Open takes (.ma, .fbx, "
               ".obj, ...) go to their translator: import-only ones open as a new untitled scene. Not undoable.",
        .positionalDoc = "path",
        .flags = {{"force", "f", T::Bool, false, "Discard unsaved changes without asking."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("openScene: path required");
            fs::path path(a.positional[0].get<std::string>());
            if (const translators::FileTranslator* t = translators::forPath(translators::Op::Open, path); t && t->id != "lila")
                return translators::run(translators::Op::Open, path, json::object(), CallSource::Script, t->id);
            try
            {
                load(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return currentPath().generic_string();
        },
    });

    ops::registerCommand({
        .name = "sceneInfo",
        .doc = "Current scene file and whether it has unsaved changes.",
        .undoable = false,
        .run = [](const Args&) -> json {
            return {{"path", currentPath().empty() ? json(nullptr) : json(currentPath().generic_string())}, {"modified", modified()}};
        },
    });

    ops::registerCommand({
        .name = "incrementAndSave",
        .doc = "Save to the next free incremental name (scene.lila -> scene_v002.lila -> scene_v003.lila) and make it "
               "the current file. Returns the new path.",
        .undoable = false,
        .run = [](const Args&) -> json {
            if (currentPath().empty())
                throw CommandError("incrementAndSave: the scene has no file yet; save it first");
            fs::path path = history::nextIncrement(currentPath());
            try
            {
                save(path);
            }
            catch (const std::exception& e)
            {
                throw CommandError(e.what());
            }
            return path.generic_string();
        },
    });

    ops::registerCommand({
        .name = "sceneVersions",
        .doc = "Backups, autosaves, emergency saves and incremental versions of a scene on disk, newest first.",
        .positionalDoc = "[path]",
        .undoable = false,
        .run = [](const Args& a) -> json {
            fs::path scene = a.positional.empty() ? currentPath() : fs::path(a.positional[0].get<std::string>());
            json out = json::array();
            for (const history::Version& v : history::list(scene))
                out.push_back({{"path", v.path.generic_string()}, {"kind", history::kindName(v.kind)}, {"time", v.time}, {"size", v.size}});
            return out;
        },
    });
}
} // namespace lila::io
