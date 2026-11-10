#include "layout/LayoutNode.h"

#include "layout/SoftNet.h"

#include "core/Log.h"
#include "core/Profile.h"
#include "layout/Edit.h"
#include "layout/Lines.h"
#include "layout/Maps.h"
#include "link/UnrealLink.h"
#include "ops/Command.h"
#include "scene/AttrBuilders.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Relocate.h"
#include "scene/Transform.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace lila::layout
{
using namespace scene;
using namespace scene::attrdefs;

geo::Mesh withPanelAttributes(const geo::Mesh& mesh, const Resolved& r)
{
    LILA_ZONE();
    geo::Mesh m = mesh;
    std::span<int32_t> id = m.pointAttrs().getOrAdd<int32_t>("panelId");
    std::span<float> random = m.pointAttrs().getOrAdd<float>("panelRandom");
    std::span<float> distance = m.pointAttrs().getOrAdd<float>("panelLineDistance");
    const double reach = std::max(r.reach() * 20.0, r.spacing() * 40.0);
    tbb::parallel_for(geo::Index(0), m.pointCapacity(), [&](geo::Index p) {
        if (!m.pointValid(p))
            return;
        const PanelSample panel = r.panelAt(m.position(p));
        id[p] = panel.panel;
        random[p] = float(panel.random);
        const LineSample line = r.lineAt(m.position(p), reach);
        distance[p] = float(line.run >= 0 ? line.distance : reach);
    });
    return m;
}

PanelLayoutNode::PanelLayoutNode()
{
    inAttr = kInMesh;
    enabledAttr = kEnabled;
    outAttr = kOutMesh;
}

void PanelLayoutNode::compute(const EvalContext&)
{
    LILA_ZONE();
    mError.clear();
    const MeshPtr& in = as<MeshPtr>(kInMesh);
    const PanelLayout* stored = layoutOf(as<DataPtr>(kLayout));
    std::shared_ptr<const Resolved> resolved;
    std::vector<std::string> warnings;
    try
    {
        resolved = Resolved::build(stored ? *stored : PanelLayout{}, in);
        warnings = resolved->warnings();
    }
    catch (const std::exception& e)
    {
        warnings.push_back(e.what());
    }
    setOutput(kOutLayout, DataPtr(std::make_shared<ResolvedData>(resolved)));

    bool renders = false;
    if (resolved)
        for (const ResolvedRun& r : resolved->runs())
            renders |= r.output == Output::Geometry || r.output == Output::Sculpt || r.output == Output::Decal;
    if (!in || !resolved || !renders || !as<bool>(kEnabled) || modifiers::bypassed(in))
        setOutput(kOutMesh, in);
    else
    {
        try
        {
            LinesResult lines = applyLines(*resolved);
            warnings.insert(warnings.end(), lines.warnings.begin(), lines.warnings.end());
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(std::move(lines.mesh))));
        }
        catch (const std::exception& e)
        {
            log::warn("{}: {}", name(), e.what());
            mError = e.what();
            warnings.push_back(e.what());
            setOutput(kOutMesh, in);
        }
    }
    if (resolved && as<bool>(kWriteAttributes))
        if (const MeshPtr& shown = as<MeshPtr>(kOutMesh))
            setOutput(kOutMesh, MeshPtr(std::make_shared<geo::Mesh>(withPanelAttributes(*shown, *resolved))));
    std::string text;
    for (const std::string& w : warnings)
        text += (text.empty() ? "" : "\n") + w;
    setOutput(kWarning, std::move(text));
}

Node* findLayoutNode(Graph& g, NodeId id)
{
    Node* n = g.node(id);
    if (!n)
        return nullptr;
    if (n->type().name == kLayoutType)
        return n;
    if (n->type().isA("mesh"))
    {
        const std::vector<NodeId> s = modifiers::stack(g, id);
        for (auto it = s.rbegin(); it != s.rend(); ++it)
            if (Node* m = g.node(*it); m && m->type().name == kLayoutType)
                return m;
    }
    return nullptr;
}

