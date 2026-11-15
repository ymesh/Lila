#include "bake/Arrange.h"
#include "bake/AutoDistance.h"
#include "bake/Backend.h"
#include "bake/Bake.h"
#include "bake/BakeCommon.h"
#include "bake/BakeInputs.h"
#include "bake/BakeNodes.h"
#include "bake/Cage.h"
#include "bake/HighFiles.h"
#include "bake/Lint.h"
#include "bake/Targets.h"
#include "core/Events.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "core/Profile.h"
#include "core/Progress.h"
#include "fbx/Fbx.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <algorithm>
#include <fstream>
#include <limits>

namespace lila::bake
{
using T = ArgType;

namespace
{
using detail::findNode;
using detail::listArg;
using detail::projectArg;
using detail::shortName;
using detail::worldOf;
using scene::Graph;
using scene::Node;
using scene::NodeId;

Node& groupArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    std::vector<std::string> names = a.objects();
    for (const std::string& name : names)
        if (Node* n = findNode(g, name); n && isGroup(*n))
            return *n;
    // A selected low or cage stands for the group that holds it.
    for (const std::string& name : names)
        if (Node* n = findNode(g, name); n && n->type().isA("mesh"))
            for (NodeId id : g.nodes())
                if (Node* grp = g.node(id); grp && isGroup(*grp))
                {
                    const GroupSettings s = groupSettings(*grp);
                    if (std::ranges::find(s.lows, n->path()) != s.lows.end() || s.cage == n->path())
                        return *grp;
                }
    throw CommandError(std::string(cmd) + ": name a bake group (or select one, or its low)");
}

std::string safeName(std::string s)
{
    for (char& c : s)
        if (c == ':' || c == '|')
            c = '_';
    return s;
}

json groupJson(Graph& g, const Node& group)
{
    json j = groupSettings(group).toJson();
    j.erase("version");
    j["name"] = group.path();
    Node* p = groupProject(g, group);
    j["project"] = p ? json(p->path()) : json();
    return j;
}

json statsOf(std::span<const float> v)
{
    if (v.empty())
        return {{"min", 0.0}, {"max", 0.0}, {"mean", 0.0}};
    double sum = 0.0;
    for (float x : v)
        sum += x;
    const auto [lo, hi] = std::ranges::minmax(v);
    return {{"min", lo}, {"max", hi}, {"mean", sum / double(v.size())}};
}

int axisArg(const Args& a)
{
    const std::string s = a.get<std::string>("symmetry", "none");
    if (s == "x" || s == "X")
        return 0;
    if (s == "y" || s == "Y")
        return 1;
    if (s == "z" || s == "Z")
        return 2;
    return -1;
}

/** World bounds of a group's scene meshes (lows and node highs). */
geo::Bounds groupBounds(Graph& g, const GroupSettings& s)
{
    geo::Bounds b;
    auto add = [&](const std::string& name) {
        Node* n = findNode(g, name);
        if (!n || !n->type().isA("mesh"))
            return;
        scene::MeshPtr m = std::get<scene::MeshPtr>(g.value({n->id(), scene::MeshNode::kOutMesh, -1}));
        if (!m)
            return;
        const glm::dmat4 w = worldOf(g, n->id());
        for (geo::Index p = 0; p < m->pointCapacity(); ++p)
            if (m->pointValid(p))
                b.extend(glm::dvec3(w * glm::dvec4(m->position(p), 1.0)));
    };
    for (const std::string& l : s.lows)
        add(l);
    for (const HighEntry& h : s.highs)
        if (!h.self && h.file.empty())
            add(h.node);
    return b;
}

/** A palette colour as given by a generator: [r, g, b] in 0-1 or 0-255, or "#rrggbb". */
json colourOf(const json& c)
{
    if (c.is_string())
        return c;
    if (c.is_array() && c.size() >= 3)
    {
        const bool bytes = c[0].get<double>() > 1.0 || c[1].get<double>() > 1.0 || c[2].get<double>() > 1.0;
        const double s = bytes ? 1.0 / 255.0 : 1.0;
        return json::array({c[0].get<double>() * s, c[1].get<double>() * s, c[2].get<double>() * s});
    }
    return nullptr;
}

/** Imports an FBX into the scene (undoable) and returns its meshes. */
std::vector<std::string> importLows(Graph& g, const std::string& file, const char* cmd)
{
    fbx::ImportOptions opts;
    opts.animation = false;
    opts.setPlayback = false;
    const fbx::ImportResult r = fbx::importFbx(g, file, opts);
    if (!r.ok)
        throw CommandError(std::string(cmd) + ": " + r.error);
    std::vector<std::string> out;
    for (NodeId id : r.nodes)
        if (Node* n = g.node(id); n && n->type().isA("mesh"))
            out.push_back(n->path());
    return out;
}

/** One-call bake project from a Ground Tiles group or a Foliage tree, through their hand-off commands. */
json fromGenerator(const Args& a)
{
    Graph& g = Graph::current();
    std::vector<std::string> objs = a.objects();
    if (objs.empty())
        throw CommandError("createBakeFromGenerator: name or select a generated ground tile group or tree");
    const std::string source = objs.front();
    std::string kind = a.get<std::string>("kind", "auto");
    json h;
    auto tryRun = [&](const char* cmd) -> bool {
        if (!ops::find(cmd))
            return false;
        try
        {
            h = ops::execute(cmd, {{"args", {source}}});
            return true;
        }
        catch (const std::exception& e)
        {
            if (kind != "auto")
                throw CommandError(std::string("createBakeFromGenerator: ") + e.what());
            return false;
        }
    };
    if ((kind == "auto" || kind == "groundTile") && tryRun("groundTileBakeData"))
        kind = "groundTile";
    else if ((kind == "auto" || kind == "tree") && tryRun("treeBakeHandoff"))
        kind = "tree";
    else
        throw CommandError("createBakeFromGenerator: " + source +
                           " is not a generated ground tile group or tree (the GroundTiles or Foliage extension must be loaded)");

    const std::string targetName = a.get<std::string>("target", "substancePainter");
    const auto target = findTarget(targetName);
    if (!target)
        throw CommandError("createBakeFromGenerator: no target named " + targetName);
    const std::string high = h.value("high", std::string());
    if (high.empty())
        throw CommandError("createBakeFromGenerator: " + source + " has not been exported yet; export it so the high FBX exists");

    UndoChunk chunk("Create Bake From Generator");
    const std::string base = safeName(shortName(source));
    ProjectSettings ps = applyTarget(ProjectSettings::defaults(), *target, false, nullptr);
    // Generators hand over an ID palette, so their bakes keep the target's ID map even where it is optional.
    for (const TargetMap& m : target->maps)
        if (m.settings.map == "id" && std::ranges::none_of(ps.maps, [](const MapSettings& x) { return x.map == "id"; }))
            ps.maps.push_back(m.settings);
    GroupSettings gs;
    gs.highs.push_back(HighEntry{.file = high});
    json palette = json::object();
    std::string idSource = "material";
    if (kind == "groundTile")
    {
        const std::string plane = h.value("bakePlane", std::string());
        if (plane.empty())
            throw CommandError("createBakeFromGenerator: " + source + " has no bake plane to bake onto");
        gs.lows.push_back(plane);
        const double top = h.value("maxHeight", 0.0) - h.value("bakePlaneHeight", 0.0);
        const double below = h.value("bakePlaneHeight", 0.0) - h.value("minHeight", 0.0);
        gs.frontal = std::max(0.01, top * 1.05);
        gs.rear = std::max(0.01, below * 1.05 + 0.01);
        // Painter reads height by its range: the measured 99th percentile beats the outlier-prone maximum.
        const double range = h.value("measured", false) && h.value("heightP99", 0.0) > 0.0 ? h.value("heightP99", 0.0) : h.value("heightAbovePlane", 0.0);
        for (MapSettings& m : ps.maps)
            if (m.map == "height" && range > 0.0)
                m.options["range"] = range;
        if (h.contains("legend") && h["legend"].is_object())
            for (const auto& [id, entry] : h["legend"].items())
                if (json c = entry.is_object() ? colourOf(entry.value("colour", json())) : json(); !c.is_null())
                    palette[id] = c;
        idSource = "vertexColor";
    }
    else
    {
        if (a.has("lows"))
            for (const std::string& l : listArg(a, "lows"))
                gs.lows.push_back(l);
        else
        {
            const std::string low = h.value("low", std::string());
            if (low.empty())
                throw CommandError("createBakeFromGenerator: " + source + " has no exported low FBX; export it, or give lows=");
            gs.lows = importLows(g, low, "createBakeFromGenerator");
        }
        // The tree measures how far its high strays out of and into the low per part; the largest 99th percentile, padded.
        double out = 0.0, in = 0.0;
        if (h.contains("rays") && h["rays"].is_object())
            for (const auto& [part, s] : h["rays"].items())
            {
                out = std::max(out, s.value("out99", s.value("out", 0.0)));
                in = std::max(in, s.value("in99", s.value("in", 0.0)));
            }
        if (out > 0.0)
            gs.frontal = out * 1.1 + 0.05;
        if (in > 0.0)
            gs.rear = in * 1.1 + 0.05;
        const std::string pal = h.value("palette", std::string());
        if (!pal.empty())
            if (std::ifstream f{paths::fromUtf8(pal)}; f)
            {
                const json p = json::parse(f, nullptr, false);
                const json& colours = p.is_object() && p.contains("colours") ? p["colours"] : p;
                if (colours.is_object())
                    for (auto it = colours.begin(); it != colours.end(); ++it)
                        if (json c = colourOf(it.value()); !c.is_null())
                            palette[it.key()] = c;
            }
    }
    for (MapSettings& m : ps.maps)
        if (m.map == "id")
        {
            m.options["source"] = idSource;
            if (!palette.empty())
                m.options["palette"] = palette;
        }
    const std::string projectName = a.get<std::string>("name", base + "_bakeProject");
    Node* project = findNode(g, projectName);
    if (project && !isProject(*project))
        throw CommandError("createBakeFromGenerator: " + projectName + " exists and is not a bake project");
    if (project)
        setProjectSettings(g, project->id(), ps);
    else
        project = createProject(g, projectName, ps);
    Node* group = createGroup(g, project->id(), base + "_bake", gs);
    json out = {{"project", project->path()}, {"group", groupJson(g, *group)}, {"kind", kind}, {"target", target->name}, {"handoff", h}};
    return out;
}
} // namespace

