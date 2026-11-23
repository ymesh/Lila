#include "vcolor/VertexColors.h"

#include "ops/Command.h"
#include "ops/Undo.h"
#include "core/Profile.h"
#include "geo/ColorSets.h"
#include "scene/Components.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"

#include <format>
#include <map>

namespace lila::vcolor
{
using scene::Graph;
using scene::MeshPtr;
using scene::Node;
using scene::NodeId;
using scene::PlugRef;

namespace
{
Graph& graph() { return Graph::current(); }

Node& meshArg(const std::string& name, const char* cmd)
{
    Node* n = graph().find(name);
    if (!n || !n->type().isA("mesh"))
        throw CommandError(std::string(cmd) + ": " + name + " is not a mesh");
    return *n;
}

/** Puts one side of a color delta back into the stored base. */
struct ColorDeltaAction final : UndoAction
{
    PlugRef base;
    std::string set;
    ColorDelta delta;

    ColorDeltaAction(PlugRef b, std::string s, ColorDelta d) : base(b), set(std::move(s)), delta(std::move(d)) {}

    void put(bool undo)
    {
        Graph& g = graph();
        if (!g.node(base.node))
            return;
        const MeshPtr& cur = std::get<MeshPtr>(g.storedValue(base));
        if (!cur)
            return;
        auto m = std::make_shared<geo::Mesh>(*cur);
        std::span<glm::vec4> v = geo::colors::values(*m, set);
        const auto& values = undo ? delta.before : delta.after;
        for (size_t i = 0; i < delta.corners.size(); ++i)
            if (delta.corners[i] < v.size())
                v[delta.corners[i]] = values[i];
        g.setValue(base, MeshPtr(std::move(m)));
    }
    void undo() override { put(true); }
    void redo() override { put(false); }
    size_t bytes() const override { return sizeof(*this) + delta.bytes(); }
};

struct LiveStroke
{
    uint64_t token = 0;
    NodeId mesh = scene::kNoNode;
    std::string set;
    ColorDelta delta;
};
LiveStroke gLive;
uint64_t gNextToken = 1;
std::pair<uint64_t, std::vector<glm::vec4>> gStashed;

glm::vec4 colorArg(const Args& a, const char* flag, const glm::vec4& fallback)
{
    if (!a.has(flag))
        return fallback;
    const json& j = a.flags[flag];
    if (!j.is_array() || j.size() < 3)
        throw CommandError(std::string(flag) + ": give [r, g, b] or [r, g, b, a] (linear)");
    return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j.size() > 3 ? j[3].get<float>() : 1.0f};
}

glm::dvec3 vec3Arg(const json& j, const glm::dvec3& fallback = glm::dvec3(0.0))
{
    if (!j.is_array() || j.size() < 3)
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

uint8_t channelsArg(const Args& a, const char* fallback = "rgba")
{
    uint8_t ch = channelsFromString(a.get<std::string>("channels", fallback));
    if (!ch)
        throw CommandError("channels: give some of r, g, b, a (e.g. \"rgb\" or \"a\")");
    return ch;
}

scene::softSelect::Curve falloffArg(const Args& a)
{
    if (!a.has("falloff"))
        return scene::softSelect::Curve();
    const json& j = a.flags["falloff"];
    if (j.is_string())
    {
        std::string name = j.get<std::string>();
        if (const auto* p = scene::softSelect::findBuiltin(name))
            return p->curve;
        for (const auto& p : scene::softSelect::userPresets())
            if (p.name == name)
                return p.curve;
        if (name == "softSelect")
            return scene::softSelect::currentCurve();
        throw CommandError("falloff: unknown preset " + name);
    }
    return scene::softSelect::Curve::fromJson(j);
}

/** Selected points of the node when in component mode, sorted and unique. */
std::vector<geo::Index> selectedPoints(NodeId id)
{
    if (scene::components::mode() == scene::ComponentMode::Object)
        return {};
    std::vector<geo::Index> out = scene::components::pointsOf(id);
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<geo::Index> targetsArg(const Args& a, NodeId id, const geo::Mesh& m, const char* cmd)
{
    std::string target = a.get<std::string>("target", "auto");
    std::vector<geo::Index> sel = selectedPoints(id);
    if (target == "all" || (target == "auto" && sel.empty()))
        return targetPoints(m, {});
    if (sel.empty())
        throw CommandError(std::string(cmd) + ": target=" + target + " needs selected components");
    if (target == "element")
        return elementPoints(m, sel);
    return targetPoints(m, sel);
}

const std::vector<std::string> kTargets = {"auto", "all", "selection", "element"};

FlagDef setFlag() { return {"colorSet", "", ArgType::String, "", "Color set; empty uses the current one (made, white, when the mesh has none)."}; }
FlagDef channelsFlag(const char* def = "rgba") { return {"channels", "", ArgType::String, def, "Channels to change: some of r, g, b, a."}; }
FlagDef targetFlag()
{
    return {"target", "", ArgType::String, "auto",
            "auto (the component selection, or everything without one), all, selection, or element (whole shells of the selection).",
            kTargets};
}

/** Runs an edit on a working copy of each mesh's base and commits it as one undo step. Returns the sets edited. */
json editEach(const Args& a, const char* cmd, const std::string& label,
              const std::function<ColorDelta(Node& n, geo::Mesh& m, const std::string& set, std::span<const geo::Index> points)>& op)
{
    const std::vector<std::string> objs = scene::meshObjects(a, cmd);
    Graph& g = graph();
    UndoChunk chunk(label);
    json out = json::array();
    for (const std::string& name : objs)
    {
        Node& n = meshArg(name, cmd);
        PlugRef base = scene::modifiers::editPlug(g, n.id());
        g.evaluate();
        MeshPtr cur = std::get<MeshPtr>(g.storedValue(base));
        if (!cur)
            continue;
        auto m = std::make_shared<geo::Mesh>(*cur);
        bool created = false;
        std::string set = resolveSet(*m, a.get<std::string>("colorSet", ""), &created);
        std::vector<geo::Index> points = targetsArg(a, n.id(), *m, cmd);
        ColorDelta d = op(n, *m, set, points);
        commit(n.id(), std::move(m), set, std::move(d), created, label);
        out.push_back(set);
    }
    return out;
}
} // namespace

std::string resolveSet(geo::Mesh& mesh, const std::string& requested, bool* created)
{
    std::string set = requested.empty() ? geo::colors::current(mesh) : requested;
    if (set.empty())
        set = "colorSet";
    bool make = !geo::colors::has(mesh, set);
    if (make)
    {
        geo::colors::add(mesh, set);
        geo::colors::setCurrent(mesh, set);
    }
    if (created)
        *created = make;
    return set;
}

geo::SymmetryMap symmetryOf(const Node& n, const geo::Mesh& mesh)
{
    const std::optional<geo::SymmetrySpec> spec = scene::symmetrySpec(n);
    if (!spec)
        return {};
    double tol = std::max(1e-4, glm::length(mesh.bounds().size()) * 1e-4);
    return geo::SymmetryMap::buildFor(mesh, *spec, tol);
}

MeshPtr commit(NodeId mesh, std::shared_ptr<geo::Mesh> edited, const std::string& set, ColorDelta delta, bool created, const std::string& label)
{
    Graph& g = graph();
    PlugRef base = scene::modifiers::editPlug(g, mesh);
    MeshPtr stored(std::move(edited));
    UndoChunk chunk(label);
    if (created)
    {
        g.setValue(base, stored);
        return stored;
    }
    if (delta.empty())
        return std::get<MeshPtr>(g.storedValue(base));
    if (geo::colors::isHalf(*stored, set))
    {
        auto v = geo::colors::values(const_cast<geo::Mesh&>(*stored), set);
        for (size_t i = 0; i < delta.corners.size(); ++i)
        {
            for (int k = 0; k < 4; ++k)
                delta.after[i][k] = geo::colors::roundToHalf(delta.after[i][k]);
            v[delta.corners[i]] = delta.after[i];
        }
    }
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    g.setValue(base, stored);
    u.setEnabled(was);
    u.record(std::make_unique<ColorDeltaAction>(base, set, std::move(delta)));
    return stored;
}

MeshPtr preview(NodeId mesh, const std::string& set, std::span<const glm::vec4> colors)
{
    Graph& g = graph();
    PlugRef base = scene::modifiers::editPlug(g, mesh);
    const MeshPtr& cur = std::get<MeshPtr>(g.storedValue(base));
    if (!cur)
        return nullptr;
    auto m = std::make_shared<geo::Mesh>(*cur);
    std::span<glm::vec4> v = geo::colors::values(*m, set);
    std::copy_n(colors.begin(), std::min(colors.size(), v.size()), v.begin());
    MeshPtr stored(std::move(m));
    UndoStack& u = UndoStack::get();
    bool was = u.enabled();
    u.setEnabled(false);
    g.setValue(base, stored);
    u.setEnabled(was);
    return stored;
}

uint64_t stashLiveStroke(NodeId mesh, std::string set, ColorDelta delta)
{
    gLive = {gNextToken++, mesh, std::move(set), std::move(delta)};
    return gLive.token;
}

uint64_t stashValues(std::vector<glm::vec4> values)
{
    gStashed = {gNextToken++, std::move(values)};
    return gStashed.first;
}

std::vector<geo::Index> cornerOrder(const geo::Mesh& mesh)
{
    std::vector<geo::Index> out;
    out.reserve(mesh.numCorners());
    for (geo::Index f = 0; f < mesh.faceCapacity(); ++f)
        if (mesh.faceValid(f))
            mesh.forEachFaceCorner(f, [&](geo::Index c) { out.push_back(c); });
    return out;
}

void registerCommands()
{
    using T = ArgType;
    registerMaskCommands();

    ops::registerCommand({
        .name = "polyColorSet",
        .doc = "Create, delete, rename, copy, reorder or make current a mesh's vertex color sets, set their "
               "precision, or query them. Returns {sets, current, half} for the last mesh.",
        .positionalDoc = "meshes",
        .flags = {{"create", "", T::Bool, false, "Add colorSet (a free name when empty), filled with fill."},
                  {"delete", "", T::Bool, false, "Remove colorSet."},
                  {"rename", "", T::Bool, false, "Rename colorSet to newColorSet."},
                  {"copy", "", T::Bool, false, "Copy colorSet into newColorSet (made or overwritten)."},
                  {"current", "", T::Bool, false, "Make colorSet the current set (displayed and painted)."},
                  {"query", "", T::Bool, false, "Only report the sets."},
                  {"colorSet", "", T::String, "", "The set to act on; empty is the current one."},
                  {"newColorSet", "", T::String, "", "rename and copy: the new name."},
                  {"precision", "", T::String, "", "Store colorSet as float or half.", {"", "float", "half"}},
                  {"moveTo", "", T::Int, -1, "Move colorSet to this position (0 is the primary set exporters write first).", {}, {}, -1.0},
                  {"fill", "", T::List, json::array({1.0, 1.0, 1.0, 1.0}), "create: initial linear RGBA."}},
        .run = [](const Args& a) -> json {
            const std::vector<std::string> objs = scene::meshObjects(a, "polyColorSet");
            Graph& g = graph();
            json info;
            UndoChunk chunk("Color Set");
            for (const std::string& name : objs)
            {
                Node& n = meshArg(name, "polyColorSet");
                g.evaluate();
                if (a.get<bool>("query"))
                {
                    MeshPtr m = scene::modifiers::cage(g, n.id());
                    info = {{"sets", m ? geo::colors::sets(*m) : std::vector<std::string>{}},
                            {"current", m ? geo::colors::current(*m) : ""},
                            {"half", m ? m->colorSetInfo().half : std::vector<std::string>{}}};
                    continue;
                }
                PlugRef base = scene::modifiers::editPlug(g, n.id());
                g.evaluate();
                MeshPtr cur = std::get<MeshPtr>(g.storedValue(base));
                if (!cur)
                    continue;
                auto m = std::make_shared<geo::Mesh>(*cur);
                std::string set = a.get<std::string>("colorSet", "");
                std::string to = a.get<std::string>("newColorSet", "");
                if (a.get<bool>("create"))
                {
                    if (set.empty())
                        set = geo::colors::uniqueName(*m);
                    if (geo::colors::has(*m, set))
                        throw CommandError("polyColorSet: " + name + " already has a color set " + set);
                    geo::colors::add(*m, set, colorArg(a, "fill", glm::vec4(1.0f)));
                }
                else if (set.empty())
                    set = geo::colors::current(*m);
                if (!geo::colors::has(*m, set))
                    throw CommandError("polyColorSet: " + name + " has no color set " + (set.empty() ? "(none)" : set));
                if (a.get<bool>("delete"))
                    geo::colors::remove(*m, set);
                if (a.get<bool>("rename"))
                {
                    if (to.empty() || geo::colors::has(*m, to))
                        throw CommandError("polyColorSet: rename needs a free newColorSet");
                    geo::colors::rename(*m, set, to);
                    set = to;
                }
                if (a.get<bool>("copy"))
                {
                    if (to.empty())
                        to = geo::colors::uniqueName(*m, set);
                    geo::colors::copy(*m, set, to);
                }
                if (a.get<bool>("current") && geo::colors::has(*m, set))
                    geo::colors::setCurrent(*m, set);
                if (std::string p = a.get<std::string>("precision", ""); !p.empty() && geo::colors::has(*m, set))
                    geo::colors::setHalf(*m, set, p == "half");
                if (int at = a.get<int>("moveTo", -1); at >= 0 && geo::colors::has(*m, set))
                    geo::colors::move(*m, set, size_t(at));
                info = {{"sets", geo::colors::sets(*m)}, {"current", geo::colors::current(*m)}, {"half", m->colorSetInfo().half}};
                g.setValue(base, MeshPtr(std::move(m)));
            }
            return info;
        },
    });

    ops::registerCommand({
        .name = "vertexColorStroke",
        .doc = "One vertex paint stroke: dabs is a list of {center, radius, opacity, view, eye} in world space (center and radius "
               "required). Mirrors through the mesh's symmetry and stays inside the component selection. One undo step that stores "
               "only the changed corners. Returns {corners} changed.",
        .positionalDoc = "mesh",
        .flags = {setFlag(),
                  {"blend", "", T::String, "replace", "Blend mode.", {"replace", "add", "subtract", "multiply", "lighten", "darken", "blur", "sharpen"}},
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0, 1.0}), "Linear RGBA."},
                  {"strength", "", T::Float, 1.0, "Most the stroke changes a point, 0 to 1.", {}, {}, 0.0, 1.0},
                  channelsFlag(),
                  {"falloff", "", T::Any, "Smooth", "A soft select falloff preset name, \"softSelect\" for the current curve, or keys."},
                  {"frontFacesOnly", "", T::Bool, true, "Leave points facing away from the view."},
                  {"paintThrough", "", T::Bool, false, "Paint points hidden behind other parts too."},
                  {"mirror", "", T::Bool, true, "Mirror across the mesh's symmetry when it is on."},
                  {"selectionOnly", "", T::Bool, true, "With selected components, only they change."},
                  {"radialCount", "", T::Int, 1, "Copies of every dab turned evenly about an object axis; 1 is off.", {}, {}, 1.0, 64.0},
                  {"radialAxis", "", T::String, "y", "The object axis radial copies turn about.", {"x", "y", "z"}},
                  {"dabs", "", T::List, json::array(), "The stamps, in order."},
                  {"live", "", T::Int, 0, "Internal: adopts the stroke the viewport tool already applied."}},
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("vertexColorStroke: give or select a mesh");
            Graph& g = graph();
            Node& n = meshArg(objs.back(), "vertexColorStroke");
            uint64_t token = uint64_t(a.get<int64_t>("live"));
            PlugRef base = scene::modifiers::editPlug(g, n.id());
            g.evaluate();
            MeshPtr cur = std::get<MeshPtr>(g.storedValue(base));
            if (!cur)
                throw CommandError("vertexColorStroke: " + n.path() + " has no mesh");
            auto m = std::make_shared<geo::Mesh>(*cur);
            bool created = false;
            std::string set;
            ColorDelta d;
            if (token != 0 && token == gLive.token && gLive.mesh == n.id())
            {
                set = resolveSet(*m, gLive.set, &created);
                d = std::move(gLive.delta);
                gLive = {};
                std::span<glm::vec4> v = geo::colors::values(*m, set);
                for (size_t i = 0; i < d.corners.size(); ++i)
                    if (d.corners[i] < v.size())
                        v[d.corners[i]] = d.after[i];
            }
            else
            {
                set = resolveSet(*m, a.get<std::string>("colorSet", ""), &created);
                BrushSettings s;
                auto blend = blendFromName(a.get<std::string>("blend", "replace"));
                if (!blend)
                    throw CommandError("vertexColorStroke: unknown blend " + a.get<std::string>("blend", ""));
                s.blend = *blend;
                s.color = colorArg(a, "color", glm::vec4(1.0f));
                s.strength = float(std::clamp(a.get<double>("strength", 1.0), 0.0, 1.0));
                s.channels = channelsArg(a);
                s.falloff = falloffArg(a);
                s.frontFacesOnly = a.get<bool>("frontFacesOnly", true);
                s.paintThrough = a.get<bool>("paintThrough", false);
                const std::string axis = a.get<std::string>("radialAxis", "y");
                s.radial = {std::clamp(a.get<int>("radialCount", 1), 1, 64), axis == "x" ? 0 : axis == "z" ? 2 : 1};
                PaintMesh pm(m, set);
                if (a.get<bool>("mirror", true))
                {
                    pm.setSymmetry(symmetryOf(n, *m));
                    s.mirror = !pm.symmetry().empty();
                }
                if (a.get<bool>("selectionOnly", true))
                    if (auto sel = selectedPoints(n.id()); !sel.empty())
                    {
                        std::vector<uint8_t> mask(m->pointCapacity(), 0);
                        for (geo::Index p : sel)
                            if (p < mask.size())
                                mask[p] = 1;
                        pm.setMask(std::move(mask));
                    }
                const glm::dmat4 world = n.as<glm::dmat4>(scene::Transform::kWorldMatrix);
                const glm::dmat4 inv = glm::inverse(world);
                const double scale = std::max(1e-12, std::cbrt(std::abs(glm::determinant(glm::dmat3(world)))));
                pm.beginStroke(s);
                for (const json& j : a.flags.value("dabs", json::array()))
                {
                    if (!j.is_object() || !j.contains("center"))
                        throw CommandError("vertexColorStroke: each dab needs a center");
                    Dab dab;
                    dab.center = glm::dvec3(inv * glm::dvec4(vec3Arg(j["center"]), 1.0));
                    dab.radius = j.value("radius", 1.0) / scale;
                    dab.opacity = j.value("opacity", 1.0);
                    dab.viewDir = glm::dvec3(inv * glm::dvec4(vec3Arg(j.value("view", json())), 0.0));
                    dab.eye = glm::dvec3(inv * glm::dvec4(vec3Arg(j.value("eye", json()), dab.center), 1.0));
                    if (!j.contains("eye"))
                        dab.eye = dab.center - dab.viewDir * 1e6;
                    pm.dab(dab);
                }
                d = pm.endStroke();
                std::span<glm::vec4> v = geo::colors::values(*m, set);
                for (size_t i = 0; i < d.corners.size(); ++i)
                    v[d.corners[i]] = d.after[i];
                geo::colors::settle(*m, set);
            }
            size_t count = d.corners.size();
            commit(n.id(), std::move(m), set, std::move(d), created, "Vertex Paint Stroke");
            return {{"corners", count}, {"colorSet", set}};
        },
    });

    ops::registerCommand({
        .name = "vertexColorFill",
        .doc = "Flood vertex colors: blend a color over the whole mesh, the selected components, or whole shells of them. Returns the "
               "sets changed.",
        .positionalDoc = "meshes",
        .flags = {setFlag(),
                  {"color", "", T::List, json::array({1.0, 1.0, 1.0, 1.0}), "Linear RGBA."},
                  {"blend", "", T::String, "replace", "Blend mode.", {"replace", "add", "subtract", "multiply", "lighten", "darken", "blur", "sharpen"}},
                  {"opacity", "", T::Float, 1.0, "0 to 1.", {}, {}, 0.0, 1.0},
                  channelsFlag(),
                  targetFlag()},
        .run = [](const Args& a) -> json {
            auto blend = blendFromName(a.get<std::string>("blend", "replace"));
            if (!blend)
                throw CommandError("vertexColorFill: unknown blend");
            glm::vec4 color = colorArg(a, "color", glm::vec4(1.0f));
            float opacity = float(std::clamp(a.get<double>("opacity", 1.0), 0.0, 1.0));
            uint8_t ch = channelsArg(a);
            return editEach(a, "vertexColorFill", "Vertex Color Fill", [&](Node&, geo::Mesh& m, const std::string& set, std::span<const geo::Index> pts) {
                return fill(m, set, pts, *blend, color, opacity, ch);
            });
        },
    });

    ops::registerCommand({
        .name = "vertexColorGradient",
        .doc = "Gradient fill: colorA at start to colorB at end (linear), or from start out to the distance of end (radial), world "
               "space. Without start and end it spans the target points along axis.",
        .positionalDoc = "meshes",
        .flags = {setFlag(),
                  {"type", "", T::String, "linear", "linear or radial.", {"linear", "radial"}},
                  {"start", "", T::Vec3, nullptr, "World-space start."},
                  {"end", "", T::Vec3, nullptr, "World-space end."},
                  {"axis", "", T::String, "y", "Without start and end: x, y or z across the targets' bounds.", {"x", "y", "z"}},
                  {"colorA", "", T::List, json::array({0.0, 0.0, 0.0, 1.0}), "Linear RGBA at the start."},
                  {"colorB", "", T::List, json::array({1.0, 1.0, 1.0, 1.0}), "Linear RGBA at the end."},
                  channelsFlag(),
                  targetFlag()},
        .run = [](const Args& a) -> json {
            GradientType type = a.get<std::string>("type", "linear") == "radial" ? GradientType::Radial : GradientType::Linear;
            glm::vec4 ca = colorArg(a, "colorA", glm::vec4(0, 0, 0, 1)), cb = colorArg(a, "colorB", glm::vec4(1.0f));
            uint8_t ch = channelsArg(a);
            std::string axisName = a.get<std::string>("axis", "y");
            int axis = axisName == "x" ? 0 : axisName == "z" ? 2 : 1;
            return editEach(a, "vertexColorGradient", "Vertex Color Gradient", [&](Node& n, geo::Mesh& m, const std::string& set, std::span<const geo::Index> pts) {
                glm::dmat4 world = n.as<glm::dmat4>(scene::Transform::kWorldMatrix), inv = glm::inverse(world);
                glm::dvec3 from, to;
                if (a.has("start") && a.has("end"))
                {
                    from = glm::dvec3(inv * glm::dvec4(vec3Arg(a.flags["start"]), 1.0));
                    to = glm::dvec3(inv * glm::dvec4(vec3Arg(a.flags["end"]), 1.0));
                }
                else
                {
                    // Bounds in world space so "y" means up however the object is rotated.
                    geo::Bounds b;
                    for (geo::Index p : pts)
                        b.extend(glm::dvec3(world * glm::dvec4(m.position(p), 1.0)));
                    glm::dvec3 lo = b.center(), hi = b.center();
                    lo[axis] = b.min[axis];
                    hi[axis] = b.max[axis];
                    if (type == GradientType::Radial)
                        hi = b.max;
                    from = glm::dvec3(inv * glm::dvec4(lo, 1.0));
                    to = glm::dvec3(inv * glm::dvec4(hi, 1.0));
                    if (type == GradientType::Radial)
                        from = glm::dvec3(inv * glm::dvec4(b.center(), 1.0));
                }
                return gradient(m, set, pts, type, from, to, ca, cb, ch);
            });
        },
    });

    ops::registerCommand({
        .name = "vertexColorAdjust",
        .doc = "Adjust vertex colors: invert, or levels (input black and white points, gamma, output range), per channel.",
        .positionalDoc = "meshes",
        .flags = {setFlag(),
                  {"operation", "", T::String, "invert", "invert or levels.", {"invert", "levels"}},
                  {"inBlack", "", T::Float, 0.0, "levels: input black point."},
                  {"inWhite", "", T::Float, 1.0, "levels: input white point."},
                  {"gamma", "", T::Float, 1.0, "levels: midtone gamma; above 1 brightens.", {}, {}, 0.0},
                  {"outBlack", "", T::Float, 0.0, "levels: output black."},
                  {"outWhite", "", T::Float, 1.0, "levels: output white."},
                  channelsFlag(),
                  targetFlag()},
        .run = [](const Args& a) -> json {
            uint8_t ch = channelsArg(a);
            bool inv = a.get<std::string>("operation", "invert") != "levels";
            Levels l{float(a.get<double>("inBlack", 0.0)), float(a.get<double>("inWhite", 1.0)), float(a.get<double>("gamma", 1.0)),
                     float(a.get<double>("outBlack", 0.0)), float(a.get<double>("outWhite", 1.0))};
            return editEach(a, "vertexColorAdjust", inv ? "Invert Vertex Colors" : "Vertex Color Levels",
                            [&](Node&, geo::Mesh& m, const std::string& set, std::span<const geo::Index> pts) {
                                return inv ? invert(m, set, pts, ch) : levels(m, set, pts, l, ch);
                            });
        },
    });

    ops::registerCommand({
        .name = "vertexColorBlur",
        .doc = "Smooth vertex colors across edges; a negative strength sharpens instead.",
        .positionalDoc = "meshes",
        .flags = {setFlag(), {"iterations", "", T::Int, 1, "Passes.", {}, {}, 1.0, 1000.0}, {"strength", "", T::Float, 0.5, "-1 to 1 per pass.", {}, {}, -1.0, 1.0}, channelsFlag(), targetFlag()},
        .run = [](const Args& a) -> json {
            uint8_t ch = channelsArg(a);
            int it = std::clamp(a.get<int>("iterations", 1), 1, 1000);
            float st = float(std::clamp(a.get<double>("strength", 0.5), -1.0, 1.0));
            return editEach(a, "vertexColorBlur", "Blur Vertex Colors", [&](Node&, geo::Mesh& m, const std::string& set, std::span<const geo::Index> pts) {
                return blur(m, set, pts, it, st, ch);
            });
        },
    });

    ops::registerCommand({
        .name = "vertexColorBake",
        .doc = "Bake a grayscale map into vertex color channels: ambient occlusion from the mesh itself, cavity (creases dark), "
               "curvature (0.5 flat, convex brighter) or a height gradient along an axis. The classic masks for game materials.",
        .positionalDoc = "meshes",
        .flags = {setFlag(),
                  {"type", "", T::String, "ao", "ao, cavity, curvature or height.", {"ao", "cavity", "curvature", "height"}},
                  {"rays", "", T::Int, 64, "ao: rays per vertex.", {}, {}, 1.0},
                  {"maxDistance", "", T::Float, 0.0, "ao: occluders farther than this don't count (cm); 0 is a quarter of the mesh's size.", {}, {}, 0.0},
                  {"contrast", "", T::Float, 1.0, "cavity and curvature: higher reads gentler shapes.", {}, {}, 0.0},
                  {"axis", "", T::String, "y", "height: object-space axis.", {"x", "y", "z"}},
                  {"blur", "", T::Int, 0, "Smoothing passes over the result.", {}, {}, 0.0, 100.0},
                  channelsFlag("rgb"),
                  targetFlag()},
        .run = [](const Args& a) -> json {
            auto type = bakeFromName(a.get<std::string>("type", "ao"));
            if (!type)
                throw CommandError("vertexColorBake: unknown type");
            BakeOptions o;
            o.type = *type;
            o.rays = a.get<int>("rays", 64);
            o.maxDistance = a.get<double>("maxDistance", 0.0);
            o.contrast = a.get<double>("contrast", 1.0);
            std::string axis = a.get<std::string>("axis", "y");
            o.axis = axis == "x" ? 0 : axis == "z" ? 2 : 1;
            o.blur = std::clamp(a.get<int>("blur", 0), 0, 100);
            uint8_t ch = channelsArg(a, "rgb");
            return editEach(a, "vertexColorBake", "Bake Vertex Colors", [&](Node&, geo::Mesh& m, const std::string& set, std::span<const geo::Index> pts) {
                std::vector<float> values = bake(m, o);
                return writeChannel(m, set, pts, values, ch);
            });
        },
    });

    ops::registerCommand({
        .name = "vertexColorSample",
        .doc = "The vertex color on the mesh surface closest to a world-space point (the eyedropper). Returns linear [r, g, b, a].",
        .positionalDoc = "mesh",
        .flags = {setFlag(), {"at", "", T::Vec3, nullptr, "World-space point."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty() || !a.has("at"))
                throw CommandError("vertexColorSample: give a mesh and at=");
            Graph& g = graph();
            Node& n = meshArg(objs.back(), "vertexColorSample");
            MeshPtr m = scene::modifiers::cage(g, n.id());
            std::string set = a.get<std::string>("colorSet", "");
            if (!m || (set.empty() && (set = geo::colors::current(*m)).empty()) || !geo::colors::has(*m, set))
                return json::array({1.0, 1.0, 1.0, 1.0});
            geo::MeshBvh bvh(*m);
            geo::BvhClosest c;
            glm::dvec3 p(glm::inverse(n.as<glm::dmat4>(scene::Transform::kWorldMatrix)) * glm::dvec4(vec3Arg(a.flags["at"]), 1.0));
            if (!bvh.closestPoint(p, c))
                return json::array({1.0, 1.0, 1.0, 1.0});
            glm::vec4 v = bvh.interpolate<glm::vec4>(geo::colors::values(*m, set), c.triangle, c.bary);
            return json::array({v.r, v.g, v.b, v.a});
        },
    });

    ops::registerCommand({
        .name = "getVertexColorData",
        .doc = "A color set's values per face-vertex (faces in order, each face's corners around it) as a flat [r, g, b, a, ...] list. "
               "Python's Node.get_colors returns the same as a numpy array, faster.",
        .positionalDoc = "mesh",
        .flags = {setFlag(), {"base", "", T::Bool, true, "The editable base mesh; False reads the modified result."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            auto objs = a.objects();
            if (objs.empty())
                throw CommandError("getVertexColorData: give a mesh");
            Graph& g = graph();
            Node& n = meshArg(objs.back(), "getVertexColorData");
            g.evaluate();
            MeshPtr m = a.get<bool>("base", true) ? scene::modifiers::cage(g, n.id()) : n.as<MeshPtr>(scene::MeshNode::kOutMesh);
            std::string set = a.get<std::string>("colorSet", "");
            if (m && set.empty())
                set = geo::colors::current(*m);
            if (!m || !geo::colors::has(*m, set))
                throw CommandError("getVertexColorData: " + n.path() + " has no color set " + (set.empty() ? "(none)" : set));
            std::span<const glm::vec4> v = geo::colors::values(*m, set);
            json out = json::array();
            for (geo::Index c : cornerOrder(*m))
                for (int k = 0; k < 4; ++k)
                    out.push_back(v[c][k]);
            return out;
        },
    });

    ops::registerCommand({
        .name = "setVertexColorData",
        .doc = "Set a color set from a flat [r, g, b, a, ...] list per face-vertex, in getVertexColorData's order. Creates the set when "
               "missing. One undo step.",
        .positionalDoc = "mesh",
        .flags = {setFlag(), {"values", "", T::List, json::array(), "4 floats per face-vertex, linear."},
                  {"buffer", "", T::Int, 0, "Internal: values handed over from Python (Node.set_colors)."}},
        .run = [](const Args& a) -> json {
            std::vector<glm::vec4> flat;
            if (uint64_t token = uint64_t(a.get<int64_t>("buffer")); token != 0)
            {
                if (token != gStashed.first)
                    throw CommandError("setVertexColorData: the buffer is gone; pass values= instead");
                flat = std::move(gStashed.second);
                gStashed = {};
            }
            else
            {
                const json& values = a.flags.value("values", json::array());
                if (values.size() % 4)
                    throw CommandError("setVertexColorData: values needs 4 floats per face-vertex");
                for (size_t i = 0; i + 3 < values.size(); i += 4)
                    flat.emplace_back(values[i].get<float>(), values[i + 1].get<float>(), values[i + 2].get<float>(), values[i + 3].get<float>());
            }
            return editEach(a, "setVertexColorData", "Set Vertex Colors", [&](Node&, geo::Mesh& m, const std::string& set, std::span<const geo::Index>) {
                std::vector<geo::Index> order = cornerOrder(m);
                if (flat.size() != order.size())
                    throw CommandError(std::format("setVertexColorData: expected {} face-vertices, got {}", order.size(), flat.size()));
                std::vector<glm::vec4> colors(m.cornerCapacity(), glm::vec4(1.0f));
                std::span<const glm::vec4> cur = geo::colors::values(m, set);
                std::copy(cur.begin(), cur.end(), colors.begin());
                for (size_t i = 0; i < order.size(); ++i)
                    colors[order[i]] = flat[i];
                return setCorners(m, set, colors);
            });
        },
    });
}
} // namespace lila::vcolor
