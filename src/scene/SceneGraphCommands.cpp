#include "scene/SceneGraph.h"

#include "core/Log.h"
#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/SceneData.h"
#include "scene/Duplicate.h"
#include "ops/Undo.h"
#include "gn/Group.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace lila::scene::sceneGraph
{
namespace
{
using T = ArgType;

/**
 * View state kept with the scene, by context (a view, or a node group's name): {"positions": {context: {member: [x, y]}},
 * "flags": {member: {flag: bool}}, "frames": {context: [frame with member names, see gn::framesToJson]}, "ties": {context:
 * [tie whose links name scene nodes by full path, see gn::tiesToJson]}, "bookmarks": {context: {slot: [x, y, zoom]}},
 * "groups": {name: {"members": [fullPath], "parent": name, "position": [x, y]}}}. A member is a full path, or "@" and a
 * group's name for the node standing for that group.
 */
constexpr const char* kLayoutKey = "nodeEditor.scene";
constexpr const char* kClipFormat = "lila.sceneNodes";
constexpr int kClipVersion = 1;
constexpr const char* kParentSocket = "@parent";
constexpr const char* kChildrenSocket = "@children";
constexpr double kColumnWidth = 230.0;
constexpr double kRowGap = 30.0;

Graph& sceneGraphOf() { return Graph::current(); }

template <class F> auto rethrown(F&& f)
{
    try
    {
        return f();
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(e.what());
    }
}

std::string stringArg(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

/** A view, or a node group the editor has open. */
std::string viewArg(const Args& a, const char* cmd)
{
    std::string view = stringArg(a, 0, cmd, "view");
    if (!isView(view) && !hasGroup(view))
        throw CommandError(std::string(cmd) + ": no view or node group '" + view + "' (views: Selection, History, Hierarchy, Upstream, Downstream)");
    return view;
}

Node& nodeArg(const std::string& name, const char* cmd)
{
    Node* n = sceneGraphOf().find(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node named '" + name + "'");
    return *n;
}

/** "node.socket" split at the first dot; node paths never hold one, attr names may ("group.child"). */
std::pair<std::string, std::string> splitSocket(const std::string& s, const char* cmd)
{
    auto dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == s.size())
        throw CommandError(std::string(cmd) + ": expected node.socket, got '" + s + "'");
    return {s.substr(0, dot), s.substr(dot + 1)};
}

PlugRef plugArg(const std::string& path) { return rethrown([&] { return sceneGraphOf().plug(path); }); }

std::string baseType(const AttrDef& d)
{
    switch (d.type)
    {
    case PlugType::Bool:
        return "bool";
    case PlugType::Int:
    case PlugType::Enum:
        return "int";
    case PlugType::Float:
        return "float";
    case PlugType::Vec3:
        return (d.flags & AttrColor) ? "color" : "vector";
    case PlugType::Matrix:
        return "matrix";
    case PlugType::String:
        return "string";
    case PlugType::Mesh:
        return "mesh";
    case PlugType::Data:
        break;
    }
    return "data";
}

std::string socketType(const AttrDef& d, int component)
{
    if (component >= 0)
        return "float";
    return (d.flags & AttrArray) ? baseType(d) + "[]" : baseType(d);
}

std::string componentName(const AttrDef& d, int c) { return !d.childNames[size_t(c)].empty() ? d.childNames[size_t(c)] : d.childShort[size_t(c)]; }

std::string socketId(const AttrDef& d, int component) { return component >= 0 ? componentName(d, component) : d.name; }

bool hasComponents(const AttrDef& d)
{
    if (d.type != PlugType::Vec3 || (d.flags & AttrArray))
        return false;
    for (int c = 0; c < 3; ++c)
        if (componentName(d, c).empty())
            return false;
    return true;
}

/** A node's mesh in or out: hidden from attribute editors, but the wire a modifier stack is built from. */
bool meshPlug(const AttrDef& d) { return d.type == PlugType::Mesh && !(d.flags & AttrArray) && (d.name == "inMesh" || d.name == "outMesh"); }

/** An output, or an input animators drive other things from (keyable or added by hand). */
bool offeredAsSource(const AttrDef& d)
{
    if (meshPlug(d))
        return (d.flags & AttrOutput) != 0;
    if ((d.flags & (AttrArray | AttrHidden)))
        return false;
    return (d.flags & AttrOutput) || ((d.flags & AttrInput) && (d.flags & (AttrKeyable | AttrUser)));
}

bool offeredAsInput(const AttrDef& d) { return (d.flags & AttrInput) && (!(d.flags & AttrHidden) || meshPlug(d)); }

/** Nodes the views look through: they rarely matter when reading a rig. */
bool passThrough(const Node& n)
{
    const std::string& t = n.type().registered().name;
    return n.type().isA("animCurve") || t == "unitConversion" || t.ends_with("UnitConversion");
}

std::string categoryOf(const NodeType& t)
{
    if (rigmath::overloadFor(t.registered().name))
        return "Math";
    if (t.isA("animCurve"))
        return "Animation";
    if (t.isA("mesh"))
        return "Mesh";
    return t.dag ? "Object" : "Utility";
}

/** Connections by node: (dst, src) pairs into it and out of it. */
struct Edges
{
    using List = std::vector<std::pair<PlugRef, PlugRef>>;
    std::unordered_map<NodeId, List> into, outOf;

    explicit Edges(const Graph& g)
    {
        for (const auto& [dst, src] : g.connections())
        {
            into[dst.node].push_back({dst, src});
            outOf[src.node].push_back({dst, src});
        }
    }

    const List& of(NodeId id, bool upstream) const
    {
        static const List kNone;
        const auto& m = upstream ? into : outOf;
        auto it = m.find(id);
        return it == m.end() ? kNone : it->second;
    }
};

/** The nodes one step up or down from id, looking through pass-through nodes unless all. */
std::vector<NodeId> neighbours(const Graph& g, const Edges& e, NodeId id, bool upstream, bool all)
{
    std::vector<NodeId> out;
    std::unordered_set<NodeId> seen{id};
    std::vector<NodeId> stack{id};
    while (!stack.empty())
    {
        NodeId cur = stack.back();
        stack.pop_back();
        for (const auto& [dst, src] : e.of(cur, upstream))
        {
            NodeId next = upstream ? src.node : dst.node;
            const Node* n = g.node(next);
            if (!n || !seen.insert(next).second)
                continue;
            if (!all && passThrough(*n))
                stack.push_back(next);
            else
                out.push_back(next);
        }
    }
    return out;
}

struct ViewNodes
{
    std::vector<NodeId> order;
    std::unordered_set<NodeId> members;

    bool add(NodeId id)
    {
        if (!members.insert(id).second)
            return false;
        order.push_back(id);
        return true;
    }
};

/** The selected DAG nodes with their ancestors and children, in Outliner order, then any selected non-DAG nodes. */
void collectHierarchy(const Graph& g, ViewNodes& v)
{
    std::unordered_set<NodeId> wanted;
    for (NodeId id : g.selection())
    {
        const Node* n = g.node(id);
        if (!n || !n->type().dag)
            continue;
        for (NodeId p = id; p != kNoNode; p = g.parent(p))
            wanted.insert(p);
        for (NodeId c : g.children(id))
            wanted.insert(c);
    }
    std::function<void(NodeId)> visit = [&](NodeId id) {
        v.add(id);
        for (NodeId c : g.children(id))
            if (wanted.contains(c))
                visit(c);
    };
    for (NodeId r : g.roots())
        if (wanted.contains(r) && g.parent(r) == kNoNode)
            visit(r);
    for (NodeId id : g.selection())
        if (const Node* n = g.node(id); n && !n->type().dag)
            v.add(id);
}

/** Each selected mesh's whole modifier stack and the node it starts from. A selected modifier stands for the mesh it feeds. */
void collectHistory(const Graph& g, const Edges& e, ViewNodes& v)
{
    auto stackLink = [&](const Edges::List::value_type& c) {
        const Node* n = g.node(c.first.node);
        return n && (modifiers::asModifier(n) || dynamic_cast<const MeshNode*>(n));
    };
    for (NodeId id : g.selection())
    {
        NodeId mesh = id;
        while (modifiers::asModifier(g.node(mesh)))
        {
            const Edges::List& out = e.of(mesh, false);
            auto it = std::ranges::find_if(out, stackLink);
            if (it == out.end())
                break;
            mesh = it->first.node;
        }
        if (!dynamic_cast<const MeshNode*>(g.node(mesh)))
        {
            v.add(id);
            continue;
        }
        std::vector<NodeId> chain{mesh};
        PlugRef plug{mesh, MeshNode::kInMesh, -1};
        while (auto src = g.source(plug))
        {
            chain.push_back(src->node);
            const ModifierNode* mod = modifiers::asModifier(g.node(src->node));
            if (!mod)
                break;
            plug = {src->node, mod->inAttr, -1};
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            v.add(*it);
    }
}

ViewNodes collect(const Graph& g, const Edges& e, const std::string& view, int depth, bool all)
{
    ViewNodes v;
    if (view == "Hierarchy")
    {
        collectHierarchy(g, v);
        return v;
    }
    if (view == "History")
    {
        collectHistory(g, e, v);
        return v;
    }
    std::vector<NodeId> roots;
    for (NodeId id : g.selection())
        if (g.node(id) && v.add(id))
            roots.push_back(id);
    if (view == "Selection")
    {
        for (bool upstream : {true, false})
            for (NodeId id : roots)
                for (NodeId n : neighbours(g, e, id, upstream, all))
                    v.add(n);
        return v;
    }
    const bool upstream = view == "Upstream";
    std::vector<NodeId> frontier = roots;
    for (int level = 0; level < depth && !frontier.empty(); ++level)
    {
        std::vector<NodeId> next;
        for (NodeId id : frontier)
            for (NodeId n : neighbours(g, e, id, upstream, all))
                if (v.add(n))
                    next.push_back(n);
        frontier = std::move(next);
    }
    return v;
}

/** The source a link into the view is drawn from: src itself, or what feeds the pass-through nodes between it and the view. */
std::optional<PlugRef> shownSource(const Graph& g, const Edges& e, const ViewNodes& v, PlugRef src, bool all)
{
    for (int hop = 0; hop < 16; ++hop)
    {
        if (v.members.contains(src.node))
            return src;
        const Node* n = g.node(src.node);
        if (all || !n || !passThrough(*n))
            return std::nullopt;
        const Edges::List& feeds = e.of(src.node, true);
        if (feeds.empty())
            return std::nullopt;
        src = feeds.front().second;
    }
    return std::nullopt;
}

json valueJson(const AttrDef& d, const PlugValue& v)
{
    if ((d.flags & AttrArray) || d.type == PlugType::Matrix || d.type == PlugType::Mesh || d.type == PlugType::Data)
        return nullptr;
    return toJson(v);
}

std::string docOf(const AttrDef& d)
{
    std::string doc = d.doc;
    if (d.type == PlugType::Enum && !d.enumNames.empty())
    {
        std::string names;
        for (size_t i = 0; i < d.enumNames.size(); ++i)
            names += (i ? ", " : "") + std::to_string(i) + " " + d.enumNames[i];
        doc += (doc.empty() ? "" : "\n") + names;
    }
    return doc;
}

double componentOf(const PlugValue& v, int c)
{
    const glm::dvec3* p = std::get_if<glm::dvec3>(&v);
    return p ? (*p)[c] : 0.0;
}

json socketJson(const std::string& id, const std::string& type, const std::string& doc)
{
    json s = {{"id", id}, {"type", type}};
    if (!doc.empty())
        s["doc"] = doc;
    return s;
}

/** The attr's editor limits as GroupInfo inputs carry them (min, max, softMin, softMax, step); an enum spans its names, listed under "enum". */
void putLimits(json& s, const AttrDef& d)
{
    for (auto [key, v] : {std::pair{"min", &d.minValue}, {"max", &d.maxValue}, {"softMin", &d.softMin}, {"softMax", &d.softMax}, {"step", &d.step}})
        if (*v)
            s[key] = **v;
    if (d.type == PlugType::Enum && !d.enumNames.empty())
    {
        s["min"] = 0;
        s["max"] = int(d.enumNames.size()) - 1;
        s["enum"] = d.enumNames;
    }
}

/** A node as GroupInfo lists it: its plugs as sockets, a Vec3 one socket plus its components where they are wired or keyable. */
json nodeJson(const Graph& g, const Edges& e, NodeId id, bool hierarchy)
{
    const Node& n = *g.node(id);
    const NodeType& type = n.type();
    std::set<std::pair<int, int>> drivenIn, drivingOut; // (attr, component)
    for (const auto& [dst, src] : e.of(id, true))
        drivenIn.insert({dst.attr, dst.component});
    for (const auto& [dst, src] : e.of(id, false))
        drivingOut.insert({src.attr, src.component});

    json ins = json::array(), outs = json::array();
    if (hierarchy && type.dag)
    {
        json parent = socketJson(kParentSocket, kDagSocketType, "The parent in the hierarchy. Wire a node's Children here to parent under it.");
        parent["linked"] = g.parent(id) != kNoNode;
        ins.push_back(parent);
        outs.push_back(socketJson(kChildrenSocket, kDagSocketType, "The children in the hierarchy."));
    }
    for (int i = 0; i < int(type.attrs.size()); ++i)
    {
        const AttrDef& d = type.attrs[size_t(i)];
        bool componentIn = false, componentOut = false;
        for (int c = 0; c < 3; ++c)
        {
            componentIn |= drivenIn.contains({i, c});
            componentOut |= drivingOut.contains({i, c});
        }
        const bool components = hasComponents(d) && ((d.flags & AttrKeyable) || componentIn || componentOut);
        const bool linked = drivenIn.contains({i, -1});
        if ((d.flags & AttrInput) && (offeredAsInput(d) || linked || componentIn))
        {
            json s = socketJson(d.name, socketType(d, -1), docOf(d));
            s["linked"] = linked;
            if (json v = valueJson(d, n.value(i)); !v.is_null())
            {
                s["value"] = std::move(v);
                s["default"] = valueJson(d, d.defaultValue);
                putLimits(s, d);
            }
            if (d.flags & AttrArray)
                s["multi"] = true;
            ins.push_back(std::move(s));
            if (components)
                for (int c = 0; c < 3; ++c)
                {
                    json cs = socketJson(componentName(d, c), "float", {});
                    cs["linked"] = drivenIn.contains({i, c});
                    cs["value"] = componentOf(n.value(i), c);
                    cs["default"] = componentOf(d.defaultValue, c);
                    ins.push_back(std::move(cs));
                }
        }
        const bool source = offeredAsSource(d);
        if (source || drivingOut.contains({i, -1}))
            outs.push_back(socketJson(d.name, socketType(d, -1), docOf(d)));
        if (hasComponents(d))
            for (int c = 0; c < 3; ++c)
                if ((source && components) || drivingOut.contains({i, c}))
                    outs.push_back(socketJson(componentName(d, c), "float", {}));
    }
    return {{"name", n.path()},
            {"type", type.registered().name},
            {"label", n.name()},
            {"category", categoryOf(type)},
            {"inputs", ins},
            {"outputs", outs},
            {"props", json::array()}};
}

json layoutData()
{
    json j = data::get(kLayoutKey, json::object());
    return j.is_object() ? j : json::object();
}

using Position = std::pair<double, double>;

Position positionArg(const json& p, const char* cmd)
{
    if (!p.is_array() || p.size() != 2 || !p[0].is_number() || !p[1].is_number())
        throw CommandError(std::string(cmd) + ": a position is [x, y], got " + p.dump());
    return {p[0].get<double>(), p[1].get<double>()};
}

/** Positions are kept per view by full path, in the scene's data, so they save with it and undo. */
void storePositions(const std::string& view, const std::vector<std::pair<NodeId, Position>>& moves)
{
    const Graph& g = sceneGraphOf();
    json layout = layoutData();
    json& positions = layout["positions"][view];
    for (const auto& [id, p] : moves)
        positions[g.fullPath(id)] = json::array({p.first, p.second});
    data::set(kLayoutKey, layout);
}

std::string groupNodeName(const std::string& group) { return "@" + group; }

/** The group a shown node name stands for ("@Arm" for Arm); empty for a scene node. */
std::string groupOfName(const std::string& name) { return name.starts_with('@') ? name.substr(1) : std::string(); }

/** The node groups in a layout, see kLayoutKey. */
struct Groups
{
    std::map<std::string, std::string> parent;
    std::map<std::string, std::vector<std::string>> members;
    std::unordered_map<std::string, std::string> memberOf;

    explicit Groups(const json& layout)
    {
        const json all = layout.value("groups", json::object());
        if (!all.is_object())
            return;
        for (const auto& [name, rec] : all.items())
        {
            if (!rec.is_object())
                continue;
            const json p = rec.value("parent", json());
            parent[name] = p.is_string() ? p.get<std::string>() : std::string();
            std::vector<std::string>& m = members[name];
            for (const json& full : rec.value("members", json::array()))
                if (full.is_string())
                {
                    m.push_back(full.get<std::string>());
                    memberOf.emplace(full.get<std::string>(), name);
                }
        }
    }

    bool has(const std::string& group) const { return parent.contains(group); }

    /** group, the group holding it, and so on out. Bounded, so a malformed loop can't spin. */
    std::vector<std::string> chain(std::string group) const
    {
        std::vector<std::string> out;
        while (has(group) && out.size() < 64 && std::find(out.begin(), out.end(), group) == out.end())
        {
            out.push_back(group);
            group = parent.at(group);
        }
        return out;
    }

    /** The group whose node stands for the scene node at full in context; empty when the node shows as itself. */
    std::string ownerIn(const std::string& context, const std::string& full) const
    {
        auto it = memberOf.find(full);
        if (it == memberOf.end())
            return {};
        const std::vector<std::string> c = chain(it->second);
        if (c.empty())
            return {};
        if (isView(context))
            return c.back();
        auto at = std::find(c.begin(), c.end(), context);
        return at == c.end() || at == c.begin() ? std::string() : *(at - 1);
    }

    /** group and every group nested in it, outermost first. */
    std::vector<std::string> subtree(const std::string& group) const
    {
        std::vector<std::string> out{group};
        for (size_t i = 0; i < out.size() && out.size() < 4096; ++i)
            for (const auto& [name, p] : parent)
                if (p == out[i] && std::find(out.begin(), out.end(), name) == out.end())
                    out.push_back(name);
        return out;
    }

    /** The full paths of the scene nodes in group and the groups nested in it. */
    std::vector<std::string> allMembers(const std::string& group) const
    {
        std::vector<std::string> out;
        for (const std::string& g : subtree(group))
            if (auto it = members.find(g); it != members.end())
                out.insert(out.end(), it->second.begin(), it->second.end());
        return out;
    }
};

json& objectAt(json& j, const std::string& key)
{
    json& v = j[key];
    if (!v.is_object())
        v = json::object();
    return v;
}

/** base, base1, base2... whichever names no group or view and is a valid name. */
std::string uniqueGroupName(const json& layout, std::string base, const char* cmd)
{
    if (!Graph::validName(base) || isView(base))
        throw CommandError(std::string(cmd) + ": '" + base + "' isn't a usable group name");
    const Groups groups(layout);
    std::string name = base;
    for (int i = 1; groups.has(name) || isView(name); ++i)
        name = base + std::to_string(i);
    return name;
}

/** Takes the scene node at full out of whichever group lists it. */
void leaveGroups(json& layout, const std::string& full)
{
    json& all = objectAt(layout, "groups");
    for (auto& [name, rec] : all.items())
        if (rec.is_object() && rec.contains("members") && rec["members"].is_array())
        {
            json& m = rec["members"];
            m.erase(std::remove(m.begin(), m.end(), json(full)), m.end());
        }
}

void joinGroup(json& layout, const std::string& group, const std::string& full)
{
    leaveGroups(layout, full);
    json& rec = objectAt(objectAt(layout, "groups"), group);
    if (!rec["members"].is_array())
        rec["members"] = json::array();
    rec["members"].push_back(full);
}

/** Drops a group's record, what is kept for it as a context and every mention of its node. */
void forgetGroup(json& layout, const std::string& group)
{
    const std::string node = groupNodeName(group);
    objectAt(layout, "groups").erase(group);
    for (const char* key : {"positions", "frames", "ties", "bookmarks"})
    {
        json& byContext = objectAt(layout, key);
        byContext.erase(group);
        if (std::string(key) == "positions")
            for (auto& [context, positions] : byContext.items())
                if (positions.is_object())
                    positions.erase(node);
        if (std::string(key) == "frames")
            for (auto& [context, frames] : byContext.items())
                for (json& f : frames)
                    if (f.is_object() && f.contains("nodes") && f["nodes"].is_array())
                        f["nodes"].erase(std::remove(f["nodes"].begin(), f["nodes"].end(), json(node)), f["nodes"].end());
    }
    objectAt(layout, "flags").erase(node);
}

/** A scene node or group node named in a command, as kept in the layout: a full path, or "@" and a group name. */
std::string memberArg(const json& layout, const std::string& name, const char* cmd)
{
    if (std::string group = groupOfName(name); !group.empty())
    {
        if (!Groups(layout).has(group))
            throw CommandError(std::string(cmd) + ": no node group '" + group + "'");
        return name;
    }
    return sceneGraphOf().fullPath(nodeArg(name, cmd).id());
}

/** A plug named on the editor's canvas: "@Arm.L_hand.rotateX", a socket of a group node, is the member plug "L_hand.rotateX". */
std::string memberPlug(const std::string& s, const char* cmd)
{
    if (!s.starts_with('@'))
        return s;
    const size_t dot = s.find('.');
    if (dot == std::string::npos || dot + 1 == s.size())
        throw CommandError(std::string(cmd) + ": expected @group.node.socket, got '" + s + "'");
    return s.substr(dot + 1);
}

/** A tie link end as the canvas names it, as kept: the scene node's full path and its own socket. Empty when it names no node. */
std::optional<std::pair<std::string, std::string>> rawEnd(const std::string& node, const std::string& socket)
{
    std::string path = node, sock = socket;
    if (node.starts_with('@'))
    {
        const size_t dot = socket.find('.');
        if (dot == std::string::npos)
            return std::nullopt;
        path = socket.substr(0, dot);
        sock = socket.substr(dot + 1);
    }
    const Graph& g = sceneGraphOf();
    const Node* n = nullptr;
    try
    {
        n = g.find(path);
    }
    catch (const CommandError&)
    {
        return std::nullopt;
    }
    if (!n)
        return std::nullopt;
    return std::pair{g.fullPath(n->id()), sock};
}

/** A node group's members, then the nodes wired straight into and out of them. */
ViewNodes collectGroup(const Graph& g, const Edges& e, const Groups& groups, const std::string& group, bool all)
{
    ViewNodes v;
    for (const std::string& full : groups.allMembers(group))
        if (const Node* n = g.find(full))
            v.add(n->id());
    const std::vector<NodeId> inside = v.order;
    for (bool upstream : {true, false})
        for (NodeId id : inside)
            for (NodeId n : neighbours(g, e, id, upstream, all))
                v.add(n);
    return v;
}

bool isPosition(const json& p) { return p.is_array() && p.size() == 2 && p[0].is_number() && p[1].is_number(); }

Position jsonPosition(const json& p) { return isPosition(p) ? Position{p[0].get<double>(), p[1].get<double>()} : Position{0.0, 0.0}; }

json viewInfo(const std::string& view, int depth, bool all)
{
    const Graph& g = sceneGraphOf();
    const Edges e(g);
    const json layout = layoutData();
    const Groups groups(layout);
    const bool inGroup = !isView(view);
    const ViewNodes v = inGroup ? collectGroup(g, e, groups, view, all) : collect(g, e, view, depth, all);
    const bool hierarchy = view == "Hierarchy";
    const json stored = layout.value("positions", json::object()).value(view, json::object());
    const json flags = layout.value("flags", json::object());

    std::unordered_map<NodeId, size_t> index;
    std::vector<std::string> paths, fulls;
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        index[v.order[i]] = i;
        paths.push_back(g.path(v.order[i]));
        fulls.push_back(g.fullPath(v.order[i]));
    }

    json links = json::array();
    std::vector<std::pair<size_t, size_t>> flow;
    std::vector<std::set<std::string>> usedOutputs(v.order.size());
    auto link = [&](NodeId from, const std::string& fromSocket, NodeId to, const std::string& toSocket) {
        size_t a = index.at(from), b = index.at(to);
        links.push_back(json::array({paths[a], fromSocket, paths[b], toSocket}));
        flow.push_back({a, b});
        usedOutputs[a].insert(fromSocket);
    };
    for (NodeId id : v.order)
    {
        if (hierarchy)
            if (NodeId p = g.parent(id); p != kNoNode && v.members.contains(p))
                link(p, kChildrenSocket, id, kParentSocket);
        for (const auto& [dst, src] : e.of(id, true))
            if (std::optional<PlugRef> s = shownSource(g, e, v, src, all); s && s->node != id)
                link(s->node, socketId(g.attr(*s), s->component), id, socketId(g.attr(dst), dst.component));
    }

    // Depth along the links, for nodes that have no stored position. Bounded, so a loop can't spin.
    std::vector<int> rank(v.order.size(), 0);
    for (size_t pass = 0; pass < v.order.size(); ++pass)
    {
        bool moved = false;
        for (auto [a, b] : flow)
            if (rank[b] < rank[a] + 1)
            {
                rank[b] = rank[a] + 1;
                moved = true;
            }
        if (!moved)
            break;
    }

    std::vector<json> nodes, defNodes;
    std::map<int, double> columnY;
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        const std::string& full = fulls[i];
        const json f = flags.value(full, json::object());
        // A modifier's few parameters are what it is opened for; other nodes carry dozens of plugs.
        const bool hideUnused = f.value("hideUnused", !modifiers::asModifier(g.node(v.order[i]))), collapsed = f.value("collapsed", false);
        json n = nodeJson(g, e, v.order[i], hierarchy);
        json def = {{"name", paths[i]}, {"type", n["type"]}, {"hideUnused", hideUnused}};
        if (collapsed)
            def["collapsed"] = true;
        if (json p = stored.value(full, json()); p.is_array() && p.size() == 2)
            n["position"] = p;
        else
        {
            // The rows the canvas draws: with unused sockets hidden, the linked ones and the meshes, or every one on a node with neither.
            size_t rows = 0;
            if (!collapsed)
            {
                size_t trimmed = usedOutputs[i].size(), every = n["outputs"].size() + n["inputs"].size();
                bool anchored = !usedOutputs[i].empty();
                for (const json& s : n["inputs"])
                {
                    const bool mesh = s.value("type", "") == "mesh";
                    anchored |= s.value("linked", false) || mesh;
                    trimmed += s.value("linked", false) || mesh ? 1 : 0;
                }
                for (const json& s : n["outputs"])
                    if (s.value("type", "") == "mesh" && !usedOutputs[i].contains(s.value("id", "")))
                    {
                        anchored = true;
                        ++trimmed;
                    }
                rows = !hideUnused || !anchored ? every : trimmed;
            }
            double& y = columnY[rank[i]];
            n["position"] = json::array({double(rank[i]) * kColumnWidth, y});
            y += 30.0 + 20.0 * double(rows) + kRowGap;
        }
        nodes.push_back(std::move(n));
        defNodes.push_back(std::move(def));
    }

    // Members of a group nested in this context show as that group's node, its sockets named member.socket.
    std::vector<std::string> owner(v.order.size());
    std::unordered_map<std::string, size_t> byPath, byFull;
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        owner[i] = groups.ownerIn(view, fulls[i]);
        byPath[paths[i]] = i;
        byFull[fulls[i]] = i;
    }
    std::map<std::string, json> groupNodes;
    std::map<std::string, std::set<std::string>> groupSockets;
    auto shownEnd = [&](size_t i, const std::string& socket) -> std::pair<std::string, std::string> {
        if (owner[i].empty())
            return {paths[i], socket};
        return {groupNodeName(owner[i]), paths[i] + "." + socket};
    };
    auto addGroupSocket = [&](size_t i, const std::string& socket, bool output) {
        const std::string id = paths[i] + "." + socket;
        if (!groupSockets[owner[i]].insert((output ? "out:" : "in:") + id).second)
            return;
        json s = socketJson(id, "data", {});
        for (const json& m : nodes[i][output ? "outputs" : "inputs"])
            if (m.value("id", "") == socket)
            {
                s = socketJson(id, m.value("type", "data"), m.value("doc", ""));
                if (m.value("multi", false))
                    s["multi"] = true;
            }
        if (!output)
            s["linked"] = true;
        groupNodes[owner[i]][output ? "outputs" : "inputs"].push_back(std::move(s));
    };
    for (size_t i = 0; i < v.order.size(); ++i)
        if (!owner[i].empty() && !groupNodes.contains(owner[i]))
            groupNodes[owner[i]] = {{"name", groupNodeName(owner[i])},
                                    {"type", kGroupNodeType},
                                    {"label", owner[i]},
                                    {"category", "Group"},
                                    {"inputs", json::array()},
                                    {"outputs", json::array()},
                                    {"props", json::array({{{"id", "group"}, {"value", owner[i]}}})}};
    json shownLinks = json::array();
    std::vector<gn::GLink> glinks;
    for (const json& l : links)
    {
        const size_t a = byPath.at(l[0].get<std::string>()), b = byPath.at(l[2].get<std::string>());
        if (!owner[a].empty() && owner[a] == owner[b])
            continue;
        if (!owner[a].empty())
            addGroupSocket(a, l[1].get<std::string>(), true);
        if (!owner[b].empty())
            addGroupSocket(b, l[3].get<std::string>(), false);
        auto [fromNode, fromSocket] = shownEnd(a, l[1].get<std::string>());
        auto [toNode, toSocket] = shownEnd(b, l[3].get<std::string>());
        shownLinks.push_back(json::array({fromNode, fromSocket, toNode, toSocket}));
        glinks.push_back({fromNode, fromSocket, toNode, toSocket});
    }

    json outNodes = json::array(), outDefs = json::array();
    std::set<std::string> placed;
    const json records = layout.value("groups", json::object());
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        if (owner[i].empty())
        {
            outNodes.push_back(nodes[i]);
            outDefs.push_back(defNodes[i]);
            continue;
        }
        if (!placed.insert(owner[i]).second)
            continue;
        const std::string name = groupNodeName(owner[i]);
        json gn = std::move(groupNodes[owner[i]]);
        const json rec = records.is_object() ? records.value(owner[i], json::object()) : json::object();
        if (json p = stored.value(name, json()); isPosition(p))
            gn["position"] = p;
        else if (json q = rec.value("position", json()); isPosition(q))
            gn["position"] = q;
        else
        {
            Position sum{0.0, 0.0};
            int count = 0;
            for (size_t k = 0; k < v.order.size(); ++k)
                if (owner[k] == owner[i])
                {
                    const Position at = jsonPosition(nodes[k]["position"]);
                    sum.first += at.first;
                    sum.second += at.second;
                    ++count;
                }
            gn["position"] = json::array({sum.first / count, sum.second / count});
        }
        json def = {{"name", name}, {"type", kGroupNodeType}, {"values", {{"group", owner[i]}}}, {"hideUnused", false}};
        if (flags.value(name, json::object()).value("collapsed", false))
            def["collapsed"] = true;
        outNodes.push_back(std::move(gn));
        outDefs.push_back(std::move(def));
    }

    json frames = layout.value("frames", json::object()).value(view, json::array());
    if (frames.is_array())
        for (json& f : frames)
            if (f.is_object() && f.contains("nodes") && f["nodes"].is_array())
            {
                json members = json::array();
                for (const json& m : f["nodes"])
                {
                    if (!m.is_string())
                        continue;
                    json shown = m;
                    if (auto it = byFull.find(m.get<std::string>()); it != byFull.end())
                        shown = owner[it->second].empty() ? paths[it->second] : groupNodeName(owner[it->second]);
                    if (std::find(members.begin(), members.end(), shown) == members.end())
                        members.push_back(shown);
                }
                f["nodes"] = std::move(members);
            }

    std::vector<gn::GTie> ties = gn::tiesFromJson(layout.value("ties", json::object()).value(view, json::array()));
    for (gn::GTie& t : ties)
    {
        std::vector<gn::GLink> shown;
        for (const gn::GLink& l : t.links)
        {
            auto a = byFull.find(l.fromNode), b = byFull.find(l.toNode);
            if (a == byFull.end() || b == byFull.end())
                continue;
            auto [fromNode, fromSocket] = shownEnd(a->second, l.fromSocket);
            auto [toNode, toSocket] = shownEnd(b->second, l.toSocket);
            shown.push_back({fromNode, fromSocket, toNode, toSocket});
        }
        t.links = std::move(shown);
    }
    gn::pruneTies(ties, glinks);

    json out = {{"name", view}, {"view", view}, {"nodes", outNodes}};
    out["definition"] = {{"nodes", outDefs},
                         {"links", shownLinks},
                         {"frames", frames.is_array() ? frames : json::array()},
                         {"ties", gn::tiesToJson(ties)},
                         {"bookmarks", gn::cleanBookmarks(layout.value("bookmarks", json::object()).value(view, json::object()))}};
    if (!inGroup && g.selection().empty())
        out["error"] = "Nothing selected. Select objects in the viewport or the Outliner to show their nodes.";
    return out;
}

