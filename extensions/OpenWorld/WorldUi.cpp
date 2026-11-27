// The world spec in the UI: Level > Open World's World entries (Build World from a spec file, its missing real data
// fetched after the sizes are shown; Check World into the Validation panel, World Map, Save World Spec), Site Buildings,
// and the toolkit's World group.

#include "OpenWorld.h"
#include "WorldFetch.h"
#include "WorldScene.h"

#include <lila/Actions.h>
#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/FileDialog.h>
#include <lila/Graph.h>
#include <lila/LevelToolkit.h>
#include <lila/Log.h>
#include <lila/Node.h>
#include <lila/OptionBox.h>

#include <QApplication>

#include <format>

using namespace lila;
using namespace lila::ui;

namespace openworld::world
{
namespace
{
const std::vector<std::string> kWorldKeywords = {"world", "open world", "world spec", "map", "gta", "city", "region", "level"};

json runUi(const char* cmd, json call)
{
    try
    {
        return ops::execute(cmd, std::move(call), CallSource::UI);
    }
    catch (const std::exception& e)
    {
        log::warn("{}", e.what());
        return json();
    }
}

std::string flagText(const json& values, const char* flag)
{
    return values.contains(flag) && values[flag].is_string() ? values[flag].get<std::string>() : std::string();
}

bool flagOn(const json& values, const char* flag) { return values.contains(flag) && values[flag].is_boolean() && values[flag].get<bool>(); }

std::string pickSpecFile()
{
    const QString file = fileDialog::getOpenFileName(QApplication::activeWindow(), "World Spec", {}, "World spec (*.json)");
    return file.toStdString();
}

/** What Fetch Missing Data would download for the spec file, worked out without downloading. */
std::string fetchNote(const json& values)
{
    const std::string file = flagText(values, "file");
    if (file.empty())
        return "Pick a spec file to see what is missing.";
    const json e = ops::execute("worldFetch", {{"file", file}, {"estimate", true}})["estimate"];
    if (e.value("missing", 0) == 0)
        return "All data present.";
    std::string text;
    for (const json& it : e["items"])
    {
        if (it.value("state", std::string()) == "present")
            continue;
        const std::string size = it.contains("problem")     ? it["problem"].get<std::string>()
                                 : it.value("cached", false) ? std::string("in cache")
                                                             : fetchSize(it.value("bytes", uint64_t(0)));
        text += it["id"].get<std::string>() + ": " + size + "\n";
    }
    text += std::format("Total about {:.1f} MB", e.value("megabytes", 0.0));
    if (e.value("consentNeeded", false))
        text += std::format(", over {:.0f} MB: needs Allow Large Download", e.value("consentMB", 25.0));
    if (e.value("keyMissing", false))
        text += "\nGEBCO needs an OpenTopography key: worldFetch(file=..., apiKey='...', saveKey=True) once";
    return text;
}

/** Build World with the option box's settings: the spec file (asked for when none is set), then the fetch, then the build. */
void buildWorldFromUi()
{
    const json v = ops::options::values("worldBuild");
    std::string file = flagText(v, "file");
    if (file.empty())
    {
        file = pickSpecFile();
        if (file.empty())
            return;
        ops::options::setValue("worldBuild", "file", file);
    }
    json call = {{"file", file}, {"replace", true}};
    const bool fetch = flagOn(v, "fetch"), offline = flagOn(v, "offline"), consent = flagOn(v, "consent");
    if (fetch || offline)
    {
        const json est = runUi("worldFetch", {{"file", file}, {"estimate", true}});
        if (est.is_null())
            return;
        const json& e = est["estimate"];
        if (e.value("missing", 0) > 0)
            log::info("Build World: to download {}", e.value("summary", std::string()));
        if (!offline && e.value("consentNeeded", false) && !consent)
        {
            log::warn("Build World: about {:.1f} MB to download, over {:.0f} MB; turn on Allow Large Download in Build World's options to fetch it",
                      e.value("megabytes", 0.0), e.value("consentMB", 25.0));
            return;
        }
        call["fetch"] = fetch;
        call["offline"] = offline;
        call["consent"] = consent;
    }
    const json r = runUi("worldBuild", call);
    if (r.is_null())
        return;
    if (r.contains("fetch"))
        log::info("worldBuild: fetched {}, already present {}, {:.1f} MB", r["fetch"].value("fetched", 0), r["fetch"].value("present", 0),
                  r["fetch"].value("megabytes", 0.0));
    for (const json& s : r["steps"])
        if (!s.value("ok", true))
            log::warn("worldBuild {}: {}", s["step"].get<std::string>(), s.value("error", std::string()));
    log::info("worldBuild: {} in {:.0f} s, {} steps, check {}", r.value("world", std::string()), r.value("seconds", 0.0), r["steps"].size(),
              r.contains("check") && r["check"].value("ok", false) ? "clean" : "has issues (Check World lists them)");
}

void registerBuildOptions()
{
    ops::options::Override o;
    o.order = {"file", "fetch", "consent", "offline"};
    o.hidden = {"spec", "replace", "stopOnError", "folder", "skip", "check", "estimate", "apiKey", "saveKey"};
    o.labels = {{"file", "Spec File"}, {"fetch", "Fetch Missing Data"}, {"consent", "Allow Large Download"}, {"offline", "Cache Only"}};
    o.sections = {{"fetch", "Real Data"}, {"consent", "Real Data"}, {"offline", "Real Data"}};
    o.fills = {{.label = "Browse", .tip = "Pick a world spec file", .values = [] {
                    const std::string f = pickSpecFile();
                    return f.empty() ? json::object() : json{{"file", f}};
                }}};
    o.liveNotes = {{"fetch", fetchNote}};
    o.enabledWhen = {{"consent", {"fetch", {"true"}}}};
    o.docs = "open-world?id=world-spec";
    ops::options::setOverride("worldBuild", o);
}

/** Site ids of the selected site meshes, else none (every site). */
json selectedSites()
{
    json out = json::array();
    if (!ops::find("siteList"))
        return out;
    scene::Graph& g = scene::Graph::current();
    std::set<std::string> selected;
    for (scene::NodeId id : g.selection())
        if (scene::Node* n = g.node(id))
            selected.insert(n->path());
    for (const json& s : runUi("siteList", json::object()))
        if (selected.count(s.value("mesh", std::string())))
            out.push_back(s.value("id", std::string()));
    return out;
}
} // namespace

void registerWorldUi(ext::Context& ctx)
{
    actions::add(ctx, {.id = "openworld.worldBuild",
                       .label = "Build World",
                       .category = "Open World",
                       .run = buildWorldFromUi,
                       .doc = "Build a whole open-world map from a world spec file: remix, terrain, settlements, buildings, sites, slots, zones. "
                              "Fetch Missing Data downloads the real data the spec names first, after showing the size.",
                       .python = "worldBuild(file='worlds/sinaloa.json', replace=True)",
                       .command = "worldBuild",
                       .icon = "world",
                       .keywords = {"world", "open world", "world spec", "map", "gta", "city", "region", "level", "fetch", "download", "real data",
                                    "gebco", "osm", "dem"}});
    actions::add(ctx, {.id = "openworld.worldCheck",
                       .label = "Check World",
                       .category = "Open World",
                       .run =
                           [] {
                               const json c = runUi("worldCheck", {{"report", true}});
                               if (c.is_null())
                                   return;
                               const json& p = c["placeholders"];
                               log::info("worldCheck: {}, {} issues; {} of {} slots swapped from placeholders", c.value("ok", false) ? "clean" : "issues",
                                         c["issues"].size(), p.value("swapped", 0), p.value("swapped", 0) + p.value("onPlaceholder", 0));
                           },
                       .doc = "Check the whole world into the Validation panel: every part's check, real names, buildings on water or roads, props off the "
                              "ground, stale buildings, zone gaps, swap progress.",
                       .python = "worldCheck(report=True)",
                       .icon = "list-check",
                       .keywords = {"world", "validate", "check", "real names", "placeholders", "swap progress"}});
    actions::add(ctx, {.id = "openworld.worldMap",
                       .label = "World Map",
                       .category = "Open World",
                       .doc = "Write the whole map as a PNG plus JSON (districts, zones, settlements, POIs, roads, water) for an in-game map.",
                       .python = "worldMap(resolution=2048)",
                       .command = "worldMap",
                       .call = json::object(),
                       .icon = "map",
                       .keywords = {"world", "minimap", "overview", "game map", "export", "pause map"}});
    actions::add(ctx, {.id = "openworld.worldSpecSave",
                       .label = "Save World Spec",
                       .category = "Open World",
                       .run =
                           [] {
                               const QString file = fileDialog::getSaveFileName(QApplication::activeWindow(), "Save World Spec", {}, "World spec (*.json)");
                               if (file.isEmpty())
                                   return;
                               const json r = runUi("worldSpec", {{"file", file.toStdString()}});
                               if (!r.is_null())
                                   log::info("worldSpec: wrote {}", r.value("file", std::string()));
                           },
                       .doc = "Write the world as the scene holds it now (remix, terrain, slots, sites, zones, Buildings edits) to a world spec file.",
                       .python = "worldSpec(file='worlds/now.json')",
                       .icon = "device-floppy",
                       .keywords = kWorldKeywords});
    actions::add(ctx, {.id = "openworld.siteBuildings",
                       .label = "Site Buildings",
                       .category = "Open World",
                       .run =
                           [] {
                               json call = {{"args", selectedSites()}};
                               const json r = runUi("siteBuildings", call);
                               if (!r.is_null())
                                   log::info("siteBuildings: {} buildings from {} sites", r.value("buildings", 0), r.value("sites", json::array()).size());
                           },
                       .doc = "Build the selected sites' buildings (or every site's) with the Buildings generator instead of greybox massing.",
                       .python = "siteBuildings()",
                       .icon = "building-community",
                       .keywords = {"site", "buildings", "procedural building", "massing", "port", "cemetery"}});

    registerBuildOptions();
    addTools(ctx, {"openworld.siteBuildings", "", "openworld.worldBuild", "openworld.worldCheck", "openworld.worldMap", "openworld.worldSpecSave"},
             {{.action = "openworld.siteBuildings", .icon = "building-community", .text = "Buildings", .group = "Sites"},
              {.action = "openworld.worldBuild", .icon = "world", .text = "Build", .shelf = "World", .group = "World"},
              {.action = "openworld.worldCheck", .icon = "list-check", .text = "Check", .group = "World"},
              {.action = "openworld.worldMap", .icon = "map", .text = "Map", .group = "World"},
              {.action = "openworld.worldSpecSave", .text = "Save Spec", .group = "World", .rare = true}});
}
} // namespace openworld::world
