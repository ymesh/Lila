#include "ui/SceneLayout.h"

#include "core/Log.h"
#include "core/PrefStore.h"
#include "io/SceneFile.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Graph.h"
#include "sculpt/SculptSession.h"
#include "ui/Layouts.h"
#include "ui/Modes.h"
#include "ui/SculptTool.h"

#include <nlohmann/json.hpp>

namespace lila::ui::sceneLayout
{
namespace
{
using nlohmann::json;

json save()
{
    json j = {{"mode", modes::current()}};
    if (std::string layout = layouts::current(); !layout.empty())
        j["layout"] = layout;
    if (auto s = sculpt::active())
        if (const scene::Node* n = scene::Graph::current().node(s->node))
            j["sculpt"] = n->path();
    return j;
}

void restoreMode(const std::string& id)
{
    if (id.empty() || id == modes::current())
        return;
    if (!modes::activate(id))
        log::warn("Scene was saved in mode '{}', which isn't loaded; staying in '{}'", id, modes::current());
}

void restoreLayout(const std::string& name)
{
    if (name.empty() || name == layouts::current())
        return;
    try
    {
        ops::execute("applyLayout", {{"name", name}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("Scene was saved in layout '{}': {}", name, e.what());
    }
}

void restoreSculpt(const std::string& path)
{
    if (path.empty() || sculpt::active())
        return;
    scene::Node* n = scene::Graph::current().find(path);
    if (!n)
    {
        log::warn("Scene was saved sculpting '{}', which isn't in it", path);
        return;
    }
    try
    {
        ops::execute("sculptEnter", {{"args", {n->path()}}}, CallSource::UI);
    }
    catch (const CommandError& e)
    {
        log::warn("Scene was saved sculpting '{}': {}", path, e.what());
        return;
    }
    // Reopening into the session is part of opening the file, not an edit Ctrl+Z should take back.
    UndoStack::get().clear();
    startSculptTool();
}

void load(const json& j)
{
    if (!j.is_object() || !PrefStore::get().value<bool>("general", kRestorePref, true))
        return;
    restoreMode(j.value("mode", std::string()));
    restoreLayout(j.value("layout", std::string()));
    restoreSculpt(j.value("sculpt", std::string()));
}
} // namespace

void install()
{
    // Sections load in key order: "layout" before "viewports", so the layout is in place before cameras come back.
    io::registerUiSection("layout", save, load);
}
} // namespace lila::ui::sceneLayout