/** Input values worth carrying in copied text: stored, undriven, off their defaults and plain enough to read back. */
json copiedValues(const Graph& g, const Node& n)
{
    json out = json::object();
    const auto& attrs = n.type().attrs;
    for (int i = 0; i < int(attrs.size()); ++i)
    {
        const AttrDef& d = attrs[size_t(i)];
        if (!(d.flags & AttrInput) || !(d.flags & AttrStorable) || (d.flags & (AttrArray | AttrUnique | AttrUser)))
            continue;
        if (d.type == PlugType::Mesh || d.type == PlugType::Data || d.type == PlugType::Matrix || g.source({n.id(), i, -1}))
            continue;
        json v = toJson(n.value(i));
        if (v != toJson(d.defaultValue))
            out[d.name] = std::move(v);
    }
    return out;
}

/** The plug's name after its node's: "translateX", "inputs[2]". */
std::string plugPart(const Graph& g, const PlugRef& p) { return g.plugName(p).substr(g.path(p.node).size() + 1); }

/** The node at path, or null when there is none or the path is ambiguous. */
const Node* findQuiet(const Graph& g, const std::string& path)
{
    try
    {
        return g.find(path);
    }
    catch (const CommandError&)
    {
        return nullptr;
    }
}

json storedPosition(const json& layout, const std::string& context, const std::string& key)
{
    const json positions = layout.value("positions", json::object());
    const json byContext = positions.is_object() ? positions.value(context, json::object()) : json::object();
    return byContext.is_object() ? byContext.value(key, json()) : json();
}

