#include "tex/TextureSet.h"

#include "core/Profile.h"
#include "gn/Routing.h"
#include "ops/Command.h"
#include "scene/Materials.h"
#include "scene/Node.h"
#include "tex/Evaluate.h"
#include "tex/Interop.h"
#include "tex/Layers.h"
#include "tex/SlangRt.h"

#include <algorithm>
#include <functional>
#include <map>
#include <set>

namespace lila::tex
{
namespace
{
using scene::Node;

scene::Graph& graph() { return scene::Graph::current(); }

std::string positionalString(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

Node& setNode(const std::string& name, const char* cmd)
{
    Node* n = findSet(graph(), name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no texture set named '" + name + "'");
    return *n;
}

/** Reads the set's graph, lets fn change it, and writes it back undoably. */
json editGraph(const Args& a, const char* cmd, const std::function<json(Graph&, const SetSettings&)>& fn)
{
    Node& n = setNode(positionalString(a, 0, cmd, "texture set"), cmd);
    Graph g;
    SetSettings s;
    try
    {
        g = readGraph(n);
        s = readSettings(n);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    json result = fn(g, s);
    writeGraph(graph(), n, g);
    return result;
}

std::pair<std::string, std::string> splitPlug(const std::string& s, const char* cmd)
{
    auto dot = s.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == s.size())
        throw CommandError(std::string(cmd) + ": expected node.socket, got '" + s + "'");
    return {s.substr(0, dot), s.substr(dot + 1)};
}

GNode& nodeIn(Graph& g, const std::string& name, const char* cmd)
{
    GNode* n = g.node(name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no node '" + name + "' in " + g.name);
    return *n;
}

NodePorts portsChecked(const Graph& g, const GNode& n, const SetSettings& s, const char* cmd)
{
    ChannelList extra = extraChannels(s);
    NodePorts p = portsOf(g, n, &extra);
    if (!p.error.empty())
        throw CommandError(std::string(cmd) + ": " + n.name + ": " + p.error);
    return p;
}

bool reaches(const Graph& g, const std::string& from, const std::string& to)
{
    std::vector<GLink> links = gn::withPortalLinks(g.nodes, g.links);
    std::vector<std::string> stack{from};
    std::set<std::string> seen;
    while (!stack.empty())
    {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == to)
            return true;
        if (!seen.insert(cur).second)
            continue;
        for (const GLink& l : links)
            if (l.toNode == cur)
                stack.push_back(l.fromNode);
    }
    return false;
}

json portStateJson(const PortDecl& p, const GNode* n, const Graph* g, bool input)
{
    json j = {{"id", p.id}, {"type", portTypeName(p.type)}};
    if (input)
    {
        j["default"] = p.defaultValue;
        if (n && g)
        {
            j["linked"] = !g->linksInto(n->name, p.id).empty();
            auto it = n->values.find(p.id);
            j["value"] = it != n->values.end() ? *it : p.defaultValue;
        }
        if (p.taps != 1)
            j["taps"] = p.taps;
    }
    if (!p.label.empty())
        j["label"] = p.label;
    if (!p.doc.empty())
        j["doc"] = p.doc;
    return j;
}

json paramStateJson(const ParamDecl& p, const GNode* n)
{
    json j = {{"id", p.id}, {"type", paramTypeName(p.type)}, {"default", p.defaultValue}};
    if (n)
        j["value"] = paramValue(*n, p);
    if (!p.options.empty())
        j["options"] = p.options;
    for (auto [key, v] : {std::pair{"min", &p.min}, {"max", &p.max}, {"softMin", &p.softMin}, {"softMax", &p.softMax}, {"step", &p.step}})
        if (*v)
            j[key] = **v;
    if (!p.label.empty())
        j["label"] = p.label;
    if (!p.doc.empty())
        j["doc"] = p.doc;
    return j;
}

json nodeJson(const GNode& n, const Graph& g, const SetSettings& s)
{
    json j = {{"name", n.name}, {"type", n.type}, {"position", {n.position.x, n.position.y}}};
    ChannelList extra = extraChannels(s);
    NodePorts p = portsOf(g, n, &extra);
    if (!p.error.empty())
        j["error"] = p.error;
    if (p.def)
    {
        j["label"] = p.def->label;
        j["category"] = p.def->category;
        j["kind"] = nodeKindName(p.def->kind);
    }
    json ins = json::array(), outs = json::array(), prms = json::array();
    for (const PortDecl& d : p.inputs)
        ins.push_back(portStateJson(d, &n, &g, true));
    for (const PortDecl& d : p.outputs)
        outs.push_back(portStateJson(d, &n, &g, false));
    for (const ParamDecl& d : p.params)
        prms.push_back(paramStateJson(d, &n));
    j["inputs"] = ins;
    j["outputs"] = outs;
    j["params"] = prms;
    return j;
}

json defJson(const NodeDef& d, bool detail)
{
    json e = {{"type", d.type}, {"label", d.label}, {"category", d.category}, {"doc", d.doc}, {"kind", nodeKindName(d.kind)}, {"tier", d.tier}};
    if (!d.owner.empty())
        e["owner"] = d.owner;
    if (detail)
    {
        json ins = json::array(), outs = json::array(), prms = json::array();
        for (const PortDecl& p : d.inputs)
            ins.push_back(portStateJson(p, nullptr, nullptr, true));
        for (const PortDecl& p : d.outputs)
            outs.push_back(portStateJson(p, nullptr, nullptr, false));
        for (const ParamDecl& p : d.params)
            prms.push_back(paramStateJson(p, nullptr));
        e["inputs"] = ins;
        e["outputs"] = outs;
        e["params"] = prms;
        e["cost"] = d.cost;
        if (!d.file.empty())
            e["file"] = d.file.generic_string();
    }
    return e;
}

void checkInputValue(const PortDecl& p, const json& v, const char* cmd)
{
    float tmp[4];
    if (p.type == PortType::Material || !valueToFloat4(v, p.type, tmp))
        throw CommandError(std::string(cmd) + ": " + p.id + " takes a " + portTypeName(p.type) + ", got " + v.dump());
}

PlanRequest requestFor(const Graph& g, const SetSettings& s, int w, int h)
{
    PlanRequest r;
    r.width = w > 0 ? w : s.width;
    r.height = h > 0 ? h : s.height;
    r.outputs = channelRequests(g, s);
    return r;
}

json mergeSettings(const SetSettings& base, const json& changes, const char* cmd)
{
    if (!changes.is_object())
        throw CommandError(std::string(cmd) + ": settings must be a dict");
    json merged = base.toJson();
    for (auto& [k, v] : changes.items())
        merged[k] = v;
    try
    {
        return SetSettings::fromJson(merged).toJson();
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

constexpr const char* kClipFormat = "lila.textureNodes";

std::string joinIds(const std::vector<std::string>& ids)
{
    std::string out;
    for (const std::string& id : ids)
        out += (out.empty() ? "" : ", ") + id;
    return out.empty() ? "none" : out;
}

template <class Decl> std::vector<std::string> idsOf(const std::vector<Decl>& decls)
{
    std::vector<std::string> out;
    for (const Decl& d : decls)
        out.push_back(d.id);
    return out;
}

/** "A.out -> B.in", [from, to] or [fromNode, fromSocket, toNode, toSocket], as node/socket pairs. */
bool parseSpecLink(const json& l, GLink& out)
{
    auto split = [](std::string s, std::string& node, std::string& socket) {
        s.erase(0, s.find_first_not_of(" \t"));
        s.erase(s.find_last_not_of(" \t") + 1);
        auto dot = s.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 == s.size())
            return false;
        node = s.substr(0, dot);
        socket = s.substr(dot + 1);
        return true;
    };
    if (l.is_string())
    {
        std::string s = l.get<std::string>();
        auto arrow = s.find("->");
        return arrow != std::string::npos && split(s.substr(0, arrow), out.fromNode, out.fromSocket) &&
               split(s.substr(arrow + 2), out.toNode, out.toSocket);
    }
    if (l.is_array() && l.size() == 2 && l[0].is_string() && l[1].is_string())
        return split(l[0].get<std::string>(), out.fromNode, out.fromSocket) && split(l[1].get<std::string>(), out.toNode, out.toSocket);
    if (l.is_array() && l.size() == 4 && std::all_of(l.begin(), l.end(), [](const json& x) { return x.is_string(); }))
    {
        out = {l[0].get<std::string>(), l[1].get<std::string>(), l[2].get<std::string>(), l[3].get<std::string>()};
        return true;
    }
    return false;
}

/** Columns by longest path from a source, for nodes the spec gave no position. */
void layoutColumns(Graph& g, const std::set<std::string>& placed)
{
    std::map<std::string, int> depth;
    std::function<int(const std::string&, int)> d = [&](const std::string& n, int guard) -> int {
        if (auto it = depth.find(n); it != depth.end())
            return it->second;
        int best = 0;
        if (guard < 256)
            for (const GLink& l : g.links)
                if (l.toNode == n)
                    best = std::max(best, d(l.fromNode, guard + 1) + 1);
        return depth[n] = best;
    };
    std::map<int, int> rows;
    for (GNode& n : g.nodes)
        if (!placed.contains(n.name))
        {
            int col = d(n.name, 0);
            n.position = {double(col) * 240.0 - 600.0, double(rows[col]++) * 170.0};
        }
}

/**
 * A set's graph from a compact spec ({nodes, links}), with a Material Output named Output added when the spec has none.
 * Every problem goes to errors, each naming the valid choices, so one call reports them all.
 */
Graph graphFromSpec(const json& spec, const std::string& setName, const SetSettings& s, std::vector<std::string>& errors)
{
    Graph g;
    g.name = setName;
    g.description = spec.value("description", std::string());
    const ChannelList extra = extraChannels(s);
    std::set<std::string> placed;
    const json nodes = spec.value("nodes", json::array());
    if (!nodes.is_array())
        errors.push_back("nodes must be a list");
    for (const json& nj : nodes.is_array() ? nodes : json::array())
    {
        if (!nj.is_object())
        {
            errors.push_back("node " + nj.dump() + " must be an object with name, type and values");
            continue;
        }
        GNode n{nj.value("name", std::string()), nj.value("type", std::string()), {0.0, 0.0}, json::object()};
        if (n.name.empty())
            n.name = n.type;
        if (!gn::validIdentifier(n.name))
        {
            errors.push_back("node name '" + n.name + "' isn't valid (letters, digits, underscores)");
            continue;
        }
        if (g.node(n.name))
        {
            errors.push_back("node " + n.name + " is listed twice");
            continue;
        }
        if (!findNode(n.type))
        {
            std::vector<std::string> close;
            std::string want = n.type;
            std::transform(want.begin(), want.end(), want.begin(), ::tolower);
            for (const NodeDef* d : allNodes())
            {
                std::string have = d->type;
                std::transform(have.begin(), have.end(), have.begin(), ::tolower);
                if (!want.empty() && (have.find(want) != std::string::npos || want.find(have) != std::string::npos))
                    close.push_back(d->type);
            }
            errors.push_back("node " + n.name + ": unknown type '" + n.type + "'" +
                             (close.empty() ? "; texNodeTypes lists them" : "; did you mean " + joinIds(close) + "?"));
            continue;
        }
        if (auto p = nj.find("position"); p != nj.end() && p->is_array() && p->size() == 2)
        {
            n.position = {(*p)[0].get<double>(), (*p)[1].get<double>()};
            placed.insert(n.name);
        }
        g.nodes.push_back(n);
        GNode& stored = g.nodes.back();
        json values = json::object();
        for (const char* key : {"params", "values"})
            if (auto v = nj.find(key); v != nj.end() && v->is_object())
                values.update(*v);
        // Parameters first: some decide which ports exist.
        NodePorts p = portsOf(g, stored, &extra);
        for (auto& [id, v] : values.items())
            if (const ParamDecl* prm = p.param(id))
            {
                if (paramValueValid(prm->type, v, prm->options))
                    stored.values[id] = v;
                else
                    errors.push_back(n.name + "." + id + " takes a " + paramTypeName(prm->type) +
                                     (prm->options.empty() ? "" : " (one of " + joinIds(prm->options) + ")") + ", got " + v.dump());
            }
        p = portsOf(g, stored, &extra);
        if (!p.error.empty())
            errors.push_back(n.name + ": " + p.error);
        for (auto& [id, v] : values.items())
        {
            if (p.param(id))
                continue;
            if (const PortDecl* in = p.input(id))
            {
                float tmp[4];
                if (in->type != PortType::Material && valueToFloat4(v, in->type, tmp))
                    stored.values[id] = v;
                else
                    errors.push_back(n.name + "." + id + " takes a " + portTypeName(in->type) + ", got " + v.dump());
            }
            else if ((stored.type == kGroupType && id == "group") || (gn::isRoutingType(stored.type) && id == "name"))
                stored.values[id] = v;
            else
                errors.push_back(n.name + " (" + n.type + ") has no parameter or input '" + id + "'; parameters: " + joinIds(idsOf(p.params)) +
                                 "; inputs: " + joinIds(idsOf(p.inputs)));
        }
    }
    if (!g.firstOfType(kMaterialOutputType))
    {
        if (g.node("Output"))
            errors.push_back("a node named Output must be the materialOutput (or add one of type materialOutput)");
        else
            g.nodes.push_back({"Output", kMaterialOutputType, {300.0, 0.0}, json::object()});
    }

    const json links = spec.value("links", json::array());
    if (!links.is_array())
        errors.push_back("links must be a list");
    for (const json& lj : links.is_array() ? links : json::array())
    {
        GLink l;
        if (!parseSpecLink(lj, l))
        {
            errors.push_back("link " + lj.dump() + " isn't 'A.out -> B.in', [from, to] or [fromNode, fromSocket, toNode, toSocket]");
            continue;
        }
        const GNode* from = g.node(l.fromNode);
        const GNode* to = g.node(l.toNode);
        if (!from || !to)
        {
            errors.push_back("link " + lj.dump() + ": no node '" + (from ? l.toNode : l.fromNode) + "'");
            continue;
        }
        NodePorts fp = portsOf(g, *from, &extra), tp = portsOf(g, *to, &extra);
        const PortDecl* o = fp.output(l.fromSocket);
        const PortDecl* i = tp.input(l.toSocket);
        if (!o)
            errors.push_back("link " + lj.dump() + ": " + l.fromNode + " has no output '" + l.fromSocket + "'; outputs: " + joinIds(idsOf(fp.outputs)));
        if (!i)
            errors.push_back("link " + lj.dump() + ": " + l.toNode + " has no input '" + l.toSocket + "'; inputs: " + joinIds(idsOf(tp.inputs)));
        if (!o || !i)
            continue;
        if (!gn::isRoutingType(to->type) && !linkable(o->type, i->type))
        {
            errors.push_back("link " + lj.dump() + ": can't link " + portTypeName(o->type) + " to " + portTypeName(i->type));
            continue;
        }
        if (!g.linksInto(l.toNode, l.toSocket).empty())
        {
            errors.push_back("link " + lj.dump() + ": " + l.toNode + "." + l.toSocket + " is already linked");
            continue;
        }
        g.links.push_back(l);
    }
    if (gn::hasCycle(gn::withPortalLinks(g.nodes, g.links)))
        errors.push_back("the links make a cycle");
    layoutColumns(g, placed);
    return g;
}
} // namespace

void registerCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texCreateSet",
        .doc = "Create a texture set: a texture graph with a Material Output, and the channels, tile sizes and export preset its maps use. "
               "Returns its name.",
        .positionalDoc = "name",
        .flags = {{"settings", "", T::Any, nullptr, "Settings dict: channels [{name, type, bits}], width, height, tiles [udim or {udim, width, height}], "
                                                    "normalConvention (opengl/directx), bakeProject, exportPreset, material."},
                  presetFlag({"resolution", "", T::Int, nullptr, "Width and height of every tile.", {}, {}, 1.0}, kTextureSizePresets)},
        .run = [](const Args& a) -> json {
            std::string name = positionalString(a, 0, "texCreateSet", "name");
            if (!gn::validIdentifier(name))
                throw CommandError("texCreateSet: '" + name + "' isn't a valid name (letters, digits, underscores)");
            json settings = SetSettings::makeDefault().toJson();
            if (a.has("settings"))
                settings = mergeSettings(SetSettings::makeDefault(), a.flags["settings"], "texCreateSet");
            if (a.has("resolution"))
                settings = mergeSettings(SetSettings::fromJson(settings), json{{"width", a.get<int>("resolution")}, {"height", a.get<int>("resolution")}},
                                         "texCreateSet");
            Node* n = graph().createNode(*scene::types::find(kTextureSetType), name, scene::kNoNode, false);
            if (n->name() != name)
            {
                graph().deleteNode(n->id());
                throw CommandError("texCreateSet: a node named " + name + " already exists");
            }
            writeGraph(graph(), *n, Graph::makeMaterial(name));
            writeSettings(graph(), *n, SetSettings::fromJson(settings));
            return name;
        },
    });

