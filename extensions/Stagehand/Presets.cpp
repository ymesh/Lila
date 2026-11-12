// Arrangement presets: saved specs that dress a place in one call (a storeroom, a rubble corner, a library shelf, a
// messy desk). A spec names its slots (the floor, the boards, the books) and lists arrangement commands that use them;
// arrangePreset fills the slots and runs the steps as one undo step, gathering what they make into one group.
//
//   { "name", "description",
//     "slots": { "<slot>": { "kind": "mesh" | "meshes" | "items", "doc", "default": ..., "optional": false } },
//     "steps": [ { "command", "args": [...], "flags": { ... }, "needs": ["<slot>"] } ] }
//
// Strings in args and flags may be "$slot" (the slot's value), "$slot.top" (the top centre of a mesh slot's bounds) or
// "$slot.corner" (a point on its top, a fifth of the way in from its lowest corner). A step whose needs include an unset
// optional slot is skipped.

#include "Library.h"
#include "Stagehand.h"

#include <lila/Command.h>
#include <lila/Extension.h>
#include <lila/Prefs.h>
#include <lila/Profile.h>
#include <lila/Transform.h>
#include <lila/Workspace.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>

using namespace lila;
namespace fs = std::filesystem;

namespace stagehand
{
namespace
{
using T = ArgType;

/** The presets every install has, built on the starter library where they can be. */
std::vector<json> builtinPresets()
{
    static const json specs = json::parse(R"([
  {"name": "Storeroom",
   "description": "Crates, drums and supply cases scattered over a floor, a stack of pallets in one corner.",
   "slots": {"floor": {"kind": "mesh", "doc": "The floor to dress."},
             "boxes": {"kind": "items", "doc": "What stands about.", "default": ["Industrial/Crate", "Industrial/Barrel", "Facility/SupplyCrate"]},
             "pallets": {"kind": "items", "doc": "What is stacked in the corner.", "default": ["Industrial/Pallet"]}},
   "steps": [{"command": "scatterProps", "args": ["$floor"],
              "flags": {"items": "$boxes", "density": 0.6, "spacing": 1.2, "realize": true, "settle": true, "name": "Boxes"}},
             {"command": "stackProps",
              "flags": {"items": "$pallets", "at": "$floor.corner", "count": 6, "onto": ["$floor"], "realize": true, "name": "Pallets"}}]},
  {"name": "Rubble Corner",
   "description": "A heap of broken stone in a corner, rebar and chunks strewn about, chips around anything broken.",
   "slots": {"floor": {"kind": "mesh", "doc": "The ground the rubble lies on."},
             "rubble": {"kind": "items", "doc": "Chunks for the heap.", "default": ["Debris/StoneChunkS1", "Debris/StoneChunkM1", "Debris/StoneChunkM2"]},
             "litter": {"kind": "items", "doc": "What lies strewn about.", "default": ["Debris/Rebar", "Debris/RebarBent", "Debris/StoneChunkS1", "Debris/StoneChunkS2"]},
             "broken": {"kind": "meshes", "doc": "Broken walls or fracture pieces to shed chips.", "optional": true}},
   "steps": [{"command": "pileProps",
              "flags": {"items": "$rubble", "at": "$floor.corner", "count": 40, "radius": 90.0, "onto": ["$floor"], "realize": true, "name": "Heap"}},
             {"command": "scatterProps", "args": ["$floor"],
              "flags": {"items": "$litter", "density": 1.5, "realize": true, "settle": true, "name": "Litter"}},
             {"command": "debrisProps", "args": ["$broken"], "flags": {"onto": ["$floor"], "name": "Chips"}, "needs": ["broken"]}]},
  {"name": "Library Shelf",
   "description": "Every board filled with books, some leaning, a few lying in small stacks, the boards sagging.",
   "slots": {"boards": {"kind": "meshes", "doc": "The shelf boards."},
             "books": {"kind": "items", "doc": "Books: library assets or meshes."}},
   "steps": [{"command": "fillShelf", "args": ["$boards"], "flags": {"items": "$books", "realize": true, "sag": true, "name": "Books"}}]},
  {"name": "Messy Desk",
   "description": "Clutter strewn over a desk top and a stack of books at one corner.",
   "slots": {"desk": {"kind": "mesh", "doc": "The desk top."},
             "clutter": {"kind": "items", "doc": "Small things: mugs, pens, papers."},
             "books": {"kind": "items", "doc": "Books for the stack.", "optional": true}},
   "steps": [{"command": "scatterProps", "args": ["$desk"],
              "flags": {"items": "$clutter", "density": 40.0, "realize": true, "settle": true, "name": "Clutter"}},
             {"command": "stackProps",
              "flags": {"items": "$books", "at": "$desk.corner", "count": 5, "onto": ["$desk"], "settle": true, "realize": true, "name": "BookStack"},
              "needs": ["books"]}]}
])");
    return specs.get<std::vector<json>>();
}

fs::path userPresetDir() { return PrefStore::get().userDir() / "arrangement-presets"; }

fs::path workspacePresetDir()
{
    const Workspace& ws = Workspace::get();
    return ws.isOpen() ? ws.root() / "ArrangementPresets" : fs::path();
}

std::string fileSafe(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' ? c : '_';
    return out.empty() ? "Preset" : out;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** Every preset, workspace first, then the user's, then the built-ins; the first of a name wins. */
std::vector<json> allPresets()
{
    std::vector<json> out;
    std::set<std::string> seen;
    auto add = [&](json spec, const std::string& source, const fs::path& file) {
        const std::string key = lower(spec.value("name", std::string()));
        if (key.empty() || !seen.insert(key).second)
            return;
        spec["source"] = source;
        if (!file.empty())
            spec["path"] = file.generic_string();
        out.push_back(std::move(spec));
    };
    for (const auto& [dir, source] : {std::pair{workspacePresetDir(), "workspace"}, std::pair{userPresetDir(), "user"}})
    {
        std::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            continue;
        std::vector<fs::path> files;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
            if (e.path().extension() == ".json")
                files.push_back(e.path());
        std::ranges::sort(files);
        for (const fs::path& f : files)
            try
            {
                std::ifstream in(f);
                add(json::parse(in), source, f);
            }
            catch (const std::exception& e)
            {
                log::write(LogLevel::Warning, "arrangement preset " + f.generic_string() + ": " + e.what());
            }
    }
    for (json& b : builtinPresets())
        add(std::move(b), "builtin", {});
    return out;
}

json findPreset(const std::string& name, const char* cmd)
{
    for (json& p : allPresets())
        if (lower(p.value("name", std::string())) == lower(name))
            return p;
    std::string names;
    for (const json& p : allPresets())
        names += (names.empty() ? "" : ", ") + p.value("name", std::string());
    throw CommandError(std::string(cmd) + ": no preset named '" + name + "' (there are " + names + ")");
}

/** A spec's shape, before it is saved or run: slots with kinds, steps naming commands that exist. */
void validate(const json& spec, const char* cmd)
{
    if (!spec.is_object() || !spec.contains("steps") || !spec["steps"].is_array() || spec["steps"].empty())
        throw CommandError(std::string(cmd) + ": a preset needs steps: [{command, args, flags}]");
    const json slots = spec.value("slots", json::object());
    if (!slots.is_object())
        throw CommandError(std::string(cmd) + ": slots is a dict of name: {kind, doc, default}");
    for (const auto& [name, s] : slots.items())
    {
        const std::string kind = s.value("kind", std::string());
        if (kind != "mesh" && kind != "meshes" && kind != "items")
            throw CommandError(std::string(cmd) + ": slot '" + name + "' kind is mesh, meshes or items, not '" + kind + "'");
    }
    for (const json& step : spec["steps"])
    {
        const std::string c = step.value("command", std::string());
        if (c.empty() || !ops::registry().find(c))
            throw CommandError(std::string(cmd) + ": step command '" + c + "' does not exist");
    }
}

struct Filled
{
    std::map<std::string, json> values; // slot -> value; absent when an optional slot is unset
};

/** A mesh slot's top centre, or a point a fifth in from its lowest corner. */
json pointOf(Graph& g, const json& value, bool corner, const std::string& slot, const char* cmd)
{
    const std::string name = value.is_array() ? value.at(0).get<std::string>() : value.get<std::string>();
    Node* n = g.find(name);
    glm::dvec3 lo, hi;
    if (!n || !boundsUnder(g, n->id(), lo, hi))
        throw CommandError(std::string(cmd) + ": slot " + slot + ": '" + name + "' has no bounds");
    const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
    const glm::dvec3 local = corner ? glm::dvec3(lo.x + (hi.x - lo.x) * 0.2, hi.y, lo.z + (hi.z - lo.z) * 0.2)
                                    : glm::dvec3((lo.x + hi.x) * 0.5, hi.y, (lo.z + hi.z) * 0.5);
    const glm::dvec3 p(w * glm::dvec4(local, 1.0));
    return json::array({p.x, p.y, p.z});
}

/** Replaces $slot references in a step's args or flags. */
json substitute(Graph& g, const json& j, const Filled& f, const char* cmd)
{
    if (j.is_string())
    {
        const std::string s = j.get<std::string>();
        if (s.empty() || s[0] != '$')
            return j;
        const size_t dot = s.find('.');
        const std::string slot = s.substr(1, dot == std::string::npos ? std::string::npos : dot - 1);
        const auto it = f.values.find(slot);
        if (it == f.values.end())
            throw CommandError(std::string(cmd) + ": the preset uses slot " + slot + ", which is not set");
        if (dot == std::string::npos)
            return it->second;
        const std::string what = s.substr(dot + 1);
        if (what != "top" && what != "corner")
            throw CommandError(std::string(cmd) + ": " + s + ": a slot gives .top or .corner");
        return pointOf(g, it->second, what == "corner", slot, cmd);
    }
    if (j.is_array())
    {
        json out = json::array();
        for (const json& e : j)
        {
            json v = substitute(g, e, f, cmd);
            // A meshes slot inside a list spreads into it.
            if (e.is_string() && v.is_array() && e.get<std::string>().find('.') == std::string::npos)
                for (const json& x : v)
                    out.push_back(x);
            else
                out.push_back(std::move(v));
        }
        return out;
    }
    if (j.is_object())
    {
        json out = json::object();
        for (const auto& [k, v] : j.items())
            out[k] = substitute(g, v, f, cmd);
        return out;
    }
    return j;
}

Filled fillSlots(Graph& g, const json& spec, const json& given, const char* cmd)
{
    Filled f;
    const json slots = spec.value("slots", json::object());
    for (const auto& [name, s] : slots.items())
    {
        json v = given.contains(name) ? given[name] : s.value("default", json());
        if (v.is_null() || (v.is_array() && v.empty()))
        {
            if (s.value("optional", false))
                continue;
            throw CommandError(std::string(cmd) + ": " + spec.value("name", std::string()) + " needs slot " + name + " (" + s.value("kind", std::string()) +
                               "): " + s.value("doc", std::string()));
        }
        const std::string kind = s.value("kind", std::string());
        if (kind == "mesh" && v.is_array())
        {
            if (v.size() != 1)
                throw CommandError(std::string(cmd) + ": slot " + name + " takes one mesh");
            v = v[0];
        }
        if ((kind == "meshes" || kind == "items") && v.is_string())
            v = json::array({v});
        if (kind != "items")
            for (const json& n : v.is_array() ? v : json::array({v}))
                if (!n.is_string() || !g.find(n.get<std::string>()))
                    throw CommandError(std::string(cmd) + ": slot " + name + ": no node named " + n.dump());
        f.values[name] = v;
    }
    for (const auto& [name, _] : given.items())
        if (!slots.contains(name))
            throw CommandError(std::string(cmd) + ": " + spec.value("name", std::string()) + " has no slot " + name);
    return f;
}

std::string madeGroup(const json& r)
{
    if (r.is_object() && r.contains("group") && r["group"].is_string())
        return r["group"].get<std::string>();
    return {};
}
} // namespace

void registerPresetCommands(ext::Context& ctx)
{
    ctx.registerCommand({
        .name = "arrangementPresets",
        .doc = "The arrangement presets: built in (Storeroom, Rubble Corner, Library Shelf, Messy Desk), saved to the user's "
               "folder, or to the workspace's ArrangementPresets folder (which wins on a name). Each has name, description, "
               "slots (kind, doc, default, optional), steps and source.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (json& p : allPresets())
                out.push_back(std::move(p));
            return out;
        },
        .examples = {"[p['name'] for p in arrangementPresets()]"},
    });