std::map<std::string, Position> shownPositions(const std::string& view, int depth, bool all)
{
    std::map<std::string, Position> out;
    const json info = viewInfo(view, depth, all);
    for (const json& n : info["nodes"])
        out[n.value("name", "")] = jsonPosition(n.value("position", json()));
    return out;
}

struct Copied
{
    json clip;
    /** clip as text, the header first for whoever finds it pasted in a post. */
    std::string text;
    glm::dvec2 centre{0.0};
};

/**
 * Shown nodes as text, see kClipFormat: { "format", "version", "nodes": [{name, type, path, position, values, parent,
 * parentPath}], "links": [[fromNode, fromPlug, toNode, toPlug]], "groups": [{name, parent, members, position}], "ties",
 * "frames" }. A group node brings its members and nested groups; a member's position is on its group's canvas, any
 * other position relative to the copied nodes' centre.
 */
Copied copyNodes(const std::string& view, const std::vector<std::string>& names, int depth, bool all, const char* cmd)
{
    const Graph& g = sceneGraphOf();
    const json layout = layoutData();
    const Groups groups(layout);
    const std::map<std::string, Position> shownAt = shownPositions(view, depth, all);

    std::vector<NodeId> order;
    std::unordered_set<NodeId> copied, picked;
    std::vector<std::string> pickedGroups, allGroups;
    Copied out;
    int counted = 0;
    for (const std::string& name : names)
    {
        std::string shown = name;
        if (std::string group = groupOfName(name); !group.empty())
        {
            if (!groups.has(group))
                throw CommandError(std::string(cmd) + ": no node group '" + group + "'");
            pickedGroups.push_back(group);
        }
        else
        {
            const NodeId id = nodeArg(name, cmd).id();
            if (copied.insert(id).second)
                order.push_back(id);
            picked.insert(id);
            shown = g.path(id);
        }
        if (auto it = shownAt.find(shown); it != shownAt.end())
        {
            out.centre += glm::dvec2(it->second.first, it->second.second);
            ++counted;
        }
    }
    if (counted)
        out.centre /= double(counted);
    for (const std::string& group : pickedGroups)
        for (const std::string& sub : groups.subtree(group))
            if (std::find(allGroups.begin(), allGroups.end(), sub) == allGroups.end())
                allGroups.push_back(sub);
    for (const std::string& group : allGroups)
        for (const std::string& full : groups.members.at(group))
            if (const Node* n = findQuiet(g, full); n && copied.insert(n->id()).second)
                order.push_back(n->id());

    std::unordered_map<NodeId, std::string> clipName;
    std::map<std::string, std::string> renamed;
    std::set<std::string> fulls, framed;
    for (NodeId id : order)
    {
        clipName[id] = g.path(id);
        renamed[g.fullPath(id)] = clipName[id];
        fulls.insert(g.fullPath(id));
        if (picked.contains(id))
            framed.insert(g.fullPath(id));
    }
    for (const std::string& group : pickedGroups)
    {
        renamed[groupNodeName(group)] = groupNodeName(group);
        framed.insert(groupNodeName(group));
    }

    auto relative = [&](const std::string& shown) -> json {
        auto it = shownAt.find(shown);
        if (it == shownAt.end())
            return nullptr;
        return json::array({std::round(it->second.first - out.centre.x), std::round(it->second.second - out.centre.y)});
    };
    json nodes = json::array(), links = json::array(), records = json::array();
    for (NodeId id : order)
    {
        const Node& n = *g.node(id);
        const std::string full = g.fullPath(id);
        json o = {{"name", clipName[id]}, {"type", n.type().registered().name}, {"path", full}};
        if (picked.contains(id))
        {
            if (json p = relative(clipName[id]); !p.is_null())
                o["position"] = p;
        }
        else if (auto holder = groups.memberOf.find(full); holder != groups.memberOf.end())
            if (json p = storedPosition(layout, holder->second, full); isPosition(p))
                o["position"] = p;
        if (json v = copiedValues(g, n); !v.empty())
            o["values"] = std::move(v);
        if (NodeId p = g.parent(id); p != kNoNode)
        {
            if (copied.contains(p))
                o["parent"] = clipName[p];
            else
                o["parentPath"] = g.fullPath(p);
        }
        nodes.push_back(std::move(o));
    }
    for (const auto& [dst, src] : g.connections())
        if (copied.contains(dst.node) && copied.contains(src.node))
            links.push_back(json::array({clipName[src.node], plugPart(g, src), clipName[dst.node], plugPart(g, dst)}));
    for (const std::string& group : allGroups)
    {
        json rec = {{"name", group}};
        const std::string parent = groups.parent.at(group);
        const bool nested = std::find(allGroups.begin(), allGroups.end(), parent) != allGroups.end();
        if (nested)
            rec["parent"] = parent;
        json members = json::array();
        for (const std::string& full : groups.members.at(group))
            if (const Node* n = findQuiet(g, full); n && copied.contains(n->id()))
                members.push_back(clipName[n->id()]);
        rec["members"] = std::move(members);
        if (!nested)
        {
            if (json p = relative(groupNodeName(group)); !p.is_null())
                rec["position"] = p;
        }
        else if (json p = storedPosition(layout, parent, groupNodeName(group)); isPosition(p))
            rec["position"] = p;
        records.push_back(std::move(rec));
    }

    using ojson = nlohmann::ordered_json;
    ojson clip = {{"format", kClipFormat}, {"version", kClipVersion}, {"nodes", ojson::parse(nodes.dump())}, {"links", ojson::parse(links.dump())}};
    if (!records.empty())
        clip["groups"] = ojson::parse(records.dump());
    const json ties = layout.value("ties", json::object()).value(view, json::array());
    if (std::vector<gn::GTie> t = gn::tiesAmong(gn::tiesFromJson(ties), fulls, renamed, -out.centre); !t.empty())
    {
        for (gn::GTie& tie : t)
            tie.position = glm::round(tie.position);
        clip["ties"] = ojson::parse(gn::tiesToJson(t).dump());
    }
    const json frames = layout.value("frames", json::object()).value(view, json::array());
    if (std::vector<gn::GFrame> f = gn::framesAmong(gn::framesFromJson(frames), framed, renamed, -out.centre); !f.empty())
    {
        for (gn::GFrame& frame : f)
            frame.position = glm::round(frame.position);
        clip["frames"] = ojson::parse(gn::framesToJson(f).dump());
    }
    out.text = clip.dump(2);
    out.clip = json::parse(out.text);
    return out;
}

