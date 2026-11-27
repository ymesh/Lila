#include "ui/AchievementRules.h"

#include "core/Achievements.h"
#include "core/CrashHandler.h"
#include "core/CrashUpload.h"
#include "core/MainQueue.h"
#include "core/Paths.h"
#include "core/PrefStore.h"
#include "extensions/ExtensionManager.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/HudStats.h"
#include "scene/Instancing.h"
#include "ui/AchievementBanner.h"

#include <QTimer>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <string>
#include <unordered_map>

namespace lila::ui::achievementRules
{
namespace
{
namespace A = achievements;
namespace fs = std::filesystem;

constexpr const char* kCreditedCrashesPref = "/achievements/creditedCrashes";

/** Commands whose first successful run unlocks an achievement. */
const std::unordered_map<std::string_view, const char*>& firstRuns()
{
    static const std::unordered_map<std::string_view, const char*> map = {
        {"bake", "FRESH_OUT_OF_THE_OVEN"},
        {"bakeAll", "FRESH_OUT_OF_THE_OVEN"},
        {"bakeMaps", "FRESH_OUT_OF_THE_OVEN"},
        {"bakeForTarget", "FRESH_OUT_OF_THE_OVEN"},
        {"openRecoveredScene", "PHOENIX"},
        {"freezeTransforms", "FREEZE"},
        {"breakApart", "DEMOLITION_EXPERT"},
        {"wedgeDestruction", "DEMOLITION_EXPERT"},
        {"collapse", "DEMOLITION_EXPERT"},
        {"settlementCreate", "URBAN_PLANNER"},
        {"europeanCity", "URBAN_PLANNER"},
        {"hamlet", "URBAN_PLANNER"},
        {"manhattan", "URBAN_PLANNER"},
        {"modernCity", "URBAN_PLANNER"},
        {"mexicanCity", "URBAN_PLANNER"},
        {"scifiMegacity", "URBAN_PLANNER"},
        {"streetVillage", "URBAN_PLANNER"},
        {"usTown", "URBAN_PLANNER"},
        {"unrealSync", "HELLO_UNREAL"},
        {"unrealMap", "HELLO_UNREAL"},
        {"unrealSendCurve", "HELLO_UNREAL"},
        {"unrealSendTextures", "HELLO_UNREAL"},
        {"unrealSendTextureSet", "HELLO_UNREAL"},
        {"unrealPlaceInstances", "HELLO_UNREAL"},
        {"terrainSendToUnreal", "HELLO_UNREAL"},
        {"quadRemesh", "RETOPOLOGIST"},
        {"retopoConstruct", "RETOPOLOGIST"},
        {"mayaImport", "DEFECTOR"},
        {"mayaOpen", "DEFECTOR"},
        {"mayaReference", "DEFECTOR"},
        {"masonryWall", "ANCIENT_RUINS"},
        {"terrainErode", "TERRAFORMER"},
        {"drop", "INTERIOR_DECORATOR"},
        {"settle", "INTERIOR_DECORATOR"},
        {"pileProps", "INTERIOR_DECORATOR"},
        {"stackProps", "INTERIOR_DECORATOR"},
        {"fillShelf", "INTERIOR_DECORATOR"},
        {"pipe", "PIPE_DREAM"},
        {"bakeSkinWeights", "BONES_OF_CONTENTION"},
        {"gameExport", "SHIP_IT"},
        {"settlementExport", "SHIP_IT"},
        {"terrainExport", "SHIP_IT"},
        {"masonryShip", "SHIP_IT"},
        {"symmetrizeRadial", "HUBCAP"},
        {"autoBevel", "EDGE_LORD"},
        {"polySplitRings", "LORD_OF_THE_RINGS"},
        {"polyGridFill", "FAN_FICTION"},
        {"polySpherize", "SPHERE_OF_INFLUENCE"},
        {"polyCircularize", "GOING_IN_CIRCLES"},
        {"orientPivot", "POINT_TAKEN"},
    };
    return map;
}

/** Commands that cut into a mesh, for Measure Twice. */
bool cuts(std::string_view name)
{
    return name == "polyKnife" || name == "polyInsertLoop" || name == "polyConnect" || name == "polySplitRings" ||
           name == "polyBoolean";
}

struct State
{
    int listener = -1;
    QTimer* sceneCheck = nullptr;
    int copyRun = 0;        // Shift+D in a row
    int undos = 0;          // this session
    int undoRun = 0;        // undos in a row
    int redoRun = 0;        // redos in a row, right after an undo run
    int timeLordTarget = 0; // the undo run the redos must match
    std::string measured;   // the mesh Alt+M last measured
    bool openedUsd = false;
};

State& state()
{
    static State s;
    return s;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/** The call's file path (first positional), lower case, or empty. */
std::string callPath(const json& call)
{
    auto it = call.find("args");
    if (it == call.end() || !it->is_array() || it->empty() || !(*it)[0].is_string())
        return {};
    return lower((*it)[0].get<std::string>());
}

std::vector<std::string> callObjects(const json& call)
{
    std::vector<std::string> out;
    auto it = call.find("args");
    if (it != call.end() && it->is_array())
        for (const json& a : *it)
            if (a.is_string())
                out.push_back(a.get<std::string>());
    if (out.empty())
        if (json sel = ops::execute("ls", {{"selection", true}}); sel.is_array())
            for (const json& a : sel)
                if (a.is_string())
                    out.push_back(a.get<std::string>());
    return out;
}

bool isUsd(std::string_view path)
{
    return path.ends_with(".usd") || path.ends_with(".usda") || path.ends_with(".usdc") || path.ends_with(".usdz");
}

int localHour()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    return tm.tm_hour;
}

void onUndoRedo(State& s, std::string_view name)
{
    if (name == "undo")
    {
        if (++s.undos == 1000)
            A::unlock("CTRL_Z_ENTHUSIAST");
        s.redoRun = 0;
        ++s.undoRun;
        return;
    }
    if (s.redoRun == 0)
        s.timeLordTarget = s.undoRun;
    s.undoRun = 0;
    if (++s.redoRun >= 100 && s.redoRun == s.timeLordTarget)
        A::unlock("TIME_LORD");
}

/** Bit of name among the sorted names, 0 when absent or past 32. */
uint32_t bitOf(std::vector<std::string> names, std::string_view name)
{
    std::sort(names.begin(), names.end());
    auto it = std::find(names.begin(), names.end(), name);
    size_t i = size_t(it - names.begin());
    return it == names.end() || i >= 32 ? 0 : 1u << i;
}

uint32_t allBits(size_t count)
{
    return count >= 32 ? ~0u : (1u << count) - 1;
}

void themeChosen(const std::string& theme)
{
    std::vector<std::string> builtIn;
    if (json themes = ops::execute("listThemes", json::object()); themes.is_array())
        for (const json& th : themes)
            if (th.value("builtIn", false))
                builtIn.push_back(th.value("name", std::string()));
    if (uint32_t bit = bitOf(builtIn, theme))
        A::addBits("THEMES_TRIED", bit, "RAINBOW", allBits(builtIn.size()));
}

/** Export with a built-in translator, by command name or (for exportFile) the path's extension. */
void exported(std::string_view command, const std::string& path)
{
    std::vector<std::string> ids;
    std::string used;
    json translators = ops::execute("fileTranslators", {{"operation", "export"}});
    for (const json& tr : translators.is_array() ? translators : json::array())
    {
        if (tr.value("owner", std::string()) != "lila")
            continue;
        std::string id = tr.value("id", std::string());
        ids.push_back(id);
        const json& op = tr["export"];
        bool byCommand = op.is_object() && op["command"].is_string() && op["command"].get<std::string>() == command;
        bool byExtension = false;
        if (command == "exportFile" && tr["extensions"].is_array())
            for (const json& ext : tr["extensions"])
                byExtension = byExtension || (ext.is_string() && path.ends_with(lower(ext.get<std::string>())));
        if (byCommand || byExtension)
            used = id;
    }
    if (uint32_t bit = bitOf(ids, used))
        A::addBits("EXPORT_FORMATS", bit, "POLYGLOT", allBits(ids.size()));
}

/** Clone Wars and Polygon Hoarder: counts over the whole scene, so they're checked now and then, not per command. */
void checkScene()
{
    scene::Graph& g = scene::Graph::current();
    if (scene::hud::polyStats(g).scene.tris >= 10'000'000)
        A::unlock("POLYGON_HOARDER");
    int instances = 0;
    for (scene::NodeId id : g.nodes())
        if (scene::instancing::kind(g, id) == scene::instancing::Kind::Instance && ++instances >= 10'000)
            return A::unlock("CLONE_WARS");
}

/** Extensionist: an extension loaded from anywhere but Lila's own extensions folder. */
void checkExtensions()
{
    fs::path builtIn = paths::executableDir() / "extensions";
    for (const extensions::ExtensionInfo& e : extensions::ExtensionManager::get().available())
    {
        if (!e.loaded)
            continue;
        std::error_code ec;
        fs::path rel = fs::relative(e.path, builtIn, ec);
        if (ec || rel.empty() || *rel.begin() == "..")
            return A::unlock("EXTENSIONIST");
    }
}

/** Checks that need a scene query, run after the command has returned. */
void afterCommand(std::string name, json call)
{
    State& s = state();
    if (name == "setTheme")
    {
        const json& args = call.contains("args") ? call["args"] : json();
        bool positional = args.is_array() && !args.empty() && args[0].is_string();
        return themeChosen(positional ? args[0].get<std::string>() : call.value("name", std::string()));
    }
    if (name == "exportFile" || name.starts_with("export") || name == "gameExport")
        return exported(name, callPath(call));
    if (name == "uvAuto")
    {
        for (const std::string& o : callObjects(call))
            if (json info = ops::execute("meshInfo", {{"args", {o}}}); info.value("faces", 0) > 1000)
                return A::unlock("UNWRAPPED");
        return;
    }
    if (name == "measureSelection")
    {
        std::vector<std::string> objects = callObjects(call);
        s.measured = objects.empty() ? std::string() : objects.front();
        return;
    }
    if (cuts(name) && !s.measured.empty())
        for (const std::string& o : callObjects(call))
            if (o == s.measured)
                return A::unlock("MEASURE_TWICE");
}

void onCommand(const ExecuteEvent& e)
{
    if (!e.done || !e.ok || ops::agentCalling())
        return;
    State& s = state();
    std::string_view name = e.name;
    if (auto it = firstRuns().find(name); it != firstRuns().end())
        A::unlock(it->second);

    if (name == "undo" || name == "redo")
        return onUndoRedo(s, name);
    s.undoRun = 0;
    s.redoRun = 0;
    if (name == "duplicateWithTransform")
    {
        if (++s.copyRun == 10)
            A::unlock("COPY_THAT");
        return;
    }
    s.copyRun = 0;

    if (name == "saveScene" || name == "incrementAndSave")
    {
        if (localHour() == 3)
            A::unlock("NIGHT_OWL");
    }
    else if (name == "importFile" || name == "openFile")
    {
        std::string path = callPath(e.call);
        if (path.ends_with(".ma") || path.ends_with(".mb") || path.ends_with(".blend"))
            A::unlock("DEFECTOR");
        s.openedUsd = s.openedUsd || isUsd(path);
    }
    else if (name == "exportFile" && s.openedUsd && isUsd(callPath(e.call)))
        A::unlock("UNIVERSAL_TRANSLATOR");
    else if (name == "transferAttributes" && e.call.value("sampleSpace", std::string()) == "guided")
        A::unlock("HAND_ME_DOWNS");
    else if (name == "gnSetTies" || name == "texSetTies")
        A::unlock("ZIP_IT");
    else if (name == "ezCurve")
        A::add("EZ_CURVES", 1, "EZ_MODE", 5);
    else if (name == "sculptStroke")
        A::add("SCULPT_STROKES", 1, "SCULPTOR", 1000000);
    else if (name == "cameraBookmark" && e.call.value("set", false))
        A::add("BOOKMARKS_SAVED", 1, "BOOKMARKED", 9);
    else if (name == "namingConvention" && e.call.value("fix", false))
        A::unlock("NAME_AND_SHAME");
    else if (name == "uvAuto" || name == "measureSelection" || cuts(name) || name == "setTheme" ||
             name.starts_with("export") || name == "gameExport")
        mainq::post([n = std::string(name), call = e.call] { afterCommand(n, call); });
}

/** Credits crash reports the watchdog sent since the last run: Snitch, and Frequent Flyer for distinct crashes. */
void creditSentCrashes()
{
    PrefStore& prefs = PrefStore::get();
    json credited = prefs.value("general", kCreditedCrashesPref, json::array());
    if (!credited.is_array())
        credited = json::array();
    bool changed = false;
    for (const fs::path& dir : crash::reports())
    {
#ifdef _WIN32
        if (!crash::upload::alreadySent(dir))
            continue;
#endif
        std::string key = std::to_string(std::hash<std::string>{}(crash::reportSummary(dir)));
        A::unlock("SNITCH");
        if (std::find(credited.begin(), credited.end(), key) != credited.end())
            continue;
        credited.push_back(key);
        changed = true;
        A::add("CRASH_REPORTS", 1, "FREQUENT_FLYER", 10);
    }
    if (changed)
        prefs.set("general", kCreditedCrashesPref, credited);
}
} // namespace

void start()
{
    A::setOnShown(achievementBanner::show);
    A::start();
    State& s = state();
    s.listener = ops::addExecuteListener(onCommand);
    creditSentCrashes();
    checkExtensions();
    s.sceneCheck = new QTimer;
    QObject::connect(s.sceneCheck, &QTimer::timeout, [] { checkScene(); });
    s.sceneCheck->start(30'000);
}

void stop()
{
    State& s = state();
    if (s.listener >= 0)
        ops::removeExecuteListener(s.listener);
    s.listener = -1;
    delete s.sceneCheck;
    s.sceneCheck = nullptr;
    A::stop();
    A::setOnShown({});
}

void scriptEditorRan()
{
    A::unlock("PYTHONISTA");
}

void cheatSheetShown()
{
    A::unlock("CHEAT_CODE");
}

void commandSearchRan()
{
    A::add("SEARCH_RUNS", 1, "SEARCH_PARTY", 50);
}

void sceneSearchSelected()
{
    A::unlock("HIDE_AND_SEEK");
}

void primitivePlaced()
{
    A::add("PRIMITIVES_PLACED", 1, "STICK_THE_LANDING", 10);
}

void hotboxPicked()
{
    A::add("HOTBOX_PICKS", 1, "HOTBOXER", 1000);
}

void lastSceneReopened()
{
    A::unlock("DEJA_VU");
}

void recoveredSceneOpened()
{
    A::unlock("PHOENIX");
}

void presenterModeOn()
{
    A::unlock("GOING_LIVE");
}

void todoListTicked(int items, int ticked)
{
    if (items >= 10 && ticked == items)
        A::unlock("DONE_AND_DUSTED");
}

void outlinerFilter(std::string_view text)
{
    if (text.starts_with('!') || text.find(" !") != std::string_view::npos || text.find(",!") != std::string_view::npos)
        A::unlock("NOT_YOU");
}

void texturesDropped(int assigned)
{
    if (assigned >= 3)
        A::unlock("CARE_PACKAGE");
}

} // namespace lila::ui::achievementRules
