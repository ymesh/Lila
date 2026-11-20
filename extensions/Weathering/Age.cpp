// Age presets and the age command. A preset is a Stagehand arrangement preset whose steps are weathering commands; the
// built-in ones are registered with Stagehand at load, so they show in its library and run through arrangePreset like
// any other. Every preset comes in a realistic and a toon version, and every piece slot defaults to the library's pieces
// for a role, the user's own first.

#include "Pieces.h"
#include "Placeholders.h"
#include "Weathering.h"

#include <lila/Command.h>
#include <lila/Deformer.h>
#include <lila/Extension.h>
#include <lila/Log.h>
#include <lila/Mesh.h>
#include <lila/Profile.h>
#include <lila/Transform.h>

#include <algorithm>
#include <cmath>
#include <map>

using namespace lila;

namespace weathering
{
void registerBasePieces();
void registerUnreal(ext::Context& ctx);
void registerWeatheringUi(ext::Context& ctx);

namespace age
{
using T = ArgType;

/** Items slot defaulting to the library's pieces with a role (user pieces first, in the style). */
json roleSlot(const char* doc, std::initializer_list<const char*> roles, const std::string& style)
{
    json def = json::array();
    for (const char* r : roles)
        def.push_back(std::string("tag:weathering+") + r + "+" + style);
    return {{"kind", "items"}, {"doc", doc}, {"default", def}};
}

json targetsSlot() { return {{"kind", "meshes"}, {"doc", "What to age: walls, buildings, Blockwork blocks, rocks."}}; }

json blockersSlot()
{
    return {{"kind", "meshes"}, {"doc", "Neighbours nothing is put on (set by weather when it dresses a level in tiles)."}, {"optional", true},
            {"default", json::array()}};
}

json baseStep(const char* slot, json flags)
{
    flags["items"] = std::string("$") + slot;
    flags["blockers"] = "$blockers";
    return {{"command", "baseDressing"}, {"args", json::array({"$targets"})}, {"flags", std::move(flags)}};
}

/**
 * The built-in presets in one style: each is ONE effect (a scatter set, grunge, edge decals, ivy, a roof), dragged
 * onto a mesh from the library and applied alone. Toon versions are chunkier: fewer, bigger pieces.
 */
std::vector<json> builtins(const std::string& style)
{
    const bool toon = style == "toon";
    const double few = toon ? 0.6 : 1.0;  // toon dressing is sparser
    const double big = toon ? 1.25 : 1.0; // and its pieces bigger
    const std::string suffix = toon ? " Toon" : "";
    auto preset = [&](const std::string& name, const char* kind, const std::string& description, json slots, json steps) {
        slots["targets"] = targetsSlot();
        slots["blockers"] = blockersSlot();
        return json{{"name", name + suffix},
                    {"description", description},
                    {"category", "Weathering"},
                    {"kind", kind},
                    {"style", style},
                    {"thumbnailVersion", 4},
                    {"tags", json::array({"weathering", kind, style})},
                    {"slots", std::move(slots)},
                    {"steps", std::move(steps)}};
    };
    auto scatter = [&](const std::string& name, const std::string& description, std::initializer_list<const char*> roles, json flags) {
        std::string node = name;
        std::ranges::replace(node, ' ', '_');
        flags["name"] = node + "_Scatter";
        return preset(name, "scatter", description, {{"pieces", roleSlot("Pieces to scatter.", roles, style)}}, json::array({baseStep("pieces", std::move(flags))}));
    };
    auto effect = [&](const std::string& name, const char* kind, const std::string& description, const char* command, json flags) {
        flags["style"] = style;
        return preset(name, kind, description, json::object(),
                      json::array({json{{"command", command}, {"args", json::array({"$targets"})}, {"flags", std::move(flags)}}}));
    };
    std::vector<json> out;
    out.push_back(scatter("Concrete Debris", "Broken concrete heaped at the foot of walls, thinning outward.", {"rubble"},
                          {{"width", 90.0 * big}, {"density", 9.0 * few}, {"falloff", 1.8}}));
    out.push_back(scatter("Heavy Rubble", "A deep bank of rubble round the base, as after a collapse.", {"rubble", "rock"},
                          {{"width", 170.0 * big}, {"density", 14.0 * few}, {"falloff", 1.2}}));
    out.push_back(scatter("Bricks", "Loose bricks along the base.", {"brick"}, {{"width", 80.0 * big}, {"density", 7.0 * few}}));
    out.push_back(scatter("Rocks", "Stones scattered round the base.", {"rock"}, {{"width", 110.0 * big}, {"density", 3.0 * few}, {"falloff", 1.0}}));
    out.push_back(scatter("Leaning Planks", "Boards and beams leaning on the walls.", {"plank", "beam"},
                          {{"width", 70.0}, {"density", 2.5 * few}, {"leanChance", 0.85}}));
    out.push_back(scatter("Charred Timber", "Burned boards fallen against the walls.", {"charred"},
                          {{"width", 100.0}, {"density", 3.0 * few}, {"leanChance", 0.4}}));
    out.push_back(scatter("Weeds", "Weeds and grass coming up along the base.", {"plant", "grass"},
                          {{"width", 60.0 * big}, {"density", 14.0 * few}, {"falloff", 1.8}}));
    out.push_back(scatter("Moss Clumps", "Moss gathered at the foot of the walls.", {"moss"}, {{"width", 40.0}, {"density", 14.0 * few}, {"falloff", 2.5}}));
    out.push_back(scatter("Bushes", "Bushes grown up beside the walls.", {"bush"},
                          {{"width", 140.0 * big}, {"density", 0.8 * few}, {"falloff", 0.8}, {"spacing", 1.1}}));
    out.push_back(scatter("Litter", "Paper, cans and leaves blown against the walls.", {"litter", "leaf"},
                          {{"width", 120.0}, {"density", 1.2 * few}, {"falloff", 0.6}, {"scaleNear", 1.0}, {"scaleFar", 1.0}}));

    out.push_back(effect("Grunge", "surface", "Grime at the foot and in the corners, worn edges, dust and streaks; keeps the surface's own texture.",
                         "grungeMask", json::object()));
    out.push_back(effect("Worn Edges", "surface", "Chipped and worn decal strips along the outer edges.", "edgeDecals", {{"edges", "convex"}}));
    out.push_back(effect("Corner Dirt", "surface", "Dirt and moss decal strips packed into the inner corners.", "edgeDecals", {{"edges", "concave"}}));
    out.push_back(effect("Chipped Edges", "surface", "Small chips broken out of the outer edges.", "edgeChip", {{"amount", 0.4}, {"depth", 3.0}}));
    out.push_back(effect("Broken Edges", "surface", "Big chunks broken out of the outer edges.", "edgeChip",
                         {{"amount", 0.8}, {"depth", 6.0}, {"size", 14.0}}));

    out.push_back(effect("Ivy", "growth", "Leafy ivy climbing from the ground.", "ivy", {{"plant", "ivy"}}));
    out.push_back(effect("Vines", "growth", "Long vines that drape over ledges.", "ivy", {{"plant", "vine"}}));
    out.push_back(effect("Roots", "growth", "Bare roots spreading over the surface.", "ivy", {{"plant", "roots"}}));
    out.push_back(effect("Moss Strands", "growth", "Flat, dense moss creeping over the surface.", "ivy", {{"plant", "moss"}}));

    out.push_back(effect("Roof Sheets", "generator", "Corrugated sheets laid over the roof, some missing or lifted.", "roofing", {{"kind", "sheets"}}));
    out.push_back(effect("Roof Tiles", "generator", "Tiles laid over the roof in rows.", "roofing", {{"kind", "tiles"}}));
    out.push_back(effect("Roof Planks", "generator", "Boards laid over the roof.", "roofing", {{"kind", "planks"}}));
    out.push_back(effect("Windows", "generator", "Frames, glass and interiors fitted into the wall's openings.", "fitWindows", json::object()));
    out.push_back(effect("Rooftop Clutter", "generator", "Vents, AC units, pipes and railings on flat roofs.", "rooftopClutter", json::object()));
    return out;
}

/** Every weathering preset (category Weathering), as Stagehand lists them. */
json agePresetList()
{
    json out = json::array();
    for (const json& p : ops::execute("arrangementPresets", json::object()))
        if (p.value("category", std::string()) == "Weathering")
            out.push_back(p);
    return out;
}

std::string lower(std::string s)
{
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

/** The preset's name in a style: "Ivy" in toon is "Ivy Toon" when there is one. */
std::string styledPreset(const std::string& name, const std::string& style, const char* cmd)
{
    const json all = agePresetList();
    auto has = [&](const std::string& n) {
        return std::ranges::any_of(all, [&](const json& p) { return lower(p.value("name", std::string())) == lower(n); });
    };
    std::string base = name;
    if (lower(base).ends_with(" toon"))
        base = base.substr(0, base.size() - 5);
    const std::string want = style == "toon" ? base + " Toon" : base;
    if (has(want))
        return want;
    if (has(name))
        return name;
    std::string names;
    for (const json& p : all)
        names += (names.empty() ? "" : ", ") + p.value("name", std::string());
    throw CommandError(std::string(cmd) + ": no weathering preset named '" + name + "' (there are " + names + ")");
}

/** A part of a level aged on its own: its meshes, and the neighbours its dressing must keep off. */
struct Tile
{
    std::vector<std::string> meshes;
    std::vector<std::string> blockers;
};

/**
 * Splits meshes into square tiles by their bounds' centre, so a live result depends only on its tile: editing a block
 * rebuilds that tile's dressing alone. Neighbours within reach of a tile become its blockers. tile <= 0 keeps one tile.
 */
std::vector<Tile> tileMeshes(Graph& g, const std::vector<std::string>& meshes, double tile)
{
    if (tile <= 0.0)
        return {Tile{meshes, {}}};
    struct Box
    {
        glm::dvec3 lo{1e300}, hi{-1e300};
    };
    std::vector<Box> boxes(meshes.size());
    for (size_t k = 0; k < meshes.size(); ++k)
    {
        scene::Node* n = g.find(meshes[k]);
        const auto m = n ? n->as<scene::MeshPtr>(n->type().attrIndex("outMesh")) : scene::MeshPtr();
        if (!m)
            continue;
        const glm::dmat4 w = n->as<glm::dmat4>(scene::Transform::kWorldMatrix);
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
            {
                const glm::dvec3 q(w * glm::dvec4(m->position(p), 1.0));
                boxes[k].lo = glm::min(boxes[k].lo, q);
                boxes[k].hi = glm::max(boxes[k].hi, q);
            }
    }
    std::map<std::pair<int64_t, int64_t>, std::vector<size_t>> byTile;
    for (size_t k = 0; k < meshes.size(); ++k)
    {
        const glm::dvec3 c = boxes[k].lo.x <= boxes[k].hi.x ? (boxes[k].lo + boxes[k].hi) * 0.5 : glm::dvec3(0.0);
        byTile[{int64_t(std::floor(c.x / tile)), int64_t(std::floor(c.z / tile))}].push_back(k);
    }
    // Further than the widest band any preset lays, nothing a neighbour has can reach.
    const double reach = 250.0;
    std::vector<Tile> out;
    for (const auto& [_, members] : byTile)
    {
        Tile t;
        Box b;
        for (size_t k : members)
        {
            t.meshes.push_back(meshes[k]);
            b.lo = glm::min(b.lo, boxes[k].lo);
            b.hi = glm::max(b.hi, boxes[k].hi);
        }
        for (size_t k = 0; k < meshes.size(); ++k)
        {
            if (std::ranges::find(members, k) != members.end())
                continue;
            const Box& o = boxes[k];
            if (o.hi.x < b.lo.x - reach || o.lo.x > b.hi.x + reach || o.hi.z < b.lo.z - reach || o.lo.z > b.hi.z + reach)
                continue;
            t.blockers.push_back(meshes[k]);
        }
        out.push_back(std::move(t));
    }
    return out;
}

/** Every mesh an object stands for: itself, the meshes under a group, or an object set's members. */
void expandMeshes(Graph& g, const std::string& name, std::vector<std::string>& out, const char* cmd)
{
    scene::Node* n = g.find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    if (n->type().isA("objectSet"))
    {
        for (const json& m : ops::execute("setMembers", {{"args", {n->path()}}, {"nodes", true}}))
            expandMeshes(g, m.get<std::string>(), out, cmd);
        return;
    }
    if (n->type().isA("mesh"))
    {
        if (std::ranges::find(out, n->path()) == out.end())
            out.push_back(n->path());
        return;
    }
    for (NodeId c : g.children(n->id()))
        expandMeshes(g, g.node(c)->path(), out, cmd);
}
} // namespace age

bool registerAge(ext::Context& ctx)
{
    using namespace age;
    registerBasePieces();
    registerUnreal(ctx);
    registerWeatheringUi(ctx);
    for (const char* style : {"realistic", "toon"})
        for (json& p : builtins(style))
        {
            const std::string name = p["name"].get<std::string>();
            ops::execute("arrangementRegister", {{"args", {name}}, {"spec", std::move(p)}, {"source", "Weathering"}});
        }

    ctx.registerCommand({
        .name = "weatherPresets",
        .doc = "The weathering presets in the library, each one effect: scatter sets (Concrete Debris, Leaning Planks, Weeds, ...), "
               "surface (Grunge, Worn Edges, Corner Dirt, Chipped Edges), growth (Ivy, Vines, Roots), generators (Roof Sheets, "
               "Windows, Rooftop Clutter), each with a Toon version, and any arrangement preset saved with category 'Weathering'. "
               "Each has name, kind, description, style, slots and steps, as arrangementPresets gives them.",
        .undoable = false,
        .run = [](const Args&) -> json { return agePresetList(); },
        .examples = {"[p['name'] for p in weatherPresets()]"},
    });

    ctx.registerCommand({
        .name = "weather",
        .doc = "Apply one weathering preset to objects, as dragging its tile onto them does: one effect (a scatter set, grunge, "
               "edge decals, ivy, a roof), one undo step. Objects may be meshes, groups (every mesh under them) or object sets "
               "such as Blockwork visgroups. Piece slots left out use the library's pieces for their role: your own assets "
               "tagged 'weathering' and the role win over the placeholders. Returns group, preset and each step's result.",
        .positionalDoc = "objects to age (default the selection)",
        .flags = {{"preset", "p", T::String, "Concrete Debris", "Preset name (weatherPresets lists them)."},
                  styleFlag(),
                  {"slots", "sl", T::Any, nullptr, "Piece slots to fill yourself: {slot: [meshes, library ids or tag:a+b]}."},
                  {"seed", "s", T::Int, int64_t(1), "Another seed gives another result."},
                  {"tile", "ti", T::Float, nullptr,
                   "Dress in tiles this wide (cm), so editing one block rebuilds only its tile; 0 for one piece. Default 1500 "
                   "when there are more than 12 meshes, else 0.", {}, {}, 0.0},
                  {"visgroup", "vg", T::String, "Weathering", "Blockwork visgroup the result joins, so it hides in one click; empty for none."},
                  {"name", "n", T::String, nullptr, "Name of the group made (default the preset's name)."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("weather: select or name what to age");
            std::vector<std::string> meshes;
            for (const std::string& o : objects)
                expandMeshes(g, o, meshes, "weather");
            if (meshes.empty())
                throw CommandError("weather: the objects have no meshes");
            const std::string preset = styledPreset(a.get<std::string>("preset", "Concrete Debris"), a.get<std::string>("style", "realistic"), "weather");
            // Placeholders must exist before slot defaults name them by tag.
            buildPlaceholders(false);
            json slots = a.has("slots") ? a.flags["slots"] : json::object();
            if (!slots.is_object())
                throw CommandError("weather: slots is a dict of slot: pieces");
            const double tileSize = a.get<double>("tile", meshes.size() > 12 ? 1500.0 : 0.0);
            // Steps whose extension is off are skipped and said so, rather than failing the whole preset.
            json skipCommands = json::array(), notes = json::array();
            if (!ops::registry().find("trimSheetCreate"))
            {
                skipCommands.push_back("edgeDecals");
                notes.push_back("edge decals skipped: they need the Trim Sheets extension, which is off");
            }
            const std::vector<Tile> tiles = tileMeshes(g, meshes, tileSize);
            json results = json::array();
            std::vector<std::string> groups;
            for (size_t k = 0; k < tiles.size(); ++k)
            {
                json s = slots;
                s["targets"] = tiles[k].meshes;
                s["blockers"] = tiles[k].blockers;
                json call = {{"args", {preset}}, {"slots", s}, {"seed", a.get<int64_t>("seed", 1) + int64_t(k) * 1000}, {"skipCommands", skipCommands}};
                if (tiles.size() == 1 && a.has("name"))
                    call["name"] = a.get<std::string>("name");
                else if (tiles.size() > 1)
                    call["name"] = "Tile" + std::to_string(k + 1);
                json r = ops::execute("arrangePreset", call);
                if (const std::string grp = r.value("group", std::string()); !grp.empty())
                    groups.push_back(grp);
                results.push_back(std::move(r));
            }
            json out = tiles.size() == 1 ? results[0] : json{{"preset", preset}, {"tiles", results}};
            out["tileCount"] = int64_t(tiles.size());
            if (!notes.empty())
                out["notes"] = notes;
            std::string group = out.value("group", std::string());
            if (tiles.size() > 1)
            {
                std::string name = a.get<std::string>("name", preset);
                std::ranges::replace(name, ' ', '_');
                const json made = ops::execute("group", {{"empty", true}, {"name", name}});
                group = made.is_array() ? made[0].get<std::string>() : made.get<std::string>();
                const NodeId id = g.find(group)->id();
                for (const std::string& grp : groups)
                    if (g.find(grp))
                        ops::execute("parent", {{"args", {grp, group}}});
                group = g.node(id)->path();
                out["group"] = group;
            }
            if (!group.empty())
            {
                // The targets, so weatheringLive can pause what the preset put on them (grunge, chips) as well as its group.
                ops::execute("addAttr", {{"args", {group}}, {"longName", "weatheringTargets"}, {"dataType", "string"}});
                ops::execute("setAttr", {{"args", {group + ".weatheringTargets", json(meshes).dump()}}});
            }
            const std::string visgroup = a.get<std::string>("visgroup", "Weathering");
            if (!group.empty() && !visgroup.empty() && ops::registry().find("blockworkVisgroup"))
            {
                ops::execute("blockworkVisgroup", {{"args", {visgroup}}, {"add", json::array({group})}});
                out["visgroup"] = visgroup;
            }
            return out;
        },
        .examples = {"weather('Shed', preset='Leaning Planks')", "weather(['Tower', 'Wall'], preset='Ivy', style='toon', seed=3)",
                     "weather('Visgroup_Ruins', preset='Heavy Rubble', slots={'pieces': ['MyRubble_grp']})"},
    });

    ctx.registerCommand({
        .name = "weatheringLive",
        .doc = "Pause or resume weathering while blocking out: live=False hides what the presets made for the objects and turns "
               "off their grunge and edge chipping, so editing blocks costs nothing; live=True brings it all back, rebuilt for "
               "the blocks as they are now. Objects are weathered meshes or groups, or the groups a preset made (default the "
               "selection). Returns groups and modifiers (how many were switched).",
        .positionalDoc = "weathered objects or weathering groups (default the selection)",
        .flags = {{"live", "l", T::Bool, true, "True resumes, False pauses."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            const bool live = a.get<bool>("live", true);
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("weatheringLive: select or name what to pause or resume");
            json groups = json::array();
            int64_t switched = 0;
            auto targetsOf = [&](const scene::Node& group) {
                try
                {
                    return json::parse(group.as<std::string>(group.type().attrIndex("weatheringTargets")));
                }
                catch (const std::exception&)
                {
                    return json(json::array());
                }
            };
            auto switchMesh = [&](scene::Node& m) {
                for (NodeId id : scene::modifiers::stack(g, m.id()))
                {
                    scene::Node* mod = g.node(id);
                    if (!mod || (mod->type().name != "grungeModifier" && mod->type().name != "edgeChipModifier"))
                        continue;
                    ops::execute("setAttr", {{"args", {mod->path() + ".enabled", live}}});
                    ++switched;
                }
            };
            auto switchGroup = [&](scene::Node& group) {
                ops::execute("setAttr", {{"args", {group.path() + ".visibility", live}}});
                if (std::ranges::find(groups, json(group.path())) == groups.end())
                    groups.push_back(group.path());
            };
            for (const std::string& o : objects)
            {
                scene::Node* n = g.find(o);
                if (!n)
                    throw CommandError("weatheringLive: no node named '" + o + "'");
                if (n->type().attrIndex("weatheringTargets") >= 0)
                {
                    // A group weather made: it, and what it put on its targets.
                    switchGroup(*n);
                    for (const json& t : targetsOf(*n))
                        if (scene::Node* m = t.is_string() ? g.find(t.get<std::string>()) : nullptr)
                            switchMesh(*m);
                    continue;
                }
                // A weathered object: its grunge and chipping, and every group made for it.
                std::vector<std::string> meshes;
                expandMeshes(g, o, meshes, "weatheringLive");
                for (const std::string& path : meshes)
                {
                    if (scene::Node* m = g.find(path))
                        switchMesh(*m);
                    for (NodeId id : g.nodes())
                    {
                        scene::Node* group = g.node(id);
                        if (!group || group->type().attrIndex("weatheringTargets") < 0)
                            continue;
                        const json targets = targetsOf(*group);
                        if (std::ranges::find(targets, json(path)) != targets.end())
                            switchGroup(*group);
                    }
                }
            }
            return {{"live", live}, {"groups", groups}, {"modifiers", switched}};
        },
        .examples = {"weatheringLive('Level', live=False)", "weatheringLive('Level')"},
    });

    ctx.registerCommand({
        .name = "weatheringAddPieces",
        .doc = "Make your own meshes the pieces for a role: each selected object goes into the library tagged 'weathering', "
               "the role and the style, so every preset that uses the role picks them instead of the placeholders. Roles in "
               "use: rubble, rock, brick, plank, beam, charred, plant, grass, moss, bush, pebble, litter, leaf, and the ones "
               "edge decals, ivy and the generators list. Returns the asset ids.",
        .positionalDoc = "objects (default the selection)",
        .flags = {{"role", "r", T::String, nullptr, "The role: rubble, plank, plant, ..."},
                  styleFlag(),
                  {"collection", "c", T::String, nullptr, "Library collection (default Weathering/<role>)."},
                  {"tier", "", T::String, "user", "user, workspace, or a library folder path."},
                  {"thumbnail", "", T::Bool, true, "Render each asset's thumbnail."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            if (!a.has("role"))
                throw CommandError("weatheringAddPieces: give role=");
            const std::string role = a.get<std::string>("role");
            const std::string style = a.get<std::string>("style", "realistic");
            const std::vector<std::string> objects = a.objects();
            if (objects.empty())
                throw CommandError("weatheringAddPieces: select the meshes to add");
            json ids = json::array();
            for (const std::string& o : objects)
            {
                scene::Node* n = g.find(o);
                if (!n)
                    throw CommandError("weatheringAddPieces: no node named '" + o + "'");
                const json made = ops::execute("libraryAdd", {{"args", {n->path()}},
                                                              {"name", n->name()},
                                                              {"collection", a.get<std::string>("collection", "Weathering/" + role)},
                                                              {"tags", json::array({"weathering", role, style})},
                                                              {"tier", a.get<std::string>("tier", "user")},
                                                              {"thumbnail", a.get<bool>("thumbnail", true)},
                                                              {"overwrite", true}});
                ids.push_back(made.is_object() ? made.value("id", json()) : made);
            }
            return ids;
        },
        .examples = {"weatheringAddPieces(['Chunk1', 'Chunk2', 'Chunk3'], role='rubble')", "weatheringAddPieces(role='ivy-leaf', style='toon')"},
    });
    return true;
}
} // namespace weathering
