#include "ui/Layouts.h"

#include "ops/Command.h"
#include "core/Events.h"
#include "core/ExtensionContext.h"
#include "core/PrefStore.h"
#include "core/Workspace.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace lila::ui::layouts
{
namespace fs = std::filesystem;

namespace
{
constexpr const char* kReserved = "main";

GuiHooks& hooks()
{
    static GuiHooks h;
    return h;
}

std::string fileFor(const std::string& name)
{
    return "layouts/" + name;
}

/** Each layout as it was last left, so switching back restores it until it is reset. */
constexpr const char* kLiveFile = "layoutsLive";

std::optional<json> liveLayout(const std::string& name)
{
    json v = PrefStore::get().value(kLiveFile, "/" + name);
    return v.is_object() && !v.empty() ? std::optional<json>(std::move(v)) : std::nullopt;
}

void dropLive(const std::string& name)
{
    PrefStore::get().remove(kLiveFile, "/" + name);
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

fs::path userLayoutsDir()
{
    return PrefStore::get().userDir() / "layouts";
}

fs::path workspaceLayoutsDir()
{
    auto& ws = Workspace::get();
    return ws.isOpen() ? ws.stateDir(false) / "layouts" : fs::path();
}

fs::path pathFor(const std::string& name, Origin origin)
{
    fs::path dir = origin == Origin::Workspace ? workspaceLayoutsDir() : userLayoutsDir();
    std::u8string file(reinterpret_cast<const char8_t*>(name.data()), name.size());
    return dir.empty() ? fs::path() : dir / (file + u8".json");
}

/**
 * A built-in layout: the docks it opens (everything else closes), the tabs it brings to the front, sizes as shares of
 * the window, docks split off under their column ("bottom") and the menu set it starts with. The docks' columns and
 * tab groups come from the window's default arrangement, which every layout starts from.
 */
json preset(std::vector<std::string> open, std::vector<std::string> raise, std::string menuSet, json size = json::object(),
            std::vector<std::string> bottom = {}, json columns = json::array())
{
    json p{{"open", open}, {"raise", raise}, {"size", std::move(size)}};
    if (!bottom.empty())
        p["bottom"] = bottom;
    if (!columns.empty())
        p["columns"] = std::move(columns);
    return {{"version", 1}, {"preset", std::move(p)}, {"menuSet", std::move(menuSet)}, {"viewport", {{"quad", false}, {"camera", "persp"}}}};
}

const std::vector<std::pair<std::string, json>>& presets()
{
    // Every layout keeps the outliner and the inspector (channels, attributes) on the right: the selection is always
    // one click from its values, whatever the work.
    static const std::vector<std::string> data = {"outliner", "channelBox", "attributeEditor", "displayLayers"};
    auto with = [](std::vector<std::string> extra) {
        std::vector<std::string> all = data;
        all.insert(all.end(), extra.begin(), extra.end());
        return all;
    };
    static const std::vector<std::pair<std::string, json>> all = {
        {"Model", preset(with({"modelingToolkit", "topologyToolkit", "curveToolkit", "modifierStack"}), {"modelingToolkit", "channelBox"}, "Model")},
        // The UV editor takes its own full-height column beside the viewport; the data column stays whole beyond it.
        {"UV", preset(with({"uvEditor", "modifierStack"}), {"uvEditor", "channelBox"}, "UV", {{"uvEditor", {{"width", 580}}}}, {},
                      json::array({{"uvEditor"}, {"outliner", "channelBox"}}))},
        {"Sculpt", preset(with({"sculptPanel"}), {"sculptPanel", "channelBox"}, "Sculpt", {{"sculptPanel", {{"width", 600}}}})},
        {"Texture", preset(with({"textureSets", "baker", "materialEditor"}), {"textureSets", "materialEditor"}, "Texture")},
        {"Bake", preset(with({"baker", "textureSets", "materialEditor"}), {"baker", "channelBox"}, "Texture", {{"baker", {{"width", 375}}}})},
        {"Rig", preset(with({"timeSlider"}), {"channelBox"}, "Rig", {{"outliner", {{"height", 0.5}}}})},
        {"Animate", preset(with({"animLayers", "graphEditor", "timeSlider"}), {"graphEditor", "channelBox"}, "Animate",
                           {{"outliner", {{"height", 0.45}}}, {"graphEditor", {{"height", 0.32}}}})},
        {"Level", preset(with({"levelToolkit", "stagehand.library", "modelingToolkit"}), {"levelToolkit", "channelBox"}, "Level",
                         {{"stagehand.library", {{"height", 0.4}}}}, {"stagehand.library"})},
        {"Effects", preset(with({"timeSlider"}), {"channelBox"}, "Effects")},
        {"Groom", preset(with({"groomPalette"}), {"groomPalette", "channelBox"}, "Groom", {{"groomPalette", {{"width", 300}}}})},
        {"Print", preset(with({"modelingToolkit"}), {"modelingToolkit", "channelBox"}, "Print")},
        {"Script", preset(with({"scriptEditor", "scriptOutput"}), {"scriptEditor", "channelBox"}, "Script", {{"scriptEditor", {{"height", 0.45}}}})},
    };
    return all;
}

std::string nameArg(const Args& a, const char* command)
{
    std::string name = a.get<std::string>("name", "");
    if (name.empty() && !a.positional.empty() && a.positional[0].is_string())
        name = a.positional[0].get<std::string>();
    if (name.empty())
        throw CommandError(std::string(command) + ": give a layout name");
    return name;
}

void requireGui(const char* command, bool present)
{
    if (!present)
        throw CommandError(std::string(command) + ": layouts need the Lila UI (not available in batch mode)");
}
} // namespace

const char* originName(Origin o)
{
    switch (o)
    {
    case Origin::BuiltIn: return "builtin";
    case Origin::User: return "user";
    case Origin::Workspace: return "workspace";
    case Origin::Extension: return "extension";
    }
    return "user";
}

std::vector<Entry> merge(const std::vector<std::string>& builtIns, const std::vector<std::string>& user,
                         const std::vector<std::string>& workspace)
{
    std::map<std::string, Origin> top;
    for (const std::string& n : user)
        top[n] = Origin::User;
    for (const std::string& n : workspace)
        top[n] = Origin::Workspace;

    std::vector<Entry> out;
    std::set<std::string> seen;
    for (const std::string& n : builtIns)
    {
        auto it = top.find(n);
        out.push_back({n, it == top.end() ? Origin::BuiltIn : it->second, it != top.end()});
        seen.insert(n);
    }
    std::vector<Entry> rest;
    for (auto& [n, origin] : top)
        if (!seen.count(n))
            rest.push_back({n, origin, false});
    std::stable_sort(rest.begin(), rest.end(), [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); });
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

std::vector<std::string> namesIn(const fs::path& dir)
{
    std::vector<std::string> out;
    std::error_code ec;
    if (dir.empty() || !fs::is_directory(dir, ec))
        return out;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        if (!e.is_regular_file(ec) || e.path().extension() != ".json")
            continue;
        std::u8string stem = e.path().stem().u8string();
        std::string name(stem.begin(), stem.end());
        if (name == kReserved || name.empty())
            continue;
        out.push_back(std::move(name));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Entry> list()
{
    std::vector<std::string> fixed = builtInNames();
    for (const std::string& name : contributed().idsInOrder())
        if (std::find(fixed.begin(), fixed.end(), name) == fixed.end())
            fixed.push_back(name);
    std::vector<Entry> out = merge(fixed, namesIn(userLayoutsDir()), namesIn(workspaceLayoutsDir()));
    std::vector<std::string> builtIns = builtInNames();
    for (Entry& e : out)
        if (e.origin == Origin::BuiltIn && std::find(builtIns.begin(), builtIns.end(), e.name) == builtIns.end())
            e.origin = Origin::Extension;
    return out;
}

Registry<json>& contributed()
{
    static Registry<json> r;
    return r;
}

void add(const std::string& name, json layout, std::string owner)
{
    contributed().put(name, std::move(layout), std::move(owner));
}

uint64_t add(ext::Context& ctx, const std::string& name, json layout)
{
    if (const std::string* owner = contributed().ownerOf(name); owner && *owner != ctx.owner())
    {
        ctx.log(LogLevel::Error, "layout '" + name + "' already belongs to " + *owner);
        return 0;
    }
    add(name, std::move(layout), ctx.owner());
    return ctx.track("layout:" + name, [name, own = ctx.owner()] {
        if (const std::string* o = contributed().ownerOf(name); o && *o == own)
            contributed().remove(name);
    });
}

std::optional<Entry> find(const std::string& name)
{
    for (Entry& e : list())
        if (e.name == name)
            return e;
    return std::nullopt;
}

std::string invalidReason(const std::string& name)
{
    if (name.empty())
        return "a layout needs a name";
    if (name == kReserved)
        return "\"main\" is the window's own saved layout; pick another name";
    if (name.front() == '.' || name.front() == ' ' || name.back() == ' ' || name.back() == '.')
        return "a layout name can't start or end with a space or a dot";
    for (char c : name)
        if (std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos || (unsigned char)c < 0x20)
            return std::string("a layout name can't contain '") + c + "'";
    return {};
}

std::vector<std::string> builtInNames()
{
    std::vector<std::string> out;
    for (auto& [name, layout] : presets())
        out.push_back(name);
    return out;
}

std::optional<json> builtIn(const std::string& name)
{
    for (auto& [n, layout] : presets())
        if (n == name)
            return std::optional<json>(std::in_place, layout);
    if (const json* layout = contributed().find(name))
        return std::optional<json>(std::in_place, *layout);
    return std::nullopt;
}

std::optional<json> read(const std::string& name)
{
    std::optional<Entry> e = find(name);
    if (!e)
        return std::nullopt;
    if (e->origin == Origin::BuiltIn || e->origin == Origin::Extension)
        return builtIn(name);
    json doc = PrefStore::get().merged(fileFor(name));
    if (!doc.is_object() || doc.empty())
        return builtIn(name);
    return doc;
}

void write(const std::string& name, const json& layout, Origin scope)
{
    if (std::string why = invalidReason(name); !why.empty())
        throw CommandError(why);
    if (scope == Origin::Workspace && !Workspace::get().isOpen())
        throw CommandError("no folder is open to save a workspace layout in");
    PrefStore& p = PrefStore::get();
    p.set(fileFor(name), "", layout, scope == Origin::Workspace ? PrefScope::Workspace : PrefScope::User);
    p.flush();
}

bool remove(const std::string& name)
{
    std::optional<Entry> e = find(name);
    if (!e || e->origin == Origin::BuiltIn || e->origin == Origin::Extension)
        return false;
    PrefStore& p = PrefStore::get();
    // Emptied through the store first so its cached copy doesn't outlive the file.
    p.set(fileFor(name), "", json::object(), e->origin == Origin::Workspace ? PrefScope::Workspace : PrefScope::User);
    p.flush();
    std::error_code ec;
    fs::remove(pathFor(name, e->origin), ec);
    dropLive(name);
    if (current() == name && !find(name))
        setCurrent("");
    return true;
}

void rename(const std::string& from, const std::string& to)
{
    if (std::string why = invalidReason(to); !why.empty())
        throw CommandError(why);
    std::optional<Entry> e = find(from);
    if (!e)
        throw CommandError("no layout named \"" + from + "\"");
    if (e->origin == Origin::BuiltIn || e->origin == Origin::Extension)
        throw CommandError("\"" + from + "\" is built in; save it under a new name instead");
    if (from == to)
        return;
    if (std::optional<Entry> clash = find(to); clash && clash->origin != Origin::BuiltIn && clash->origin != Origin::Extension)
        throw CommandError("a layout named \"" + to + "\" already exists");
    std::optional<json> doc = read(from);
    bool wasCurrent = current() == from;
    std::optional<json> live = liveLayout(from);
    write(to, doc.value_or(json::object()), e->origin);
    remove(from);
    if (live)
        PrefStore::get().set(kLiveFile, "/" + to, *live);
    if (wasCurrent)
        setCurrent(to);
}

std::string current()
{
    return PrefStore::get().value<std::string>("general", "/layouts/current", "");
}

void setCurrent(const std::string& name)
{
    PrefStore::get().set("general", "/layouts/current", name);
}

void setGuiHooks(GuiHooks h)
{
    hooks() = std::move(h);
}

void registerCommands()
{
    using T = ArgType;
    FlagDef nameFlag{"name", "n", T::String, nullptr, "Layout name"};

    ops::registerCommand({
        .name = "listLayouts",
        .doc = "UI layouts: [{name, origin, current, shadowsBuiltIn}]. origin is builtin | user | workspace.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            std::string cur = current();
            for (const Entry& e : list())
                out.push_back({{"name", e.name}, {"origin", originName(e.origin)}, {"current", e.name == cur}, {"shadowsBuiltIn", e.shadowsBuiltIn}});
            return out;
        },
    });

    ops::registerCommand({
        .name = "applyLayout",
        .doc = "Arrange the window's panels, viewports and shelf as the named layout, as it was last left. reset=True "
               "applies its saved or built-in arrangement instead. UI only.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"reset", "r", T::Bool, false, "Apply the saved or built-in arrangement, dropping how it was last left"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = nameArg(a, "applyLayout");
            requireGui("applyLayout", bool(hooks().apply));
            std::optional<json> layout = read(name);
            if (!layout)
                throw CommandError("applyLayout: no layout named \"" + name + "\"");
            const bool reset = a.get<bool>("reset", false);
            const std::string from = current();
            if (from == name && !reset)
                return name;
            if (!from.empty() && from != name && hooks().capture && find(from))
                PrefStore::get().set(kLiveFile, "/" + from, hooks().capture());
            if (reset)
                dropLive(name);
            else if (std::optional<json> live = liveLayout(name))
                layout = std::move(live);
            hooks().apply(name, *layout);
            setCurrent(name);
            events::send({.type = events::Event::LayoutChanged, .name = name});
            return name;
        },
    });

    ops::registerCommand({
        .name = "saveLayout",
        .doc = "Save the current panel arrangement as a named layout. workspace=True saves it in the open folder's .lila/layouts.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"workspace", "ws", T::Bool, false, "Save in the workspace instead of the user prefs"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = nameArg(a, "saveLayout");
            requireGui("saveLayout", bool(hooks().capture));
            write(name, hooks().capture(), a.get<bool>("workspace", false) ? Origin::Workspace : Origin::User);
            dropLive(name);
            setCurrent(name);
            return name;
        },
    });

    ops::registerCommand({
        .name = "deleteLayout",
        .doc = "Delete a saved layout (the workspace copy first, when both exist). Built-ins can't be deleted; deleting a "
               "saved layout that shadows one brings the built-in back.",
        .positionalDoc = "name",
        .flags = {nameFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = nameArg(a, "deleteLayout");
            if (!remove(name))
                throw CommandError("deleteLayout: no saved layout named \"" + name + "\"");
            return name;
        },
    });

    ops::registerCommand({
        .name = "renameLayout",
        .doc = "Rename a saved layout, keeping where it is saved.",
        .positionalDoc = "name",
        .flags = {nameFlag, {"to", "", T::String, nullptr, "New name"}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string name = nameArg(a, "renameLayout");
            std::string to = a.get<std::string>("to", "");
            if (to.empty() && a.positional.size() > 1 && a.positional[1].is_string())
                to = a.positional[1].get<std::string>();
            rename(name, to);
            return to;
        },
    });
}
} // namespace lila::ui::layouts
