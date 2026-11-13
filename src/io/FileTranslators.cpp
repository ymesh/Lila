#include "io/FileTranslators.h"

#include "ops/CommandOptions.h"
#include "core/ExtensionContext.h"
#include "core/Log.h"
#include "io/SceneFile.h"
#include "ops/Undo.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace lila::io::translators
{
namespace
{
std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string extensionOf(const fs::path& path)
{
    std::u8string u8 = path.extension().u8string();
    return lower(std::string(u8.begin(), u8.end()));
}

std::string pathArg(const fs::path& path)
{
    std::u8string u8 = path.generic_u8string();
    return std::string(u8.begin(), u8.end());
}

bool supports(const FileTranslator& t, Op op)
{
    return t.operation(op).supported() || (op == Op::Open && t.import.supported());
}

void sortByPriority(std::vector<const FileTranslator*>& list, Op op)
{
    std::stable_sort(list.begin(), list.end(), [op](const FileTranslator* a, const FileTranslator* b) {
        if (op == Op::Open && a->open.supported() != b->open.supported())
            return a->open.supported();
        return a->priority > b->priority;
    });
}

json invoke(const Operation& o, const fs::path& path, const json& options, CallSource source)
{
    if (o.run)
        return o.run(path, options.is_object() ? options : json::object());

    json call = options.is_object() ? options : json::object();
    if (source == CallSource::UI)
        call = ops::options::callWith(o.command, call);
    for (auto& [k, v] : o.flags.items())
        call[k] = v;
    call["args"] = json::array({pathArg(path)});
    return ops::execute(o.command, call, source);
}

json describeOp(const Operation& o)
{
    if (!o.supported())
        return nullptr;
    json j = {{"command", o.command.empty() ? json(nullptr) : json(o.command)}};
    if (!o.flags.empty())
        j["flags"] = o.flags;
    return j;
}
} // namespace

const char* opName(Op op)
{
    switch (op)
    {
    case Op::Open: return "open";
    case Op::Import: return "import";
    case Op::Export: return "export";
    }
    return "?";
}

const Operation& FileTranslator::operation(Op op) const
{
    switch (op)
    {
    case Op::Open: return open;
    case Op::Import: return import;
    case Op::Export: return exportSelection;
    }
    return open;
}

bool FileTranslator::handles(const fs::path& path) const
{
    std::string ext = extensionOf(path);
    return !ext.empty() && std::find(extensions.begin(), extensions.end(), ext) != extensions.end();
}

Registry<FileTranslator>& registry()
{
    static Registry<FileTranslator> r;
    return r;
}

namespace
{
void normalize(FileTranslator& t)
{
    for (std::string& e : t.extensions)
    {
        e = lower(e);
        if (!e.empty() && e[0] != '.')
            e.insert(e.begin(), '.');
    }
    if (t.label.empty())
        t.label = t.id;
}
} // namespace

const FileTranslator& add(FileTranslator t)
{
    normalize(t);
    std::string id = t.id;
    return registry().add(std::move(id), std::move(t));
}

const FileTranslator* add(ext::Context& ctx, FileTranslator t)
{
    normalize(t);
    std::string id = t.id;
    try
    {
        return ctx.add(registry(), "fileTranslator", id, std::move(t)) ? registry().find(id) : nullptr;
    }
    catch (const std::exception& e)
    {
        log::error("extension {}: file translator: {}", ctx.owner(), e.what());
        return nullptr;
    }
}

bool remove(std::string_view id)
{
    return registry().remove(id);
}

const FileTranslator* find(std::string_view id)
{
    return registry().find(id);
}

std::vector<const FileTranslator*> supporting(Op op)
{
    std::vector<const FileTranslator*> out;
    for (const FileTranslator* t : registry().all())
        if (supports(*t, op))
            out.push_back(t);
    sortByPriority(out, op);
    return out;
}

const FileTranslator* forPath(Op op, const fs::path& path)
{
    for (const FileTranslator* t : supporting(op))
        if (t->handles(path))
            return t;
    return nullptr;
}

std::string dialogFilter(Op op, std::string_view all)
{
    std::vector<const FileTranslator*> list = supporting(op);
    if (list.empty())
        return {};
    auto patterns = [](const std::vector<std::string>& exts) {
        std::string s;
        for (const std::string& e : exts)
            s += (s.empty() ? "*" : " *") + e;
        return s;
    };
    std::vector<std::string> every;
    std::set<std::string> seen;
    std::string entries;
    for (const FileTranslator* t : list)
    {
        for (const std::string& e : t->extensions)
            if (seen.insert(e).second)
                every.push_back(e);
        entries += ";;" + t->label + " (" + patterns(t->extensions) + ")";
    }
    // Export dialogs pick the format from the chosen entry, so they list formats only.
    if (op == Op::Export)
        return entries.substr(2);
    return std::string(all) + " (" + patterns(every) + ")" + entries;
}

json run(Op op, const fs::path& path, const json& options, CallSource source, std::string_view translatorId)
{
    const FileTranslator* t = translatorId.empty() ? forPath(op, path) : find(translatorId);
    if (!t)
        throw CommandError(translatorId.empty() ? std::string("no file translator can ") + opName(op) + " " + pathArg(path.filename())
                                                : "unknown file translator '" + std::string(translatorId) + "'");
    const bool openByImport = op == Op::Open && !t->open.supported() && t->import.supported();
    const Operation& o = openByImport ? t->import : t->operation(op);
    if (!o.supported())
        throw CommandError(t->label + " files can't be " + (op == Op::Open ? "opened" : op == Op::Import ? "imported" : "exported"));
    if (!openByImport)
        return invoke(o, path, options, source);

    SceneSnapshot previous;
    ops::execute("newScene", json::object(), source);
    setCurrentPath({});
    json result;
    try
    {
        result = invoke(o, path, options, source);
    }
    catch (const CommandCancelled&)
    {
        previous.restore();
        throw;
    }
    // Like any open: no undo back into the empty scene, and unsaved until it is saved as a .lila.
    UndoStack::get().clear();
    markClean();
    markModified();
    return result;
}

json describe(const FileTranslator& t)
{
    const std::string* owner = registry().ownerOf(t.id);
    return {{"id", t.id},
            {"label", t.label},
            {"doc", t.doc},
            {"extensions", t.extensions},
            {"open", describeOp(t.open)},
            {"import", describeOp(t.import)},
            {"export", describeOp(t.exportSelection)},
            {"priority", t.priority},
            {"owner", owner ? *owner : std::string()}};
}

void registerBuiltins()
{
    if (registry().contains("lila"))
        return;
    add({.id = "lila",
         .label = "Lila scenes",
         .doc = "Lila's own scene files: binary .lila and text .lilaa.",
         .extensions = {".lila", ".lilaa"},
         .open = {.command = "openScene"}});
    add({.id = "maya",
         .label = "Maya scenes",
         .doc = "Maya ASCII and binary scenes, read without Maya. Opening one starts an untitled scene; saving it makes the .lila.",
         .extensions = {".ma", ".mb"},
         .open = {.command = "mayaOpen", .flags = {{"force", true}}},
         .import = {.command = "mayaImport"}});
    add({.id = "blender",
         .label = "Blender scenes",
         .doc = "Blender .blend files, read without Blender: objects, collections, meshes, materials, cameras, lights, curves, armatures, "
                "shape keys, animation and the modifiers Lila has. Opening one starts an untitled scene; saving it makes the .lila.",
         .extensions = {".blend"},
         .open = {.command = "blendOpen", .flags = {{"force", true}}},
         .import = {.command = "blendImport"}});
    add({.id = "fbx",
         .label = "FBX",
         .doc = "Autodesk FBX: meshes, skeletons, skin weights, morph targets and animation. Exports convert to Unreal's axes.",
         .extensions = {".fbx"},
         .import = {.command = "importFbx"},
         .exportSelection = {.command = "exportFbx", .flags = {{"profile", "ue5"}}},
         .priority = 1});
    add({.id = "gltf",
         .label = "glTF",
         .doc = "Khronos glTF 2.0, JSON .gltf and binary .glb: meshes, skeletons, skin weights, morph targets, animation, cameras and "
                "PBR materials with their textures. Converts to and from glTF's Y-up metres.",
         .extensions = {".gltf", ".glb"},
         .import = {.command = "importGltf"},
         .exportSelection = {.command = "exportGltf"}});
    add({.id = "collada",
         .label = "COLLADA",
         .doc = "COLLADA 1.4.1 .dae: meshes, skeletons, skin weights, animation and materials. Exports match Maya's COLLADA exporter "
                "and its jointOrients, so pipelines built on Maya's files read it.",
         .extensions = {".dae"},
         .import = {.command = "importCollada"},
         .exportSelection = {.command = "exportCollada"}});
    add({.id = "obj",
         .label = "OBJ",
         .doc = "Wavefront OBJ meshes.",
         .extensions = {".obj"},
         .import = {.command = "importObj"},
         .exportSelection = {.command = "exportObj"}});
    add({.id = "stl",
         .label = "STL",
         .doc = "STL, binary and ASCII: triangle meshes for 3D printing and CAD, welded on import. Millimetres, Z up by default.",
         .extensions = {".stl"},
         .import = {.command = "importStl"},
         .exportSelection = {.command = "exportStl"}});
    add({.id = "ifc",
         .label = "IFC",
         .doc = "IFC building models: elements as meshes, the spatial structure as groups, property sets as attributes, placed by IfcMapConversion.",
         .extensions = {".ifc"},
         .import = {.command = "importIfc"}});
    add({.id = "pointCloud",
         .label = "Point Cloud",
         .doc = "LAS, LAZ and E57 scans as streamed point clouds, placed by their CRS.",
         .extensions = {".las", ".laz", ".e57"},
         .import = {.command = "importPointCloud"}});
    add({.id = "ply",
         .label = "PLY",
         .doc = "Stanford PLY, binary and ASCII: meshes and point clouds with normals, UVs and colors, as scanners and "
                "photogrammetry write them. Metres, Z up by default.",
         .extensions = {".ply"},
         .import = {.command = "importPly"},
         .exportSelection = {.command = "exportPly"}});
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "fileTranslators",
        .doc = "List the file translators: which extensions File > Open, Import and Export handle, and the command each runs.",
        .flags = {{"operation", "", T::String, nullptr, "Only translators that support this: open | import | export", {"open", "import", "export"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            json out = json::array();
            std::string want = a.get<std::string>("operation");
            for (const FileTranslator* t : registry().all())
            {
                json d = describe(*t);
                if (want.empty() || !d[want].is_null())
                    out.push_back(std::move(d));
            }
            return out;
        },
    });
    auto fileCommand = [](const char* name, Op op, const char* doc) {
        ops::registerCommand({
            .name = name,
            .doc = doc,
            .positionalDoc = "path",
            .flags = {{"translator", "", T::String, nullptr, "Translator id to use instead of the one for the file's extension."},
                      {"options", "", T::Any, nullptr, "{flag: value} passed on to the translator's command."}},
            // The translator's own command records the undo step.
            .undoable = false,
            .run = [op, name](const Args& a) -> json {
                if (a.positional.size() != 1 || !a.positional[0].is_string())
                    throw CommandError(std::string(name) + ": give one path");
                json options = a.has("options") ? a.flags["options"] : json::object();
                if (!options.is_object())
                    throw CommandError(std::string(name) + ": options must be an object");
                const std::string& path = a.positional[0].get_ref<const std::string&>();
                return run(op, fs::path(std::u8string(path.begin(), path.end())), options, CallSource::Script, a.get<std::string>("translator"));
            },
        });
    };
    fileCommand("openFile", Op::Open, "Open a file with the translator for its extension (.lila, .ma, ...), replacing the scene. Formats that only "
                                     "import (.fbx, .gltf, .obj, ...) open as a new untitled scene with the file imported into it.");
    fileCommand("importFile", Op::Import, "Import a file into the scene with the translator for its extension (.fbx, .gltf, .glb, .obj, .ma, ...).");
    fileCommand("exportFile", Op::Export, "Export the selection with the translator for the file's extension (.fbx, .gltf, .glb, .obj, ...).");
}
} // namespace lila::io::translators