    ops::registerCommand({
        .name = "texSets",
        .doc = "Names of the texture sets in the scene.",
        .undoable = false,
        .run = [](const Args&) -> json { return setNames(graph()); },
    });

    ops::registerCommand({
        .name = "texSetSettings",
        .doc = "Change a texture set's settings: the keys given replace the set's (channels, width, height, tiles, normalConvention, "
               "bakeProject, exportPreset, material, heightDepth, uvWidth, uvHeight, normalFromHeight, normalFade, heightSource). Returns the full settings.",
        .positionalDoc = "set settings",
        .run = [](const Args& a) -> json {
            Node& n = setNode(positionalString(a, 0, "texSetSettings", "texture set"), "texSetSettings");
            if (a.positional.size() < 2)
                throw CommandError("texSetSettings: settings required");
            const SetSettings before = readSettings(n);
            json next = mergeSettings(before, a.positional[1], "texSetSettings");
            const SetSettings after = SetSettings::fromJson(next);
            const Graph g = readGraph(n);
            const StackState st = readStack(g, before);
            writeSettings(graph(), n, after);
            // The stack's height normal reads the set's depth and UV size.
            if (st.layered && !st.graphMode && !st.stack.layers.empty())
                writeGraph(graph(), n, compileStack(st.stack, after, g).graph);
            return next;
        },
    });

