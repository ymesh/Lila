#include "io/ExportOnSave.h"

#include "core/Log.h"
#include "core/Paths.h"
#include "io/FileTranslators.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/SceneData.h"

#include <algorithm>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace lila::io
{
namespace
{
constexpr const char* kKey = "exportOnSave";

const translators::FileTranslator& exporterFor(const fs::path& path)
{
    if (const translators::FileTranslator* t = translators::forPath(translators::Op::Export, path))
        return *t;
    std::string exts;
    for (const translators::FileTranslator* e : translators::supporting(translators::Op::Export))
        for (const std::string& x : e->extensions)
            exts += (exts.empty() ? "" : ", ") + x;
    throw CommandError("no exporter writes '" + paths::toUtf8(path.extension()) + "' files; use one of " + exts);
}

bool hasFlag(const CommandDef& cmd, std::string_view name)
{
    return std::any_of(cmd.flags.begin(), cmd.flags.end(), [&](const FlagDef& f) { return f.name == name; });
}
} // namespace

json exportScene(const fs::path& path, json options)
{
    const translators::FileTranslator& t = exporterFor(path);
    if (!options.is_object())
        throw CommandError("export options must be a dict of the export command's flags");
    const CommandDef* cmd = ops::find(t.exportSelection.command);
    if (cmd)
        for (const char* flag : {"selectionOnly", "unrealCheck"})
            if (!options.contains(flag) && hasFlag(*cmd, flag))
                options[flag] = false;
    std::error_code ec;
    if (path.has_parent_path())
        fs::create_directories(path.parent_path(), ec);
    const fs::file_time_type before = fs::exists(path, ec) ? fs::last_write_time(path, ec) : fs::file_time_type::min();
    // Exporters without selectionOnly (OBJ, STL, PLY) write the selection: hand them every node, then put it back.
    scene::Graph& g = scene::Graph::current();
    const std::vector<scene::NodeId> selection = g.selection();
    const bool selectAll = cmd && !hasFlag(*cmd, "selectionOnly");
    if (selectAll)
        g.select(g.nodes());
    json result;
    try
    {
        result = translators::run(translators::Op::Export, path, options);
    }
    catch (...)
    {
        if (selectAll)
            g.select(selection);
        throw;
    }
    if (selectAll)
        g.select(selection);
    if (!fs::exists(path, ec) || fs::last_write_time(path, ec) == before)
        throw CommandError("the " + t.label + " exporter wrote nothing to " + paths::toUtf8(path) + " (is the scene empty?)");
    return result;
}

namespace exportOnSave
{
json setting()
{
    json s = scene::data::get(kKey);
    return s.is_object() ? s : json::object();
}

fs::path target(const fs::path& scene)
{
    const json s = setting();
    const std::string path = s.value("path", std::string());
    if (!s.value("enabled", false) || path.empty())
        return {};
    fs::path p = paths::fromUtf8(path);
    if (p.is_relative())
        p = (scene.has_parent_path() ? scene.parent_path() : fs::current_path()) / p;
    return p.lexically_normal();
}

void onSceneSaved(const fs::path& scene)
{
    const fs::path out = target(scene);
    if (out.empty())
        return;
    try
    {
        const json options = setting().value("options", json::object());
        exportScene(out, options.is_object() ? options : json::object());
        log::info("Export on save: wrote {}", paths::toUtf8(out));
    }
    catch (const std::exception& e)
    {
        log::error("Export on save: {} was not written: {}", paths::toUtf8(out), e.what());
    }
}

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "exportOnSave",
        .doc = "Query or set the scene's export on save: while enabled, every save also exports the whole scene to path, "
               "relative to the scene file, in the format its extension names (.fbx, .glb, .gltf, .obj, ...). Saved with "
               "the scene. Returns the setting and, for a saved scene, the file it writes.",
        .flags = {
            {"path", "p", T::String, nullptr, "File to write, relative to the scene file or absolute. Giving it turns the setting on."},
            {"enable", "e", T::Bool, nullptr, "Turn export on save on or off, keeping the path."},
            {"options", "o", T::Any, nullptr, "{flag: value} for the format's export command, e.g. {'profile': 'unity', 'units': 'm'}."},
            {"query", "q", T::Bool, false, ""},
        },
        .run = [](const Args& a) -> json {
            json s = setting();
            if (!a.get<bool>("query"))
            {
                if (a.has("path"))
                {
                    const std::string p = a.get<std::string>("path");
                    if (p.empty())
                        throw CommandError("exportOnSave: path is empty");
                    exporterFor(paths::fromUtf8(p));
                    s["path"] = p;
                    s["enabled"] = true;
                }
                if (a.has("enable"))
                {
                    if (a.get<bool>("enable") && s.value("path", std::string()).empty())
                        throw CommandError("exportOnSave: give a path to export to");
                    s["enabled"] = a.get<bool>("enable");
                }
                if (a.has("options"))
                {
                    const json& o = a.flags["options"];
                    if (!o.is_object() && !o.is_null())
                        throw CommandError("exportOnSave: options must be a dict of the export command's flags");
                    if (o.is_null() || o.empty())
                        s.erase("options");
                    else
                        s["options"] = o;
                }
                scene::data::set(kKey, s.empty() ? json() : s);
            }
            json out = {{"enabled", s.value("enabled", false)}, {"path", s.value("path", std::string())},
                        {"options", s.value("options", json::object())}, {"file", nullptr}};
            if (const fs::path file = target(currentPath()); !file.empty() && !currentPath().empty())
                out["file"] = paths::toUtf8(file);
            return out;
        },
        .examples = {"exportOnSave(path='Exports/Ship.fbx', options={'profile': 'unity', 'units': 'm'})", "exportOnSave(enable=False)"},
    });
}
} // namespace exportOnSave
} // namespace lila::io
