#include "gn/Tool.h"

#include "gn/Exec.h"
#include "gn/Gn.h"

#include "core/Profile.h"
#include "geo/Mesh.h"
#include "ops/Command.h"
#include "ops/Undo.h"
#include "scene/Components.h"
#include "scene/Modifiers.h"
#include "scene/Transform.h"

#include <algorithm>
#include <mutex>

namespace lila::gn
{
namespace
{
using scene::ComponentMode;
using scene::Graph;
using scene::Node;
using scene::NodeId;

struct ToolGizmoStore
{
    std::mutex mutex;
    std::map<NodeId, std::shared_ptr<const GizmoList>> byModifier;
};

ToolGizmoStore& toolGizmoStore()
{
    // Never destroyed, like the modifier outputs: static scene nodes clear their entries during exit.
    static ToolGizmoStore* s = new ToolGizmoStore;
    return *s;
}

std::vector<uint32_t> toolSelectedIds(const geo::Mesh& m, int domain, const char* attr)
{
    std::vector<uint32_t> out;
    auto sel = m.attributes(geo::Domain(domain)).get<bool>(attr);
    for (size_t k = 0; k < sel.size(); ++k)
        if (sel[k])
            out.push_back(uint32_t(k));
    return out;
}

glm::dvec2 toolVec2Flag(const Args& a, const char* name, glm::dvec2 fallback)
{
    const json& j = a.flags.value(name, json());
    if (!j.is_array() || j.size() < 2 || !j[0].is_number() || !j[1].is_number())
        return fallback;
    return {j[0].get<double>(), j[1].get<double>()};
}

glm::dvec3 toolVec3Flag(const Args& a, const char* name, glm::dvec3 fallback)
{
    const json& j = a.flags.value(name, json());
    if (!j.is_array() || j.size() < 3 || !j[0].is_number() || !j[1].is_number() || !j[2].is_number())
        return fallback;
    return {j[0].get<double>(), j[1].get<double>(), j[2].get<double>()};
}

glm::dmat4 toolMatrixFlag(const Args& a, const char* name)
{
    glm::dmat4 m(1.0);
    const json& j = a.flags.value(name, json());
    if (j.is_array() && j.size() == 16)
        for (int k = 0; k < 16; ++k)
            if (j[size_t(k)].is_number())
                m[k / 4][k % 4] = j[size_t(k)].get<double>();
    return m;
}

Node& toolGroupNode(Graph& g, const Args& a, const char* cmd)
{
    if (a.positional.empty() || !a.positional[0].is_string())
        throw CommandError(std::string(cmd) + ": group name required");
    Node* n = findGroupNode(g, a.positional[0].get<std::string>());
    if (!n)
        throw CommandError(std::string(cmd) + ": no geometry-nodes group '" + a.positional[0].get<std::string>() + "'");
    return *n;
}
} // namespace

bool isGizmoType(std::string_view type) { return type == "LinearGizmo" || type == "DialGizmo" || type == "TransformGizmo"; }

std::shared_ptr<const GizmoList> modifierGizmos(NodeId modifier)
{
    std::lock_guard lock(toolGizmoStore().mutex);
    auto it = toolGizmoStore().byModifier.find(modifier);
    return it == toolGizmoStore().byModifier.end() ? nullptr : it->second;
}

void setModifierGizmos(NodeId modifier, std::shared_ptr<const GizmoList> gizmos)
{
    std::lock_guard lock(toolGizmoStore().mutex);
    if (gizmos && !gizmos->empty())
        toolGizmoStore().byModifier[modifier] = std::move(gizmos);
    else
        toolGizmoStore().byModifier.erase(modifier);
}

const char* toolSelectionAttr(int domain)
{
    static const char* names[3] = {"__toolSelection_point", "__toolSelection_edge", "__toolSelection_face"};
    return names[std::clamp(domain, 0, 2)];
}

const char* toolSetSelectionAttr(int domain)
{
    static const char* names[3] = {"__toolSetSelection_point", "__toolSetSelection_edge", "__toolSetSelection_face"};
    return names[std::clamp(domain, 0, 2)];
}

ToolResult runTool(const Group& grp, const geo::Mesh& mesh, const std::array<std::vector<uint32_t>, 3>* selection, const ToolContext& ctx,
                   const std::map<std::string, json>& params)
{
    LILA_ZONE();

    geo::Mesh in = mesh;
    if (selection)
    {
        const geo::Index sizes[3] = {in.pointCapacity(), in.edgeCapacity(), in.faceCapacity()};
        for (int d = 0; d < 3; ++d)
        {
            auto sel = in.attributes(geo::Domain(d)).getOrAdd<bool>(toolSelectionAttr(d));
            std::fill(sel.begin(), sel.end(), false);
            for (uint32_t id : (*selection)[size_t(d)])
                if (id < sizes[d])
                    sel[id] = true;
        }
    }
    PlanPtr plan = compile(grp);
    if (!plan->error.empty())
        throw ExecError(plan->error);
    ExecEnv env;
    env.input = meshGeometry(std::move(in));
    env.tool = &ctx;
    for (const GInput& i : plan->group.inputs)
    {
        auto it = params.find(i.name);
        env.params[i.name] = paramValue(i, it != params.end() ? it->second : i.defaultValue);
    }
    ToolResult r;
    env.gizmos = &r.gizmos;
    GeometryPtr out = execute(*plan, env);
    if (out->mesh)
        for (int d = 0; d < 3; ++d)
            if (out->mesh->attributes(geo::Domain(d)).find(toolSetSelectionAttr(d)))
                r.selection[size_t(d)] = toolSelectedIds(*out->mesh, d, toolSetSelectionAttr(d));
    r.mesh = toOutputMesh(*out);
    return r;
}

ToolTarget toolTarget(Graph& g, NodeId id, const ToolContext& base, const glm::dvec3& rayOrigin, const glm::dvec3& rayDirection)
{
    ToolTarget t;
    t.node = id;
    t.ctx = base;
    Node* n = g.node(id);
    if (!n)
        return t;
    g.evaluate();
    const scene::MeshPtr& stored = std::get<scene::MeshPtr>(g.storedValue(scene::modifiers::editPlug(g, id)));
    if (stored)
        t.mesh = *stored;
    const glm::dmat4 toLocal = glm::inverse(n->as<glm::dmat4>(scene::Transform::kWorldMatrix));
    t.ctx.rayOrigin = glm::dvec3(toLocal * glm::dvec4(rayOrigin, 1.0));
    t.ctx.rayDirection = glm::dvec3(toLocal * glm::dvec4(rayDirection, 0.0));
    if (glm::length(t.ctx.rayDirection) > 1e-12)
        t.ctx.rayDirection = glm::normalize(t.ctx.rayDirection);
    const ComponentMode kinds[3] = {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face};
    for (int k = 0; k < 3; ++k)
    {
        const auto order = scene::components::ordered(kinds[k]);
        for (auto it = order.rbegin(); it != order.rend(); ++it)
            if (it->first == id)
            {
                t.ctx.active[size_t(k)] = it->second;
                break;
            }
    }
    const ComponentMode mode = scene::components::mode();
    if (mode != ComponentMode::Object)
    {
        t.selection.emplace();
        const scene::components::Ids& picked = scene::components::selected(id, mode);
        scene::MeshPtr cage = scene::modifiers::cage(g, id);
        for (int k = 0; k < 3; ++k)
            (*t.selection)[size_t(k)] = cage ? scene::components::convert(*cage, mode, picked, kinds[k]) : scene::components::Ids{};
    }
    return t;
}

bool readsPointer(const Group& g)
{
    for (const GNode& n : g.nodes)
        if (n.type == "MousePosition")
            return true;
    for (const auto& [name, def] : g.subgroups)
        try
        {
            if (readsPointer(Group::fromJson(def)))
                return true;
        }
        catch (const std::exception&)
        {
        }
    return false;
}

std::vector<std::string> toolGroups(Graph& g)
{
    std::vector<std::string> out;
    for (const std::string& name : groupNames(g))
        if (Node* n = findGroupNode(g, name))
            try
            {
                if (readGroup(*n).tool)
                    out.push_back(name);
            }
            catch (const std::exception&)
            {
            }
    return out;
}

void registerToolCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "gnSetTool",
        .doc = "Flag a geometry-nodes group as a tool, so it runs on the selected meshes from the Mesh Tools menu and search, or "
               "clear the flag.",
        .positionalDoc = "group",
        .flags = {{"tool", "", T::Bool, true, "On makes it a tool; off makes it a modifier group only."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& n = toolGroupNode(g, a, "gnSetTool");
            Group next = readGroup(n);
            next.tool = a.get<bool>("tool", true);
            commitGroup(g, n, std::move(next));
            return a.positional[0];
        },
    });

    ops::registerCommand({
        .name = "gnRunTool",
        .doc = "Run a tool group on meshes (the selected ones by default), replacing each base mesh with what the group makes. In a "
               "component mode the selection reaches the group's Selection node and Set Selection writes it back; the pointer and "
               "view flags feed Mouse Position and Viewport Transform. Returns the meshes edited.",
        .positionalDoc = "group",
        .flags = {{"targets", "", T::List, nullptr, "Mesh names; the selected meshes when left out."},
                  {"mouse", "", T::List, nullptr, "[x, y] pointer position in viewport pixels from the top-left."},
                  {"region", "", T::List, nullptr, "[width, height] of the viewport in pixels."},
                  {"rayOrigin", "", T::Vec3, nullptr, "World-space start of the pointer's ray."},
                  {"rayDirection", "", T::Vec3, nullptr, "World-space direction of the pointer's ray."},
                  {"view", "", T::List, nullptr, "World-to-camera matrix, 16 numbers column by column."},
                  {"projection", "", T::List, nullptr, "Camera-to-clip matrix, 16 numbers column by column."},
                  {"orthographic", "", T::Bool, false, "The view is orthographic."},
                  {"values", "", T::Any, nullptr, "Group input values by name, as a modifier attr holds them (angles in degrees)."}},
        .run = [](const Args& a) -> json {
            Graph& g = Graph::current();
            Node& groupNode = toolGroupNode(g, a, "gnRunTool");
            Group grp = readGroup(groupNode);
            g.evaluate();

            std::vector<NodeId> targets;
            if (a.has("targets"))
            {
                for (const json& t : a.flags["targets"])
                    if (Node* n = t.is_string() ? g.find(t.get<std::string>()) : nullptr; n && n->type().isA("mesh"))
                        targets.push_back(n->id());
                    else
                        throw CommandError("gnRunTool: " + t.dump() + " isn't a mesh");
            }
            else
                for (NodeId id : g.selection())
                    if (Node* n = g.node(id); n && n->type().isA("mesh"))
                        targets.push_back(id);
            if (targets.empty())
                throw CommandError("gnRunTool: select a mesh");

            std::map<std::string, json> params;
            if (const json& v = a.flags.value("values", json()); v.is_object())
                for (auto& [k, val] : v.items())
                    params[k] = val;

            ToolContext base;
            base.mouse = toolVec2Flag(a, "mouse", glm::dvec2(0.0));
            base.region = toolVec2Flag(a, "region", glm::dvec2(0.0));
            base.view = toolMatrixFlag(a, "view");
            base.projection = toolMatrixFlag(a, "projection");
            base.orthographic = a.get<bool>("orthographic", false);
            const glm::dvec3 rayOrigin = toolVec3Flag(a, "rayOrigin", glm::dvec3(0.0));
            const glm::dvec3 rayDirection = toolVec3Flag(a, "rayDirection", glm::dvec3(0.0, 0.0, -1.0));

            const ComponentMode kinds[3] = {ComponentMode::Vertex, ComponentMode::Edge, ComponentMode::Face};
            std::map<NodeId, scene::components::Ids> newSel[3] = {scene::components::selected(kinds[0]), scene::components::selected(kinds[1]),
                                                                   scene::components::selected(kinds[2])};
            UndoChunk chunk("gnRunTool " + grp.name);
            json edited = json::array();
            for (NodeId id : targets)
            {
                Node& n = *g.node(id);
                const scene::PlugRef basePlug = scene::modifiers::editPlug(g, id);
                ToolTarget t = toolTarget(g, id, base, rayOrigin, rayDirection);
                ToolResult r;
                try
                {
                    r = runTool(grp, t.mesh, t.selection ? &*t.selection : nullptr, t.ctx, params);
                }
                catch (const std::exception& e)
                {
                    throw CommandError("gnRunTool: " + n.name() + ": " + e.what());
                }
                const geo::Index sizes[3] = {r.mesh.pointCapacity(), r.mesh.edgeCapacity(), r.mesh.faceCapacity()};
                for (int k = 0; k < 3; ++k)
                {
                    if (r.selection[size_t(k)])
                    {
                        if (r.selection[size_t(k)]->empty())
                            newSel[k].erase(id);
                        else
                            newSel[k][id] = std::move(*r.selection[size_t(k)]);
                    }
                    else if (auto it = newSel[k].find(id); it != newSel[k].end())
                    {
                        std::erase_if(it->second, [&](uint32_t x) { return x >= sizes[k]; });
                        if (it->second.empty())
                            newSel[k].erase(it);
                    }
                }
                g.setValue(basePlug, scene::MeshPtr(std::make_shared<geo::Mesh>(std::move(r.mesh))));
                edited.push_back(n.name());
            }
            scene::components::setAll(newSel[0], newSel[1], newSel[2]);
            return edited;
        },
    });
}
} // namespace lila::gn