    ops::registerCommand({
        .name = "texAddNode",
        .doc = "Add a node to a texture set's graph. Returns its name. texNodeTypes lists the types.",
        .positionalDoc = "set type",
        .flags = {{"position", "", T::List, nullptr, "[x, y] on the canvas."}, {"name", "", T::String, nullptr, "Wanted name; made unique."}},
        .run = [](const Args& a) -> json {
            std::string type = positionalString(a, 1, "texAddNode", "type");
            if (!findNode(type))
                throw CommandError("texAddNode: unknown node type '" + type + "'");
            return editGraph(a, "texAddNode", [&](Graph& g, const SetSettings&) -> json {
                std::string base = a.get<std::string>("name", type);
                if (!gn::validIdentifier(base))
                    throw CommandError("texAddNode: '" + base + "' isn't a valid name");
                GNode n{g.uniqueNodeName(base), type, {0.0, 0.0}, json::object()};
                if (a.has("position"))
                    n.position = {a.flags["position"][0].get<double>(), a.flags["position"][1].get<double>()};
                g.nodes.push_back(n);
                return n.name;
            });
        },
    });

    ops::registerCommand({
        .name = "texDeleteNode",
        .doc = "Delete nodes from a texture set's graph, with their links.",
        .positionalDoc = "set nodes...",
        .run = [](const Args& a) -> json {
            return editGraph(a, "texDeleteNode", [&](Graph& g, const SetSettings&) -> json {
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    std::string name = a.positional[i].get<std::string>();
                    nodeIn(g, name, "texDeleteNode");
                    std::erase_if(g.nodes, [&](const GNode& n) { return n.name == name; });
                    std::erase_if(g.links, [&](const GLink& l) { return l.fromNode == name || l.toNode == name; });
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "texConnect",
        .doc = "Link an output to an input: texConnect(set, 'Noise.out', 'Output.roughness'). Replaces the input's link.",
        .positionalDoc = "set from to",
        .run = [](const Args& a) -> json {
            auto [fn, fsock] = splitPlug(positionalString(a, 1, "texConnect", "from"), "texConnect");
            auto [tn, tsock] = splitPlug(positionalString(a, 2, "texConnect", "to"), "texConnect");
            return editGraph(a, "texConnect", [&](Graph& g, const SetSettings& s) -> json {
                NodePorts fp = portsChecked(g, nodeIn(g, fn, "texConnect"), s, "texConnect");
                NodePorts tp = portsChecked(g, nodeIn(g, tn, "texConnect"), s, "texConnect");
                const PortDecl* o = fp.output(fsock);
                const PortDecl* i = tp.input(tsock);
                if (!o)
                    throw CommandError("texConnect: " + fn + " has no output '" + fsock + "'");
                if (!i)
                    throw CommandError("texConnect: " + tn + " has no input '" + tsock + "'");
                // A routing node takes the type of whatever feeds it.
                if (!gn::isRoutingType(nodeIn(g, tn, "texConnect").type) && !linkable(o->type, i->type))
                    throw CommandError(std::string("texConnect: can't link ") + portTypeName(o->type) + " to " + portTypeName(i->type));
                if (fn == tn || reaches(g, fn, tn))
                    throw CommandError("texConnect: that link would make a cycle");
                std::erase_if(g.links, [&](const GLink& l) { return l.toNode == tn && l.toSocket == tsock; });
                g.links.push_back({fn, fsock, tn, tsock});
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "texDisconnect",
        .doc = "Remove the link into an input, or only the one from `source` when given.",
        .positionalDoc = "set to",
        .flags = {{"source", "", T::String, nullptr, "node.socket of the one link to remove."}},
        .run = [](const Args& a) -> json {
            auto [tn, ts] = splitPlug(positionalString(a, 1, "texDisconnect", "to"), "texDisconnect");
            std::string src = a.get<std::string>("source", "");
            return editGraph(a, "texDisconnect", [&](Graph& g, const SetSettings&) -> json {
                size_t before = g.links.size();
                std::erase_if(g.links, [&](const GLink& l) {
                    return l.toNode == tn && l.toSocket == ts && (src.empty() || l.fromNode + "." + l.fromSocket == src);
                });
                return int(before - g.links.size());
            });
        },
    });

    ops::registerCommand({
        .name = "texSetValue",
        .doc = "Set a parameter or an unlinked input: texSetValue(set, 'Noise.scale', 8), texSetValue(set, 'Blend.mode', 'multiply'). "
               "GroupInput.<input> sets an exposed input's default. Value edits never recompile the graph.",
        .positionalDoc = "set node.id value",
        .run = [](const Args& a) -> json {
            auto [nn, id] = splitPlug(positionalString(a, 1, "texSetValue", "node.id"), "texSetValue");
            if (a.positional.size() < 3)
                throw CommandError("texSetValue: value required");
            json value = a.positional[2];
            return editGraph(a, "texSetValue", [&](Graph& g, const SetSettings& s) -> json {
                GNode& n = nodeIn(g, nn, "texSetValue");
                if (n.type == kGroupInputType)
                {
                    auto it = std::find_if(g.inputs.begin(), g.inputs.end(), [&](const GraphInput& in) { return in.name == id; });
                    if (it == g.inputs.end())
                        throw CommandError("texSetValue: no exposed input '" + id + "'");
                    it->defaultValue = value;
                    return nullptr;
                }
                NodePorts p = portsChecked(g, n, s, "texSetValue");
                if (const ParamDecl* prm = p.param(id))
                {
                    if (!paramValueValid(prm->type, value, prm->options))
                    {
                        std::string extra;
                        for (const std::string& o : prm->options)
                            extra += (extra.empty() ? " (one of " : ", ") + o;
                        throw CommandError("texSetValue: " + id + " takes a " + paramTypeName(prm->type) + extra + (extra.empty() ? "" : ")") +
                                           ", got " + value.dump());
                    }
                    n.values[id] = value;
                    return nullptr;
                }
                if (const PortDecl* in = p.input(id))
                {
                    checkInputValue(*in, value, "texSetValue");
                    n.values[id] = value;
                    return nullptr;
                }
                if (n.type == kGroupType && id == "group")
                {
                    n.values[id] = value;
                    return nullptr;
                }
                if (gn::isRoutingType(n.type) && id == "name")
                {
                    if (!value.is_string())
                        throw CommandError("texSetValue: a portal's name is a string");
                    n.values[id] = value;
                    if (gn::hasCycle(gn::withPortalLinks(g.nodes, g.links)))
                        throw CommandError("texSetValue: " + nn + " reading " + value.get<std::string>() + " would feed its own Send, a loop");
                    return nullptr;
                }
                throw CommandError("texSetValue: " + nn + " has no parameter or input '" + id + "'");
            });
        },
    });

    ops::registerCommand({
        .name = "texMoveNodes",
        .doc = "Move nodes on the canvas: texMoveNodes(set, positions={'Noise': [0, 100]}).",
        .positionalDoc = "set",
        .flags = {{"positions", "", T::Any, nullptr, "{node: [x, y]}"}},
        .run = [](const Args& a) -> json {
            return editGraph(a, "texMoveNodes", [&](Graph& g, const SetSettings&) -> json {
                if (a.has("positions"))
                    for (auto& [name, p] : a.flags["positions"].items())
                        nodeIn(g, name, "texMoveNodes").position = {p[0].get<double>(), p[1].get<double>()};
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "texDuplicateNodes",
        .doc = "Copy nodes with the links between them. Returns the new names in order.",
        .positionalDoc = "set nodes...",
        .flags = {{"offset", "", T::List, json::array({40, 40}), "[x, y] shift for the copies."}},
        .run = [](const Args& a) -> json {
            return editGraph(a, "texDuplicateNodes", [&](Graph& g, const SetSettings&) -> json {
                glm::dvec2 off(40.0, 40.0);
                if (a.has("offset"))
                    off = {a.flags["offset"][0].get<double>(), a.flags["offset"][1].get<double>()};
                std::map<std::string, std::string> renamed;
                json names = json::array();
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    GNode copy = nodeIn(g, a.positional[i].get<std::string>(), "texDuplicateNodes");
                    if (copy.type == kMaterialOutputType || copy.type == kGroupOutputType)
                        continue;
                    std::string old = copy.name;
                    copy.name = g.uniqueNodeName(copy.name);
                    copy.position += off;
                    renamed[old] = copy.name;
                    g.nodes.push_back(copy);
                    names.push_back(copy.name);
                }
                std::vector<GLink> extra;
                for (const GLink& l : g.links)
                    if (renamed.contains(l.fromNode) && renamed.contains(l.toNode))
                        extra.push_back({renamed[l.fromNode], l.fromSocket, renamed[l.toNode], l.toSocket});
                g.links.insert(g.links.end(), extra.begin(), extra.end());
                std::set<std::string> copied;
                for (auto& [old, fresh] : renamed)
                    copied.insert(old);
                for (GTie& t : gn::tiesAmong(g.ties, copied, renamed, off))
                    g.ties.push_back(std::move(t));
                for (GFrame& f : g.frames)
                    for (size_t k = 0, n = f.nodes.size(); k < n; ++k)
                        if (auto it = renamed.find(f.nodes[k]); it != renamed.end())
                            f.nodes.push_back(it->second);
                return names;
            });
        },
    });

    ops::registerCommand({
        .name = "texSetTies",
        .doc = "Replace a texture set's zip ties, shaped as gnSetTies takes them. Display only; links not in the graph are dropped, "
               "and so is a tie left with fewer than two. Returns how many ties the graph has.",
        .positionalDoc = "set ties",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError("texSetTies: ties must be a list");
            std::vector<GTie> ties = gn::tiesFromJson(a.positional[1]);
            return editGraph(a, "texSetTies", [&](Graph& g, const SetSettings&) -> json {
                g.ties = ties;
                gn::pruneTies(g.ties, g.links);
                return int(g.ties.size());
            });
        },
    });

    ops::registerCommand({
        .name = "texSetNodeFlags",
        .doc = "Set how nodes draw and run, as gnSetNodeFlags does: muted, collapsed, hideUnused. Flags left out stay as they are.",
        .positionalDoc = "set nodes...",
        .flags = {{"muted", "", T::Bool, nullptr, "Bypass the nodes."},
                  {"collapsed", "", T::Bool, nullptr, "Draw only their headers."},
                  {"hideUnused", "", T::Bool, nullptr, "Draw only their linked sockets."}},
        .run = [](const Args& a) -> json {
            return editGraph(a, "texSetNodeFlags", [&](Graph& g, const SetSettings&) -> json {
                for (size_t i = 1; i < a.positional.size(); ++i)
                {
                    GNode& n = nodeIn(g, a.positional[i].get<std::string>(), "texSetNodeFlags");
                    if (a.has("muted"))
                    {
                        bool m = a.get<bool>("muted", false);
                        if (m && (n.type == kGroupInputType || n.type == kGroupOutputType || n.type == kMaterialOutputType || gn::isRoutingType(n.type)))
                            throw CommandError("texSetNodeFlags: " + n.name + " can't be muted");
                        n.muted = m;
                    }
                    if (a.has("collapsed"))
                        n.collapsed = a.get<bool>("collapsed", false);
                    if (a.has("hideUnused"))
                        n.hideUnused = a.get<bool>("hideUnused", false);
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "texSetBookmarks",
        .doc = "Replace a texture set's saved canvas views, shaped as gnSetBookmarks takes them.",
        .positionalDoc = "set bookmarks",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_object())
                throw CommandError("texSetBookmarks: bookmarks must be an object of slot: [x, y, zoom]");
            json marks = gn::cleanBookmarks(a.positional[1]);
            return editGraph(a, "texSetBookmarks", [&](Graph& g, const SetSettings&) -> json {
                g.bookmarks = marks;
                return int(marks.size());
            });
        },
    });

    ops::registerCommand({
        .name = "texSetFrames",
        .doc = "Replace a texture set's frames, shaped as gnSetFrames takes them. Display only; names not in the graph are dropped. "
               "Returns how many frames there are.",
        .positionalDoc = "set frames",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2 || !a.positional[1].is_array())
                throw CommandError("texSetFrames: frames must be a list");
            std::vector<GFrame> frames = gn::framesFromJson(a.positional[1]);
            return editGraph(a, "texSetFrames", [&](Graph& g, const SetSettings&) -> json {
                g.frames = frames;
                return int(g.frames.size());
            });
        },
    });

    ops::registerCommand({
        .name = "texCopyNodes",
        .doc = "Nodes as shareable text (JSON with a format header): their values and the links between them. Paste with texPasteNodes.",
        .positionalDoc = "set nodes...",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = setNode(positionalString(a, 0, "texCopyNodes", "texture set"), "texCopyNodes");
            Graph g = readGraph(n);
            std::set<std::string> names;
            for (size_t i = 1; i < a.positional.size(); ++i)
                names.insert(a.positional[i].get<std::string>());
            json nodes = json::array(), links = json::array();
            glm::dvec2 centre(0.0);
            int count = 0;
            for (const GNode& gn : g.nodes)
                if (names.contains(gn.name) && gn.type != kMaterialOutputType)
                {
                    centre += gn.position;
                    ++count;
                }
            if (count)
                centre /= double(count);
            for (const GNode& gn : g.nodes)
                if (names.contains(gn.name) && gn.type != kMaterialOutputType)
                {
                    json o = {{"name", gn.name}, {"type", gn.type}, {"position", {gn.position.x - centre.x, gn.position.y - centre.y}}, {"values", gn.values}};
                    gn::putNodeFlags(gn, o);
                    nodes.push_back(std::move(o));
                }
            for (const GLink& l : g.links)
                if (names.contains(l.fromNode) && names.contains(l.toNode))
                    links.push_back({l.fromNode, l.fromSocket, l.toNode, l.toSocket});
            json out = {{"format", kClipFormat}, {"version", 1}, {"nodes", nodes}, {"links", links}};
            std::set<std::string> copied;
            for (const GNode& gn : g.nodes)
                if (names.contains(gn.name) && gn.type != kMaterialOutputType)
                    copied.insert(gn.name);
            if (std::vector<GTie> ties = gn::tiesAmong(g.ties, copied, {}, -centre); !ties.empty())
                out["ties"] = gn::tiesToJson(ties);
            if (std::vector<GFrame> frames = gn::framesAmong(g.frames, copied, {}, -centre); !frames.empty())
                out["frames"] = gn::framesToJson(frames);
            return out.dump(2);
        },
    });

    ops::registerCommand({
        .name = "texPasteNodes",
        .doc = "Add nodes from texCopyNodes text, centred at position; names are made unique. Returns the new names.",
        .positionalDoc = "set text",
        .flags = {{"position", "", T::List, json::array({0, 0}), "[x, y] canvas centre for the pasted nodes."}},
        .run = [](const Args& a) -> json {
            std::string text = positionalString(a, 1, "texPasteNodes", "text");
            glm::dvec2 at(0.0);
            if (a.has("position"))
                at = {a.flags["position"][0].get<double>(), a.flags["position"][1].get<double>()};
            json clip;
            size_t lb = text.find('{'), rb = text.rfind('}');
            try
            {
                clip = json::parse(text.substr(lb, rb - lb + 1));
            }
            catch (const std::exception&)
            {
                throw CommandError("texPasteNodes: no node data in the text");
            }
            return editGraph(a, "texPasteNodes", [&](Graph& g, const SetSettings&) -> json {
                std::map<std::string, std::string> renamed;
                json names = json::array();
                for (const json& nj : clip.value("nodes", json::array()))
                {
                    std::string type = nj.value("type", "");
                    if (!findNode(type))
                        throw CommandError("texPasteNodes: unknown node type '" + type + "'");
                    GNode n{g.uniqueNodeName(nj.value("name", type)), type, at, nj.value("values", json::object())};
                    gn::getNodeFlags(n, nj);
                    if (nj.contains("position") && nj["position"].is_array() && nj["position"].size() == 2)
                        n.position = at + glm::dvec2(nj["position"][0].get<double>(), nj["position"][1].get<double>());
                    renamed[nj.value("name", type)] = n.name;
                    g.nodes.push_back(n);
                    names.push_back(n.name);
                }
                for (const json& l : clip.value("links", json::array()))
                    if (renamed.contains(l[0].get<std::string>()) && renamed.contains(l[2].get<std::string>()))
                        g.links.push_back({renamed[l[0].get<std::string>()], l[1].get<std::string>(), renamed[l[2].get<std::string>()], l[3].get<std::string>()});
                std::set<std::string> pasted;
                for (auto& [old, fresh] : renamed)
                    pasted.insert(old);
                for (GTie& t : gn::tiesAmong(gn::tiesFromJson(clip.value("ties", json::array())), pasted, renamed, at))
                    g.ties.push_back(std::move(t));
                for (GFrame& f : gn::framesAmong(gn::framesFromJson(clip.value("frames", json::array())), pasted, renamed, at))
                    g.frames.push_back(std::move(f));
                return names;
            });
        },
    });

    ops::registerCommand({
        .name = "texSetDefinition",
        .doc = "Replace a set's whole graph (JSON as texGraphInfo returns under 'definition'). The name is kept.",
        .positionalDoc = "set definition",
        .run = [](const Args& a) -> json {
            if (a.positional.size() < 2)
                throw CommandError("texSetDefinition: definition required");
            json def = a.positional[1].is_string() ? json::parse(a.positional[1].get<std::string>()) : a.positional[1];
            return editGraph(a, "texSetDefinition", [&](Graph& g, const SetSettings&) -> json {
                try
                {
                    Graph next = Graph::fromJson(def);
                    next.name = g.name;
                    g = std::move(next);
                }
                catch (const std::exception& e)
                {
                    throw CommandError(std::string("texSetDefinition: ") + e.what());
                }
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "texBuildGraph",
        .doc = "Build a material's texture graph from a compact spec, creating its texture set when it doesn't exist: {material, "
               "set, settings, description, nodes: [{name, type, params: {...}, values: {...}, position}], links: "
               "['A.out -> Output.baseColor', ...]}. The set defaults to <material>Tex. A Material Output named Output is added "
               "when missing; positions are laid out when omitted. Nothing changes unless the whole spec is valid; the error lists "
               "every problem with the valid names. Returns the set, the material, the channels the graph drives and any compile "
               "error.",
        .positionalDoc = "spec",
        .flags = {{"assign", "", T::List, nullptr, "Meshes to give the material (created when missing)."}},
        .run = [](const Args& a) -> json {
            if (a.positional.empty())
                throw CommandError("texBuildGraph: spec required");
            json spec = a.positional[0];
            if (spec.is_string())
            {
                spec = json::parse(spec.get<std::string>(), nullptr, false);
                if (spec.is_discarded())
                    throw CommandError("texBuildGraph: spec isn't valid JSON");
            }
            if (!spec.is_object())
                throw CommandError("texBuildGraph: the spec must be an object with material, nodes and links");
            std::string material = spec.value("material", std::string());
            if (material.empty() && spec.contains("settings") && spec["settings"].is_object())
                material = spec["settings"].value("material", std::string());
            const std::string name = spec.contains("set") ? spec.value("set", std::string()) : material.empty() ? std::string() : material + "Tex";
            if (!gn::validIdentifier(name))
                throw CommandError("texBuildGraph: set '" + name + "' isn't a valid name (letters, digits, underscores); give material or set");
            // Nodes share one namespace, so a set can't be named like its material.
            if (material == name)
                throw CommandError("texBuildGraph: the set and its material can't both be named " + name + "; leave set out to get " + name + "Tex");

            Node* existing = findSet(graph(), name);
            if (!existing && graph().find(name))
                throw CommandError("texBuildGraph: a node named " + name + " already exists and isn't a texture set");
            SetSettings settings = existing ? readSettings(*existing) : SetSettings::makeDefault();
            if (spec.contains("settings"))
                settings = SetSettings::fromJson(mergeSettings(settings, spec["settings"], "texBuildGraph"));
            if (!material.empty())
                settings.material = material;
            if (a.has("assign") && (settings.material.empty() || settings.material == name))
                throw CommandError("texBuildGraph: assign needs a material name other than the set's; add \"material\" to the spec");

            std::vector<std::string> errors;
            Graph g = graphFromSpec(spec, name, settings, errors);
            if (!errors.empty())
            {
                std::string msg = "texBuildGraph: " + std::to_string(errors.size()) + " problem" + (errors.size() == 1 ? "" : "s");
                for (const std::string& e : errors)
                    msg += "\n  " + e;
                throw CommandError(msg);
            }

            Node* n = existing ? existing : graph().createNode(*scene::types::find(kTextureSetType), name, scene::kNoNode, false);
            writeSettings(graph(), *n, settings);
            writeGraph(graph(), *n, g);

            const std::string slot = settings.material.empty() ? name : settings.material;
            if (a.has("assign"))
            {
                if (!scene::materials::find(graph(), slot))
                    ops::execute("createMaterial", {{"args", {slot}}});
                json call = {{"args", json::array({slot})}};
                for (const json& m : a.flags["assign"])
                    call["args"].push_back(m);
                ops::execute("assignMaterial", call);
            }

            json channels = json::array();
            if (const GNode* out = g.firstOfType(kMaterialOutputType))
                for (const GLink& l : g.links)
                    if (l.toNode == out->name)
                        channels.push_back(l.toSocket);
            Plan plan = buildPlan(g, requestFor(g, settings, 0, 0));
            json result = {{"set", name}, {"material", slot}, {"nodes", g.nodes.size()}, {"links", g.links.size()}, {"channels", channels},
                           {"passes", plan.passes.size()}};
            if (!plan.error.empty())
                result["error"] = plan.error;
            return result;
        },
    });

    ops::registerCommand({
        .name = "texGraphInfo",
        .doc = "A texture set's graph definition, settings, nodes with resolved ports, parameters and values, and its evaluation plan "
               "(passes, buffers, structure hash) or compile error.",
        .positionalDoc = "set",
        .flags = {{"source", "", T::Bool, false, "Include each pass's Slang source."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = setNode(positionalString(a, 0, "texGraphInfo", "texture set"), "texGraphInfo");
            Graph g = readGraph(n);
            SetSettings s = readSettings(n);
            json nodes = json::array();
            std::map<std::string, std::string> problems = gn::portalProblems(g.nodes, g.links);
            for (const GNode& node : g.nodes)
            {
                nodes.push_back(nodeJson(node, g, s));
                if (auto it = problems.find(node.name); it != problems.end())
                    nodes.back()["problem"] = it->second;
            }
            Plan plan = buildPlan(g, requestFor(g, s, 0, 0));
            json summary = plan.summary();
            if (a.get<bool>("source", false))
                for (size_t i = 0; i < plan.passes.size(); ++i)
                    summary["passes"][i]["source"] = plan.passes[i].source;
            json out = {{"name", n.name()}, {"definition", g.toJson()}, {"settings", s.toJson()}, {"nodes", nodes}, {"plan", summary}};
            if (!plan.error.empty())
                out["error"] = plan.error;
            return out;
        },
    });

    ops::registerCommand({
        .name = "texNodeTypes",
        .doc = "The texture node library: type, label, category, kind, tier and description of each. With linkType, only nodes that can "
               "take a link from (or, with linkInput, give one to) a port of that type, each with the socket it would use.",
        .flags = {{"linkType", "", T::String, nullptr, "Port type of a dragged link's loose end."},
                  {"linkInput", "", T::Bool, false, "The drag started at an input, so the node needs a matching output."},
                  {"detail", "", T::Bool, false, "Also list ports, parameters (with options and defaults) and cost."},
                  {"reload", "", T::Bool, false, "Rescan the node folders first."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("reload", false))
                reloadLibrary();
            std::optional<PortType> link;
            if (a.has("linkType"))
            {
                PortType t;
                if (!parsePortType(a.get<std::string>("linkType"), &t))
                    throw CommandError("texNodeTypes: unknown port type '" + a.get<std::string>("linkType") + "'");
                link = t;
            }
            const bool fromInput = a.get<bool>("linkInput", false);
            json out = json::array();
            for (const NodeDef* d : allNodes())
            {
                json e = defJson(*d, a.get<bool>("detail", false));
                if (link)
                {
                    std::string socket;
                    if (!fromInput)
                    {
                        for (const PortDecl& p : d->inputs)
                            if (socket.empty() && linkable(*link, p.type))
                                socket = p.id;
                    }
                    else
                        for (const PortDecl& p : d->outputs)
                            if (socket.empty() && linkable(p.type, *link))
                                socket = p.id;
                    if (socket.empty())
                        continue;
                    e["socket"] = socket;
                }
                out.push_back(std::move(e));
            }
            return out;
        },
    });

    ops::registerCommand({
        .name = "texEvaluate",
        .doc = "Evaluate a texture set and write its channels as images, on the GPU when there is one (else on the CPU). Returns the files, "
               "pass count, time, how many kernels compiled or came from the cache, and the backend used.",
        .positionalDoc = "set",
        .flags = {{"channels", "", T::List, nullptr, "Channel names; all when left out."},
                  presetFlag({"resolution", "", T::Int, nullptr, "Width and height, overriding the tiles'.", {}, {}, 1.0}, kTextureSizePresets),
                  {"folder", "", T::String, nullptr, "Output folder; relative ones resolve like bake folders. Default textures/<set>."},
                  {"format", "", T::String, "png", "png, exr, tga or tif.", {"png", "exr", "tga", "tif"}},
                  {"bits", "", T::Int, 0, "8, 16 or 32; 0 uses each channel's.", {}, {}, 0.0, 32.0},
                  {"backend", "", T::String, nullptr, "gpu or cpu; the GPU when left out and the renderer can run.", {"gpu", "cpu"}}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = setNode(positionalString(a, 0, "texEvaluate", "texture set"), "texEvaluate");
            EvaluateOptions o;
            if (a.has("channels"))
                o.channels = a.flags["channels"].get<std::vector<std::string>>();
            o.width = o.height = a.get<int>("resolution", 0);
            o.folder = a.get<std::string>("folder", "");
            o.format = a.get<std::string>("format", "png");
            o.bits = a.get<int>("bits", 0);
            o.backend = a.get<std::string>("backend", "");
            try
            {
                return evaluateSet(n, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texEvaluate: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "texExport",
        .doc = "Export a texture set's channels through an export preset (a bake target: unreal, substancePainter, or a saved one): its "
               "maps, channel packing (ORM), normal convention, names and bit depths. Returns the files.",
        .positionalDoc = "set",
        .flags = {{"preset", "", T::String, nullptr, "Bake target name; the set's exportPreset when left out."},
                  {"folder", "", T::String, "textures", "Output folder; relative ones resolve like bake folders."},
                  presetFlag({"resolution", "", T::Int, nullptr, "Width and height, overriding the tiles'.", {}, {}, 1.0}, kTextureSizePresets),
                  {"optional", "", T::Bool, true, "Also write the preset's optional maps the set has channels for."},
                  {"incremental", "", T::Bool, false, "Only rewrite files whose inputs or pixels changed since the last export to this folder."},
                  {"backend", "", T::String, nullptr, "gpu or cpu; the GPU when left out and the renderer can run.", {"gpu", "cpu"}},
                  {"normalFromHeight", "", T::Bool, nullptr,
                   "Write the normal as detail over the height, with the height at 16 bits, for materials that rebuild normals from "
                   "it; the set's normalFromHeight when left out."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = setNode(positionalString(a, 0, "texExport", "texture set"), "texExport");
            ExportOptions o;
            o.preset = a.get<std::string>("preset", "");
            o.folder = a.get<std::string>("folder", "textures");
            o.width = o.height = a.get<int>("resolution", 0);
            o.includeOptional = a.get<bool>("optional", true);
            o.incremental = a.get<bool>("incremental", false);
            o.backend = a.get<std::string>("backend", "");
            if (a.has("normalFromHeight"))
                o.normalFromHeight = a.get<bool>("normalFromHeight", false);
            try
            {
                return exportSet(n, o);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("texExport: ") + e.what());
            }
        },
    });

    ops::registerCommand({
        .name = "texCompileStats",
        .doc = "CPU kernel cache counters: compiles, cache hits and failures since start (or the last clear).",
        .flags = {{"clear", "", T::Bool, false, "Empty the cache and reset the counters."}},
        .undoable = false,
        .run = [](const Args& a) -> json {
            if (a.get<bool>("clear", false))
                slangrt::clearCache();
            slangrt::CacheStats s = slangrt::cacheStats();
            return {{"compiles", s.compiles}, {"hits", s.hits}, {"failures", s.failures}};
        },
    });

    registerLayerCommands();
    registerInteropCommands();
}
} // namespace lila::tex