/** The JSON object in text: from the first '{' to the last '}', so prose and code fences around it don't matter. */
json clipJson(const std::string& text, const char* cmd)
{
    const size_t a = text.find('{'), b = text.rfind('}');
    if (a == std::string::npos || b == std::string::npos || b < a)
        throw CommandError(std::string(cmd) + ": can't paste: no node data in the text");
    try
    {
        return json::parse(text.substr(a, b - a + 1));
    }
    catch (const json::parse_error& e)
    {
        throw CommandError(std::string(cmd) + ": can't paste: the node data isn't valid JSON (" + e.what() + ")");
    }
}

int attrIndex(const NodeType& t, const std::string& name)
{
    for (int i = 0; i < int(t.attrs.size()); ++i)
        if (t.attrs[size_t(i)].name == name)
            return i;
    return -1;
}

/** Every problem with clip text as one message; empty when it pastes. */
std::string clipProblems(const json& clip)
{
    std::vector<std::string> errors;
    if (!clip.is_object() || clip.value("format", json()) != kClipFormat)
        return "not Lila scene nodes (no \"format\": \"" + std::string(kClipFormat) + "\" header)";
    if (!clip.contains("version") || !clip["version"].is_number_integer())
        return "the version number is missing";
    if (int v = clip["version"].get<int>(); v > kClipVersion)
        return "made by a newer Lila (version " + std::to_string(v) + ", this one reads up to " + std::to_string(kClipVersion) + ")";
    if (!clip.contains("nodes") || !clip["nodes"].is_array())
        return "\"nodes\" must be a list";
    std::set<std::string> names;
    for (const json& n : clip["nodes"])
    {
        if (!n.is_object() || !n.value("name", json()).is_string() || !n.value("type", json()).is_string())
        {
            errors.push_back("a node has no name or type");
            continue;
        }
        const std::string name = n["name"].get<std::string>(), type = n["type"].get<std::string>();
        if (!names.insert(name).second)
            errors.push_back("two nodes are named '" + name + "'");
        const NodeType* t = types::find(type);
        if (!t || !t->create)
        {
            errors.push_back("node '" + name + "': unknown type '" + type + "'");
            continue;
        }
        if (n.contains("position") && !isPosition(n["position"]))
            errors.push_back("node '" + name + "': position must be [x, y]");
        const json values = n.value("values", json::object());
        for (const auto& [attr, v] : values.items())
        {
            const int i = attrIndex(*t, attr);
            if (i < 0)
            {
                errors.push_back("node '" + name + "': " + type + " has no attribute '" + attr + "'");
                continue;
            }
            try
            {
                fromJson(t->attrs[size_t(i)], v);
            }
            catch (const std::exception& e)
            {
                errors.push_back("node '" + name + "'." + attr + ": " + e.what());
            }
        }
    }
    for (const json& n : clip["nodes"])
        if (n.is_object() && n.contains("parent") && (!n["parent"].is_string() || !names.contains(n["parent"].get<std::string>())))
            errors.push_back("node '" + n.value("name", std::string()) + "': its parent isn't among the pasted nodes");
    for (const json& l : clip.value("links", json::array()))
        if (!l.is_array() || l.size() != 4 || !std::all_of(l.begin(), l.end(), [](const json& x) { return x.is_string(); }) ||
            !names.contains(l[0].get<std::string>()) || !names.contains(l[2].get<std::string>()))
            errors.push_back("a link " + l.dump() + " doesn't join two pasted nodes");
    std::set<std::string> groups;
    for (const json& r : clip.value("groups", json::array()))
        if (r.is_object() && r.value("name", json()).is_string())
            groups.insert(r["name"].get<std::string>());
    for (const json& r : clip.value("groups", json::array()))
    {
        if (!r.is_object() || !r.value("name", json()).is_string() || !Graph::validName(r["name"].get<std::string>()))
        {
            errors.push_back("a group has no usable name");
            continue;
        }
        if (r.contains("parent") && (!r["parent"].is_string() || !groups.contains(r["parent"].get<std::string>())))
            errors.push_back("group '" + r["name"].get<std::string>() + "': its parent isn't among the pasted groups");
        for (const json& m : r.value("members", json::array()))
            if (!m.is_string() || !names.contains(m.get<std::string>()))
                errors.push_back("group '" + r["name"].get<std::string>() + "': member " + m.dump() + " isn't among the pasted nodes");
    }
    std::string out;
    for (const std::string& e : errors)
        out += (out.empty() ? "" : "; ") + e;
    return out;
}

