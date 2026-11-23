#include "tex/GraphFiles.h"

#include "ops/Command.h"
#include "scene/Graph.h"
#include "scene/Node.h"
#include "tex/Graph.h"
#include "tex/NodeDef.h"
#include "tex/Ports.h"
#include "tex/TextureSet.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

namespace lila::tex
{
namespace
{
using scene::Node;

std::string groupArg(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

Node& setOf(const GraphAddress& at, const char* cmd)
{
    Node* n = findSet(scene::Graph::current(), at.set);
    if (!n)
        throw CommandError(std::string(cmd) + ": no texture set named '" + at.set + "'");
    return *n;
}

/** The graph at the address, changed by fn and written back undoably. */
json editAt(const std::string& address, const char* cmd, const std::function<json(Graph&, const ChannelList&)>& fn)
{
    Node* set = nullptr;
    Graph g;
    ChannelList extra;
    try
    {
        g = readGraphAt(scene::Graph::current(), address, &set);
        extra = extraChannels(readSettings(*set));
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    json result = fn(g, extra);
    writeGraphAt(scene::Graph::current(), address, g);
    return result;
}

std::string uniqueKey(const std::set<std::string>& taken, const std::string& base)
{
    if (!taken.contains(base))
        return base;
    for (int i = 1;; ++i)
        if (std::string c = base + std::to_string(i); !taken.contains(c))
            return c;
}

std::set<std::string> inputNames(const Graph& g)
{
    std::set<std::string> out;
    for (const GraphInput& in : g.inputs)
        out.insert(in.name);
    return out;
}

std::set<std::string> outputNames(const Graph& g)
{
    std::set<std::string> out;
    for (const GraphOutput& o : g.outputs)
        out.insert(o.name);
    return out;
}

std::string nodeOfType(const Graph& g, const char* type)
{
    const GNode* n = g.firstOfType(type);
    return n ? n->name : std::string();
}

json portDefault(PortType t)
{
    switch (t)
    {
    case PortType::Color: return json::array({0.5, 0.5, 0.5});
    case PortType::Vector: return json::array({0.0, 0.0, 0.0});
    default: return 0.0;
    }
}

/** A set's graph and its node groups, edited together so a group's users follow changes to its interface. */
struct SetGraphs
{
    Node* set = nullptr;
    Graph top;
    std::string group;
    Graph sub;
};

SetGraphs openGroup(const std::string& address, const char* cmd)
{
    const GraphAddress at = splitAddress(address);
    if (at.group.empty())
        throw CommandError(std::string(cmd) + ": a texture set's own graph has no inputs or outputs; give a node group as \"" + at.set +
                           "/Group\"");
    SetGraphs s;
    s.set = &setOf(at, cmd);
    try
    {
        s.top = readGraph(*s.set);
        auto it = s.top.subgroups.find(at.group);
        if (it == s.top.subgroups.end())
            throw CommandError(std::string(cmd) + ": " + at.set + " has no node group named '" + at.group + "'");
        s.sub = Graph::fromJson(it->second);
    }
    catch (const std::invalid_argument& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
    s.group = at.group;
    return s;
}

/** Runs fn on every Group node running the group, in the set's graph and its other groups, then writes all of it back. */
void commitGroup(SetGraphs& s, const std::function<void(Graph&, GNode&)>& users)
{
    auto visit = [&](Graph& g) {
        for (GNode& n : g.nodes)
            if (n.type == kGroupType && n.values.value("group", std::string()) == s.group)
                users(g, n);
    };
    visit(s.top);
    for (auto& [name, def] : s.top.subgroups)
    {
        if (name == s.group)
            continue;
        Graph other = Graph::fromJson(def);
        visit(other);
        def = other.toJson();
    }
    s.top.subgroups[s.group] = s.sub.toJson();
    writeGraph(scene::Graph::current(), *s.set, s.top);
}

struct Collapsed
{
    std::string group;
    std::string node;
};

Collapsed collapse(Graph& outer, const ChannelList& extra, const std::vector<std::string>& names, const std::string& base, const char* cmd)
{
    std::set<std::string> sel(names.begin(), names.end());
    if (sel.empty())
        throw CommandError(std::string(cmd) + ": no nodes given");
    std::set<std::string> taken;
    for (const auto& [k, v] : outer.subgroups)
        taken.insert(k);
    Collapsed out;
    out.group = uniqueKey(taken, base);
    Graph inner = Graph::makeGroup(out.group);
    inner.outputs.clear();
    const std::string gi = nodeOfType(inner, kGroupInputType), go = nodeOfType(inner, kGroupOutputType);
    glm::dvec2 lo(1e30), hi(-1e30);
    for (const std::string& n : sel)
    {
        const GNode* gn = outer.node(n);
        if (!gn)
            throw CommandError(std::string(cmd) + ": no node '" + n + "' in " + outer.name);
        if (gn->type == kGroupInputType || gn->type == kGroupOutputType || gn->type == kMaterialOutputType)
            throw CommandError(std::string(cmd) + ": " + n + " can't go in a group (Group Input, Group Output and Material Output stay put)");
        inner.nodes.push_back(*gn);
        lo = glm::min(lo, gn->position);
        hi = glm::max(hi, gn->position);
    }
    inner.node(gi)->position = {lo.x - 300.0, (lo.y + hi.y) / 2.0};
    inner.node(go)->position = {hi.x + 300.0, (lo.y + hi.y) / 2.0};

    out.node = outer.uniqueNodeName(out.group);
    std::vector<GLink> outerLinks;
    std::map<std::string, std::string> inputFor, outputFor;
    for (const GLink& l : outer.links)
    {
        const bool fromIn = sel.contains(l.fromNode), toIn = sel.contains(l.toNode);
        if (fromIn && toIn)
            inner.links.push_back(l);
        else if (toIn)
        {
            const GNode& target = *outer.node(l.toNode);
            const NodePorts ports = portsOf(outer, target, &extra);
            const PortDecl* decl = ports.input(l.toSocket);
            if (!decl)
                continue;
            const std::string key = l.fromNode + "." + l.fromSocket;
            std::string name;
            if (auto it = inputFor.find(key); it != inputFor.end())
                name = it->second;
            else
            {
                name = uniqueKey(inputNames(inner), l.toSocket);
                GraphInput in;
                in.name = name;
                in.port = decl->type;
                auto v = target.values.find(l.toSocket);
                in.defaultValue = v != target.values.end() ? *v : decl->defaultValue.is_null() ? portDefault(decl->type) : decl->defaultValue;
                inner.inputs.push_back(std::move(in));
                inputFor[key] = name;
            }
            inner.links.push_back({gi, name, l.toNode, l.toSocket});
            GLink ol{l.fromNode, l.fromSocket, out.node, name};
            if (std::find(outerLinks.begin(), outerLinks.end(), ol) == outerLinks.end())
                outerLinks.push_back(ol);
        }
        else if (fromIn)
        {
            const GNode& source = *outer.node(l.fromNode);
            const NodePorts ports = portsOf(outer, source, &extra);
            const PortDecl* decl = ports.output(l.fromSocket);
            if (!decl)
                continue;
            const std::string key = l.fromNode + "." + l.fromSocket;
            std::string name;
            if (auto it = outputFor.find(key); it != outputFor.end())
                name = it->second;
            else
            {
                name = uniqueKey(outputNames(inner), l.fromSocket);
                inner.outputs.push_back({name, decl->type, {}});
                outputFor[key] = name;
                inner.links.push_back({l.fromNode, l.fromSocket, go, name});
            }
            outerLinks.push_back({out.node, name, l.toNode, l.toSocket});
        }
        else
            outerLinks.push_back(l);
    }
    inner.ties = gn::tiesAmong(outer.ties, sel);
    inner.frames = gn::framesAmong(outer.frames, sel);
    std::erase_if(outer.nodes, [&](const GNode& n) { return sel.contains(n.name); });
    outer.links = std::move(outerLinks);
    outer.nodes.push_back({out.node, kGroupType, (lo + hi) / 2.0, json{{"group", out.group}}});
    outer.subgroups[out.group] = inner.toJson();
    return out;
}

/** Whether any Group node in the graph or its groups still runs name. */
bool groupInUse(const Graph& g, const std::string& name)
{
    auto uses = [&](const Graph& x) {
        return std::any_of(x.nodes.begin(), x.nodes.end(),
                           [&](const GNode& n) { return n.type == kGroupType && n.values.value("group", std::string()) == name; });
    };
    if (uses(g))
        return true;
    for (const auto& [k, def] : g.subgroups)
        if (k != name && uses(Graph::fromJson(def)))
            return true;
    return false;
}

json ungroup(Graph& outer, const std::string& nodeName, const char* cmd)
{
    const GNode* gptr = outer.node(nodeName);
    if (!gptr || gptr->type != kGroupType)
        throw CommandError(std::string(cmd) + ": " + nodeName + " is not a Group node in " + outer.name);
    const GNode gnode = *gptr;
    const std::string groupName = gnode.values.value("group", std::string());
    auto def = outer.subgroups.find(groupName);
    if (def == outer.subgroups.end())
        throw CommandError(std::string(cmd) + ": " + nodeName + " runs '" + groupName + "', which this set doesn't have");
    const Graph inner = Graph::fromJson(def->second);
    const std::string gi = nodeOfType(inner, kGroupInputType), go = nodeOfType(inner, kGroupOutputType);

    glm::dvec2 centre(0.0);
    int count = 0;
    for (const GNode& n : inner.nodes)
        if (n.type != kGroupInputType && n.type != kGroupOutputType)
        {
            centre += n.position;
            ++count;
        }
    if (count)
        centre /= double(count);
    const glm::dvec2 offset = gnode.position - centre;

    std::map<std::string, std::string> renamed;
    std::set<std::string> innerNames;
    json added = json::array();
    for (const GNode& n : inner.nodes)
    {
        if (n.type == kGroupInputType || n.type == kGroupOutputType)
            continue;
        GNode copy = n;
        copy.name = outer.uniqueNodeName(n.name);
        copy.position += offset;
        renamed[n.name] = copy.name;
        innerNames.insert(n.name);
        added.push_back(copy.name);
        outer.nodes.push_back(std::move(copy));
    }
    // Param inputs carry the Group node's setting into the inner parameters they target.
    for (const GraphInput& in : inner.inputs)
        if (in.param)
            if (auto v = gnode.values.find(in.name); v != gnode.values.end())
                for (const std::string& t : in.targets)
                    if (size_t dot = t.find('.'); dot != std::string::npos)
                        if (auto r = renamed.find(t.substr(0, dot)); r != renamed.end())
                            outer.node(r->second)->values[t.substr(dot + 1)] = *v;

    std::vector<GLink> into, outOf, kept;
    for (const GLink& l : outer.links)
    {
        if (l.toNode == nodeName)
            into.push_back(l);
        else if (l.fromNode == nodeName)
            outOf.push_back(l);
        else
            kept.push_back(l);
    }
    auto push = [&](const GLink& l) {
        if (std::find(kept.begin(), kept.end(), l) == kept.end())
            kept.push_back(l);
    };
    for (const GLink& l : inner.links)
    {
        const bool fromGi = l.fromNode == gi, toGo = l.toNode == go;
        if (!fromGi && !toGo)
            push({renamed[l.fromNode], l.fromSocket, renamed[l.toNode], l.toSocket});
        else if (fromGi && !toGo)
        {
            bool fed = false;
            for (const GLink& o : into)
                if (o.toSocket == l.fromSocket)
                {
                    push({o.fromNode, o.fromSocket, renamed[l.toNode], l.toSocket});
                    fed = true;
                }
            if (!fed)
            {
                const GraphInput* in = inner.input(l.fromSocket);
                auto v = gnode.values.find(l.fromSocket);
                json value = v != gnode.values.end() ? *v : in ? in->defaultValue : json();
                if (!value.is_null())
                    outer.node(renamed[l.toNode])->values[l.toSocket] = value;
            }
        }
        else if (!fromGi && toGo)
        {
            for (const GLink& o : outOf)
                if (o.fromSocket == l.toSocket)
                    push({renamed[l.fromNode], l.fromSocket, o.toNode, o.toSocket});
        }
        else
            for (const GLink& i : into)
                if (i.toSocket == l.fromSocket)
                    for (const GLink& o : outOf)
                        if (o.fromSocket == l.toSocket)
                            push({i.fromNode, i.fromSocket, o.toNode, o.toSocket});
    }
    outer.links = std::move(kept);
    for (GTie& t : gn::tiesAmong(inner.ties, innerNames, renamed, offset))
        outer.ties.push_back(std::move(t));
    for (GFrame& f : gn::framesAmong(inner.frames, innerNames, renamed, offset))
        outer.frames.push_back(std::move(f));
    std::erase_if(outer.nodes, [&](const GNode& n) { return n.name == nodeName; });
    if (!groupInUse(outer, groupName))
        outer.subgroups.erase(groupName);
    return added;
}

std::vector<std::string> restStrings(const Args& a, size_t from)
{
    std::vector<std::string> out;
    for (size_t i = from; i < a.positional.size(); ++i)
        if (a.positional[i].is_string())
            out.push_back(a.positional[i].get<std::string>());
    return out;
}

GraphInput& inputIn(Graph& g, const std::string& name, const char* cmd)
{
    auto it = std::find_if(g.inputs.begin(), g.inputs.end(), [&](const GraphInput& in) { return in.name == name; });
    if (it == g.inputs.end())
        throw CommandError(std::string(cmd) + ": no input '" + name + "' in " + g.name);
    return *it;
}

bool validName(const std::string& n)
{
    return !n.empty() && n.find('/') == std::string::npos && n.find('.') == std::string::npos;
}
} // namespace

void registerGroupCommands()
{
    using T = ArgType;
    ops::registerCommand({
        .name = "texGroupNodes",
        .doc = "Collapse nodes into a new node group of the set and put a Group node running it in their place. Links crossing the "
               "boundary become the group's inputs and outputs. Open it as \"Set/Group\". Returns {group, node}.",
        .positionalDoc = "set nodes...",
        .flags = {{"name", "", T::String, "NodeGroup", "Name for the new group; made unique."}},
        .run = [](const Args& a) -> json {
            const std::string address = groupArg(a, 0, "texGroupNodes", "texture set");
            const std::string base = a.get<std::string>("name", "NodeGroup");
            if (!validName(base))
                throw CommandError("texGroupNodes: '" + base + "' can't name a group (no / or .)");
            return editAt(address, "texGroupNodes", [&](Graph& g, const ChannelList& extra) -> json {
                Collapsed c = collapse(g, extra, restStrings(a, 1), base, "texGroupNodes");
                return {{"group", c.group}, {"node", c.node}};
            });
        },
        .examples = {"texGroupNodes(\"RockTex\", \"Noise\", \"Levels\", name=\"Grit\")"},
    });

    ops::registerCommand({
        .name = "texUngroup",
        .doc = "Replace a Group node with the nodes of the group it runs, rewired in place. The group goes when nothing else runs it. "
               "Returns the added node names.",
        .positionalDoc = "set node",
        .run = [](const Args& a) -> json {
            const std::string address = groupArg(a, 0, "texUngroup", "texture set");
            const std::string node = groupArg(a, 1, "texUngroup", "node");
            return editAt(address, "texUngroup", [&](Graph& g, const ChannelList&) -> json { return ungroup(g, node, "texUngroup"); });
        },
    });

    ops::registerCommand({
        .name = "texAddInput",
        .doc = "Add an input to a node group (\"Set/Group\"): an output of its Group Input and a socket on the Group nodes running it. "
               "Returns its name, made unique.",
        .positionalDoc = "group name [type]",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texAddInput", "group"), "texAddInput");
            const std::string base = groupArg(a, 1, "texAddInput", "name");
            if (!validName(base))
                throw CommandError("texAddInput: '" + base + "' can't name an input");
            PortType t = PortType::Float;
            const std::string type = a.positional.size() > 2 && a.positional[2].is_string() ? a.positional[2].get<std::string>() : "float";
            if (!parsePortType(type, &t) || t == PortType::Material || t == PortType::Sdf2d)
                throw CommandError("texAddInput: type is float, gray, color or vector, got '" + type + "'");
            GraphInput in;
            in.name = uniqueKey(inputNames(s.sub), base);
            in.port = t;
            in.defaultValue = portDefault(t);
            s.sub.inputs.push_back(in);
            commitGroup(s, [](Graph&, GNode&) {});
            return in.name;
        },
    });

    ops::registerCommand({
        .name = "texRemoveInput",
        .doc = "Remove a node group's input, its links inside, and the links into it on the Group nodes running the group.",
        .positionalDoc = "group name",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texRemoveInput", "group"), "texRemoveInput");
            const std::string name = groupArg(a, 1, "texRemoveInput", "name");
            inputIn(s.sub, name, "texRemoveInput");
            std::erase_if(s.sub.inputs, [&](const GraphInput& in) { return in.name == name; });
            const std::string gi = nodeOfType(s.sub, kGroupInputType);
            std::erase_if(s.sub.links, [&](const GLink& l) { return l.fromNode == gi && l.fromSocket == name; });
            commitGroup(s, [&](Graph& g, GNode& n) {
                n.values.erase(name);
                std::erase_if(g.links, [&](const GLink& l) { return l.toNode == n.name && l.toSocket == name; });
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "texRenameInput",
        .doc = "Rename a node group's input, keeping its links inside and on the Group nodes running the group.",
        .positionalDoc = "group name newName",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texRenameInput", "group"), "texRenameInput");
            const std::string from = groupArg(a, 1, "texRenameInput", "name"), to = groupArg(a, 2, "texRenameInput", "new name");
            if (!validName(to))
                throw CommandError("texRenameInput: '" + to + "' can't name an input");
            GraphInput& in = inputIn(s.sub, from, "texRenameInput");
            if (from == to)
                return to;
            if (s.sub.input(to))
                throw CommandError("texRenameInput: " + s.group + " already has an input '" + to + "'");
            in.name = to;
            const std::string gi = nodeOfType(s.sub, kGroupInputType);
            for (GLink& l : s.sub.links)
                if (l.fromNode == gi && l.fromSocket == from)
                    l.fromSocket = to;
            commitGroup(s, [&](Graph& g, GNode& n) {
                if (auto v = n.values.find(from); v != n.values.end())
                {
                    n.values[to] = *v;
                    n.values.erase(from);
                }
                for (GLink& l : g.links)
                    if (l.toNode == n.name && l.toSocket == from)
                        l.toSocket = to;
            });
            return to;
        },
    });

    ops::registerCommand({
        .name = "texMoveInput",
        .doc = "Move a node group's input to another place in its interface, 0 first.",
        .positionalDoc = "group name index",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texMoveInput", "group"), "texMoveInput");
            const std::string name = groupArg(a, 1, "texMoveInput", "name");
            if (a.positional.size() < 3 || !a.positional[2].is_number())
                throw CommandError("texMoveInput: index required");
            inputIn(s.sub, name, "texMoveInput");
            auto it = std::find_if(s.sub.inputs.begin(), s.sub.inputs.end(), [&](const GraphInput& in) { return in.name == name; });
            GraphInput moved = *it;
            s.sub.inputs.erase(it);
            const int to = std::clamp(a.positional[2].get<int>(), 0, int(s.sub.inputs.size()));
            s.sub.inputs.insert(s.sub.inputs.begin() + to, std::move(moved));
            commitGroup(s, [](Graph&, GNode&) {});
            return to;
        },
    });

    ops::registerCommand({
        .name = "texSetInputInfo",
        .doc = "Set how a node group's input presents: label, description, panel and number limits. An empty string clears one.",
        .positionalDoc = "group name",
        .flags = {{"label", "", T::String, nullptr, "Shown instead of the name."},
                  {"description", "", T::String, nullptr, "Tooltip."},
                  {"subtype", "", T::String, nullptr, "Accepted for other node editors' inputs; texture inputs ignore it."},
                  {"panel", "", T::String, nullptr, "Panel it is listed under."},
                  {"min", "", T::Any, nullptr, "Hard minimum, or empty."},
                  {"max", "", T::Any, nullptr, "Hard maximum, or empty."},
                  {"softMin", "", T::Any, nullptr, "Slider minimum, or empty."},
                  {"softMax", "", T::Any, nullptr, "Slider maximum, or empty."},
                  {"step", "", T::Any, nullptr, "Drag step, or empty."}},
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texSetInputInfo", "group"), "texSetInputInfo");
            GraphInput& in = inputIn(s.sub, groupArg(a, 1, "texSetInputInfo", "name"), "texSetInputInfo");
            if (a.has("label"))
                in.label = a.get<std::string>("label", "");
            if (a.has("description"))
                in.description = a.get<std::string>("description", "");
            if (a.has("panel"))
                in.panel = a.get<std::string>("panel", "");
            auto number = [&](const char* key, std::optional<double>& into) {
                if (!a.has(key))
                    return;
                const json v = a.get<json>(key, json());
                into = v.is_number() ? std::optional<double>(v.get<double>()) : std::nullopt;
            };
            number("min", in.min);
            number("max", in.max);
            number("softMin", in.softMin);
            number("softMax", in.softMax);
            number("step", in.step);
            commitGroup(s, [](Graph&, GNode&) {});
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "texExposeInput",
        .doc = "Make a node's input inside a node group an input of the group, linked from Group Input with the input's value as its "
               "default. Returns the new input's name.",
        .positionalDoc = "group node.input",
        .run = [](const Args& a) -> json {
            const std::string address = groupArg(a, 0, "texExposeInput", "group");
            const std::string plug = groupArg(a, 1, "texExposeInput", "node.input");
            const size_t dot = plug.find('.');
            if (dot == std::string::npos)
                throw CommandError("texExposeInput: expected node.input, got '" + plug + "'");
            const std::string nn = plug.substr(0, dot), sid = plug.substr(dot + 1);
            SetGraphs s = openGroup(address, "texExposeInput");
            const GNode* n = s.sub.node(nn);
            if (!n)
                throw CommandError("texExposeInput: no node '" + nn + "' in " + s.group);
            Graph resolved = s.sub;
            resolved.subgroups = s.top.subgroups;
            const NodePorts ports = portsOf(resolved, *n);
            const PortDecl* decl = ports.input(sid);
            if (!decl)
                throw CommandError("texExposeInput: " + nn + " has no input '" + sid + "'");
            GraphInput in;
            in.name = uniqueKey(inputNames(s.sub), sid);
            in.port = decl->type;
            auto v = n->values.find(sid);
            in.defaultValue = v != n->values.end() ? *v : decl->defaultValue.is_null() ? portDefault(decl->type) : decl->defaultValue;
            s.sub.inputs.push_back(in);
            std::erase_if(s.sub.links, [&](const GLink& l) { return l.toNode == nn && l.toSocket == sid; });
            s.sub.links.push_back({nodeOfType(s.sub, kGroupInputType), in.name, nn, sid});
            commitGroup(s, [](Graph&, GNode&) {});
            return in.name;
        },
    });

    ops::registerCommand({
        .name = "texRemoveOutput",
        .doc = "Remove a node group's output, its link inside, and the links out of it on the Group nodes running the group.",
        .positionalDoc = "group name",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texRemoveOutput", "group"), "texRemoveOutput");
            const std::string name = groupArg(a, 1, "texRemoveOutput", "name");
            if (!s.sub.output(name))
                throw CommandError("texRemoveOutput: no output '" + name + "' in " + s.group);
            std::erase_if(s.sub.outputs, [&](const GraphOutput& o) { return o.name == name; });
            const std::string go = nodeOfType(s.sub, kGroupOutputType);
            std::erase_if(s.sub.links, [&](const GLink& l) { return l.toNode == go && l.toSocket == name; });
            commitGroup(s, [&](Graph& g, GNode& n) {
                std::erase_if(g.links, [&](const GLink& l) { return l.fromNode == n.name && l.fromSocket == name; });
            });
            return nullptr;
        },
    });

    ops::registerCommand({
        .name = "texRenameGroup",
        .doc = "Rename a node group (\"Set/Group\") and point the Group nodes running it at the new name. Returns the name, made unique.",
        .positionalDoc = "group newName",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texRenameGroup", "group"), "texRenameGroup");
            const std::string base = groupArg(a, 1, "texRenameGroup", "new name");
            if (!validName(base))
                throw CommandError("texRenameGroup: '" + base + "' can't name a group (no / or .)");
            if (base == s.group)
                return base;
            std::set<std::string> taken;
            for (const auto& [k, v] : s.top.subgroups)
                taken.insert(k);
            const std::string to = uniqueKey(taken, base);
            commitGroup(s, [&](Graph&, GNode& n) { n.values["group"] = to; });
            Graph top = readGraph(*s.set);
            top.subgroups[to] = top.subgroups[s.group];
            top.subgroups.erase(s.group);
            writeGraph(scene::Graph::current(), *s.set, top);
            return to;
        },
    });