std::shared_ptr<const Resolved> resolvedLayout(Graph& g, NodeId layoutNode)
{
    Node* n = g.node(layoutNode);
    if (!n || n->type().name != kLayoutType)
        return nullptr;
    g.evaluate();
    return resolvedOf(n->as<DataPtr>(PanelLayoutNode::kOutLayout));
}

namespace
{
const std::vector<std::string> kAxisNames = {"off", "x", "y", "z"};

/** A layout node with the mesh it is drawn on, ready to edit. */
struct Target
{
    Node* node = nullptr;
    NodeId mesh = kNoNode;
    glm::dmat4 world{1.0};
    glm::dmat4 toLocal{1.0};
    /** How much the world matrix scales, to turn world distances into mesh ones. */
    double scale = 1.0;
    MeshPtr surface;
};

Target targetArg(const Args& a, const char* cmd)
{
    Graph& g = Graph::current();
    for (const std::string& name : a.objects())
        if (Node* n = g.find(name))
            if (Node* l = findLayoutNode(g, n->id()))
            {
                Target t;
                t.node = l;
                t.mesh = modifiers::meshOf(g, l->id());
                g.evaluate();
                if (Node* m = g.node(t.mesh); m && m->type().isA("transform"))
                    t.world = m->as<glm::dmat4>(Transform::kWorldMatrix);
                t.toLocal = glm::inverse(t.world);
                t.scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(t.world)))));
                t.surface = l->as<MeshPtr>(PanelLayoutNode::kInMesh);
                if (!t.surface || t.surface->numFaces() == 0)
                    throw CommandError(std::string(cmd) + ": " + l->path() + " has no mesh to draw on");
                return t;
            }
    throw CommandError(std::string(cmd) + ": give a panel layout or a mesh that has one");
}

PanelLayout currentLayout(const Node& n)
{
    if (const PanelLayout* l = layoutOf(n.as<DataPtr>(PanelLayoutNode::kLayout)))
        return *l;
    return PanelLayout::withDefaultSets();
}

glm::dvec3 local(const Target& t, const glm::dvec3& p) { return glm::dvec3(t.toLocal * glm::dvec4(p, 1.0)); }
glm::dvec3 world(const Target& t, const glm::dvec3& p) { return glm::dvec3(t.world * glm::dvec4(p, 1.0)); }

