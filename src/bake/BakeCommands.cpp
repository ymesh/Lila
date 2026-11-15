#include "bake/Bake.h"

#include "bake/Backend.h"
#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/HighFiles.h"
#include "bake/BakePipeline.h"
#include "bake/CageEstimate.h"
#include "bake/Maps.h"
#include "bake/Naming.h"
#include "bake/Targets.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/ProxySet.h"

#include <algorithm>
#include <set>

namespace lila::bake
{
using T = ArgType;

namespace
{
using detail::findNode;
using detail::listArg;
using detail::projectArg;
using scene::Graph;
using scene::Node;
using scene::NodeId;

/** A mesh node's path, or a CommandError. */
std::string meshPath(Graph& g, const std::string& name, const char* cmd)
{
    Node* n = findNode(g, name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return n->path();
}

json groupInfo(Graph& g, const Node& group)
{
    const GroupSettings s = groupSettings(group);
    json j = s.toJson();
    j.erase("version");
    j["name"] = group.path();
    Node* p = groupProject(g, group);
    j["project"] = p ? json(p->path()) : json();
    return j;
}

json projectInfo(Graph& g, const Node& project)
{
    json groups = json::array();
    for (NodeId id : groupsOf(g, project.id()))
        groups.push_back(groupInfo(g, *g.node(id)));
    json s = projectSettings(project).toJson();
    s.erase("version");
    return {{"name", project.path()}, {"settings", s}, {"groups", groups}, {"folder", resolveFolder(projectSettings(project).output.folder).generic_string()}};
}

json stackArg(const json& v, const char* cmd)
{
    if (v.is_string())
    {
        json p = modifierPreset(v.get<std::string>());
        if (p.is_null())
            throw CommandError(std::string(cmd) + ": no bake modifier preset named " + v.get<std::string>());
        return p;
    }
    if (!v.is_array())
        throw CommandError(std::string(cmd) + ": modifiers= takes a list of {kind, params}");
    for (const json& e : v)
        if (!(e.is_string() || (e.is_object() && e.contains("kind"))))
            throw CommandError(std::string(cmd) + ": each modifier is a kind name or {kind, params}");
    return v;
}

void erase(std::vector<std::string>& list, const std::string& v) { std::erase(list, v); }

void addUnique(std::vector<std::string>& list, const std::string& v)
{
    if (std::ranges::find(list, v) == list.end())
        list.push_back(v);
}

/** Every mesh in the scene but anim proxy pieces, by path. */
std::vector<std::string> allMeshes(Graph& g)
{
    std::vector<std::string> out;
    for (NodeId id : g.nodes())
        if (Node* n = g.node(id); n && n->type().isA("mesh") && !scene::proxy::excludedFromExport(g, id))
            out.push_back(n->path());
    return out;
}

/** The project's group whose lows include low, if any. */
Node* groupWithLow(Graph& g, NodeId project, const std::string& low)
{
    for (NodeId id : groupsOf(g, project))
    {
        Node* n = g.node(id);
        const GroupSettings s = groupSettings(*n);
        if (std::ranges::find(s.lows, low) != s.lows.end())
            return n;
    }
    return nullptr;
}

json autoGroup(Graph& g, Node& project, std::vector<std::string> names, const json& stack)
{
    if (names.empty())
        names = allMeshes(g);
    std::vector<std::string> meshes;
    for (const std::string& n : names)
        if (Node* m = findNode(g, n); m && m->type().isA("mesh"))
            meshes.push_back(m->path());
    std::vector<std::string> unmatched;
    const std::vector<AutoGroup> matches = matchAutoGroups(meshes, &unmatched);
    json groups = json::array();
    for (const AutoGroup& m : matches)
    {
        Node* grp = nullptr;
        for (const std::string& l : m.lows)
            if (!grp)
                grp = groupWithLow(g, project.id(), l);
        GroupSettings s = grp ? groupSettings(*grp) : GroupSettings{};
        for (const std::string& l : m.lows)
            addUnique(s.lows, l);
        for (const std::string& h : m.highs)
            if (std::ranges::none_of(s.highs, [&](const HighEntry& e) { return !e.self && e.node == h; }))
                s.highs.push_back(HighEntry{.node = h, .modifiers = stack});
        if (!grp)
        {
            std::string base = m.base;
            std::replace(base.begin(), base.end(), ':', '_');
            std::replace(base.begin(), base.end(), '|', '_');
            grp = createGroup(g, project.id(), base + "_bake", s);
        }
        else
            setGroupSettings(g, grp->id(), s);
        groups.push_back(groupInfo(g, *grp));
    }
    return {{"groups", groups}, {"unmatched", unmatched}};
}

json runBakeCommand(Node& project, const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    BakeRequest r;
    r.project = project.id();
    for (const std::string& name : listArg(a, "groups"))
    {
        Node* n = findNode(g, name);
        if (!n || !isGroup(*n))
            throw CommandError(std::string(cmd) + ": " + name + " is not a bake group");
        r.groups.push_back(n->id());
    }
    r.maps = listArg(a, "maps");
    r.dryRun = a.get<bool>("dryRun", false);
    r.backend = a.get<std::string>("backend", "cpu");
    try
    {
        if (a.get<bool>("async", false) && !r.dryRun)
        {
            bakeAsync(g, r);
            return {{"async", true}, {"project", project.name()}, {"backend", r.backend}};
        }
        return bake(g, r).toJson();
    }
    catch (const progress::Cancelled&)
    {
        throw CommandError(std::string(cmd) + ": cancelled");
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

const FlagDef kProjectFlag{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."};

const FlagDef kOverrideGroupsFlag{"overrideGroups", "og", T::Bool, false,
                                  "Give every existing group the target's group defaults, including settings made on a group by hand."};

/** A target's maps and output onto a project, then its group defaults onto the project's groups (rebaseGroupDefaults). */
void applyTargetTo(Graph& g, Node& project, const TargetProfile& t, bool optional, bool force, std::vector<std::string>& skipped)
{
    const ProjectSettings before = projectSettings(project);
    setProjectSettings(g, project.id(), applyTarget(before, t, optional, &skipped));
    if (t.groupDefaults.empty())
        return;
    for (NodeId id : groupsOf(g, project.id()))
    {
        const GroupSettings s = groupSettings(*g.node(id));
        const GroupSettings next = rebaseGroupDefaults(s, before.groupDefaults, t.groupDefaults, force);
        if (next.toJson() != s.toJson())
            setGroupSettings(g, id, next);
    }
}

/** The target's backend when this build has it, else empty. */
std::string targetBackend(const TargetProfile& t)
{
    const std::vector<std::string> names = backendNames();
    return std::ranges::find(names, t.backend) != names.end() ? t.backend : std::string();
}
} // namespace

/** bakeCage, bakePaint, bakeLint, bakeGroupPerMesh, bakeArrange, createBakeFromGenerator (BakeGroupCommands.cpp). */
void registerGroupCommands();

void registerCommands()
{
    registerTypes();
    registerBuiltinMaps();
    registerGroupCommands();
    registerCageCommands();
    registerPipelineCommands();
    if (ops::find("createBakeProject"))
        return;
    events::subscribe(events::Event::Shutdown, [](const events::EventData&) { cancelAsyncBake(true); });

    ops::registerCommand({
        .name = "createBakeProject",
        .doc = "Create a bake project: output folder, file naming, resolution and the maps to bake (normal, object normal, height, "
               "position, curvature, cavity, thickness and ao by default). Add groups with bakeGroup or autoGroupBake, then bake. Returns its name.",
        .flags = {{"name", "n", T::String, "", "Name; bakeProject1 when empty."},
                  {"target", "t", T::String, "", "Apply a target profile's maps and naming (see bakeTargets)."},
                  {"folder", "fo", T::String, nullptr, "Output folder; relative paths are under the workspace, else the scene file's folder."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            ProjectSettings s = ProjectSettings::defaults();
            if (a.has("folder"))
                s.output.folder = a.get<std::string>("folder");
            if (!a.get<std::string>("target").empty())
            {
                const auto t = findTarget(a.get<std::string>("target"));
                if (!t)
                    throw CommandError("createBakeProject: no target named " + a.get<std::string>("target"));
                std::vector<std::string> skipped;
                s = applyTarget(s, *t, false, &skipped);
                for (const std::string& m : skipped)
                    log::warn("createBakeProject: {} is not implemented yet; skipped", m);
            }
            return createProject(g, a.get<std::string>("name", ""), s)->path();
        },
    });

    ops::registerCommand({
        .name = "bakeGroup",
        .doc = "Create or edit a bake group: the lows that receive maps and the highs projected onto them. A group only traces "
               "its own highs, so groups overlapping in space never affect each other, and several groups can write into one "
               "texture. Each high carries a bake-only modifier stack (modifiers= or preset=, e.g. \"Smooth x2\") evaluated into "
               "the bake's snapshot only: nothing is added to the scene. self=True bakes the lows onto themselves through such a "
               "stack. Creates the group (and a project) when the name is new. Returns the group.",
        .positionalDoc = "[group]",
        .flags = {kProjectFlag,
                  {"lows", "lo", T::List, nullptr, "Replace the lows."},
                  nodeFlag({"highs", "hi", T::List, nullptr, "Replace the highs (mesh names)."}, "mesh"),
                  {"addLow", "al", T::List, nullptr, "Add lows."},
                  {"removeLow", "rl", T::List, nullptr, "Remove lows."},
                  {"addHigh", "ah", T::List, nullptr, "Add highs."},
                  {"removeHigh", "rh", T::List, nullptr, "Remove highs."},
                  {"self", "sf", T::Bool, nullptr, "Add (True) or remove (False) the lows themselves as a high."},
                  {"modifiers", "md", T::Any, nullptr,
                   "Bake-only modifier stack, [{\"kind\": \"subdivision\", \"params\": {\"levels\": 2}}] or a preset name, for the "
                   "highs given in this call (else every high). Kinds are modifierKinds(). A subdivision here blends its uvs unless "
                   "its params say uvs: 1 (Fit Surface), which takes many times longer on dense highs."},
                  {"preset", "ps", T::String, nullptr, "A bake modifier preset instead of modifiers=: Smooth x2, Smooth x1-x4, Auto Bevel + Smooth, none."},
                  nodeFlag({"cage", "cg", T::String, nullptr, "A cage mesh with the low's topology (one low); empty uses the auto cage."}, "mesh"),
                  {"cageOffset", "co", T::Float, nullptr, "Scales the auto cage's push (frontal distance).", {}, {}, 0.0},
                  {"frontal", "fd", T::Float, nullptr, "How far outside the low rays start, cm; 0 is auto (measured against the highs).", {}, {}, 0.0},
                  {"rear", "rd", T::Float, nullptr, "How far inside the low rays reach, cm; 0 is auto (measured against the highs).", {}, {}, 0.0},
                  {"ignoreBackfaces", "ib", T::Bool, nullptr, "Rays only hit high faces facing them."},
                  {"aoScope", "as", T::String, nullptr, "AO occluders: self, group or all.", {"self", "group", "all"}},
                  {"skew", "sk", T::Float, nullptr, "0 to 1: tilts rays from the smooth normals toward each low face's normal.", {}, {}, 0.0, 1.0},
                  {"unlinks", "ul", T::Any, nullptr,
                   "Replace the unlinks: [{\"lows\": [...] (empty: every low), \"meshes\": [...], \"materials\": [...], \"occlusion\": True, "
                   "\"projection\": False}]. The lows' AO and other occlusion rays pass through those meshes (any high or occluder, a "
                   "self high by its low's name) and through every high with faces of those materials; projection=True also bakes "
                   "nothing from them onto the lows."},
                  {"addHighFile", "ahf", T::List, nullptr, "Add highs streamed from files at bake time (.fbx, .obj, .abc), never loaded into the scene."},
                  {"removeHighFile", "rhf", T::List, nullptr, "Remove file highs."},
                  {"fileObject", "fob", T::String, nullptr, "With addHighFile: only the file's meshes whose name contains this."},
                  {"fileFrame", "ffr", T::Float, nullptr, "With addHighFile: the Alembic frame to read."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node* grp = nullptr;
            std::string name;
            if (!a.positional.empty() && a.positional[0].is_string())
                name = a.positional[0].get<std::string>();
            if (!name.empty())
                grp = findNode(g, name);
            if (grp && !isGroup(*grp))
                throw CommandError("bakeGroup: " + name + " is not a bake group");
            const bool created = !grp;
            if (!grp)
            {
                Node& project = projectArg(Graph::current(), a, "bakeGroup", true);
                grp = createGroup(g, project.id(), name);
            }
            else if (a.has("project"))
            {
                Node& project = projectArg(Graph::current(), a, "bakeGroup", true);
                setGroupProject(g, grp->id(), project.name());
            }

            GroupSettings s = groupSettings(*grp);
            const Node* owner = groupProject(g, *grp);
            const json newStack = owner ? defaultStack(projectSettings(*owner).groupDefaults) : json::array();
            if (a.has("lows"))
            {
                s.lows.clear();
                for (const std::string& l : listArg(a, "lows"))
                    addUnique(s.lows, meshPath(g, l, "bakeGroup"));
            }
            for (const std::string& l : listArg(a, "addLow"))
                addUnique(s.lows, meshPath(g, l, "bakeGroup"));
            for (const std::string& l : listArg(a, "removeLow"))
                erase(s.lows, meshPath(g, l, "bakeGroup"));

            std::vector<std::string> named;
            if (a.has("highs"))
            {
                std::erase_if(s.highs, [](const HighEntry& h) { return !h.self; });
                for (const std::string& h : listArg(a, "highs"))
                    named.push_back(meshPath(g, h, "bakeGroup"));
            }
            for (const std::string& h : listArg(a, "addHigh"))
                named.push_back(meshPath(g, h, "bakeGroup"));
            for (const std::string& h : named)
                if (std::ranges::none_of(s.highs, [&](const HighEntry& e) { return !e.self && e.node == h; }))
                    s.highs.push_back(HighEntry{.node = h, .modifiers = newStack});
            for (const std::string& f : listArg(a, "addHighFile"))
            {
                HighEntry e{.file = f, .object = a.get<std::string>("fileObject", ""), .frame = a.get<double>("fileFrame", 0.0)};
                if (highFileKey(e).empty())
                    throw CommandError("bakeGroup: high file " + f + " doesn't exist");
                e.modifiers = newStack;
                if (std::ranges::none_of(s.highs, [&](const HighEntry& x) { return x.file == f && x.object == e.object && x.frame == e.frame; }))
                    s.highs.push_back(e);
                named.push_back(f);
            }
            for (const std::string& f : listArg(a, "removeHighFile"))
                std::erase_if(s.highs, [&](const HighEntry& x) { return x.file == f; });
            for (const std::string& h : listArg(a, "removeHigh"))
            {
                const std::string p = meshPath(g, h, "bakeGroup");
                std::erase_if(s.highs, [&](const HighEntry& e) { return !e.self && e.file.empty() && e.node == p; });
            }
            // A self high the defaults gave a new group gives way to the highs this call names.
            if (created && !named.empty() && !a.has("self"))
                std::erase_if(s.highs, [](const HighEntry& e) { return e.self; });
            if (a.has("self"))
            {
                const bool on = a.get<bool>("self");
                const bool has = std::ranges::any_of(s.highs, [](const HighEntry& e) { return e.self; });
                if (on && !has)
                    s.highs.push_back(HighEntry{.self = true, .modifiers = newStack});
                if (!on)
                    std::erase_if(s.highs, [](const HighEntry& e) { return e.self; });
                if (on)
                    named.push_back("");
            }
            if (a.has("modifiers") || a.has("preset"))
            {
                const json stack = stackArg(a.has("modifiers") ? a.flags["modifiers"] : a.flags["preset"], "bakeGroup");
                for (HighEntry& h : s.highs)
                    if (named.empty() || std::ranges::find(named, h.self ? std::string() : h.file.empty() ? h.node : h.file) != named.end())
                        h.modifiers = stack;
            }
            if (a.has("cage"))
                s.cage = a.get<std::string>("cage").empty() ? std::string() : meshPath(g, a.get<std::string>("cage"), "bakeGroup");
            if (a.has("cageOffset"))
                s.cageOffset = std::max(0.0, a.get<double>("cageOffset"));
            if (a.has("frontal"))
                s.frontal = std::max(0.0, a.get<double>("frontal"));
            if (a.has("rear"))
                s.rear = std::max(0.0, a.get<double>("rear"));
            if (a.has("ignoreBackfaces"))
                s.ignoreBackfaces = a.get<bool>("ignoreBackfaces");
            if (a.has("skew"))
                s.skew = std::clamp(a.get<double>("skew"), 0.0, 1.0);
            if (a.has("aoScope"))
                s.aoScope = a.get<std::string>("aoScope");
            if (a.has("unlinks"))
            {
                const json& list = a.flags["unlinks"];
                if (!list.is_array() && !list.is_null())
                    throw CommandError("bakeGroup: unlinks is a list of {lows, meshes, materials, occlusion, projection}");
                s.unlinks.clear();
                for (const json& j : list.is_array() ? list : json::array())
                {
                    Unlink u;
                    try
                    {
                        u = Unlink::fromJson(j);
                    }
                    catch (const std::invalid_argument& e)
                    {
                        throw CommandError(std::string("bakeGroup: ") + e.what());
                    }
                    for (std::string& l : u.lows)
                        l = meshPath(g, l, "bakeGroup");
                    for (std::string& m : u.meshes)
                        m = meshPath(g, m, "bakeGroup");
                    if (!u.empty())
                        s.unlinks.push_back(std::move(u));
                }
            }
            setGroupSettings(g, grp->id(), s);
            return groupInfo(g, *grp);
        },
    });

    ops::registerCommand({
        .name = "autoGroupBake",
        .doc = "Make bake groups from names: lows ending _low, _lo or _LP pair with highs ending _high, _hi or _HP (numbered highs "
               "like _high1 join the same group). Uses the given meshes, else the selection, else every mesh. A group already "
               "holding the low is updated rather than duplicated. Returns {groups, unmatched}.",
        .positionalDoc = "[meshes]",
        .flags = {kProjectFlag,
                  {"preset", "ps", T::String, nullptr, "Bake modifier preset for the new highs (Smooth x2, Auto Bevel + Smooth)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(Graph::current(), a, "autoGroupBake", true);
            const json stack = a.has("preset") ? stackArg(a.flags["preset"], "autoGroupBake") : json::array();
            return autoGroup(g, project, a.objects(), stack);
        },
    });

    ops::registerCommand({
        .name = "setBakeMap",
        .doc = "Add or change a map of a bake project: on/off, file suffix ({map}), bit depth, format and options (normal: greenUp, "
               "flipX; height: range; id: source, palette; ao: rays, distance, scope). bakeMaps lists the maps. Returns the "
               "project's maps.",
        .positionalDoc = "map",
        .flags = {kProjectFlag,
                  {"enabled", "en", T::Bool, nullptr, "Bake it."},
                  {"suffix", "sx", T::String, nullptr, "The {map} token in file names; empty uses the map's default."},
                  {"bits", "b", T::Int, nullptr, "8, 16 or 32 (float EXR/TIFF); 0 uses the output's.", {}, {}, 0.0, 32.0},
                  {"format", "f", T::String, nullptr, "png, exr, tga, tif or psd; empty uses the output's."},
                  {"options", "o", T::Any, nullptr, "Map options to merge in (a dict); null values remove keys."},
                  {"remove", "rm", T::Bool, false, "Remove the map from the project."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(Graph::current(), a, "setBakeMap", true);
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("setBakeMap: name the map (bakeMaps lists them)");
            const std::string id = a.positional[0].get<std::string>();
            ProjectSettings s = projectSettings(project);
            auto it = std::ranges::find_if(s.maps, [&](const MapSettings& m) { return m.map == id; });
            if (a.get<bool>("remove"))
            {
                if (it != s.maps.end())
                    s.maps.erase(it);
            }
            else
            {
                if (!findMap(id))
                    log::warn("setBakeMap: {} is not implemented yet; bakes will skip it", id);
                if (it == s.maps.end())
                {
                    s.maps.push_back(MapSettings{.map = id});
                    it = s.maps.end() - 1;
                }
                if (a.has("enabled"))
                    it->enabled = a.get<bool>("enabled");
                if (a.has("suffix"))
                    it->suffix = a.get<std::string>("suffix");
                if (a.has("bits"))
                {
                    const int b = a.get<int>("bits");
                    if (b != 0 && b != 8 && b != 16 && b != 32)
                        throw CommandError("setBakeMap: bits must be 0, 8, 16 or 32");
                    it->bits = b;
                }
                if (a.has("format"))
                {
                    const std::string f = a.get<std::string>("format");
                    if (!f.empty() && f != "png" && f != "exr" && f != "tga" && f != "tif" && f != "tiff" && f != "psd")
                        throw CommandError("setBakeMap: format must be png, exr, tga, tif or psd");
                    it->format = f == "tiff" ? "tif" : f;
                }
                if (a.has("options"))
                {
                    if (!a.flags["options"].is_object())
                        throw CommandError("setBakeMap: options= takes a dict");
                    it->options.merge_patch(a.flags["options"]);
                }
            }
            setProjectSettings(g, project.id(), s);
            json out = json::array();
            for (const MapSettings& m : s.maps)
                out.push_back(m.toJson());
            return out;
        },
    });

    ops::registerCommand({
        .name = "setBakeOutput",
        .doc = "Set where and how a bake project writes: folder, file name template (tokens {project} {group} {mesh} {material} "
               "{map} {udim} {res} {ext}; several groups share a texture when their names resolve to the same file), format, bit "
               "depth, resolution, supersampling and padding. Returns the output settings.",
        .flags = {kProjectFlag,
                  {"folder", "fo", T::String, nullptr, "Output folder; relative paths are under the workspace, else the scene file's folder."},
                  {"name", "n", T::String, nullptr, "File name template, e.g. T_{mesh}_{map} or {material}_{map}; .{ext} is added when absent."},
                  {"format", "f", T::String, nullptr,
                   "png, exr, tga, tif or psd (one layered file per texture set, a layer per map).", {"png", "exr", "tga", "tif", "tiff", "psd"}},
                  {"bits", "b", T::Int, nullptr, "8 or 16 (PNG, PSD); 8, 16 or 32 float (TIFF); 16 half or 32 float (EXR).", {}, {}, 8.0, 32.0},
                  {"dither", "di", T::Bool, nullptr, "Triangular dither when writing 8 bit (never on ID or island maps)."},
                  {"compression", "cmp", T::String, nullptr, "deflate or none: TIFF deflates, TGA run-length encodes.", {"deflate", "none"}},
                  {"compressionLevel", "cl", T::Int, nullptr, "Deflate level for PNG, EXR and TIFF: 1 fastest (default 2) to 9 smallest.", {}, {}, 1.0, 9.0},
                  {"udimNaming", "un", T::String, nullptr, "How {udim} reads: 1001, uv (u1_v1) or uv0 (u0_v0).", {"1001", "uv", "uv0"}},
                  {"sidecar", "sc", T::Bool, nullptr, "Write <texture>.json beside each texture (map, colour space, decode data)."},
                  presetFlag({"resolution", "res", T::Int, nullptr, "Width and height.", {}, {}, 1.0}, kTextureSizePresets),
                  presetFlag({"width", "w", T::Int, nullptr, "Width in texels.", {}, {}, 1.0}, kTextureSizePresets),
                  presetFlag({"height", "h", T::Int, nullptr, "Height in texels.", {}, {}, 1.0}, kTextureSizePresets),
                  {"supersampling", "ss", T::Int, nullptr, "Samples per texel: 1, 4, 16 or 64.", {}, {}, 1.0, 64.0},
                  {"padding", "pad", T::Int, nullptr, "Texels of dilation past island borders; -1 fills the texture.", {}, {}, -1.0},
                  {"material", "mat", T::String, nullptr,
                   "Set resolution, width, height, supersampling, padding, bits and format for this texture set ({material}) only; "
                   "the name needs {material}."},
                  {"clear", "clr", T::Bool, false, "With material: drop that texture set's own settings (all of them, or only the flags given)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(Graph::current(), a, "setBakeOutput", true);
            ProjectSettings s = projectSettings(project);
            json o = s.output.toJson();
            if (a.has("material"))
            {
                const std::string mat = a.get<std::string>("material");
                if (mat.empty())
                    throw CommandError("setBakeOutput: name the material");
                json over = o.contains("materials") && o["materials"].contains(mat) ? o["materials"][mat] : json::object();
                const bool clear = a.get<bool>("clear");
                bool any = false;
                for (const char* k : kMaterialOutputKeys)
                    if (a.has(k))
                    {
                        any = true;
                        if (clear)
                            over.erase(k);
                        else
                            over[k] = a.flags[k];
                    }
                if (a.has("resolution"))
                {
                    any = true;
                    if (clear)
                        over.erase("width"), over.erase("height");
                    else
                        over["width"] = over["height"] = a.flags["resolution"];
                }
                if (clear && !any)
                    over = json::object();
                if (over.empty())
                {
                    if (o.contains("materials"))
                        o["materials"].erase(mat);
                }
                else
                    o["materials"][mat] = over;
                try
                {
                    s.output = OutputSettings::fromJson(o);
                }
                catch (const std::invalid_argument& e)
                {
                    throw CommandError(std::string("setBakeOutput: ") + e.what());
                }
                if (!s.output.materials.empty() && !hasToken(s.output.name, "material"))
                    log::warn("setBakeOutput: per-material settings need {{material}} in the file name; they are ignored until it has it");
                setProjectSettings(g, project.id(), s);
                return s.output.toJson();
            }
            for (const char* k : {"folder", "name", "format", "bits", "width", "height", "supersampling", "padding", "dither", "compression",
                                  "compressionLevel", "udimNaming", "sidecar"})
                if (a.has(k))
                    o[k] = a.flags[k];
            if (a.has("resolution"))
                o["width"] = o["height"] = a.flags["resolution"];
            try
            {
                s.output = OutputSettings::fromJson(o);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("setBakeOutput: ") + e.what());
            }
            setProjectSettings(g, project.id(), s);
            return s.output.toJson();
        },
    });

    ops::registerCommand({
        .name = "bake",
        .doc = "Bake a project's maps (or maps=) for its groups (or groups=) and write the textures, plus a {project}_bake.json "
               "manifest. Synchronous and cancellable (async=True returns at once and bakes off the main thread). dryRun=True only "
               "plans the files. Returns {outputs, stats (samples, hits, misses, backfaces, per group), warnings, manifest, seconds, "
               "timings (seconds per stage), backend, backendMode}.",
        .flags = {kProjectFlag,
                  {"groups", "g", T::List, nullptr, "Bake only these groups."},
                  {"maps", "m", T::List, nullptr, "Bake only these maps (ids from bakeMaps)."},
                  {"dryRun", "dr", T::Bool, false, "Return the files without baking or writing."},
                  {"backend", "be", T::String, "cpu", "Where to bake: cpu, or a GPU backend when one is registered."},
                  {"async", "as", T::Bool, false,
                   "Return at once and bake off the main thread (bakeStatus shows it running, bakeCancel stops it); the result "
                   "lands in bakeStatus last."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Node& project = projectArg(Graph::current(), a, "bake", false);
            return runBakeCommand(project, a, "bake");
        },
    });

    ops::registerCommand({
        .name = "bakeCancel",
        .doc = "Stop the running bake (an async one, or a synchronous one from another thread). Returns whether one was running.",
        .undoable = false,
        .run = [](const Args&) -> json {
            bool any = bakeRunning();
            cancelAsyncBake(false);
            for (const progress::Snapshot& s : progress::snapshot())
                if (s.title.starts_with("Baking") && s.cancellable && !s.cancelled)
                {
                    progress::cancel(s.id);
                    any = true;
                }
            return any;
        },
    });

    ops::registerCommand({
        .name = "bakeStatus",
        .doc = "A bake project's settings, groups and last bake result this session; with several projects and no project=, "
               "every project.",
        .flags = {kProjectFlag},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (!a.has("project") && projects(g).size() != 1)
            {
                json all = json::array();
                for (NodeId id : projects(g))
                    all.push_back(projectInfo(g, *g.node(id)));
                return {{"projects", all}, {"backends", backendNames()}};
            }
            Node& project = projectArg(Graph::current(), a, "bakeStatus", false);
            json j = projectInfo(g, project);
            j["last"] = lastResult(project.id());
            j["backends"] = backendNames();
            j["running"] = bakeRunning();
            return j;
        },
    });

    ops::registerCommand({
        .name = "bakeForTarget",
        .doc = "Bake everything a downstream tool needs in one go: applies the target profile's maps and file naming to the "
               "project (creating one if needed), auto-groups by _low/_high names when the project has no groups, bakes, and "
               "returns the bake result plus {target, skipped} (maps the target wants that are not implemented yet). Targets: "
               "unreal, substancePainter, or any in the user or workspace bake/targets folders (bakeTargets).",
        .flags = {kProjectFlag,
                  {"target", "t", T::String, nullptr, "Target profile name."},
                  {"optional", "op", T::Bool, false, "Also bake the target's optional maps."},
                  {"autoGroup", "ag", T::Bool, true, "Auto-group by name when the project has no groups."},
                  {"dryRun", "dr", T::Bool, false, "Apply the target and return the planned files without baking."},
                  {"backend", "be", T::String, nullptr, "Where to bake; defaults to the target's backend, else cpu."},
                  {"async", "as", T::Bool, false, "Return at once and bake off the main thread, as bake async=True."},
                  kOverrideGroupsFlag},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            if (!a.has("target"))
                throw CommandError("bakeForTarget: target= required (bakeTargets lists them)");
            const std::string name = a.get<std::string>("target");
            const auto target = findTarget(name);
            if (!target)
                throw CommandError("bakeForTarget: no target named " + name);
            Node& project = projectArg(Graph::current(), a, "bakeForTarget", true);
            std::vector<std::string> skipped;
            applyTargetTo(g, project, *target, a.get<bool>("optional"), a.get<bool>("overrideGroups"), skipped);
            json grouped;
            const std::vector<NodeId> existing = groupsOf(g, project.id());
            const bool highless = std::ranges::any_of(existing, [&](NodeId id) { return groupSettings(*g.node(id)).highs.empty(); });
            if (a.get<bool>("autoGroup", true) && (existing.empty() || highless))
                grouped = autoGroup(g, project, {}, json::array());
            if (groupsOf(g, project.id()).empty())
                throw CommandError("bakeForTarget: no bake groups (name meshes _low/_high, or use bakeGroup)");
            Args b = a;
            b.flags.erase("maps");
            b.flags.erase("groups");
            if (!a.has("backend"))
                if (const std::string be = targetBackend(*target); !be.empty())
                    b.flags["backend"] = be;
            json out = runBakeCommand(project, b, "bakeForTarget");
            if (!skipped.empty())
            {
                std::string list;
                for (const std::string& s : skipped)
                    list += (list.empty() ? "" : ", ") + s;
                const std::string w = "target " + target->name + ": not implemented yet, skipped: " + list;
                log::warn("{}", w);
                out["warnings"].push_back(w);
            }
            out["target"] = target->name;
            out["skipped"] = skipped;
            if (!grouped.is_null())
                out["autoGroup"] = grouped;
            return out;
        },
    });

    ops::registerCommand({
        .name = "applyBakeTarget",
        .doc = "Apply a target profile or saved preset to a project without grouping or baking: its maps, output settings and "
               "group defaults. A group setting (or a high's modifier stack) takes the new default only while it still holds the "
               "project's previous default, so settings made on one group by hand survive; overrideGroups=True replaces them too. "
               "Groups created later start from the defaults. Returns {target, backend, skipped}.",
        .positionalDoc = "target",
        .flags = {kProjectFlag,
                  {"optional", "op", T::Bool, false, "Also add the target's optional maps."},
                  kOverrideGroupsFlag},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("applyBakeTarget: name a target (bakeTargets lists them)");
            const std::string name = a.positional[0].get<std::string>();
            const auto target = findTarget(name);
            if (!target)
                throw CommandError("applyBakeTarget: no target named " + name);
            Node& project = projectArg(Graph::current(), a, "applyBakeTarget", true);
            std::vector<std::string> skipped;
            applyTargetTo(g, project, *target, a.get<bool>("optional"), a.get<bool>("overrideGroups"), skipped);
            return {{"target", target->name}, {"project", project.path()}, {"backend", targetBackend(*target)}, {"skipped", skipped}};
        },
    });

    ops::registerCommand({
        .name = "saveBakeTarget",
        .doc = "Save a project's bake settings as a reusable preset (a target profile): output settings, maps with their options "
               "and channel packs, the backend, and group defaults (distances, cage offset, AO occluders, backfaces, the bake-only "
               "modifier stack, self as high) taken from group= or the project's first group. Meshes are never saved. scope=user "
               "(every project) or workspace (.lila/bake/targets). It then lists in bakeTargets and the Baker's target menu. "
               "Returns {name, file}.",
        .positionalDoc = "name",
        .flags = {kProjectFlag,
                  {"label", "l", T::String, nullptr, "Display name; defaults to the name."},
                  {"scope", "sc", T::String, "user", "Where it is saved.", {"user", "workspace"}},
                  {"overwrite", "ow", T::Bool, false, "Replace a preset of the same name in that scope."},
                  {"group", "g", T::String, nullptr, "The group whose settings become the group defaults."},
                  {"backend", "be", T::String, nullptr, "The backend bakes for this preset run on."},
                  {"doc", "d", T::String, nullptr, "A description shown as its tooltip."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (a.positional.empty() || !a.positional[0].is_string() || a.positional[0].get<std::string>().empty())
                throw CommandError("saveBakeTarget: name the preset");
            Node& project = projectArg(Graph::current(), a, "saveBakeTarget", false);
            const ProjectSettings ps = projectSettings(project);
            TargetProfile t;
            t.name = a.positional[0].get<std::string>();
            t.label = a.get<std::string>("label", t.name);
            t.doc = a.get<std::string>("doc", "");
            t.preset = true;
            t.backend = a.get<std::string>("backend", "");
            t.output = ps.output.toJson();
            for (const MapSettings& m : ps.maps)
                t.maps.push_back(TargetMap{.settings = m});
            Node* source = nullptr;
            if (a.has("group"))
            {
                source = findNode(g, a.get<std::string>("group"));
                if (!source || !isGroup(*source))
                    throw CommandError("saveBakeTarget: " + a.get<std::string>("group") + " is not a bake group");
            }
            else if (const std::vector<NodeId> groups = groupsOf(g, project.id()); !groups.empty())
                source = g.node(groups.front());
            t.groupDefaults = source ? groupDefaultsOf(groupSettings(*source)) : ps.groupDefaults;
            t.pipeline = {{"autoAssign", ps.autoAssign}};
            try
            {
                const auto file = saveTarget(t, a.get<std::string>("scope", "user"), a.get<bool>("overwrite"));
                return {{"name", t.name}, {"file", file.generic_string()}};
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("saveBakeTarget: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "deleteBakeTarget",
        .doc = "Delete a saved bake preset (a user or workspace target profile file). Built-in targets can't be deleted. Returns "
               "the deleted file.",
        .positionalDoc = "name",
        .flags = {{"scope", "sc", T::String, nullptr, "user or workspace; defaults to the one bakeTargets shows.", {"user", "workspace"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.positional.empty() || !a.positional[0].is_string())
                throw CommandError("deleteBakeTarget: name the preset");
            try
            {
                return deleteTarget(a.positional[0].get<std::string>(), a.get<std::string>("scope", "")).generic_string();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("deleteBakeTarget: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "bakeTargets",
        .doc = "The bake target profiles available (built in, user and workspace): name, label, file, maps.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const TargetProfile& t : targets())
            {
                json j = t.toJson();
                j["builtin"] = isBuiltinTarget(t);
                json missing = json::array();
                for (const TargetMap& m : t.maps)
                    if (!findMap(m.settings.map))
                        missing.push_back(m.settings.map);
                j["unimplemented"] = missing;
                out.push_back(j);
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bakeMaps",
        .doc = "The maps the baker implements: id, label, default suffix, how texels resolve (average, exact, coverage) and options.",
        .undoable = false,
        .run = [](const Args&) -> json {
            json out = json::array();
            for (const MapDef* m : allMaps())
                out.push_back({{"id", m->id},
                               {"label", m->label},
                               {"suffix", m->suffix},
                               {"doc", m->doc},
                               {"resolve", m->resolve == Resolve::Exact ? "exact" : m->resolve == Resolve::Coverage ? "coverage" : "average"},
                               {"preferredBits", m->preferredBits}});
            return out;
        },
    });
}
} // namespace lila::bake