void registerGroupCommands()
{
    if (ops::find("bakeCage"))
        return;
    for (events::Event e : {events::Event::SceneNew, events::Event::SceneOpened})
        events::subscribe(e, [](const events::EventData&) { arrange::clearAll(); });

    ops::registerCommand({
        .name = "bakeCage",
        .doc = "A bake group's editable cage. create=True duplicates the low pushed out by the auto cage (frontal x cageOffset x "
               "the painted bakeOffset) as a mesh the group then uses; edit its points freely, its topology must stay the low's. "
               "fromPaint=True moves an existing cage to the auto cage as painted now. reset=True goes back to the auto cage "
               "(delete=True also removes the cage mesh). With no flag, reports the cage and whether it matches the low.",
        .positionalDoc = "group (or its low; default the selection)",
        .flags = {{"create", "c", T::Bool, false, "Create an editable cage from the auto cage."},
                  {"name", "n", T::String, nullptr, "Name of the created cage; <low>_cage by default."},
                  {"fromPaint", "fp", T::Bool, false, "Move the cage's points to the painted auto cage."},
                  {"reset", "rs", T::Bool, false, "Use the auto cage again."},
                  {"delete", "del", T::Bool, false, "With reset: delete the cage mesh too."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& grp = groupArg(a, "bakeCage");
            try
            {
                if (a.get<bool>("reset"))
                {
                    const std::string was = resetCage(g, grp.id(), a.get<bool>("delete"));
                    return {{"group", grp.path()}, {"cage", nullptr}, {"was", was.empty() ? json() : json(was)}};
                }
                if (a.get<bool>("create"))
                {
                    Node* cage = createCage(g, grp.id(), a.get<std::string>("name", ""));
                    return {{"group", grp.path()}, {"cage", cage->path()}};
                }
                if (a.get<bool>("fromPaint"))
                {
                    cageFromPaint(g, grp.id());
                    return {{"group", grp.path()}, {"cage", groupSettings(grp).cage}};
                }
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeCage: ") + e.what());
            }
            const GroupSettings s = groupSettings(grp);
            json out = {{"group", grp.path()}, {"cage", s.cage.empty() ? json() : json(s.cage)}, {"frontal", groupFrontal(g, s)}};
            if (!s.cage.empty())
            {
                Node* c = findNode(g, s.cage);
                std::string why = "the cage is not a mesh";
                if (c && c->type().isA("mesh") && s.lows.size() == 1)
                    if (Node* low = findNode(g, s.lows.front()))
                    {
                        scene::MeshPtr lm = scene::modifiers::exportMesh(g, low->id()), cm = scene::modifiers::exportMesh(g, c->id());
                        why = lm && cm ? cageMismatch(*lm, *cm) : "no mesh";
                    }
                out["matches"] = why.empty();
                if (!why.empty())
                    out["mismatch"] = why;
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bakePaint",
        .doc = "Paint a low's bake cage attributes: offset (bakeOffset, scales the auto cage push per point, 1 unpainted) or skew "
               "(bakeSkew, added to the group's skew, tilts rays toward the face normal). op add, subtract, smooth or set, at "
               "points=, within radius of center= (world space), or flood=True everywhere (the component selection in component "
               "mode). symmetry mirrors across an axis. One undo step. Returns the attribute's min, max and mean.",
        .positionalDoc = "mesh (default the selection)",
        .flags = {{"attr", "at", T::String, "offset", "offset or skew.", {"offset", "skew"}},
                  {"op", "o", T::String, "set", "add, subtract, smooth or set.", {"add", "subtract", "smooth", "set"}},
                  {"value", "v", T::Float, 1.0, "The amount to add or subtract, or the value to set."},
                  {"strength", "st", T::Float, 1.0, "0 to 1: how far each dab moves toward its result.", {}, {}, 0.0, 1.0},
                  {"points", "pt", T::List, nullptr, "Point ids to paint at full weight."},
                  {"center", "ce", T::Vec3, nullptr, "World-space brush centre; points within radius get a smooth falloff."},
                  {"radius", "r", T::Float, nullptr, "World-space brush radius, cm.", {}, {}, 0.0},
                  {"flood", "fl", T::Bool, false, "Apply to every point (or the selected components)."},
                  {"reset", "rs", T::Bool, false, "Set every point back to unpainted (offset 1, skew 0), whatever is selected."},
                  {"symmetry", "sy", T::String, "none", "Mirror across x, y or z (object space).", {"none", "x", "y", "z"}}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            const auto attr = paintAttr(a.get<std::string>("attr", "offset"));
            if (!attr)
                throw CommandError("bakePaint: attr must be offset or skew");
            const auto op = paintOpFromName(a.get<std::string>("op", "set"));
            if (!op)
                throw CommandError("bakePaint: op must be add, subtract, smooth or set");
            std::vector<std::string> objs = a.objects();
            if (objs.empty())
                throw CommandError("bakePaint: give or select a mesh");
            UndoChunk chunk("Bake Paint");
            json out = json::array();
            for (const std::string& name : objs)
            {
                Node* n = findNode(g, name);
                if (!n || !n->type().isA("mesh"))
                    throw CommandError("bakePaint: " + name + " is not a mesh");
                g.evaluate();
                scene::MeshPtr base = std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, n->id())));
                if (!base)
                    continue;
                PointPaint paint(base, *attr);
                paint.setSymmetry(axisArg(a));
                const float value = float(a.get<double>("value", 1.0)), strength = float(a.get<double>("strength", 1.0));
                if (a.get<bool>("reset"))
                    paint.flood(PaintOp::Set, attr->unpainted, 1.0f, {});
                else if (a.get<bool>("flood"))
                {
                    std::vector<geo::Index> mask;
                    if (scene::components::mode() != scene::ComponentMode::Object)
                        for (uint32_t p : scene::components::pointsOf(n->id()))
                            mask.push_back(p);
                    paint.flood(*op, value, strength, mask);
                }
                if (a.has("points"))
                {
                    std::vector<std::pair<geo::Index, float>> pts;
                    for (const json& p : a.flags["points"])
                        if (p.is_number_integer() && p.get<int64_t>() >= 0 && p.get<int64_t>() < int64_t(base->pointCapacity()))
                            pts.push_back({geo::Index(p.get<int64_t>()), 1.0f});
                    paint.apply(pts, *op, value, strength);
                }
                if (a.has("center"))
                {
                    if (!a.has("radius"))
                        throw CommandError("bakePaint: center= needs radius=");
                    const glm::dmat4 w = worldOf(g, n->id());
                    const json c = a.flags["center"];
                    const glm::dvec3 center(glm::inverse(w) * glm::dvec4(c[0].get<double>(), c[1].get<double>(), c[2].get<double>(), 1.0));
                    const double scale = std::max(1e-12, glm::length(glm::dvec3(w[0])));
                    paint.apply(paint.pointsNear(center, a.get<double>("radius") / scale), *op, value, strength);
                }
                if (!setPointValues(g, n->id(), *attr, paint.values(), "Bake Paint"))
                    throw CommandError("bakePaint: couldn't write " + std::string(attr->name) + " on " + n->path());
                json j = statsOf(paint.values());
                j["mesh"] = n->path();
                j["attr"] = attr->name;
                out.push_back(j);
            }
            return out.size() == 1 ? out[0] : out;
        },
    });

    ops::registerCommand({
        .name = "bakeLint",
        .doc = "Check a bake project before baking: hard edges off UV seams, overlapping UVs (also across groups writing one "
               "texture), missing or collapsed UVs, islands off their mesh's texel density, mirrored islands against tangent signs, "
               "lows with no high in range, highs outside the cage, cage topology and lows shown without a modifier their export "
               "runs, where the viewport reads texels baked elsewhere (previewMesh); non-planar faces whose triangulation depends "
               "on the exporter only when checks names triangulation. Problems too small to show in the map are left out. bake runs it first and lists its warnings. Returns {issues: [{check, severity, group, mesh, message, "
               "data}], warnings, checks, distances: [{group, frontal, rear, frontalAuto, rearAuto, measured}]} (Auto distances are "
               "measured against the highs when highs is on).",
        .flags = {{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."},
                  {"groups", "g", T::List, nullptr, "Lint only these groups."},
                  {"checks", "c", T::List, nullptr, "Run only these checks (the names in checks of the result)."},
                  {"highs", "hi", T::Bool, true, "Load the highs for the range and cage checks (False is quick: low checks only)."},
                  {"backend", "be", T::String, "cpu", "Backend tracing the range check's rays."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            Graph& g = Graph::current();
            Node& project = projectArg(Graph::current(), a, "bakeLint", false);
            std::vector<NodeId> groups;
            for (const std::string& name : listArg(a, "groups"))
            {
                Node* n = findNode(g, name);
                if (!n || !isGroup(*n))
                    throw CommandError("bakeLint: " + name + " is not a bake group");
                groups.push_back(n->id());
            }
            LintOptions opts;
            opts.checks = listArg(a, "checks");
            for (const std::string& c : opts.checks)
                if (std::ranges::find(lintChecks(), c) == lintChecks().end())
                    throw CommandError("bakeLint: no check named " + c);
            const bool highs = a.get<bool>("highs", true);
            try
            {
                BakeInputs in = buildInputs(g, project.id(), groups, highs);
                std::unique_ptr<Backend> backend = highs ? createBackend(a.get<std::string>("backend", "cpu")) : nullptr;
                if (backend)
                {
                    backend->prepare(in);
                    resolveAutoDistances(in, *backend);
                }
                json r = lint(g, in, projectSettings(project), backend.get(), opts).toJson();
                r["inputWarnings"] = in.warnings;
                json distances = json::array();
                for (const GroupInputs& gi : in.groups)
                    distances.push_back({{"group", gi.name},
                                         {"frontal", gi.frontal},
                                         {"rear", gi.rear},
                                         {"frontalAuto", gi.settings.frontal <= 0.0},
                                         {"rearAuto", gi.settings.rear <= 0.0},
                                         {"measured", gi.measured}});
                r["distances"] = std::move(distances);
                return r;
            }
            catch (const progress::Cancelled&)
            {
                throw CommandError("bakeLint: cancelled");
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("bakeLint: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "bakeGroupPerMesh",
        .doc = "One bake group per mesh: each given (or selected) mesh becomes the low of its own group, so meshes that overlap "
               "or share a UV sheet still bake in isolation. Highs come from _high/_hi/_HP names matching the mesh's base name; a "
               "mesh without one bakes onto itself (self) through preset= (e.g. Smooth x2). A mesh already a low of a group of "
               "the project keeps that group. Returns the groups.",
        .positionalDoc = "[meshes]",
        .flags = {{"project", "pj", T::String, nullptr, "The bake project; created when the scene has none."},
                  {"preset", "ps", T::String, nullptr, "Bake modifier preset for self highs (Smooth x2, Auto Bevel + Smooth)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            std::vector<std::string> names = a.objects();
            if (names.empty())
                throw CommandError("bakeGroupPerMesh: give or select meshes");
            json stack = json::array();
            if (a.has("preset"))
            {
                stack = modifierPreset(a.get<std::string>("preset"));
                if (stack.is_null())
                    throw CommandError("bakeGroupPerMesh: no bake modifier preset named " + a.get<std::string>("preset"));
            }
            Node& project = projectArg(Graph::current(), a, "bakeGroupPerMesh", true);
            std::vector<std::string> all;
            for (NodeId id : g.nodes())
                if (Node* n = g.node(id); n && n->type().isA("mesh"))
                    all.push_back(n->path());
            const std::vector<AutoGroup> matches = matchAutoGroups(all);
            UndoChunk chunk("One Bake Group Per Mesh");
            json out = json::array();
            for (const std::string& name : names)
            {
                Node* n = findNode(g, name);
                if (!n || !n->type().isA("mesh"))
                    continue;
                const std::string path = n->path();
                const bool isHigh = std::ranges::any_of(matches, [&](const AutoGroup& m) { return std::ranges::find(m.highs, path) != m.highs.end(); });
                const bool isCage = std::ranges::any_of(groupsOf(g, project.id()), [&](NodeId id) { return groupSettings(*g.node(id)).cage == path; });
                if (isHigh || isCage)
                    continue;
                Node* grp = nullptr;
                for (NodeId id : groupsOf(g, project.id()))
                    if (const GroupSettings s = groupSettings(*g.node(id)); std::ranges::find(s.lows, path) != s.lows.end())
                        grp = g.node(id);
                GroupSettings s = grp ? groupSettings(*grp) : GroupSettings{};
                s.lows = {path};
                if (s.highs.empty())
                {
                    for (const AutoGroup& m : matches)
                        if (std::ranges::find(m.lows, path) != m.lows.end())
                            for (const std::string& h : m.highs)
                                s.highs.push_back(HighEntry{.node = h});
                    if (s.highs.empty())
                        s.highs.push_back(HighEntry{.self = true, .modifiers = stack});
                }
                if (grp)
                    setGroupSettings(g, grp->id(), s);
                else
                    grp = createGroup(g, project.id(), safeName(shortName(path)) + "_bake", s);
                out.push_back(groupJson(g, *grp));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "bakeArrange",
        .doc = "Toggle the Arrange display: a project's groups drawn side by side along X (by their bounds) for inspection, "
               "each group's lows, highs and cage moved together. View only: scene meshes never move, the scene file doesn't "
               "change and bakes always run where the meshes are. on= sets it instead of toggling; reset=True turns it off. "
               "Returns {arranged, offsets: {group: [x, y, z]}}.",
        .flags = {{"project", "pj", T::String, nullptr, "The bake project; defaults to the scene's only one."},
                  {"on", "on", T::Bool, nullptr, "Arranged (True) or not (False); toggles when left out."},
                  {"spacing", "sp", T::Float, nullptr, "Gap between groups, cm; 10% of the largest group by default.", {}, {}, 0.0},
                  {"reset", "rs", T::Bool, false, "Turn it off: everything back where the meshes are."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& project = projectArg(Graph::current(), a, "bakeArrange", false);
            const bool on = !a.get<bool>("reset") && (a.has("on") ? a.get<bool>("on") : !arrange::active(g, project.id()));
            json out = json::object();
            if (!on)
            {
                arrange::clear(project.id());
                return {{"arranged", false}, {"offsets", out}};
            }
            const std::vector<NodeId> ids = groupsOf(g, project.id());
            std::vector<geo::Bounds> bounds;
            double largest = 0.0;
            for (NodeId id : ids)
            {
                bounds.push_back(groupBounds(g, groupSettings(*g.node(id))));
                largest = std::max(largest, glm::length(bounds.back().size()));
            }
            const double gap = a.has("spacing") ? std::max(0.0, a.get<double>("spacing")) : 0.1 * largest;
            std::map<NodeId, glm::dvec3> offsets;
            double cursor = 0.0;
            bool first = true;
            for (size_t i = 0; i < ids.size(); ++i)
            {
                const geo::Bounds& b = bounds[i];
                glm::dvec3 o(0.0);
                if (b.valid())
                {
                    if (first)
                        cursor = b.min.x;
                    o.x = cursor - b.min.x;
                    cursor += b.size().x + gap;
                    first = false;
                }
                offsets[ids[i]] = o;
                out[g.node(ids[i])->path()] = {o.x, o.y, o.z};
            }
            arrange::set(g, project.id(), offsets);
            return {{"arranged", true}, {"offsets", out}};
        },
    });

    ops::registerCommand({
        .name = "createBakeFromGenerator",
        .doc = "Make a bake project from a Ground Tiles group or a Foliage tree in one call, from their exported files: "
               "the high streams from its FBX at bake time (never loaded into the scene), the low is the ground tile's bake plane or "
               "the tree's exported low, frontal and rear come from the generator's measurements, the ID map uses its palette and "
               "the height map its measured range, with the Substance Painter target's maps and names. Returns {project, group, "
               "kind, target, handoff}.",
        .positionalDoc = "ground tile group or tree (default the selection)",
        .flags = {{"kind", "k", T::String, "auto", "groundTile, tree or auto.", {"auto", "groundTile", "tree"}},
                  {"target", "t", T::String, "substancePainter", "Target profile (bakeTargets)."},
                  {"name", "n", T::String, nullptr, "Project name; <source>_bakeProject by default (reused when it exists)."},
                  nodeFlag({"lows", "lo", T::List, nullptr, "Tree: scene meshes to bake onto instead of importing the exported low."}, "mesh")},
        .run = [](const Args& a) -> json { return fromGenerator(a); },
    });
}
} // namespace lila::bake