struct Pasted
{
    /** The pasted nodes as the canvas of the view shows them: scene paths and group nodes. */
    std::vector<std::string> shown;
    /** Copied full paths and "@group" names to their copies'. */
    std::map<std::string, std::string> renamed;
};

/** Creates the nodes and groups of clip text centred at `at`; frames in the text are added when withFrames. */
Pasted pasteNodes(const std::string& view, const json& clip, glm::dvec2 at, bool withFrames, const char* cmd)
{
    if (std::string problems = clipProblems(clip); !problems.empty())
        throw CommandError(std::string(cmd) + ": can't paste: " + problems);
    Graph& g = sceneGraphOf();
    const bool inGroup = !isView(view);
    Pasted out;
    const size_t mark = UndoStack::get().openEdits();
    try
    {
        json layout = layoutData();
        std::map<std::string, NodeId> made;
        std::unordered_set<NodeId> fresh;
        for (const json& n : clip["nodes"])
        {
            const std::string name = n["name"].get<std::string>(), type = n["type"].get<std::string>();
            const NodeType& t = *types::find(type);
            NodeId parent = kNoNode;
            if (!n.contains("parent") && t.dag)
                if (json p = n.value("parentPath", json()); p.is_string())
                    if (const Node* under = findQuiet(g, p.get<std::string>()); under && under->type().dag)
                        parent = under->id();
            Node* created = rethrown([&] { return g.createNode(t, name.substr(name.rfind('|') + 1), parent); });
            fresh.insert(created->id());
            // The copied node, when it is still in the scene, gives what text can't carry: its mesh, its locks.
            if (json src = n.value("path", json()); src.is_string())
                if (const Node* from = findQuiet(g, src.get<std::string>()); from && !fresh.contains(from->id()) && from->type().registered().name == type)
                    duplicate::copyValues(g, *from, *created);
            const json values = n.value("values", json::object());
            for (const auto& [attr, v] : values.items())
            {
                const int i = attrIndex(created->type(), attr);
                if (i >= 0)
                    rethrown([&] {
                        g.setValue({created->id(), i, -1}, fromJson(created->type().attrs[size_t(i)], v));
                        return 0;
                    });
            }
            made[name] = created->id();
            if (json src = n.value("path", json()); src.is_string())
                out.renamed[src.get<std::string>()] = g.fullPath(created->id());
        }
        for (const json& n : clip["nodes"])
            if (n.contains("parent"))
                rethrown([&] {
                    g.setParent(made.at(n["name"].get<std::string>()), made.at(n["parent"].get<std::string>()), false);
                    return 0;
                });
        for (const json& l : clip.value("links", json::array()))
            rethrown([&] {
                PlugRef s = g.plug(g.path(made.at(l[0].get<std::string>())) + "." + l[1].get<std::string>());
                PlugRef d = g.plug(g.path(made.at(l[2].get<std::string>())) + "." + l[3].get<std::string>());
                g.connect(s, d, true);
                return 0;
            });

        std::map<std::string, std::string> clipRenamed;
        for (const auto& [name, id] : made)
            clipRenamed[name] = g.fullPath(id);
        std::map<std::string, std::string> groupOf, groupRenamed;
        json& records = objectAt(layout, "groups");
        for (const json& r : clip.value("groups", json::array()))
        {
            const std::string name = r["name"].get<std::string>(), newName = uniqueGroupName(layout, name, cmd);
            groupRenamed[name] = newName;
            records[newName] = {{"members", json::array()}, {"parent", ""}};
            clipRenamed[groupNodeName(name)] = groupNodeName(newName);
            out.renamed[groupNodeName(name)] = groupNodeName(newName);
            for (const json& m : r.value("members", json::array()))
                groupOf[m.get<std::string>()] = newName;
        }
        json& positions = objectAt(layout, "positions");
        for (const json& r : clip.value("groups", json::array()))
        {
            const std::string newName = groupRenamed.at(r["name"].get<std::string>());
            const bool nested = r.contains("parent");
            const std::string parent = nested ? groupRenamed.at(r["parent"].get<std::string>()) : inGroup ? view : std::string();
            records[newName]["parent"] = parent;
            json p = r.value("position", json());
            if (isPosition(p) && !nested)
                p = json::array({p[0].get<double>() + at.x, p[1].get<double>() + at.y});
            if (isPosition(p))
            {
                objectAt(positions, nested ? parent : view)[groupNodeName(newName)] = p;
                if (!nested)
                    records[newName]["position"] = p;
            }
            if (!nested)
                out.shown.push_back(groupNodeName(newName));
        }
        for (const json& n : clip["nodes"])
        {
            const std::string name = n["name"].get<std::string>();
            const NodeId id = made.at(name);
            const std::string full = g.fullPath(id);
            json p = n.value("position", json());
            if (auto it = groupOf.find(name); it != groupOf.end())
            {
                joinGroup(layout, it->second, full);
                if (isPosition(p))
                    objectAt(positions, it->second)[full] = p;
                continue;
            }
            if (inGroup)
                joinGroup(layout, view, full);
            if (isPosition(p))
                objectAt(positions, view)[full] = json::array({p[0].get<double>() + at.x, p[1].get<double>() + at.y});
            out.shown.push_back(g.path(id));
        }

        auto renameAll = [&](std::vector<std::string>& names) {
            std::vector<std::string> next;
            for (const std::string& s : names)
                if (auto it = clipRenamed.find(s); it != clipRenamed.end())
                    next.push_back(it->second);
            names = std::move(next);
        };
        std::vector<gn::GTie> ties = gn::tiesFromJson(objectAt(layout, "ties").value(view, json::array()));
        for (gn::GTie& t : gn::tiesFromJson(clip.value("ties", json::array())))
        {
            std::vector<gn::GLink> links;
            for (const gn::GLink& l : t.links)
                if (clipRenamed.contains(l.fromNode) && clipRenamed.contains(l.toNode))
                    links.push_back({clipRenamed[l.fromNode], l.fromSocket, clipRenamed[l.toNode], l.toSocket});
            if (links.size() < 2)
                continue;
            t.links = std::move(links);
            t.position += at;
            ties.push_back(std::move(t));
        }
        objectAt(layout, "ties")[view] = gn::tiesToJson(ties);
        if (withFrames)
        {
            std::vector<gn::GFrame> frames = gn::framesFromJson(objectAt(layout, "frames").value(view, json::array()));
            for (gn::GFrame& f : gn::framesFromJson(clip.value("frames", json::array())))
            {
                renameAll(f.nodes);
                f.position += at;
                frames.push_back(std::move(f));
            }
            objectAt(layout, "frames")[view] = gn::framesToJson(frames);
        }
        data::set(kLayoutKey, layout);
        if (!inGroup)
        {
            std::vector<NodeId> ids;
            for (const auto& [name, id] : made)
                ids.push_back(id);
            g.select(ids, SelectMode::Add);
        }
    }
    catch (...)
    {
        UndoStack::get().rollbackTo(mark);
        throw;
    }
    return out;
}