    ops::registerCommand({
        .name = "texDeleteGroup",
        .doc = "Delete a node group (\"Set/Group\") and the Group nodes running it, with their links.",
        .positionalDoc = "group",
        .run = [](const Args& a) -> json {
            SetGraphs s = openGroup(groupArg(a, 0, "texDeleteGroup", "group"), "texDeleteGroup");
            auto drop = [&](Graph& g) {
                std::set<std::string> gone;
                for (const GNode& n : g.nodes)
                    if (n.type == kGroupType && n.values.value("group", std::string()) == s.group)
                        gone.insert(n.name);
                std::erase_if(g.links, [&](const GLink& l) { return gone.contains(l.fromNode) || gone.contains(l.toNode); });
                std::erase_if(g.nodes, [&](const GNode& n) { return gone.contains(n.name); });
            };
            s.top.subgroups.erase(s.group);
            drop(s.top);
            for (auto& [name, def] : s.top.subgroups)
            {
                Graph other = Graph::fromJson(def);
                drop(other);
                def = other.toJson();
            }
            writeGraph(scene::Graph::current(), *s.set, s.top);
            return true;
        },
    });

    ops::registerCommand({
        .name = "texSaveGroup",
        .doc = "Save a node group (\"Set/Group\") to the user's texture node library as a graph node, so Add Node offers it in every "
               "scene; a set name saves the set to the material library instead. Returns the file path.",
        .positionalDoc = "group",
        .undoable = false,
        .run = [](const Args& a) -> json {
            const std::string address = groupArg(a, 0, "texSaveGroup", "group");
            // A whole set keeps in the material library, the way its own Save to Library does.
            if (splitAddress(address).group.empty())
            {
                json r = ops::execute("saveLibraryMaterial", {{"args", {address}}});
                return r.is_object() ? r.value("file", json()) : r;
            }
            SetGraphs s = openGroup(address, "texSaveGroup");
            json graph = s.sub.toJson();
            // A library node carries the groups it runs, since it leaves the set behind.
            json subs = json::object();
            for (const auto& [name, def] : s.top.subgroups)
                if (name != s.group)
                    subs[name] = def;
            if (!subs.empty())
                graph["subgroups"] = subs;
            json def = {{"type", s.group}, {"label", s.group}, {"category", "Graphs"}, {"kind", "graph"}, {"graph", graph}};
            try
            {
                NodeDef::fromJson(def);
            }
            catch (const std::exception& e)
            {
                throw CommandError("texSaveGroup: " + s.group + ": " + e.what());
            }
            namespace fs = std::filesystem;
            const fs::path dir = nodeDirs().at(1);
            std::error_code ec;
            fs::create_directories(dir, ec);
            const fs::path path = dir / (s.group + ".lilatexnode");
            std::ofstream f(path, std::ios::binary);
            f << json{{"format", "lilatexnode"}, {"version", 1}, {"nodes", json::array({def})}}.dump(2);
            if (!f)
                throw CommandError("texSaveGroup: can't write " + path.string());
            f.close();
            reloadLibrary();
            return path.generic_string();
        },
    });
}
} // namespace lila::tex