glm::dvec3 vecArg(const json& j, const char* cmd)
{
    if (!j.is_array() || j.size() != 3)
        throw CommandError(std::string(cmd) + ": expected a point [x, y, z]");
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

std::vector<glm::dvec3> pointsArg(const Args& a, const char* flag, const Target& t, const char* cmd)
{
    std::vector<glm::dvec3> out;
    if (!a.has(flag))
        return out;
    for (const json& p : a.flags[flag])
        out.push_back(local(t, vecArg(p, cmd)));
    return out;
}

std::vector<uint32_t> idsArg(const Args& a, const char* flag)
{
    std::vector<uint32_t> out;
    if (a.has(flag))
        for (const json& v : a.flags[flag])
            out.push_back(v.get<uint32_t>());
    return out;
}

json vecJson(const glm::dvec3& v) { return {v.x, v.y, v.z}; }

/** Runs an edit on the layout and stores the result when it changed anything. */
json edit(const Args& a, const char* cmd, const std::function<EditResult(PanelLayout&, const EditContext&, const Target&)>& fn)
{
    Target t = targetArg(a, cmd);
    PanelLayout l = currentLayout(*t.node);
    const Surface surf(*t.surface);
    EditContext ctx{surf};
    ctx.tolerance = a.has("tolerance") ? a.get<double>("tolerance") / t.scale : surf.size() / 200.0;
    const EditResult r = fn(l, ctx, t);
    if (r.changed)
        Graph::current().setValue({t.node->id(), PanelLayoutNode::kLayout, -1}, makeLayoutData(std::move(l)));
    return {{"changed", r.changed}, {"runs", r.runs}, {"junctions", r.junctions}, {"note", r.note}};
}

const LineSet& setArg(const PanelLayout& l, const Args& a, const char* cmd)
{
    const std::string name = a.get<std::string>("set");
    for (const LineSet& s : l.sets)
        if (s.name == name)
            return s;
    throw CommandError(std::string(cmd) + ": no line set named '" + name + "'");
}

void applySetValues(LineSet& s, const Args& a, const char* cmd)
{
    if (a.has("width"))
        s.width = std::max(0.0, a.get<double>("width"));
    if (a.has("depth"))
        s.depth = a.get<double>("depth");
    if (a.has("profile"))
    {
        auto p = profileFromName(a.get<std::string>("profile"));
        if (!p)
            throw CommandError(std::string(cmd) + ": profile is v, u, square, step or chamfer");
        s.profile = *p;
    }
    if (a.has("output"))
    {
        auto o = outputFromName(a.get<std::string>("output"));
        if (!o)
            throw CommandError(std::string(cmd) + ": output is geometry, sculpt, texture, decal or none");
        s.output = *o;
    }
}

json layoutInfo(const Target& t, bool withPoints)
{
    Graph& g = Graph::current();
    const PanelLayout l = currentLayout(*t.node);
    const std::shared_ptr<const Resolved> r = resolvedLayout(g, t.node->id());
    json sets = json::array();
    for (const LineSet& s : l.sets)
        sets.push_back({{"id", s.id},
                        {"name", s.name},
                        {"width", s.width},
                        {"depth", s.depth},
                        {"profile", profileName(s.profile)},
                        {"output", outputName(s.output)},
                        {"active", s.id == l.activeSet}});
    json runs = json::array(), junctions = json::array();
    if (r)
    {
        for (const ResolvedRun& run : r->runs())
        {
            json j = {{"id", run.id},     {"source", run.source()}, {"mirrored", run.mirrored()}, {"a", run.a},
                      {"b", run.b},       {"set", run.set},         {"length", run.path.length() * t.scale},
                      {"width", run.width}, {"depth", run.depth}, {"profile", profileName(run.profile)}, {"output", outputName(run.output)}};
            if (const Run* src = l.run(run.source()))
            {
                j["controls"] = src->controls.size();
                j["loose"] = src->loose;
            }
            if (withPoints)
            {
                json pts = json::array();
                for (const glm::dvec3& p : run.path.points)
                    pts.push_back(vecJson(world(t, p)));
                j["points"] = std::move(pts);
            }
            runs.push_back(std::move(j));
        }
        for (const ResolvedJunction& j : r->junctions())
        {
            const Junction* src = l.junction(j.id & ~kMirrored);
            junctions.push_back({{"id", j.id},
                                 {"position", vecJson(world(t, j.position))},
                                 {"degree", j.degree},
                                 {"corner", j.corner},
                                 {"end", src ? endStyleName(src->end) : "round"}});
        }
    }
    return {{"layout", t.node->path()},
            {"sets", sets},
            {"runs", runs},
            {"junctions", junctions},
            {"panels", r ? json(r->panels().size()) : json(0)},
            {"mirror", kAxisNames[size_t(l.mirrorAxis + 1)]},
            {"warning", t.node->as<std::string>(PanelLayoutNode::kWarning)}};
}
} // namespace