std::vector<std::string> nodeNames(const Args& a, const char* cmd)
{
    std::vector<std::string> names;
    for (size_t i = 1; i < a.positional.size(); ++i)
        names.push_back(stringArg(a, i, cmd, "node"));
    if (names.empty())
        throw CommandError(std::string(cmd) + ": no nodes given");
    return names;
}

/** Collects nodes into a new node group, see sceneGraphGroupNodes. */
json collectIntoGroup(const std::string& view, const std::vector<std::string>& names, const std::string& base, int depth, bool all, const char* cmd)
{
    json layout = layoutData();
    const Groups groups(layout);
    const std::string name = uniqueGroupName(layout, base, cmd);
    const std::vector<std::string> outer = isView(view) ? std::vector<std::string>{} : groups.chain(view);
    const std::map<std::string, Position> shownAt = shownPositions(view, depth, all);
    objectAt(layout, "groups")[name] = {{"members", json::array()}, {"parent", isView(view) ? std::string() : view}};
    json& inner = objectAt(objectAt(layout, "positions"), name);
    glm::dvec2 centre(0.0);
    int counted = 0;
    for (const std::string& shown : names)
    {
        const std::string member = memberArg(layout, shown, cmd);
        std::string key = shown;
        if (std::string sub = groupOfName(member); !sub.empty())
        {
            if (std::find(outer.begin(), outer.end(), sub) != outer.end())
                throw CommandError(std::string(cmd) + ": " + sub + " holds this group; it can't go inside it");
            objectAt(layout, "groups")[sub]["parent"] = name;
        }
        else
        {
            joinGroup(layout, name, member);
            key = sceneGraphOf().path(sceneGraphOf().find(member)->id());
        }
        if (auto it = shownAt.find(key); it != shownAt.end())
        {
            inner[member] = json::array({it->second.first, it->second.second});
            centre += glm::dvec2(it->second.first, it->second.second);
            ++counted;
        }
    }
    if (counted)
        centre /= double(counted);
    const json at = json::array({std::round(centre.x), std::round(centre.y)});
    objectAt(layout, "groups")[name]["position"] = at;
    objectAt(objectAt(layout, "positions"), view)[groupNodeName(name)] = at;
    data::set(kLayoutKey, layout);
    return {{"group", name}, {"node", groupNodeName(name)}};
}

/** Takes a node group apart, see sceneGraphUngroup. */
json ungroup(const std::string& view, const std::string& node, const char* cmd)
{
    const std::string group = groupOfName(node);
    json layout = layoutData();
    const Groups groups(layout);
    if (group.empty() || !groups.has(group))
        throw CommandError(std::string(cmd) + ": '" + node + "' isn't a node group's node");
    const Graph& g = sceneGraphOf();
    const std::string parent = groups.parent.at(group);
    std::vector<std::pair<std::string, std::string>> items; // (layout key, shown name)
    for (const std::string& full : groups.members.at(group))
    {
        if (parent.empty())
            leaveGroups(layout, full);
        else
            joinGroup(layout, parent, full);
        if (const Node* n = findQuiet(g, full))
            items.push_back({full, g.path(n->id())});
    }
    for (const auto& [sub, p] : groups.parent)
        if (p == group)
        {
            objectAt(layout, "groups")[sub]["parent"] = parent;
            items.push_back({groupNodeName(sub), groupNodeName(sub)});
        }

    // Members without a place here keep their layout inside the group, centred where its node was.
    json groupAt = storedPosition(layout, view, node);
    if (!isPosition(groupAt))
        groupAt = layout["groups"][group].value("position", json());
    json& here = objectAt(objectAt(layout, "positions"), view);
    std::vector<std::pair<std::string, Position>> moving;
    glm::dvec2 centre(0.0);
    for (const auto& [key, shown] : items)
        if (json p = storedPosition(layout, group, key); isPosition(p) && !here.contains(key))
        {
            moving.push_back({key, jsonPosition(p)});
            centre += glm::dvec2(jsonPosition(p).first, jsonPosition(p).second);
        }
    if (!moving.empty() && isPosition(groupAt))
    {
        centre /= double(moving.size());
        const Position target = jsonPosition(groupAt);
        for (const auto& [key, p] : moving)
            here[key] = json::array({p.first - centre.x + target.first, p.second - centre.y + target.second});
    }
    forgetGroup(layout, group);
    data::set(kLayoutKey, layout);
    json shown = json::array();
    for (const auto& [key, name] : items)
        shown.push_back(name);
    return shown;
}

/** The addModifier kind a node type is ("bevel" for bevelModifier); empty for anything else. */
std::string modifierKind(const std::string& type)
{
    constexpr std::string_view kSuffix = "Modifier";
    if (!type.ends_with(kSuffix))
        return {};
    std::string kind = type.substr(0, type.size() - kSuffix.size());
    std::vector<std::string> all = modifiers::kinds();
    return std::find(all.begin(), all.end(), kind) != all.end() ? kind : std::string();
}

/** The mesh the newest selected mesh or modifier stands for, kNoNode when the selection holds neither. */
NodeId selectedMesh(Graph& g)
{
    const std::vector<NodeId> sel = g.selection();
    for (auto it = sel.rbegin(); it != sel.rend(); ++it)
    {
        Node* n = g.node(*it);
        if (n && n->type().isA("mesh"))
            return *it;
        if (modifiers::asModifier(n))
            if (NodeId mesh = modifiers::meshOf(g, *it); mesh != kNoNode)
                return mesh;
    }
    return kNoNode;
}

/** The socket a new node of type t would link a dragged wire of linkType through; empty when none fits. */
std::string linkSocket(const NodeType& t, const std::string& linkType, bool needOutput)
{
    std::string component;
    for (const AttrDef& d : t.attrs)
    {
        if (!(needOutput ? offeredAsSource(d) : offeredAsInput(d)))
            continue;
        const std::string type = socketType(d, -1);
        if (needOutput ? linkable(type, linkType) : linkable(linkType, type))
            return d.name;
        if (component.empty() && hasComponents(d) && (d.flags & AttrKeyable) && (needOutput ? linkable("float", linkType) : linkable(linkType, "float")))
            component = componentName(d, 0);
    }
    return component;
}

json nodeTypes(const Args& a)
{
    std::optional<std::string> link;
    if (a.has("linkType"))
        link = a.get<std::string>("linkType");
    const bool needOutput = a.get<bool>("linkInput", false);
    json out = json::array();
    std::set<std::string> listed;
    auto offer = [&](const NodeType& t, const std::string& label, const std::string& category, const std::string& doc) {
        if (!t.create || !listed.insert(t.name).second)
            return;
        json e = {{"type", t.name}, {"label", label}, {"category", category}, {"doc", doc}};
        if (link)
        {
            std::string socket = linkSocket(t, *link, needOutput);
            if (socket.empty())
                return;
            e["socket"] = socket;
        }
        out.push_back(std::move(e));
    };
    for (const rigmath::Function& f : rigmath::functions())
        for (const rigmath::Overload& o : f.overloads)
            if (const NodeType* t = types::find(o.nodeType))
                offer(*t, o.nodeType == f.name ? f.name : f.name + " (" + o.nodeType.substr(f.name.size()) + ")", "Math / " + f.family, f.doc);
    for (const NodeType* t : types::all())
        if (!t->dag)
            offer(*t, t->name, "Utility", t->doc);
    return out;
}
} // namespace

bool isView(std::string_view name)
{
    return std::any_of(kViews.begin(), kViews.end(), [&](const char* v) { return name == v; });
}

bool hasGroup(const std::string& name) { return !name.empty() && Groups(layoutData()).has(name); }

std::string parentGroup(const std::string& name)
{
    const Groups groups(layoutData());
    auto it = groups.parent.find(name);
    return it == groups.parent.end() || !groups.has(it->second) ? std::string() : it->second;
}

bool linkable(std::string_view from, std::string_view to)
{
    if (from.ends_with("[]"))
        return false;
    if (to.ends_with("[]"))
        to.remove_suffix(2);
    auto numeric = [](std::string_view t) { return t == "float" || t == "int" || t == "bool"; };
    auto vector = [](std::string_view t) { return t == "vector" || t == "color"; };
    return from == to || (numeric(from) && numeric(to)) || (vector(from) && vector(to));
}