    ctx.registerCommand({
        .name = "arrangePreset",
        .doc = "Dress a place from an arrangement preset: fill its slots (the floor, the boards, the books) and it runs its "
               "steps as one undo step, gathering what they make into one group. Slots left out take their defaults; an "
               "optional slot left unset skips the steps that need it. Returns group, preset and each step's result.",
        .positionalDoc = "preset name",
        .flags = {{"slots", "sl", T::Any, nullptr, "Slot values: {slot: mesh name, [meshes] or [items]}."},
                  {"seed", "s", T::Int, int64_t(1), "Another seed gives another arrangement; each step adds its index to it."},
                  {"name", "n", T::String, nullptr, "Name of the group made (default the preset's name)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("arrangePreset: give one preset name (arrangementPresets lists them)");
            const json spec = findPreset(a.positional[0].get<std::string>(), "arrangePreset");
            validate(spec, "arrangePreset");
            const json given = a.has("slots") ? a.flags["slots"] : json::object();
            if (!given.is_object())
                throw CommandError("arrangePreset: slots is a dict of slot: value");
            const Filled f = fillSlots(g, spec, given, "arrangePreset");
            const int64_t seed = a.get<int64_t>("seed", 1);
            json steps = json::array();
            std::vector<std::string> groups;
            int64_t k = 0;
            for (const json& step : spec["steps"])
            {
                bool skip = false;
                for (const json& need : step.value("needs", json::array()))
                    skip = skip || !f.values.contains(need.get<std::string>());
                if (skip)
                {
                    steps.push_back({{"command", step["command"]}, {"skipped", true}});
                    ++k;
                    continue;
                }
                json call = substitute(g, step.value("flags", json::object()), f, "arrangePreset");
                json args = substitute(g, step.value("args", json::array()), f, "arrangePreset");
                call["args"] = args.is_array() ? args : json::array({args});
                if (!call.contains("seed"))
                    call["seed"] = seed + k;
                const json r = ops::execute(step["command"].get<std::string>(), call);
                if (const std::string grp = madeGroup(r); !grp.empty())
                    groups.push_back(grp);
                steps.push_back({{"command", step["command"]}, {"result", r}});
                ++k;
            }
            std::string group;
            if (!groups.empty())
            {
                const json made = ops::execute("group", {{"empty", true}, {"name", a.get<std::string>("name", fileSafe(spec.value("name", std::string())))}});
                group = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
                const NodeId id = g.find(group)->id();
                for (const std::string& grp : groups)
                    if (g.find(grp))
                        ops::execute("parent", {{"args", {grp, group}}});
                group = g.node(id)->path();
            }
            return {{"preset", spec.value("name", std::string())}, {"group", group}, {"steps", steps}};
        },
        .examples = {"arrangePreset('Storeroom', slots={'floor': 'Floor'})",
                     "arrangePreset('Rubble Corner', slots={'floor': 'Ground', 'broken': ['Wall_pieces']}, seed=4)",
                     "arrangePreset('Library Shelf', slots={'boards': ['Shelf_1', 'Shelf_2'], 'books': ['Books/Hardback']})"},
    });

    ctx.registerCommand({
        .name = "arrangementSave",
        .doc = "Save an arrangement preset: a spec of slots and steps (see arrangementPresets for the shape), so arrangePreset "
               "can dress any place with it. Saved to the user's presets, or the workspace's ArrangementPresets folder to "
               "share it with the project. Returns the file.",
        .positionalDoc = "preset name",
        .flags = {{"spec", "sp", T::Any, nullptr, "{description, slots: {...}, steps: [{command, args, flags, needs}]}."},
                  {"tier", "t", T::String, "user", "user or workspace.", {"user", "workspace"}},
                  {"overwrite", "o", T::Bool, false, "Replace a preset of the same name in that folder."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.size() != 1 || !a.positional[0].is_string())
                throw CommandError("arrangementSave: give the preset's name");
            json spec = a.has("spec") ? a.flags["spec"] : json();
            validate(spec, "arrangementSave");
            spec["name"] = a.positional[0].get<std::string>();
            spec.erase("source");
            spec.erase("path");
            const fs::path dir = a.get<std::string>("tier", "user") == "workspace" ? workspacePresetDir() : userPresetDir();
            if (dir.empty())
                throw CommandError("arrangementSave: no workspace is open");
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path file = dir / (fileSafe(spec["name"].get<std::string>()) + ".json");
            if (fs::exists(file) && !a.get<bool>("overwrite", false))
                throw CommandError("arrangementSave: " + file.generic_string() + " exists (overwrite=True replaces it)");
            std::ofstream(file) << spec.dump(2);
            return file.generic_string();
        },
        .examples = {"arrangementSave('Crate Row', spec={'slots': {'floor': {'kind': 'mesh'}}, 'steps': [{'command': 'scatterProps', "
                     "'args': ['$floor'], 'flags': {'items': ['Industrial/Crate'], 'density': 2, 'realize': True}}]})"},
    });
}
} // namespace stagehand
