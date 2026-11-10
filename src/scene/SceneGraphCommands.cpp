#include "scene/SceneGraph.h"

#include "ops/Command.h"
#include "rigmath/MathNodes.h"
#include "scene/Graph.h"
#include "scene/MeshNodes.h"
#include "scene/Modifiers.h"
#include "scene/SceneData.h"

#include <algorithm>
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

/** View state kept with the scene: {"positions": {view: {fullPath: [x, y]}}, "flags": {fullPath: {flag: bool}}}. */
constexpr const char* kLayoutKey = "nodeEditor.scene";
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

std::string viewArg(const Args& a, const char* cmd)
{
    std::string view = stringArg(a, 0, cmd, "view");
    if (std::none_of(kViews.begin(), kViews.end(), [&](const char* v) { return view == v; }))
        throw CommandError(std::string(cmd) + ": no view '" + view + "' (Selection, History, Hierarchy, Upstream or Downstream)");
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

/** An output, or an input animators drive other things from (keyable or added by hand). */
bool offeredAsSource(const AttrDef& d)
{
    if ((d.flags & (AttrArray | AttrHidden)))
        return false;
    return (d.flags & AttrOutput) || ((d.flags & AttrInput) && (d.flags & (AttrKeyable | AttrUser)));
}

bool offeredAsInput(const AttrDef& d) { return (d.flags & AttrInput) && !(d.flags & AttrHidden); }

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

json viewInfo(const std::string& view, int depth, bool all)
{
    const Graph& g = sceneGraphOf();
    const Edges e(g);
    const ViewNodes v = collect(g, e, view, depth, all);
    const bool hierarchy = view == "Hierarchy";
    const json layout = layoutData();
    const json stored = layout.value("positions", json::object()).value(view, json::object());
    const json flags = layout.value("flags", json::object());

    std::unordered_map<NodeId, size_t> index;
    std::vector<std::string> paths;
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        index[v.order[i]] = i;
        paths.push_back(g.path(v.order[i]));
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

    json nodes = json::array(), defNodes = json::array();
    std::map<int, double> columnY;
    for (size_t i = 0; i < v.order.size(); ++i)
    {
        const std::string full = g.fullPath(v.order[i]);
        const json f = flags.value(full, json::object());
        const bool hideUnused = f.value("hideUnused", true), collapsed = f.value("collapsed", false);
        json n = nodeJson(g, e, v.order[i], hierarchy);
        json def = {{"name", paths[i]}, {"type", n["type"]}, {"hideUnused", hideUnused}};
        if (collapsed)
            def["collapsed"] = true;
        if (json p = stored.value(full, json()); p.is_array() && p.size() == 2)
            n["position"] = p;
        else
        {
            size_t rows = 0;
            if (!collapsed)
            {
                for (const json& s : n["inputs"])
                    rows += !hideUnused || s.value("linked", false) ? 1 : 0;
                rows += hideUnused ? usedOutputs[i].size() : n["outputs"].size();
            }
            double& y = columnY[rank[i]];
            n["position"] = json::array({double(rank[i]) * kColumnWidth, y});
            y += 30.0 + 20.0 * double(rows) + kRowGap;
        }
        nodes.push_back(std::move(n));
        defNodes.push_back(std::move(def));
    }

    json out = {{"name", view}, {"view", view}, {"nodes", nodes}};
    out["definition"] = {{"nodes", defNodes}, {"links", links}};
    if (g.selection().empty())
        out["error"] = "Nothing selected. Select objects in the viewport or the Outliner to show their nodes.";
    return out;
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
               "conversions unless all is on.",
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
        .doc = "Create a scene node from the node editor and add it to the selection, so the view shows it. Returns its path.",
        .positionalDoc = "view type",
        .flags = {{"position", "", T::List, nullptr, "[x, y] on the canvas."}, {"name", "", T::String, nullptr, "Wanted name; made unique."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphAddNode";
            std::string view = viewArg(a, cmd);
            std::string typeName = stringArg(a, 1, cmd, "type");
            const NodeType* t = types::find(typeName);
            if (!t)
                throw CommandError(std::string(cmd) + ": unknown node type '" + typeName + "'");
            Graph& g = sceneGraphOf();
            Node* n = rethrown([&] { return g.createNode(*t, a.get<std::string>("name", "")); });
            g.select({n->id()}, SelectMode::Add);
            if (a.has("position"))
                storePositions(view, {std::pair{n->id(), positionArg(a.flags.at("position"), cmd)}});
            return n->path();
        },
    });

    ops::registerCommand({
        .name = "sceneGraphDeleteNode",
        .doc = "Delete scene nodes from the node editor, with their connections (and, for objects, their children).",
        .positionalDoc = "view nodes...",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphDeleteNode";
            viewArg(a, cmd);
            std::vector<NodeId> ids;
            for (size_t i = 1; i < a.positional.size(); ++i)
                ids.push_back(nodeArg(stringArg(a, i, cmd, "node"), cmd).id());
            Graph& g = sceneGraphOf();
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
               "connection. In the Hierarchy view, 'A.@children' to 'B.@parent' parents B under A.",
        .positionalDoc = "view from to",
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphConnect";
            viewArg(a, cmd);
            std::string from = stringArg(a, 1, cmd, "from"), to = stringArg(a, 2, cmd, "to");
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
            std::string to = stringArg(a, 1, cmd, "to");
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
                from = plugArg(a.get<std::string>("source"));
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
        .name = "sceneGraphMoveNodes",
        .doc = "Move nodes on the node editor's canvas: sceneGraphMoveNodes(view, positions={'Box': [0, 100]}). Kept per view with the "
               "scene.",
        .positionalDoc = "view",
        .flags = {{"positions", "", T::Any, nullptr, "{node: [x, y]}"}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphMoveNodes";
            std::string view = viewArg(a, cmd);
            std::vector<std::pair<NodeId, Position>> moves;
            if (a.has("positions"))
            {
                const json& positions = a.flags.at("positions");
                if (!positions.is_object())
                    throw CommandError(std::string(cmd) + ": positions is {node: [x, y]}");
                for (const auto& [name, p] : positions.items())
                    moves.push_back({nodeArg(name, cmd).id(), positionArg(p, cmd)});
            }
            if (!moves.empty())
                storePositions(view, moves);
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
        .name = "sceneGraphSetNodeFlags",
        .doc = "Show nodes collapsed, or with every plug (hideUnused=False) rather than only the connected ones. Kept with the scene.",
        .positionalDoc = "view nodes...",
        .flags = {{"collapsed", "", T::Bool, nullptr, "Only the header."},
                  {"hideUnused", "", T::Bool, nullptr, "Only the connected plugs; on for every node until turned off."},
                  {"muted", "", T::Bool, nullptr, "Accepted for the editor's mute key; scene nodes don't mute."}},
        .run = [](const Args& a) -> json {
            const char* cmd = "sceneGraphSetNodeFlags";
            viewArg(a, cmd);
            json layout = layoutData();
            json& flags = layout["flags"];
            for (size_t i = 1; i < a.positional.size(); ++i)
            {
                json& f = flags[sceneGraphOf().fullPath(nodeArg(stringArg(a, i, cmd, "node"), cmd).id())];
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