void registerCommands()
{
    ops::registerCommand({
        .name = "sceneGraphInfo",
        .doc = "A view of the scene's nodes for the node editor: the nodes with their plugs as sockets and values, the connections "
               "between them as links, and each node's canvas position.\n\n"
               "Views: Selection (the selection and what is wired straight into and out of it), History (each selected mesh's whole "
               "modifier stack and its source), Hierarchy (the selection's parents "
               "and children), Upstream and Downstream (depth steps from the selection). Views look through anim curves and unit "
               "conversions unless all is on. A node group shows as one node wherever its members show; given a group's name instead "
               "of a view, the group's members with the nodes wired to them.",
        .positionalDoc = "view",
        .flags = {{"depth", "", T::Int, 2, "Steps Upstream and Downstream follow from the selection.", {}, {}, 1.0},
                  {"all", "", T::Bool, false, "Also show anim curves and unit conversions."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            std::string view = viewArg(a, "sceneGraphInfo");
            return viewInfo(view, std::max(1, a.get<int>("depth", 2)), a.get<bool>("all", false));
        },
        .examples = {"sceneGraphInfo(\"Upstream\", depth=3)"},
    });

    ops::registerCommand({
        .name = "sceneGraphNodeTypes",
        .doc = "Node types the node editor's Scene backend adds: the rig math nodes first, then the other utility (non-DAG) types. With "
               "linkType, only types that can take a link from (or, with linkInput, give one to) a socket of that type, each with the "
               "socket it would use.",
        .flags = {{"linkType", "", T::String, nullptr, "Socket type of a dragged wire's loose end."},
                  {"linkInput", "", T::Bool, false, "The drag started at an input, so the node needs a matching output."}},
        .undoable = false,
        .run = [](const Args& a) -> json { return nodeTypes(a); },
    });

    ops::registerCommand({
        .name = "sceneGraphAddNode",
        .doc = "Create a scene node from the node editor and add it to the selection, so the view shows it (in a node group, to the "
               "group). Returns its path. A "
               "modifier added with a mesh (or one of its modifiers) selected goes on top of that mesh's stack, as addModifier puts it, "
               "and is laid out in the stack's columns rather than at position.",
        .positionalDoc = "view type",
        .flags = {{"position", "", T::List, nullptr, "[x, y] on the canvas."},
                  {"name", "", T::String, nullptr, "Wanted name; made unique."},
                  {"insert", "", T::Bool, true, "Put a modifier into the selected mesh's stack; off leaves it unlinked."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphAddNode";
            std::string view = viewArg(a, cmd);
            std::string typeName = stringArg(a, 1, cmd, "type");
            const NodeType* t = types::find(typeName);
            if (!t)
                throw CommandError(std::string(cmd) + ": unknown node type '" + typeName + "'");
            Graph& g = sceneGraphOf();
            Node* n = nullptr;
            bool stacked = false;
            if (std::string kind = modifierKind(typeName); !kind.empty() && a.get<bool>("insert", true))
                if (NodeId mesh = selectedMesh(g); mesh != kNoNode)
                {
                    json made = ops::execute("addModifier", {{"args", {g.path(mesh)}}, {"kind", kind}});
                    n = made.is_string() ? g.find(made.get<std::string>()) : nullptr;
                    stacked = n != nullptr;
                    if (n && a.has("name"))
                        rethrown([&] {
                            g.rename(n->id(), a.get<std::string>("name"));
                            return 0;
                        });
                }
            if (!n)
                n = rethrown([&] { return g.createNode(*t, a.get<std::string>("name", "")); });
            if (a.has("position") && !stacked)
                storePositions(view, {std::pair{n->id(), positionArg(a.flags.at("position"), cmd)}});
            if (isView(view))
                g.select({n->id()}, SelectMode::Add);
            else
            {
                json layout = layoutData();
                joinGroup(layout, view, g.fullPath(n->id()));
                data::set(kLayoutKey, layout);
            }
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "sceneGraphDeleteNode",
        .doc = "Delete scene nodes from the node editor, with their connections (and, for objects, their children); a node group's "
               "node deletes the group with every node in it.",
        .positionalDoc = "view nodes...",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphDeleteNode";
            viewArg(a, cmd);
            Graph& g = sceneGraphOf();
            json layout = layoutData();
            const Groups groups(layout);
            std::vector<NodeId> ids;
            bool forgot = false;
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                const std::string name = stringArg(a, i, cmd, "node");
                if (std::string group = groupOfName(memberArg(layout, name, cmd)); !group.empty())
                {
                    size_t members = 0;
                    for (const std::string& full : groups.allMembers(group))
                        if (const Node* n = findQuiet(g, full))
                        {
                            ids.push_back(n->id());
                            ++members;
                        }
                    // One canvas node removes several real ones, so say how many went.
                    log::info("Deleted group {} and its {} scene node{}", group, members, members == 1 ? "" : "s");
                    for (const std::string& sub : groups.subtree(group))
                        forgetGroup(layout, sub);
                    forgot = true;
                }
                else
                    ids.push_back(nodeArg(name, cmd).id());
            }
            if (forgot)
                data::set(kLayoutKey, layout);
            for (NodeId id : ids)
                if (g.node(id))
                    rethrown([&] {
                        g.deleteNode(id);
                        return 0;
                    });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphConnect",
        .doc = "Connect plugs from the node editor: sceneGraphConnect(view, 'Ctrl.translateX', 'Box.rotateY') replaces the input's "
               "connection. In the Hierarchy view, 'A.@children' to 'B.@parent' parents B under A. A node group's socket "
               "'@Arm.L_hand.rotateX' is its member's plug.",
        .positionalDoc = "view from to",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphConnect";
            viewArg(a, cmd);
            std::string from = memberPlug(stringArg(a, 1, cmd, "from"), cmd), to = memberPlug(stringArg(a, 2, cmd, "to"), cmd);
            auto [fromNode, fromSocket] = splitSocket(from, cmd);
            auto [toNode, toSocket] = splitSocket(to, cmd);
            Graph& g = sceneGraphOf();
            if (toSocket == kParentSocket || fromSocket == kChildrenSocket)
            {
                if (toSocket != kParentSocket || fromSocket != kChildrenSocket)
                    throw CommandError(std::string(cmd) + ": Children wires only into Parent");
                NodeId child = nodeArg(toNode, cmd).id();
                NodeId parent = nodeArg(fromNode, cmd).id();
                rethrown([&] {
                    g.setParent(child, parent);
                    return 0;
                });
                return nullptr;
            }
            PlugRef s = plugArg(from), d = plugArg(to);
            return g.plugName(rethrown([&] { return g.connect(s, d, true); }));
        },
    });

    ops::registerCommand({
        .name = "sceneGraphDisconnect",
        .doc = "Break the connection into an input from the node editor; on an array input, every element's, or only the ones `source` "
               "drives. In the Hierarchy view, 'B.@parent' unparents B. Returns how many connections went.",
        .positionalDoc = "view to",
        .flags = {{"source", "", T::String, nullptr, "node.socket of the one link to remove."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphDisconnect";
            viewArg(a, cmd);
            std::string to = memberPlug(stringArg(a, 1, cmd, "to"), cmd);
            auto [toNode, toSocket] = splitSocket(to, cmd);
            Graph& g = sceneGraphOf();
            if (toSocket == kParentSocket)
            {
                NodeId child = nodeArg(toNode, cmd).id();
                if (g.parent(child) == kNoNode)
                    return 0;
                rethrown([&] {
                    g.setParent(child, kNoNode);
                    return 0;
                });
                return 1;
            }
            PlugRef d = plugArg(to);
            std::optional<PlugRef> from;
            if (a.has("source"))
                from = plugArg(memberPlug(a.get<std::string>("source"), cmd));
            int removed = 0;
            if ((g.attr(d).flags & AttrArray) && d.element < 0)
            {
                for (const PlugRef& el : g.elements(d))
                    if (std::optional<PlugRef> s = g.source(el); s && (!from || *s == *from))
                    {
                        g.disconnect(el);
                        ++removed;
                    }
                return removed;
            }
            // A wire drawn through a skipped conversion names the conversion's source, so any link into d goes.
            if (g.source(d))
            {
                g.disconnect(d);
                ++removed;
            }
            return removed;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphInsertNode",
        .doc = "Drop an unlinked modifier into a link of a mesh's stack from the node editor: sceneGraphInsertNode(view, 'bevel1', "
               "'polyCube1.outMesh', 'Cube.inMesh') wires it in as addModifier does. Returns False, changing nothing, for any other "
               "node or link; the editor then wires the node through its matching sockets.",
        .positionalDoc = "view node from to",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphInsertNode";
            viewArg(a, cmd);
            Graph& g = sceneGraphOf();
            Node& node = nodeArg(stringArg(a, 1, cmd, "node"), cmd);
            const ModifierNode* mod = modifiers::asModifier(&node);
            if (!mod || g.source({node.id(), mod->inAttr, -1}) || !g.destinations({node.id(), mod->outAttr, -1}).empty())
                return false;
            PlugRef from = plugArg(memberPlug(stringArg(a, 2, cmd, "from"), cmd)), into = plugArg(memberPlug(stringArg(a, 3, cmd, "to"), cmd));
            if (g.source(into) != std::optional<PlugRef>(from))
                return false;
            const Node* target = g.node(into.node);
            NodeId mesh = kNoNode;
            if (target && target->type().isA("mesh") && into.attr == MeshNode::kInMesh)
                mesh = into.node;
            else if (const ModifierNode* below = modifiers::asModifier(target); below && into.attr == below->inAttr)
                mesh = modifiers::meshOf(g, into.node);
            if (mesh == kNoNode)
                return false;
            rethrown([&] {
                modifiers::insert(g, node, mesh, into);
                return 0;
            });
            return true;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphMoveNodes",
        .doc = "Move nodes on the node editor's canvas: sceneGraphMoveNodes(view, positions={'Box': [0, 100]}). Kept per view with the "
               "scene.",
        .positionalDoc = "view",
        .flags = {{"positions", "", T::Any, nullptr, "{node: [x, y]}"}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphMoveNodes";
            std::string view = viewArg(a, cmd);
            if (!a.has("positions"))
                return nullptr;
            const json& positions = a.flags.at("positions");
            if (!positions.is_object())
                throw CommandError(std::string(cmd) + ": positions is {node: [x, y]}");
            json layout = layoutData();
            std::vector<std::pair<std::string, Position>> moves;
            for (const auto& [name, p] : positions.items())
                moves.push_back({memberArg(layout, name, cmd), positionArg(p, cmd)});
            if (moves.empty())
                return nullptr;
            json& here = objectAt(objectAt(layout, "positions"), view);
            for (const auto& [key, p] : moves)
                here[key] = json::array({p.first, p.second});
            data::set(kLayoutKey, layout);
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphSetValue",
        .doc = "Set an unconnected plug from the node editor: sceneGraphSetValue(view, 'Box.translateX', 2).",
        .positionalDoc = "view node.attr value",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetValue";
            viewArg(a, cmd);
            std::string path = stringArg(a, 1, cmd, "node.attr");
            if (path.starts_with('@') && path.ends_with(".group") && std::count(path.begin(), path.end(), '.') == 1)
                throw CommandError(std::string(cmd) + ": a node group's node always shows its own group");
            path = memberPlug(path, cmd);
            if (a.positional.size() < 3)
                throw CommandError(std::string(cmd) + ": value required");
            Graph& g = sceneGraphOf();
            PlugRef p = plugArg(path);
            AttrDef def = g.attr(p);
            if ((def.flags & AttrArray) && p.element < 0)
                throw CommandError(std::string(cmd) + ": " + g.plugName(p) + " is an array; set it with setAttr");
            if (p.component >= 0)
                def.type = PlugType::Float;
            rethrown([&] {
                g.setValue(p, fromJson(def, a.positional[2]));
                return 0;
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphSetFrames",
        .doc = "Replace a view's frames and notes, shaped as gnSetFrames takes them: sceneGraphSetFrames(view, [{label: 'Arm', position: "
               "[x, y], size: [w, h], nodes: ['L_arm'], text: ''}]). Members that name no node are dropped. Kept with the scene.",
        .positionalDoc = "view frames",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetFrames";
            std::string view = viewArg(a, cmd);
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError(std::string(cmd) + ": frames must be a list");
            std::vector<gn::GFrame> frames = gn::framesFromJson(a.positional[1]);
            const Graph& g = sceneGraphOf();
            json layout = layoutData();
            const Groups groups(layout);
            for (gn::GFrame& f : frames)
            {
                std::vector<std::string> members;
                for (const std::string& m : f.nodes)
                    if (groups.has(groupOfName(m)))
                        members.push_back(m);
                    else if (const Node* n = findQuiet(g, m))
                        members.push_back(g.fullPath(n->id()));
                f.nodes = std::move(members);
            }
            layout["frames"][view] = gn::framesToJson(frames);
            data::set(kLayoutKey, layout);
            return int(frames.size());
        },
    });

    ops::registerCommand({
        .name = "sceneGraphSetTies",
        .doc = "Replace a view's zip ties, shaped as gnSetTies takes them: sceneGraphSetTies(view, [{links: [[fromNode, fromSocket, toNode, "
               "toSocket], ...], position: [x, y], angle: 0, color: ''}]). Links naming no node are dropped, and so is a tie left with "
               "fewer than two. Kept with the scene.",
        .positionalDoc = "view ties",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetTies";
            std::string view = viewArg(a, cmd);
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError(std::string(cmd) + ": ties must be a list");
            std::vector<gn::GTie> ties = gn::tiesFromJson(a.positional[1]);
            for (gn::GTie& t : ties)
            {
                std::vector<gn::GLink> links;
                for (const gn::GLink& l : t.links)
                {
                    auto from = rawEnd(l.fromNode, l.fromSocket), to = rawEnd(l.toNode, l.toSocket);
                    if (from && to)
                        links.push_back({from->first, from->second, to->first, to->second});
                }
                t.links = std::move(links);
            }
            std::erase_if(ties, [](const gn::GTie& t) { return t.links.size() < 2; });
            json layout = layoutData();
            objectAt(layout, "ties")[view] = gn::tiesToJson(ties);
            data::set(kLayoutKey, layout);
            return int(ties.size());
        },
    });

    ops::registerCommand({
        .name = "sceneGraphSetBookmarks",
        .doc = "Replace a view's saved canvas views: sceneGraphSetBookmarks(view, {'1': [x, y, zoom]}), centre and zoom by slot. Kept with "
               "the scene.",
        .positionalDoc = "view bookmarks",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetBookmarks";
            std::string view = viewArg(a, cmd);
            if (a.positional.size() < 2 || !a.positional[1].is_object())
                throw CommandError(std::string(cmd) + ": bookmarks must be an object of slot: [x, y, zoom]");
            json marks = gn::cleanBookmarks(a.positional[1]);
            json layout = layoutData();
            objectAt(layout, "bookmarks")[view] = marks;
            data::set(kLayoutKey, layout);
            return int(marks.size());
        },
    });

    ops::registerCommand({
        .name = "sceneGraphCopyNodes",
        .doc = "Shown nodes as shareable text (JSON with a format header): their types, values, parents, the connections among them "
               "and the node groups they stand for. Paste it with sceneGraphPasteNodes, here or in another scene.",
        .positionalDoc = "view nodes...",
        .flags = {{"depth", "", T::Int, 2, "As sceneGraphInfo takes it, for where the nodes show.", {}, {}, 1.0},
                  {"all", "", T::Bool, false, "As sceneGraphInfo takes it."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphCopyNodes";
            std::string view = viewArg(a, cmd);
            return copyNodes(view, nodeNames(a, cmd), std::max(1, a.get<int>("depth", 2)), a.get<bool>("all", false), cmd).text;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphPasteNodes",
        .doc = "Create scene nodes from text made by sceneGraphCopyNodes, centred at position, with their connections and node groups; "
               "names are made unique. Returns the new nodes as the view shows them. Bad text fails with every problem listed and "
               "changes nothing.",
        .positionalDoc = "view text",
        .flags = {{"position", "", T::List, json::array({0, 0}), "[x, y] canvas centre for the pasted nodes."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphPasteNodes";
            std::string view = viewArg(a, cmd);
            const json clip = clipJson(stringArg(a, 1, cmd, "text"), cmd);
            const Position at = a.has("position") ? positionArg(a.flags.at("position"), cmd) : Position{0.0, 0.0};
            return pasteNodes(view, clip, {at.first, at.second}, true, cmd).shown;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphDuplicateNodes",
        .doc = "Copy shown nodes in place with the connections among them; copies join their originals' frames. Returns the new nodes as "
               "the view shows them.",
        .positionalDoc = "view nodes...",
        .flags = {{"offset", "", T::List, json::array({40, 40}), "[x, y] shift for the copies."},
                  {"depth", "", T::Int, 2, "As sceneGraphInfo takes it, for where the nodes show.", {}, {}, 1.0},
                  {"all", "", T::Bool, false, "As sceneGraphInfo takes it."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphDuplicateNodes";
            std::string view = viewArg(a, cmd);
            const Position off = a.has("offset") ? positionArg(a.flags.at("offset"), cmd) : Position{40.0, 40.0};
            Copied c = copyNodes(view, nodeNames(a, cmd), std::max(1, a.get<int>("depth", 2)), a.get<bool>("all", false), cmd);
            Pasted p = pasteNodes(view, c.clip, c.centre + glm::dvec2(off.first, off.second), false, cmd);
            json layout = layoutData();
            if (layout.contains("frames") && layout["frames"].is_object() && layout["frames"].contains(view))
            {
                std::vector<gn::GFrame> frames = gn::framesFromJson(layout["frames"][view]);
                for (gn::GFrame& f : frames)
                    for (size_t k = 0, n = f.nodes.size(); k < n; ++k)
                        if (auto it = p.renamed.find(f.nodes[k]); it != p.renamed.end())
                            f.nodes.push_back(it->second);
                layout["frames"][view] = gn::framesToJson(frames);
                data::set(kLayoutKey, layout);
            }
            return p.shown;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphGroupNodes",
        .doc = "Gather shown nodes into a node group, drawn as one node in their place: sceneGraphGroupNodes(view, 'L_arm', 'L_hand', "
               "name='Arm'). Scene nodes are untouched; links crossing the group become its node's sockets. Returns {group, node}.",
        .positionalDoc = "view nodes...",
        .flags = {{"name", "", T::String, "NodeGroup", "Name for the new group; made unique."},
                  {"depth", "", T::Int, 2, "As sceneGraphInfo takes it, for where the nodes show.", {}, {}, 1.0},
                  {"all", "", T::Bool, false, "As sceneGraphInfo takes it."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphGroupNodes";
            std::string view = viewArg(a, cmd);
            return collectIntoGroup(view, nodeNames(a, cmd), a.get<std::string>("name", "NodeGroup"), std::max(1, a.get<int>("depth", 2)),
                                    a.get<bool>("all", false), cmd);
        },
    });

    ops::registerCommand({
        .name = "sceneGraphUngroup",
        .doc = "Remove the node group a group node stands for, showing its nodes in its place; the scene nodes are untouched. Returns "
               "them as the view shows them.",
        .positionalDoc = "view node",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphUngroup";
            std::string view = viewArg(a, cmd);
            return ungroup(view, stringArg(a, 1, cmd, "node"), cmd);
        },
    });

    ops::registerCommand({
        .name = "sceneGraphRenameNode",
        .doc = "Rename a scene node from the node editor, as rename does, keeping its place, flags, frames, ties and node groups in every view: "
               "sceneGraphRenameNode(view, 'pCube1', 'Crate'). Returns its new path.",
        .positionalDoc = "view node name",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphRenameNode";
            viewArg(a, cmd);
            Graph& g = sceneGraphOf();
            const NodeId id = nodeArg(stringArg(a, 1, cmd, "node"), cmd).id();
            const std::string name = stringArg(a, 2, cmd, "name");
            const std::string before = g.fullPath(id);
            json renamed = ops::execute("rename", {{"args", {g.path(id), name}}});
            const std::string after = g.fullPath(id);
            if (after != before)
            {
                // Children's full paths start with the node's, so their stored state moves with it.
                auto moved = [&](const std::string& key) -> std::optional<std::string> {
                    if (key == before)
                        return after;
                    if (key.starts_with(before + "|"))
                        return after + key.substr(before.size());
                    return std::nullopt;
                };
                auto rekey = [&](json& byPath) {
                    if (!byPath.is_object())
                        return;
                    json next = json::object();
                    for (auto& [key, value] : byPath.items())
                        next[moved(key).value_or(key)] = value;
                    byPath = std::move(next);
                };
                json layout = layoutData();
                if (layout.contains("positions") && layout["positions"].is_object())
                    for (auto& [view, positions] : layout["positions"].items())
                        rekey(positions);
                if (layout.contains("flags"))
                    rekey(layout["flags"]);
                auto movedAll = [&](json& list) {
                    if (list.is_array())
                        for (json& m : list)
                            if (m.is_string())
                                m = moved(m.get<std::string>()).value_or(m.get<std::string>());
                };
                if (layout.contains("frames") && layout["frames"].is_object())
                    for (auto& [view, frames] : layout["frames"].items())
                        for (json& f : frames)
                            if (f.is_object() && f.contains("nodes"))
                                movedAll(f["nodes"]);
                if (layout.contains("ties") && layout["ties"].is_object())
                    for (auto& [view, ties] : layout["ties"].items())
                        for (json& t : ties)
                            if (t.is_object() && t.contains("links") && t["links"].is_array())
                                for (json& l : t["links"])
                                    if (l.is_array() && l.size() == 4 && l[0].is_string() && l[2].is_string())
                                    {
                                        l[0] = moved(l[0].get<std::string>()).value_or(l[0].get<std::string>());
                                        l[2] = moved(l[2].get<std::string>()).value_or(l[2].get<std::string>());
                                    }
                if (layout.contains("groups") && layout["groups"].is_object())
                    for (auto& [group, rec] : layout["groups"].items())
                        if (rec.is_object() && rec.contains("members"))
                            movedAll(rec["members"]);
                data::set(kLayoutKey, layout);
            }
            return renamed;
        },
    });

    ops::registerCommand({
        .name = "sceneGraphSetNodeFlags",
        .doc = "Show nodes collapsed, or with every plug (hideUnused=False) rather than only the connected ones and their meshes. A node "
               "with neither shows every plug. Kept with the scene.",
        .positionalDoc = "view nodes...",
        .flags = {{"collapsed", "", T::Bool, nullptr, "Only the header."},
                  {"hideUnused", "", T::Bool, nullptr, "Only the connected plugs and meshes; on for every node but modifiers until turned off."},
                  {"muted", "", T::Bool, nullptr, "Accepted for the editor's mute key; scene nodes don't mute."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetNodeFlags";
            viewArg(a, cmd);
            json layout = layoutData();
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                json& f = objectAt(layout, "flags")[memberArg(layout, stringArg(a, i, cmd, "node"), cmd)];
                for (const char* flag : {"collapsed", "hideUnused"})
                    if (a.has(flag))
                        f[flag] = a.get<bool>(flag);
            }
            data::set(kLayoutKey, layout);
            return nullptr;
        },
    });
}
} // namespace lila::scene::sceneGraph