void registerTypes()
{
    registerSoftNetTypes();
    if (types::find(kLayoutType))
        return;
    data::registerKind("panelLayout", &LayoutData::fromJson);
    registerBakeMaps();

    NodeType t;
    t.name = kLayoutType;
    t.icon = "layout-grid";
    t.doc = "Panel Layout: lines drawn on the mesh's surface with the Layout Pencil. Line sets render as grooves cut into the mesh "
            "(Geometry), grooves displaced into a dense mesh (Sculpt), or only feed other systems (Texture, Decal, None). "
            "Closed loops of lines bound panels that plates, texture masks and nodes read.";
    t.ownAttrs = {
        {.name = "inMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrInput | AttrHidden},
        param("enabled", PlugType::Bool, true, "Turn the lines off without removing them."),
        {.name = "exportEnabled", .type = PlugType::Bool, .defaultValue = true, .flags = AttrInput | AttrStorable,
         .doc = "Apply the lines when exporting."},
        {.name = "layout", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrInput | AttrStorable | AttrHidden,
         .doc = "The lines, junctions and line sets."},
        {.name = "writeAttributes", .type = PlugType::Bool, .defaultValue = false, .flags = AttrInput | AttrStorable,
         .doc = "Write point attributes for geometry nodes, shaders and export: panelId (int), panelRandom (0 to 1) and "
                "panelLineDistance (cm to the nearest line)."},
        {.name = "outMesh", .type = PlugType::Mesh, .defaultValue = MeshPtr(), .flags = AttrOutput | AttrHidden},
        {.name = "outLayout", .type = PlugType::Data, .defaultValue = DataPtr(), .flags = AttrOutput | AttrHidden,
         .doc = "The layout laid on the mesh, for plates, texture masks and nodes."},
        {.name = "warning", .type = PlugType::String, .defaultValue = std::string(), .flags = AttrOutput,
         .doc = "Problems with the last build."},
    };
    t.create = [] { return std::make_unique<PanelLayoutNode>(); };
    // The lines are anchored in the mesh's own space, so moving the object moves them with it.
    relocate::registerNeutral(t.name);
    types::registerType(std::move(t));
    modifiers::setCreateHook(kLayoutType, [](Graph& g, Node& mod, NodeId) {
        g.setValue({mod.id(), PanelLayoutNode::kLayout, -1}, makeLayoutData(PanelLayout::withDefaultSets()));
    });
}

void registerCommands()
{
    registerSoftNetCommands();
    using T = ArgType;
    const FlagDef tolerance{"tolerance", "", T::Float, nullptr, "Join and pick distance in world units (cm); default 1/200 of the mesh size.", {}, {}, 0.0};

    ops::registerCommand({
        .name = "panelLayout",
        .doc = "Add a Panel Layout to each mesh: lines drawn on its surface (Layout Pencil, or panelLayoutStroke) that render as "
               "grooves and bound panels. Returns the layout names.",
        .positionalDoc = "meshes",
        .flags = {{"mirror", "", T::String, "x", "Mirror plane through the mesh's origin: off, x, y or z.", kAxisNames}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            json out = json::array();
            for (const std::string& name : a.objects())
                if (Node* n = g.find(name); n && n->type().isA("mesh"))
                {
                    const std::string mod = ops::execute("addModifier", {{"args", {name}}, {"kind", "panelLayout"}});
                    ops::execute("panelLayoutMirror", {{"args", {mod}}, {"axis", a.get<std::string>("mirror", "x")}});
                    out.push_back(mod);
                }
            if (out.empty())
                throw CommandError("panelLayout: select or name a mesh");
            return out;
        },
        .examples = {"layout = panelLayout(\"Hull\", mirror=\"x\")[0]"},
    });

    ops::registerCommand({
        .name = "panelLayoutStroke",
        .doc = "Draw on a Panel Layout as the Layout Pencil does. draw cleans the stroke into lines (joining, trimming and crossing "
               "the lines it meets; starting on a line heading along it redraws that line), erase removes the lines it crosses, "
               "smooth relaxes the lines under it. Returns the runs made or changed.",
        .positionalDoc = "layout or mesh",
        .flags = {{"points", "p", T::List, nullptr, "The stroke: world-space points on the surface, [[x, y, z], ...]."},
                  {"mode", "", T::String, "draw", "draw, erase or smooth.", {"draw", "erase", "smooth"}},
                  tolerance,
                  {"cleanup", "", T::Float, 0.5, "0 keeps the drawn wobble, 1 fits the fewest points.", {}, {}, 0.0, 1.0},
                  {"cornerAngle", "", T::Float, 50.0, "Turns sharper than this (degrees) become corners.", {}, {}, 1.0, 179.0},
                  {"straight", "", T::Bool, false, "Every piece between corners straight."},
                  {"straightenLast", "", T::Bool, false, "The last piece straight (the stroke ended with a hold)."},
                  {"set", "", T::String, nullptr, "Line set for new lines; default the active set."},
                  {"radius", "", T::Float, nullptr, "smooth: brush radius in world units; default 4 tolerances.", {}, {}, 0.0},
                  {"strength", "", T::Float, 0.5, "smooth: 0 to 1.", {}, {}, 0.0, 1.0}},
        .run = [](const Args& a) -> json {
            return edit(a, "panelLayoutStroke", [&](PanelLayout& l, const EditContext& c, const Target& t) {
                const std::vector<glm::dvec3> pts = pointsArg(a, "points", t, "panelLayoutStroke");
                const std::string mode = a.get<std::string>("mode", "draw");
                if (mode == "erase")
                    return eraseStroke(l, c, pts);
                if (mode == "smooth")
                {
                    const double radius = a.has("radius") ? a.get<double>("radius") / t.scale : c.tolerance * 4.0;
                    return smoothStroke(l, c, pts, radius, a.get<double>("strength", 0.5));
                }
                if (mode != "draw")
                    throw CommandError("panelLayoutStroke: mode is draw, erase or smooth");
                StrokeOptions o;
                o.cleanup = a.get<double>("cleanup", 0.5);
                o.cornerAngle = a.get<double>("cornerAngle", 50.0);
                o.straight = a.get<bool>("straight", false);
                o.straightenLast = a.get<bool>("straightenLast", false);
                if (a.has("set"))
                    o.set = setArg(l, a, "panelLayoutStroke").id;
                return addStroke(l, c, pts, o);
            });
        },
        .examples = {"panelLayoutStroke(\"Hull\", points=[[0, 10, 5], [20, 10, 5]])"},
    });

    ops::registerCommand({
        .name = "panelLayoutEdit",
        .doc = "Edit a Panel Layout's lines and junctions by id (ids from panelLayoutInfo). Operations: moveJunction (junction, to), "
               "bend (run, grab, to), split (run, at), corner (junction: toggles sharp corner), delete (runs and/or junctions), "
               "offset (runs, distance: parallel copies), loosen (runs: release the fit), refit (runs, cleanup).",
        .positionalDoc = "layout or mesh",
        .flags = {{"operation", "op", T::String, nullptr, "What to do.",
                   {"moveJunction", "bend", "split", "corner", "delete", "offset", "loosen", "refit"}},
                  {"junction", "", T::Int, nullptr, "A junction id."},
                  {"run", "", T::Int, nullptr, "A run id."},
                  {"runs", "", T::List, nullptr, "Run ids."},
                  {"junctions", "", T::List, nullptr, "Junction ids."},
                  {"to", "", T::Vec3, nullptr, "World-space target."},
                  {"grab", "", T::Vec3, nullptr, "bend: the world-space point on the run being dragged."},
                  {"at", "", T::Vec3, nullptr, "split: world-space point on the run."},
                  {"distance", "", T::Float, nullptr, "offset: world distance, positive to the left of each run."},
                  {"cleanup", "", T::Float, 0.5, "refit: 0 to 1.", {}, {}, 0.0, 1.0},
                  tolerance},
        .run = [](const Args& a) -> json {
            const std::string op = a.get<std::string>("operation");
            return edit(a, "panelLayoutEdit", [&](PanelLayout& l, const EditContext& c, const Target& t) {
                auto point = [&](const char* flag) {
                    if (!a.has(flag))
                        throw CommandError("panelLayoutEdit: " + op + " needs " + flag);
                    return local(t, vecArg(a.flags[flag], "panelLayoutEdit"));
                };
                const uint32_t junction = uint32_t(a.get<int64_t>("junction", 0)) & ~kMirrored;
                const uint32_t run = uint32_t(a.get<int64_t>("run", 0)) & ~kMirrored;
                std::vector<uint32_t> runs = idsArg(a, "runs"), junctions = idsArg(a, "junctions");
                for (uint32_t& id : runs)
                    id &= ~kMirrored;
                for (uint32_t& id : junctions)
                    id &= ~kMirrored;
                if (op == "moveJunction")
                    return moveJunction(l, c, junction, point("to"));
                if (op == "bend")
                    return bendRun(l, c, run, point("grab"), point("to"));
                if (op == "split")
                    return splitRun(l, c, run, point("at"));
                if (op == "corner")
                    return toggleCorner(l, junction);
                if (op == "delete")
                {
                    EditResult r = deleteRuns(l, c, runs);
                    EditResult j = deleteJunctions(l, c, junctions);
                    r.changed |= j.changed;
                    return r;
                }
                if (op == "offset")
                    return offsetRuns(l, c, runs, a.get<double>("distance") / t.scale);
                if (op == "loosen")
                    return loosenRuns(l, c, runs);
                if (op == "refit")
                    return refitRuns(l, c, a.get<double>("cleanup", 0.5), runs);
                throw CommandError("panelLayoutEdit: unknown operation '" + op + "'");
            });
        },
        .adjustable = true,
    });

    ops::registerCommand({
        .name = "panelLayoutSetRuns",
        .doc = "Move runs to a line set, or give them their own width, depth or profile. clear drops their own values so they "
               "follow their set again.",
        .positionalDoc = "layout or mesh",
        .flags = {{"runs", "", T::List, nullptr, "Run ids."},
                  {"set", "", T::String, nullptr, "Line set name."},
                  {"width", "", T::Float, nullptr, "Width, cm.", {}, {}, 0.0},
                  {"depth", "", T::Float, nullptr, "Depth, cm; negative raises a ridge."},
                  {"profile", "", T::String, nullptr, "v, u, square, step or chamfer.", {"v", "u", "square", "step", "chamfer"}},
                  {"clear", "", T::Bool, false, "Follow the set's width, depth and profile again."}},
        .run = [](const Args& a) -> json {
            return edit(a, "panelLayoutSetRuns", [&](PanelLayout& l, const EditContext&, const Target&) {
                EditResult r;
                std::optional<uint32_t> set;
                if (a.has("set"))
                    set = setArg(l, a, "panelLayoutSetRuns").id;
                std::optional<Profile> profile;
                if (a.has("profile") && !(profile = profileFromName(a.get<std::string>("profile"))))
                    throw CommandError("panelLayoutSetRuns: profile is v, u, square, step or chamfer");
                for (uint32_t id : idsArg(a, "runs"))
                    if (Run* run = l.run(id & ~kMirrored))
                    {
                        if (a.get<bool>("clear", false))
                            run->width = run->depth = std::nullopt, run->profile = std::nullopt;
                        if (set)
                            run->set = *set;
                        if (a.has("width"))
                            run->width = std::max(0.0, a.get<double>("width"));
                        if (a.has("depth"))
                            run->depth = a.get<double>("depth");
                        if (profile)
                            run->profile = profile;
                        r.runs.push_back(run->id);
                    }
                r.changed = !r.runs.empty();
                return r;
            });
        },
    });

    ops::registerCommand({
        .name = "panelLayoutLineSet",
        .doc = "Add, change, rename, remove or activate a Panel Layout line set. Each set says how its lines render (output) and "
               "their default width, depth and profile; new strokes go to the active set. Removing a set moves its lines to the first.",
        .positionalDoc = "layout or mesh",
        .flags = {{"action", "", T::String, "edit", "add, edit, remove or activate.", {"add", "edit", "remove", "activate"}},
                  {"set", "", T::String, nullptr, "The set's name."},
                  {"rename", "", T::String, nullptr, "edit: a new name."},
                  {"width", "", T::Float, nullptr, "Width, cm.", {}, {}, 0.0},
                  {"depth", "", T::Float, nullptr, "Depth, cm; negative raises a ridge."},
                  {"profile", "", T::String, nullptr, "v, u, square, step or chamfer.", {"v", "u", "square", "step", "chamfer"}},
                  {"output", "", T::String, nullptr, "geometry, sculpt, texture, decal or none.", {"geometry", "sculpt", "texture", "decal", "none"}}},
        .run = [](const Args& a) -> json {
            return edit(a, "panelLayoutLineSet", [&](PanelLayout& l, const EditContext&, const Target&) {
                EditResult r;
                const std::string action = a.get<std::string>("action", "edit");
                if (action == "add")
                {
                    LineSet s;
                    s.id = l.newId();
                    s.name = a.get<std::string>("set", "Lines");
                    for (const LineSet& other : l.sets)
                        if (other.name == s.name)
                            throw CommandError("panelLayoutLineSet: a set named '" + s.name + "' exists");
                    applySetValues(s, a, "panelLayoutLineSet");
                    l.sets.push_back(s);
                    l.activeSet = s.id;
                }
                else
                {
                    const uint32_t id = setArg(l, a, "panelLayoutLineSet").id;
                    LineSet& s = *l.set(id);
                    if (action == "edit")
                    {
                        applySetValues(s, a, "panelLayoutLineSet");
                        if (a.has("rename"))
                            s.name = a.get<std::string>("rename");
                    }
                    else if (action == "activate")
                        l.activeSet = id;
                    else if (action == "remove")
                    {
                        if (l.sets.size() == 1)
                            throw CommandError("panelLayoutLineSet: a layout keeps at least one set");
                        std::erase_if(l.sets, [id](const LineSet& x) { return x.id == id; });
                        for (Run& run : l.runs)
                            if (run.set == id)
                                run.set = l.sets.front().id;
                        if (l.activeSet == id)
                            l.activeSet = l.sets.front().id;
                    }
                    else
                        throw CommandError("panelLayoutLineSet: action is add, edit, remove or activate");
                }
                r.changed = true;
                return r;
            });
        },
    });

    ops::registerCommand({
        .name = "panelLayoutMirror",
        .doc = "Mirror a Panel Layout's lines across a plane through the mesh's origin: lines are drawn on one side and the other side "
               "follows. side picks which side is drawn on.",
        .positionalDoc = "layout or mesh",
        .flags = {{"axis", "", T::String, "x", "off, x, y or z.", kAxisNames}, {"side", "", T::Int, 1, "1 or -1: the side lines are drawn on."}},
        .run = [](const Args& a) -> json {
            return edit(a, "panelLayoutMirror", [&](PanelLayout& l, const EditContext&, const Target&) {
                const std::string axis = a.get<std::string>("axis", "x");
                auto it = std::find(kAxisNames.begin(), kAxisNames.end(), axis);
                if (it == kAxisNames.end())
                    throw CommandError("panelLayoutMirror: axis is off, x, y or z");
                l.mirrorAxis = int(it - kAxisNames.begin()) - 1;
                l.mirrorSide = a.get<int64_t>("side", 1) < 0 ? -1 : 1;
                EditResult r;
                r.changed = true;
                return r;
            });
        },
    });

    ops::registerCommand({
        .name = "panelLayoutClear",
        .doc = "Remove every line from a Panel Layout, keeping its line sets.",
        .positionalDoc = "layout or mesh",
        .run = [](const Args& a) -> json {
            return edit(a, "panelLayoutClear", [](PanelLayout& l, const EditContext&, const Target&) {
                EditResult r;
                r.changed = !l.runs.empty() || !l.junctions.empty();
                l.runs.clear();
                l.junctions.clear();
                l.panels.clear();
                return r;
            });
        },
    });

    ops::registerCommand({
        .name = "panelLayoutInfo",
        .doc = "A Panel Layout's line sets, runs (with their mirror images), junctions, panel count and warnings. points=True adds "
               "each run's world-space polyline.",
        .positionalDoc = "layout or mesh",
        .flags = {{"points", "", T::Bool, false, "Include run polylines."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return layoutInfo(targetArg(a, "panelLayoutInfo"), a.get<bool>("points", false)); },
    });

    ops::registerCommand({
        .name = "panelLayoutBakeMaps",
        .doc = "Bake a Panel Layout into textures in the mesh's UV space, for texturing in Lila, Painter or Unreal: a height map of "
               "the Texture lines' grooves, a panel ID map (a flat colour per panel) and a line mask. Returns the files written.",
        .positionalDoc = "layout or mesh",
        .flags = {{"path", "p", T::String, nullptr, "Folder to write the maps into."},
                  {"name", "n", T::String, nullptr, "File name prefix; default the mesh's name."},
                  {"resolution", "res", T::Int, int64_t(2048), "Width and height in texels.", {}, {}, 16.0, 16384.0},
                  {"uvSet", "", T::Int, int64_t(0), "UV set to bake into.", {}, {}, 0.0, 7.0},
                  {"lines", "", T::String, "texture", "Lines in the height map: texture (Texture lines) or all.", {"texture", "all"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const Target t = targetArg(a, "panelLayoutBakeMaps");
            if (!a.has("path"))
                throw CommandError("panelLayoutBakeMaps: give path=, the folder to write into");
            const std::shared_ptr<const Resolved> r = resolvedLayout(Graph::current(), t.node->id());
            if (!r)
                throw CommandError("panelLayoutBakeMaps: the layout has no surface");
            MapOptions o;
            o.resolution = int(a.get<int64_t>("resolution", 2048));
            o.uvSet = int(a.get<int64_t>("uvSet", 0));
            if (a.get<std::string>("lines", "texture") == "all")
                o.heightOutputs = ~0u;
            LayoutMaps maps;
            try
            {
                maps = bakeMaps(*r, o);
            }
            catch (const std::runtime_error& e)
            {
                throw CommandError(std::string("panelLayoutBakeMaps: ") + e.what());
            }
            const std::filesystem::path folder = a.get<std::string>("path");
            std::filesystem::create_directories(folder);
            const Node* mesh = Graph::current().node(t.mesh);
            const std::string prefix = a.get<std::string>("name", mesh ? mesh->name() : std::string("Layout"));
            json files = json::array();
            for (auto [suffix, img, bits] : {std::tuple{"_PanelHeight.png", &maps.height, 16}, std::tuple{"_PanelId.png", &maps.panelId, 8},
                                             std::tuple{"_PanelLines.png", &maps.lines, 8}})
            {
                const std::filesystem::path file = folder / (prefix + suffix);
                image::writePng(file, img->width, img->height, img->rgba, bits);
                files.push_back(file.generic_string());
            }
            return {{"files", files}, {"warnings", maps.warnings}};
        },
    });

    ops::registerCommand({
        .name = "panelLineMaterials",
        .doc = "Make the Unreal side of panel line decal strips through LilaLink: MF_LilaPanelLine (the groove drawn from the strip's "
               "UVs, for your own materials) and M_LilaPanelLine (masked) and M_LilaPanelLineTranslucent built on it. Meshes sent "
               "afterwards get M_LilaPanelLine on their LilaPanelLine slot. Returns the asset paths.",
        .flags = {{"regenerate", "", T::Bool, false, "Rebuild them even when they are up to date."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            try
            {
                link::Link::get().ensureConnected();
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("panelLineMaterials: Unreal is not connected (") + e.what() + ")");
            }
            const json features = link::Link::get().status().value("features", json::array());
            if (!features.is_array() || std::find(features.begin(), features.end(), json("panelLines")) == features.end())
                throw CommandError("panelLineMaterials: this LilaLink has no panel lines; update the plugin");
            link::RpcResult r = link::Link::get().client()->call("panelMaterials", {{"regenerate", a.get<bool>("regenerate", false)}},
                                                                link::importCallOptions());
            if (!r.ok)
                throw CommandError("panelLineMaterials: " + r.error);
            return r.result;
        },
    });

    ops::registerCommand({
        .name = "panelLayoutQuery",
        .doc = "Sample a Panel Layout at world-space points: the nearest line (distance, run, side) and the panel (index, key, random), "
               "as every system reading the layout sees them.",
        .positionalDoc = "layout or mesh",
        .flags = {{"points", "p", T::List, nullptr, "World-space points."},
                  {"reach", "", T::Float, nullptr, "Furthest line to report, world units; default the widest line."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const Target t = targetArg(a, "panelLayoutQuery");
            const std::shared_ptr<const Resolved> r = resolvedLayout(Graph::current(), t.node->id());
            json out = json::array();
            if (!r)
                return out;
            const double reach = a.has("reach") ? a.get<double>("reach") / t.scale : std::max(r->reach() * 4.0, r->spacing() * 8.0);
            for (const glm::dvec3& p : pointsArg(a, "points", t, "panelLayoutQuery"))
            {
                const LineSample line = r->lineAt(p, reach);
                const PanelSample panel = r->panelAt(p);
                json j = {{"panel", panel.panel}, {"panelKey", panel.key}, {"panelRandom", panel.random}};
                if (line.run >= 0)
                {
                    j["line"] = r->runs()[size_t(line.run)].id;
                    j["distance"] = line.distance * t.scale;
                    j["side"] = line.side * t.scale;
                }
                out.push_back(std::move(j));
            }
            return out;
        },
    });
}
} // namespace lila::layout
