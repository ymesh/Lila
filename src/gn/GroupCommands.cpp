#include "gn/CommandArgs.h"
#include "gn/Exec.h"
#include "gn/Gn.h"

#include "ops/Command.h"
#include "core/Paths.h"
#include "core/Profile.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <functional>
#include <map>
#include <set>

/**
 * Group interface, nesting, sharing and whole-graph authoring commands: gnAddInput, gnRenameInput, gnSetInputInfo,
 * gnMoveInput, gnExposeOutput, gnRemoveOutput, gnGroupNodes, gnUngroup, gnSetActiveViewer, gnExportGroup,
 * gnImportGroup and gnBuildGroup.
 */
namespace lila::gn
{
namespace detail
{
void createGroupNode(scene::Graph& g, const Group& grp);
} // namespace detail

namespace
{
namespace fs = std::filesystem;
using scene::Graph;
using scene::Node;
using Arg = ArgType;

constexpr const char* kFileFormat = "lila.nodes";
constexpr int kFileVersion = 1;

std::string positional(const Args& a, size_t i, const char* cmd, const char* what)
{
    if (a.positional.size() <= i || !a.positional[i].is_string())
        throw CommandError(std::string(cmd) + ": " + what + " required");
    return a.positional[i].get<std::string>();
}

Node& groupNodeOf(const std::string& name, const char* cmd)
{
    Node* n = findGroupNode(Graph::current(), name);
    if (!n)
        throw CommandError(std::string(cmd) + ": no geometry-nodes group named '" + name + "'");
    return *n;
}

Group readOrFail(Node& n, const char* cmd)
{
    try
    {
        return readGroup(n);
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

void commitOrFail(Node& n, Group grp, const char* cmd, const std::map<std::string, std::string>& renames = {})
{
    try
    {
        commitGroup(Graph::current(), n, std::move(grp), renames);
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + e.what());
    }
}

json edit(const Args& a, const char* cmd, const std::function<json(Group&, std::map<std::string, std::string>&)>& fn)
{
    Node& n = groupNodeOf(positional(a, 0, cmd, "group"), cmd);
    Group grp = readOrFail(n, cmd);
    std::map<std::string, std::string> renames;
    json result = fn(grp, renames);
    commitOrFail(n, std::move(grp), cmd, renames);
    return result;
}

Sockets socketsOf(const Group& grp, const GNode& n)
{
    const NodeDecl* d = findDecl(n.type);
    return d ? d->socketsFor(grp, n) : Sockets{};
}

std::string joinIds(const std::vector<SocketDecl>& list)
{
    std::string out;
    for (const SocketDecl& s : list)
        out += (out.empty() ? "" : ", ") + s.id + " (" + socketTypeName(s.type) + ")";
    return out.empty() ? "none" : out;
}

std::string joinStrings(const std::vector<std::string>& list)
{
    std::string out;
    for (const std::string& s : list)
        out += (out.empty() ? "" : ", ") + s;
    return out;
}

bool valueFits(SocketType t, const json& v)
{
    switch (t)
    {
    case SocketType::Geometry:
    case SocketType::Bundle:
    case SocketType::Closure: return false;
    case SocketType::Matrix: return v.is_array() && v.size() == 16 && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); });
    case SocketType::String: return v.is_string();
    case SocketType::Vector:
    case SocketType::Rotation: return (v.is_array() && v.size() == 3 && v[0].is_number() && v[1].is_number() && v[2].is_number()) || v.is_number();
    case SocketType::Color:
        return (v.is_array() && (v.size() == 3 || v.size() == 4) && std::all_of(v.begin(), v.end(), [](const json& x) { return x.is_number(); })) ||
               v.is_number() || (v.is_string() && v.get<std::string>().starts_with("#"));
    default: return v.is_number() || v.is_boolean();
    }
}

std::string uniqueInputName(const Group& g, std::string base)
{
    if (!validInputName(base))
        base = "input";
    std::string name = base;
    for (int i = 1; g.input(name) || !validInputName(name); ++i)
        name = base + std::to_string(i);
    return name;
}

std::string uniqueOutputName(const Group& g, std::string base)
{
    if (!validIdentifier(base) || base == "geometry")
        base = "output";
    std::string name = base;
    for (int i = 1; g.output(name) || name == "geometry"; ++i)
        name = base + std::to_string(i);
    return name;
}

std::string groupInputName(Group& g, glm::dvec2 near)
{
    if (const GNode* gi = g.firstOfType("GroupInput"))
        return gi->name;
    std::string name = g.uniqueNodeName("GroupInput");
    g.nodes.push_back({name, "GroupInput", near, json::object()});
    return name;
}

std::string groupOutputName(Group& g)
{
    if (const GNode* go = g.firstOfType("GroupOutput"))
        return go->name;
    std::string name = g.uniqueNodeName("GroupOutput");
    g.nodes.push_back({name, "GroupOutput", {300.0, 0.0}, json::object()});
    return name;
}

/** Applies interface fields from JSON (gnSetInputInfo flags or a spec's input entry). "" or null clears a field. */
void applyInputInfo(GInput& in, const json& info, const char* cmd)
{
    auto str = [&](const char* key, std::string& dst) {
        if (auto it = info.find(key); it != info.end() && !it->is_null())
            dst = it->is_string() ? it->get<std::string>() : std::string();
    };
    auto num = [&](const char* key, std::optional<double>& dst) {
        if (auto it = info.find(key); it != info.end() && !it->is_null())
        {
            if (it->is_number())
                dst = it->get<double>();
            else if (it->is_string() && it->get<std::string>().empty())
                dst.reset();
            else
                throw CommandError(std::string(cmd) + ": " + key + " takes a number");
        }
    };
    str("label", in.label);
    str("description", in.description);
    str("tooltip", in.description);
    str("panel", in.panel);
    if (auto it = info.find("subtype"); it != info.end() && !it->is_null())
    {
        std::string st = it->is_string() ? it->get<std::string>() : std::string();
        static const std::vector<std::string> ok = {"", "none", "factor", "percentage", "angle", "distance", "color"};
        if (std::find(ok.begin(), ok.end(), st) == ok.end())
            throw CommandError(std::string(cmd) + ": subtype must be one of factor, percentage, angle, distance, color (or empty)");
        in.subtype = st == "none" ? "" : st;
    }
    num("min", in.min);
    num("max", in.max);
    num("softMin", in.softMin);
    num("softMax", in.softMax);
    num("step", in.step);
    if (auto it = info.find("default"); it != info.end() && !it->is_null())
    {
        if (!valueFits(in.type, *it))
            throw CommandError(std::string(cmd) + ": default for " + in.name + " must be a " + socketTypeName(in.type) + ", got " + it->dump());
        in.defaultValue = *it;
    }
    if (in.min && in.max && *in.min > *in.max)
        throw CommandError(std::string(cmd) + ": min is above max");
}

json inputJson(const GInput& in)
{
    Group g;
    g.inputs = {in};
    json j = g.toJson()["inputs"][0];
    j["displayName"] = in.displayName();
    return j;
}

// ---------------------------------------------------------------------------------------------------------------------
// Sharing

/** Every group the definition depends on, deepest first, each once. */
void collectDependencies(const Group& g, std::vector<json>& out, std::set<std::string>& seen)
{
    for (const auto& [name, def] : g.subgroups)
    {
        if (seen.contains(name))
            continue;
        Group sub = Group::fromJson(def);
        collectDependencies(sub, out, seen);
        if (seen.insert(name).second)
            out.push_back(def);
    }
}

json exportDocument(const Group& g)
{
    std::vector<json> deps;
    std::set<std::string> seen;
    collectDependencies(g, deps, seen);
    return {{"format", kFileFormat}, {"version", kFileVersion}, {"group", g.toJson()}, {"dependencies", deps}};
}

/** Groups of a .lilanodes document (dependencies first) or of a bare group definition. */
std::vector<Group> groupsInDocument(const json& doc)
{
    std::vector<Group> out;
    if (doc.contains("format"))
    {
        if (doc["format"] != kFileFormat)
            throw std::invalid_argument("not a Lila node group file (format " + doc["format"].dump() + ")");
        int version = doc.value("version", 1);
        if (version > kFileVersion)
            throw std::invalid_argument("file version " + std::to_string(version) + " is newer than this Lila reads (" + std::to_string(kFileVersion) + ")");
        for (const json& d : doc.value("dependencies", json::array()))
            out.push_back(Group::fromJson(d));
        out.push_back(Group::fromJson(doc.at("group")));
        return out;
    }
    out.push_back(Group::fromJson(doc));
    return out;
}

/** Brings a group into the scene: replaces the definition of an existing one (its modifiers update) or creates it. */
void installGroup(Group grp)
{
    if (Node* existing = findGroupNode(Graph::current(), grp.name))
    {
        Group cur = readGroup(*existing);
        grp.typeVersion = cur.typeVersion;
        commitGroup(Graph::current(), *existing, std::move(grp));
        return;
    }
    if (!validIdentifier(grp.name))
        throw std::invalid_argument("'" + grp.name + "' isn't a valid group name");
    grp.typeVersion = 1;
    detail::createGroupNode(Graph::current(), grp);
}

std::vector<std::string> importFile(const fs::path& path, const char* cmd)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        throw CommandError(std::string(cmd) + ": can't read " + paths::toUtf8(path));
    std::vector<std::string> names;
    try
    {
        json doc = json::parse(in);
        for (Group& g : groupsInDocument(doc))
        {
            names.push_back(g.name);
            installGroup(std::move(g));
        }
    }
    catch (const CommandError&)
    {
        throw;
    }
    catch (const std::exception& e)
    {
        throw CommandError(std::string(cmd) + ": " + paths::toUtf8(path) + ": " + e.what());
    }
    return names;
}

// ---------------------------------------------------------------------------------------------------------------------
// Collapse and ungroup

struct Collapsed
{
    Group inner;
    std::string node;
};

Collapsed collapse(Group& outer, const std::vector<std::string>& names, const std::string& innerName, const char* cmd)
{
    std::set<std::string> sel(names.begin(), names.end());
    if (sel.empty())
        throw CommandError(std::string(cmd) + ": no nodes given");
    glm::dvec2 lo(1e30), hi(-1e30);
    Collapsed out;
    out.inner = Group::makeDefault(innerName);
    out.inner.links.clear();
    for (const std::string& n : sel)
    {
        const GNode* gn = outer.node(n);
        if (!gn)
            throw CommandError(std::string(cmd) + ": no node '" + n + "' in " + outer.name);
        if (gn->type == "GroupInput" || gn->type == "GroupOutput")
            throw CommandError(std::string(cmd) + ": the Group Input and Group Output nodes can't be grouped");
        out.inner.nodes.push_back(*gn);
        lo = glm::min(lo, gn->position);
        hi = glm::max(hi, gn->position);
    }
    GNode* gi = out.inner.node("GroupInput");
    GNode* go = out.inner.node("GroupOutput");
    gi->position = {lo.x - 300.0, (lo.y + hi.y) / 2.0};
    go->position = {hi.x + 300.0, (lo.y + hi.y) / 2.0};

    std::string node = outer.uniqueNodeName(innerName);
    std::vector<GLink> outerLinks;
    std::map<std::string, std::string> inputFor;  // "node.socket" outside -> inner input name
    std::map<std::string, std::string> outputFor; // "node.socket" inside -> inner output name
    bool geometryIn = false, geometryOut = false;
    std::string geometryInSource, geometryOutSource;
    for (const GLink& l : outer.links)
    {
        bool fromIn = sel.contains(l.fromNode), toIn = sel.contains(l.toNode);
        if (fromIn && toIn)
            out.inner.links.push_back(l);
        else if (toIn)
        {
            const GNode* target = outer.node(l.toNode);
            Sockets ts = socketsOf(outer, *target);
            int ii = ts.inputIndex(l.toSocket);
            if (ii < 0)
                continue;
            const SocketDecl& sd = ts.inputs[ii];
            std::string key = l.fromNode + "." + l.fromSocket;
            std::string name;
            if (sd.type == SocketType::Geometry)
            {
                if (geometryIn && geometryInSource != key)
                    throw CommandError(std::string(cmd) + ": the nodes take geometry from more than one place; a group has one geometry input");
                geometryIn = true;
                geometryInSource = key;
                name = "geometry";
            }
            else if (auto it = inputFor.find(key); it != inputFor.end())
                name = it->second;
            else
            {
                name = uniqueInputName(out.inner, l.toSocket);
                auto v = target->values.find(l.toSocket);
                out.inner.inputs.push_back({.name = name, .type = sd.type, .defaultValue = v != target->values.end() ? *v : sd.defaultValue});
                inputFor[key] = name;
            }
            out.inner.links.push_back({"GroupInput", name, l.toNode, l.toSocket});
            GLink ol{l.fromNode, l.fromSocket, node, name};
            if (std::find(outerLinks.begin(), outerLinks.end(), ol) == outerLinks.end())
                outerLinks.push_back(ol);
        }
        else if (fromIn)
        {
            const GNode* source = outer.node(l.fromNode);
            Sockets ss = socketsOf(outer, *source);
            int oi = ss.outputIndex(l.fromSocket);
            if (oi < 0)
                continue;
            const SocketDecl& sd = ss.outputs[oi];
            std::string key = l.fromNode + "." + l.fromSocket;
            std::string name;
            if (sd.type == SocketType::Geometry)
            {
                if (geometryOut && geometryOutSource != key)
                    throw CommandError(std::string(cmd) + ": more than one geometry leaves the nodes; a group has one geometry output");
                geometryOut = true;
                geometryOutSource = key;
                name = "geometry";
            }
            else if (auto it = outputFor.find(key); it != outputFor.end())
                name = it->second;
            else
            {
                name = uniqueOutputName(out.inner, l.fromSocket);
                out.inner.outputs.push_back({name, sd.type, {}});
                outputFor[key] = name;
            }
            GLink il{l.fromNode, l.fromSocket, "GroupOutput", name};
            if (std::find(out.inner.links.begin(), out.inner.links.end(), il) == out.inner.links.end())
                out.inner.links.push_back(il);
            outerLinks.push_back({node, name, l.toNode, l.toSocket});
        }
        else
            outerLinks.push_back(l);
    }
    std::erase_if(outer.nodes, [&](const GNode& n) { return sel.contains(n.name); });
    outer.links = std::move(outerLinks);
    outer.nodes.push_back({node, kGroupNodeDeclType, (lo + hi) / 2.0, json{{"group", innerName}}});
    out.node = node;
    return out;
}

json ungroup(Group& outer, const std::string& nodeName, const char* cmd)
{
    const GNode* gptr = outer.node(nodeName);
    if (!gptr || gptr->type != kGroupNodeDeclType)
        throw CommandError(std::string(cmd) + ": " + nodeName + " is not a Group node in " + outer.name);
    GNode gnode = *gptr;
    std::string innerName = gnode.values.value("group", std::string());
    auto def = outer.subgroups.find(innerName);
    if (def == outer.subgroups.end())
        throw CommandError(std::string(cmd) + ": " + nodeName + " uses group '" + innerName + "', which isn't available");
    Group inner = Group::fromJson(def->second);
    std::string giName = inner.firstOfType("GroupInput") ? inner.firstOfType("GroupInput")->name : "";
    std::string goName = inner.firstOfType("GroupOutput") ? inner.firstOfType("GroupOutput")->name : "";

    glm::dvec2 centre(0.0);
    int count = 0;
    for (const GNode& n : inner.nodes)
        if (n.type != "GroupInput" && n.type != "GroupOutput")
        {
            centre += n.position;
            ++count;
        }
    if (count)
        centre /= double(count);

    std::map<std::string, std::string> renamed;
    json added = json::array();
    std::vector<GNode> fresh;
    for (const GNode& n : inner.nodes)
    {
        if (n.type == "GroupInput" || n.type == "GroupOutput")
            continue;
        GNode copy = n;
        copy.name = outer.uniqueNodeName(n.name);
        // Reserve the name before the next one is chosen.
        outer.nodes.push_back({copy.name, "__reserved", {}, json::object()});
        copy.position = gnode.position + (n.position - centre);
        renamed[n.name] = copy.name;
        fresh.push_back(std::move(copy));
        added.push_back(renamed[n.name]);
    }
    std::erase_if(outer.nodes, [](const GNode& n) { return n.type == "__reserved"; });
    for (GNode& n : fresh)
        outer.nodes.push_back(std::move(n));
    // Inner socket values carry over into the inlined nodes' unlinked inputs.
    for (const GLink& l : inner.links)
    {
        bool fromGi = l.fromNode == giName, toGo = l.toNode == goName;
        std::vector<std::pair<std::string, std::string>> sources; // outer node.socket feeding this inner link
        if (fromGi)
        {
            for (const GLink& ol : outer.links)
                if (ol.toNode == nodeName && ol.toSocket == l.fromSocket)
                    sources.push_back({ol.fromNode, ol.fromSocket});
        }
        else
            sources.push_back({renamed[l.fromNode], l.fromSocket});
        if (toGo)
        {
            for (const GLink& ol : outer.links)
                if (ol.fromNode == nodeName && ol.fromSocket == l.toSocket)
                    for (auto& [sn, ss] : sources)
                        outer.links.push_back({sn, ss, ol.toNode, ol.toSocket});
            continue;
        }
        std::string target = renamed[l.toNode];
        if (fromGi && sources.empty())
        {
            if (GNode* t = outer.node(target))
            {
                const GInput* in = inner.input(l.fromSocket);
                auto v = gnode.values.find(l.fromSocket);
                json value = v != gnode.values.end() ? *v : in ? in->defaultValue : json();
                if (!value.is_null() && l.fromSocket != "geometry")
                    t->values[l.toSocket] = value;
            }
            continue;
        }
        for (auto& [sn, ss] : sources)
            outer.links.push_back({sn, ss, target, l.toSocket});
    }
    std::erase_if(outer.links, [&](const GLink& l) { return l.fromNode == nodeName || l.toNode == nodeName; });
    std::erase_if(outer.nodes, [&](const GNode& n) { return n.name == nodeName; });
    return added;
}

// ---------------------------------------------------------------------------------------------------------------------
// Whole-graph builder

struct SpecError
{
    std::vector<std::string> lines;
    void add(std::string s) { lines.push_back(std::move(s)); }
};

std::pair<std::string, std::string> parseEnd(const json& j)
{
    if (!j.is_string())
        return {};
    std::string s = j.get<std::string>();
    auto dot = s.find('.');
    if (dot == std::string::npos)
        return {s, ""};
    return {s.substr(0, dot), s.substr(dot + 1)};
}

/** Every link of a spec as [from, to]: "A.x -> B.y" strings, [from, to] pairs or [fromNode, fromSocket, toNode, toSocket]. */
std::vector<std::pair<std::string, std::string>> specLinks(const json& links, SpecError& err)
{
    std::vector<std::pair<std::string, std::string>> out;
    if (!links.is_array())
    {
        if (!links.is_null())
            err.add("links must be a list");
        return out;
    }
    for (const json& l : links)
    {
        if (l.is_string())
        {
            std::string s = l.get<std::string>();
            auto arrow = s.find("->");
            if (arrow == std::string::npos)
            {
                err.add("link '" + s + "' needs the form 'Node.output -> Node.input'");
                continue;
            }
            auto trim = [](std::string x) {
                x.erase(0, x.find_first_not_of(" \t"));
                x.erase(x.find_last_not_of(" \t") + 1);
                return x;
            };
            out.push_back({trim(s.substr(0, arrow)), trim(s.substr(arrow + 2))});
        }
        else if (l.is_array() && l.size() == 2 && l[0].is_string() && l[1].is_string())
            out.push_back({l[0].get<std::string>(), l[1].get<std::string>()});
        else if (l.is_array() && l.size() == 4)
            out.push_back({l[0].get<std::string>() + "." + l[1].get<std::string>(), l[2].get<std::string>() + "." + l[3].get<std::string>()});
        else
            err.add("link " + l.dump() + " isn't 'A.out -> B.in', [from, to] or [fromNode, fromSocket, toNode, toSocket]");
    }
    return out;
}

/** Lays nodes out in columns by their longest path from a source, for specs without positions. */
void autoLayout(Group& g, const std::set<std::string>& placed)
{
    std::map<std::string, int> depth;
    std::function<int(const std::string&, int)> d = [&](const std::string& n, int guard) -> int {
        if (auto it = depth.find(n); it != depth.end())
            return it->second;
        if (guard > 256)
            return 0;
        int best = 0;
        for (const GLink& l : g.links)
            if (l.toNode == n)
                best = std::max(best, d(l.fromNode, guard + 1) + 1);
        depth[n] = best;
        return best;
    };
    std::map<int, int> rows;
    for (GNode& n : g.nodes)
    {
        if (placed.contains(n.name))
            continue;
        int col = d(n.name, 0);
        int row = rows[col]++;
        n.position = {double(col) * 260.0 - 300.0, double(row) * 180.0};
    }
}

/** Builds a group from a spec. Problems are collected rather than thrown one at a time. */
Group buildFromSpec(const json& spec, SpecError& err, std::map<std::string, json>& available)
{
    Group g;
    if (!spec.is_object())
    {
        err.add("the spec must be an object with name, inputs, nodes and links");
        return g;
    }
    g.name = spec.value("name", std::string());
    if (!validIdentifier(g.name))
        err.add("name '" + g.name + "' isn't valid (letters, digits, underscores, not starting with a digit)");
    g.description = spec.value("description", std::string());

    for (const json& ij : spec.value("inputs", json::array()))
    {
        GInput in;
        in.name = ij.value("name", std::string());
        if (!validInputName(in.name))
        {
            err.add("input '" + in.name + "' can't be a parameter name");
            continue;
        }
        if (!parseSocketType(ij.value("type", "float"), &in.type) || in.type == SocketType::Geometry)
        {
            err.add("input " + in.name + ": type must be float, int, bool, vector, rotation, color or string");
            continue;
        }
        try
        {
            applyInputInfo(in, ij, "gnBuildGroup");
        }
        catch (const CommandError& e)
        {
            err.add(e.what());
        }
        if (g.input(in.name))
            err.add("input " + in.name + " is listed twice");
        g.inputs.push_back(std::move(in));
    }
    for (const json& oj : spec.value("outputs", json::array()))
    {
        GOutput o;
        o.name = oj.value("name", std::string());
        if (!validIdentifier(o.name) || o.name == "geometry")
        {
            err.add("output '" + o.name + "' isn't a valid name (geometry is always there)");
            continue;
        }
        if (!parseSocketType(oj.value("type", "float"), &o.type) || o.type == SocketType::Geometry)
        {
            err.add("output " + o.name + ": type must be a data type");
            continue;
        }
        o.description = oj.value("description", std::string());
        g.outputs.push_back(std::move(o));
    }

    std::set<std::string> placed;
    bool hasIn = false, hasOut = false;
    for (const json& nj : spec.value("nodes", json::array()))
    {
        GNode n;
        n.name = nj.value("name", std::string());
        n.type = nj.value("type", std::string());
        if (n.type == "GroupInput")
            hasIn = true;
        if (n.type == "GroupOutput")
            hasOut = true;
        if (n.name.empty())
            n.name = n.type;
        if (!validIdentifier(n.name))
        {
            err.add("node name '" + n.name + "' isn't valid");
            continue;
        }
        if (g.node(n.name))
        {
            err.add("node " + n.name + " is listed twice");
            continue;
        }
        const NodeDecl* d = findDecl(n.type);
        if (!d)
        {
            std::vector<std::string> close;
            for (const NodeDecl* x : allDecls())
            {
                std::string a = x->type, b = n.type;
                std::transform(a.begin(), a.end(), a.begin(), ::tolower);
                std::transform(b.begin(), b.end(), b.begin(), ::tolower);
                if (!b.empty() && (a.find(b) != std::string::npos || b.find(a) != std::string::npos))
                    close.push_back(x->type);
            }
            err.add("node " + n.name + ": unknown type '" + n.type + "'" + (close.empty() ? "; gnNodeTypes lists them" : "; did you mean " + joinStrings(close) + "?"));
            continue;
        }
        if (auto p = nj.find("position"); p != nj.end() && p->is_array() && p->size() == 2)
        {
            n.position = {(*p)[0].get<double>(), (*p)[1].get<double>()};
            placed.insert(n.name);
        }
        json values = json::object();
        for (const char* key : {"props", "values"})
            if (auto v = nj.find(key); v != nj.end() && v->is_object())
                for (auto& [k, x] : v->items())
                    values[k] = x;
        if (nj.contains("group"))
            values["group"] = nj["group"];
        // Properties first: they decide the sockets that values are checked against.
        for (auto& [k, x] : values.items())
            if (const PropDecl* p = d->prop(k))
            {
                if (!x.is_string())
                {
                    err.add(n.name + "." + k + " takes a string");
                    continue;
                }
                if (!p->options.empty() && std::find(p->options.begin(), p->options.end(), x.get<std::string>()) == p->options.end())
                {
                    err.add(n.name + "." + k + " must be one of " + joinStrings(p->options) + ", got '" + x.get<std::string>() + "'");
                    continue;
                }
                n.values[k] = x;
            }
        if (runsGroup(n.type))
        {
            std::string ref = n.values.value("group", std::string());
            if (ref.empty())
                err.add(n.name + ": a " + n.type + " node needs \"group\": the name of the group to run");
            else if (auto it = available.find(ref); it != available.end())
                g.subgroups[ref] = it->second;
            else
                err.add(n.name + ": no group named '" + ref + "' in the scene, the library or the spec's dependencies");
        }
        g.nodes.push_back(n);
        GNode& stored = g.nodes.back();
        Sockets s = d->socketsFor(g, stored);
        for (auto& [k, x] : values.items())
        {
            if (d->prop(k) || k == "active")
            {
                if (k == "active")
                    stored.values[k] = x;
                continue;
            }
            int ii = s.inputIndex(k);
            if (ii < 0)
            {
                std::vector<std::string> props;
                for (const PropDecl& p : d->props)
                    props.push_back(p.id);
                err.add(n.name + " (" + n.type + ") has no input or property '" + k + "'; inputs: " + joinIds(s.inputs) +
                        (props.empty() ? "" : "; properties: " + joinStrings(props)));
                continue;
            }
            if (!valueFits(s.inputs[ii].type, x))
            {
                err.add(n.name + "." + k + " takes a " + std::string(socketTypeName(s.inputs[ii].type)) + ", got " + x.dump());
                continue;
            }
            stored.values[k] = x;
        }
    }
    if (!hasIn)
        g.nodes.insert(g.nodes.begin(), GNode{"GroupInput", "GroupInput", {-300.0, 0.0}, json::object()});
    if (!hasOut)
        g.nodes.push_back({"GroupOutput", "GroupOutput", {300.0, 0.0}, json::object()});

    for (auto& [from, to] : specLinks(spec.value("links", json()), err))
    {
        auto [fn, fs] = parseEnd(json(from));
        auto [tn, ts] = parseEnd(json(to));
        GNode* a = g.node(fn);
        GNode* b = g.node(tn);
        if (!a || !b)
        {
            err.add("link " + from + " -> " + to + ": no node '" + (!a ? fn : tn) + "'");
            continue;
        }
        Sockets sa = socketsOf(g, *a), sb = socketsOf(g, *b);
        int oi = sa.outputIndex(fs), ii = sb.inputIndex(ts);
        if (oi < 0)
        {
            err.add("link " + from + " -> " + to + ": " + fn + " (" + a->type + ") has no output '" + fs + "'; outputs: " + joinIds(sa.outputs));
            continue;
        }
        if (ii < 0)
        {
            err.add("link " + from + " -> " + to + ": " + tn + " (" + b->type + ") has no input '" + ts + "'; inputs: " + joinIds(sb.inputs));
            continue;
        }
        if (!linkable(sa.outputs[oi].type, sb.inputs[ii].type))
        {
            err.add("link " + from + " -> " + to + ": can't link " + socketTypeName(sa.outputs[oi].type) + " to " + socketTypeName(sb.inputs[ii].type));
            continue;
        }
        GLink l{fn, fs, tn, ts};
        if (!sb.inputs[ii].multi)
            std::erase_if(g.links, [&](const GLink& x) { return x.toNode == tn && x.toSocket == ts; });
        g.links.push_back(l);
    }
    if (err.lines.empty())
    {
        PlanPtr plan = compile(g);
        if (!plan->error.empty())
            err.add(plan->error);
    }
    autoLayout(g, placed);
    return g;
}

/** Group definitions a spec may refer to by name: the scene's and the libraries'. */
std::map<std::string, json> availableGroups()
{
    std::map<std::string, json> out;
    for (const fs::path& dir : libraryDirs())
        for (const auto& e : fs::directory_iterator(dir))
        {
            std::string ext = e.path().extension().string();
            if (ext != ".json" && ext != ".lilanodes")
                continue;
            try
            {
                std::ifstream in(e.path(), std::ios::binary);
                for (Group& g : groupsInDocument(json::parse(in)))
                    out[g.name] = g.toJson();
            }
            catch (const std::exception&)
            {
            }
        }
    for (const std::string& name : groupNames(Graph::current()))
        if (Node* n = findGroupNode(Graph::current(), name))
            try
            {
                out[name] = readGroup(*n).toJson();
            }
            catch (const std::exception&)
            {
            }
    return out;
}

const std::vector<FlagDef>& infoFlags()
{
    static const std::vector<FlagDef> f = {
        {"label", "", Arg::String, nullptr, "Nice name shown in editors."},
        {"description", "", Arg::String, nullptr, "Tooltip and description."},
        {"tooltip", "", Arg::String, nullptr, "Same as description."},
        {"subtype", "", Arg::String, nullptr, "factor, percentage, angle (float in degrees, radians in the graph), distance, color; empty clears.",
         {"", "factor", "percentage", "angle", "distance", "color"}},
        {"min", "", Arg::Any, nullptr, "Hard minimum; empty string clears."},
        {"max", "", Arg::Any, nullptr, "Hard maximum; empty string clears."},
        {"softMin", "", Arg::Any, nullptr, "Slider minimum; empty string clears."},
        {"softMax", "", Arg::Any, nullptr, "Slider maximum; empty string clears."},
        {"step", "", Arg::Any, nullptr, "Drag and spin step; empty string clears."},
        {"default", "", Arg::Any, nullptr, "Default value."},
        {"panel", "", Arg::String, nullptr, "Panel (category) the input shows under; empty for none."},
    };
    return f;
}
} // namespace

namespace detail
{
void registerGroupCommands()
{
    ops::registerCommand({
        .name = "gnAddInput",
        .doc = "Add an exposed group input that isn't wired to anything yet (link it from GroupInput.<name>). Takes the same interface "
               "flags as gnSetInputInfo. Returns the name.",
        .positionalDoc = "group name type",
        .flags = infoFlags(),
        .run = [](const Args& a) -> json {
            std::string name = positional(a, 1, "gnAddInput", "name"), type = positional(a, 2, "gnAddInput", "type");
            if (!validInputName(name))
                throw CommandError("gnAddInput: '" + name + "' can't be used as a parameter name");
            GInput in{.name = name};
            if (!parseSocketType(type, &in.type) || in.type == SocketType::Geometry)
                throw CommandError("gnAddInput: type must be float, int, bool, vector, rotation, color or string");
            return edit(a, "gnAddInput", [&](Group& grp, auto&) -> json {
                if (grp.input(name))
                    throw CommandError("gnAddInput: " + grp.name + " already has an input " + name);
                applyInputInfo(in, a.flags, "gnAddInput");
                grp.inputs.push_back(in);
                groupInputName(grp, {-300.0, 0.0});
                return name;
            });
        },
    });

    ops::registerCommand({
        .name = "gnRenameInput",
        .doc = "Rename an exposed input (or, with output=True, a group output). Links follow, and modifiers keep the value and "
               "animation under the new name. Returns the new name.",
        .positionalDoc = "group old new",
        .flags = {{"output", "", Arg::Bool, false, "Rename a group output instead."}},
        .run = [](const Args& a) -> json {
            std::string from = positional(a, 1, "gnRenameInput", "old name"), to = positional(a, 2, "gnRenameInput", "new name");
            bool output = a.get<bool>("output", false);
            return edit(a, "gnRenameInput", [&](Group& grp, std::map<std::string, std::string>& renames) -> json {
                if (from == to)
                    return to;
                if (output)
                {
                    auto it = std::find_if(grp.outputs.begin(), grp.outputs.end(), [&](const GOutput& o) { return o.name == from; });
                    if (it == grp.outputs.end())
                        throw CommandError("gnRenameInput: no output '" + from + "'");
                    if (!validIdentifier(to) || to == "geometry" || grp.output(to))
                        throw CommandError("gnRenameInput: '" + to + "' can't be used as an output name");
                    it->name = to;
                    for (GLink& l : grp.links)
                        if (grp.node(l.toNode) && grp.node(l.toNode)->type == "GroupOutput" && l.toSocket == from)
                            l.toSocket = to;
                    for (GTie& t : grp.ties)
                        for (GLink& l : t.links)
                            if (grp.node(l.toNode) && grp.node(l.toNode)->type == "GroupOutput" && l.toSocket == from)
                                l.toSocket = to;
                    return to;
                }
                GInput* in = grp.input(from);
                if (!in)
                    throw CommandError("gnRenameInput: no input '" + from + "'");
                if (!validInputName(to) || grp.input(to))
                    throw CommandError("gnRenameInput: '" + to + "' can't be used as a parameter name");
                in->name = to;
                for (GLink& l : grp.links)
                    if (grp.node(l.fromNode) && grp.node(l.fromNode)->type == "GroupInput" && l.fromSocket == from)
                        l.fromSocket = to;
                for (GTie& t : grp.ties)
                    for (GLink& l : t.links)
                        if (grp.node(l.fromNode) && grp.node(l.fromNode)->type == "GroupInput" && l.fromSocket == from)
                            l.fromSocket = to;
                renames[from] = to;
                return to;
            });
        },
    });

    ops::registerCommand({
        .name = "gnSetInputInfo",
        .doc = "Set an exposed input's interface: label, description (tooltip), subtype, min/max, softMin/softMax, step, default and "
               "panel. Editors show the label, tooltip and limits on the modifier. Returns the input's interface.",
        .positionalDoc = "group input",
        .flags = infoFlags(),
        .run = [](const Args& a) -> json {
            std::string name = positional(a, 1, "gnSetInputInfo", "input");
            return edit(a, "gnSetInputInfo", [&](Group& grp, auto&) -> json {
                GInput* in = grp.input(name);
                if (!in)
                    throw CommandError("gnSetInputInfo: no input '" + name + "' in " + grp.name);
                json info = json::object();
                for (const FlagDef& f : infoFlags())
                    if (a.flags.contains(f.name) && !a.flags[f.name].is_null())
                        info[f.name] = a.flags[f.name];
                applyInputInfo(*in, info, "gnSetInputInfo");
                return inputJson(*in);
            });
        },
    });

    ops::registerCommand({
        .name = "gnMoveInput",
        .doc = "Move an exposed input (or, with output=True, a group output) to a position in the interface order (0 is first), "
               "optionally into a panel.",
        .positionalDoc = "group input index",
        .flags = {{"panel", "", Arg::String, nullptr, "Panel to put it in; empty for none."},
                  {"output", "", Arg::Bool, false, "Move a group output instead."}},
        .run = [](const Args& a) -> json {
            std::string name = positional(a, 1, "gnMoveInput", "input");
            if (a.positional.size() < 3 || !a.positional[2].is_number_integer())
                throw CommandError("gnMoveInput: index required");
            int64_t index = a.positional[2].get<int64_t>();
            return edit(a, "gnMoveInput", [&](Group& grp, auto&) -> json {
                if (a.get<bool>("output", false))
                {
                    auto it = std::find_if(grp.outputs.begin(), grp.outputs.end(), [&](const GOutput& o) { return o.name == name; });
                    if (it == grp.outputs.end())
                        throw CommandError("gnMoveInput: no output '" + name + "'");
                    GOutput o = *it;
                    grp.outputs.erase(it);
                    grp.outputs.insert(grp.outputs.begin() + std::clamp<int64_t>(index, 0, int64_t(grp.outputs.size())), o);
                    json order = json::array();
                    for (const GOutput& x : grp.outputs)
                        order.push_back(x.name);
                    return order;
                }
                auto it = std::find_if(grp.inputs.begin(), grp.inputs.end(), [&](const GInput& in) { return in.name == name; });
                if (it == grp.inputs.end())
                    throw CommandError("gnMoveInput: no input '" + name + "'");
                GInput in = *it;
                grp.inputs.erase(it);
                if (a.has("panel"))
                    in.panel = a.get<std::string>("panel");
                size_t at = size_t(std::clamp<int64_t>(index, 0, int64_t(grp.inputs.size())));
                grp.inputs.insert(grp.inputs.begin() + at, in);
                json order = json::array();
                for (const GInput& x : grp.inputs)
                    order.push_back(x.name);
                return order;
            });
        },
    });

    ops::registerCommand({
        .name = "gnExposeOutput",
        .doc = "Make an output socket a group output: it becomes a Group Output input, and an output of Group nodes that run this "
               "group. Only geometry reaches a modifier stack. Returns the output name.",
        .positionalDoc = "group node.socket [name]",
        .run = [](const Args& a) -> json {
            auto [nn, sid] = splitPlug(positional(a, 1, "gnExposeOutput", "node.socket"), "gnExposeOutput");
            std::string name = a.positional.size() > 2 && a.positional[2].is_string() ? a.positional[2].get<std::string>() : sid;
            return edit(a, "gnExposeOutput", [&](Group& grp, auto&) -> json {
                const GNode* n = grp.node(nn);
                if (!n)
                    throw CommandError("gnExposeOutput: no node '" + nn + "'");
                Sockets s = socketsOf(grp, *n);
                int oi = s.outputIndex(sid);
                if (oi < 0)
                    throw CommandError("gnExposeOutput: " + nn + " has no output '" + sid + "'; outputs: " + joinIds(s.outputs));
                SocketType t = s.outputs[oi].type;
                std::string goName = groupOutputName(grp);
                if (t == SocketType::Geometry)
                    name = "geometry";
                else if (const GOutput* existing = grp.output(name); existing && existing->type != t)
                    throw CommandError("gnExposeOutput: " + name + " is already a " + socketTypeName(existing->type) + " output");
                else if (!existing)
                {
                    if (!validIdentifier(name) || name == "geometry")
                        throw CommandError("gnExposeOutput: '" + name + "' can't be used as an output name");
                    grp.outputs.push_back({name, t, {}});
                }
                std::erase_if(grp.links, [&](const GLink& l) { return l.toNode == goName && l.toSocket == name; });
                grp.links.push_back({nn, sid, goName, name});
                return name;
            });
        },
    });

    ops::registerCommand({
        .name = "gnRemoveOutput",
        .doc = "Remove a group output and its links.",
        .positionalDoc = "group name",
        .run = [](const Args& a) -> json {
            std::string name = positional(a, 1, "gnRemoveOutput", "name");
            return edit(a, "gnRemoveOutput", [&](Group& grp, auto&) -> json {
                if (!grp.output(name))
                    throw CommandError("gnRemoveOutput: no output '" + name + "'");
                std::erase_if(grp.outputs, [&](const GOutput& o) { return o.name == name; });
                return nullptr;
            });
        },
    });

    ops::registerCommand({
        .name = "gnGroupNodes",
        .doc = "Collapse nodes into a new group and put a Group node running it in their place. Links crossing the boundary become "
               "the new group's inputs and outputs. Returns {group, node}.",
        .positionalDoc = "group nodes...",
        .flags = {{"name", "", Arg::String, "NodeGroup", "Name for the new group; made unique."}},
        .run = [](const Args& a) -> json {
            Node& n = groupNodeOf(positional(a, 0, "gnGroupNodes", "group"), "gnGroupNodes");
            Group outer = readOrFail(n, "gnGroupNodes");
            std::string base = a.get<std::string>("name", "NodeGroup");
            if (!validIdentifier(base))
                throw CommandError("gnGroupNodes: '" + base + "' isn't a valid group name");
            std::string innerName = base;
            for (int i = 1; Graph::current().find(innerName); ++i)
                innerName = base + std::to_string(i);
            std::vector<std::string> names;
            for (size_t i = 1; i < a.positional.size(); ++i)
                if (a.positional[i].is_string())
                    names.push_back(a.positional[i].get<std::string>());
            Collapsed c = collapse(outer, names, innerName, "gnGroupNodes");
            try
            {
                detail::createGroupNode(Graph::current(), c.inner);
            }
            catch (const std::exception& e)
            {
                throw CommandError(std::string("gnGroupNodes: ") + e.what());
            }
            commitOrFail(n, std::move(outer), "gnGroupNodes");
            return {{"group", innerName}, {"node", c.node}};
        },
    });

    ops::registerCommand({
        .name = "gnRenameGroup",
        .doc = "Rename a group, its modifiers' type (they keep their values) and the Group nodes running it. Returns the new name.",
        .positionalDoc = "group newName",
        .run = [](const Args& a) -> json {
            Node& n = groupNodeOf(positional(a, 0, "gnRenameGroup", "group"), "gnRenameGroup");
            const std::string to = positional(a, 1, "gnRenameGroup", "new name");
            try
            {
                renameGroup(Graph::current(), n, to);
            }
            catch (const std::invalid_argument& e)
            {
                throw CommandError(std::string("gnRenameGroup: ") + e.what());
            }
            return to;
        },
    });

    ops::registerCommand({
        .name = "gnDeleteGroup",
        .doc = "Delete a group nothing uses. Fails naming the modifiers and groups still running it.",
        .positionalDoc = "group",
        .run = [](const Args& a) -> json {
            const std::string name = positional(a, 0, "gnDeleteGroup", "group");
            Graph& g = Graph::current();
            Node& n = groupNodeOf(name, "gnDeleteGroup");
            std::vector<std::string> users;
            for (scene::NodeId id : instancesOf(g, n.id()))
                users.push_back(g.node(id)->name());
            for (const std::string& other : groupNames(g))
                if (other != name)
                    if (std::vector<std::string> refs = referencedGroups(readGroup(*findGroupNode(g, other)));
                        std::find(refs.begin(), refs.end(), name) != refs.end())
                        users.push_back(other);
            if (!users.empty())
                throw CommandError("gnDeleteGroup: " + name + " is still used by " + joinStrings(users));
            g.deleteNode(n.id());
            return true;
        },
    });

    ops::registerCommand({
        .name = "gnUngroup",
        .doc = "Replace a Group node with the nodes of the group it runs, rewired in place. Returns the added node names.",
        .positionalDoc = "group node",
        .run = [](const Args& a) -> json {
            std::string node = positional(a, 1, "gnUngroup", "node");
            return edit(a, "gnUngroup", [&](Group& grp, auto&) -> json { return ungroup(grp, node, "gnUngroup"); });
        },
    });

    ops::registerCommand({
        .name = "gnSetActiveViewer",
        .doc = "Switch a Viewer node on (the viewport previews its geometry coloured by its value) and every other Viewer in the group "
               "off. An empty node switches them all off.",
        .positionalDoc = "group [node]",
        .run = [](const Args& a) -> json {
            std::string node = a.positional.size() > 1 && a.positional[1].is_string() ? a.positional[1].get<std::string>() : "";
            return edit(a, "gnSetActiveViewer", [&](Group& grp, auto&) -> json {
                if (!node.empty())
                {
                    const GNode* v = grp.node(node);
                    if (!v || v->type != "Viewer")
                        throw CommandError("gnSetActiveViewer: " + node + " is not a Viewer node in " + grp.name);
                }
                for (GNode& n : grp.nodes)
                    if (n.type == "Viewer")
                    {
                        if (n.name == node)
                            n.values["active"] = true;
                        else
                            n.values.erase("active");
                    }
                return node.empty() ? json() : json(node);
            });
        },
    });

    ops::registerCommand({
        .name = "gnExportGroup",
        .doc = "Write a group to a .lilanodes file (JSON with a format and version header) together with every group nested in it, "
               "for sharing between scenes and people. Returns the path.",
        .positionalDoc = "group path",
        .undoable = false,
        .run = [](const Args& a) -> json {
            Node& n = groupNodeOf(positional(a, 0, "gnExportGroup", "group"), "gnExportGroup");
            fs::path path = paths::fromUtf8(positional(a, 1, "gnExportGroup", "path"));
            if (path.extension().empty())
                path += ".lilanodes";
            Group grp = readOrFail(n, "gnExportGroup");
            std::error_code ec;
            if (path.has_parent_path())
                fs::create_directories(path.parent_path(), ec);
            std::ofstream out(path, std::ios::binary | std::ios::trunc);
            if (!(out << exportDocument(grp).dump(2)))
                throw CommandError("gnExportGroup: can't write " + paths::toUtf8(path));
            return paths::toUtf8(path);
        },
    });

    ops::registerCommand({
        .name = "gnImportGroup",
        .doc = "Read a .lilanodes file (or a group .json) into the scene, nested groups first. A group the scene already has is "
               "replaced and its modifiers update. Returns the group names, the file's main group last.",
        .positionalDoc = "path",
        .run = [](const Args& a) -> json { return importFile(paths::fromUtf8(positional(a, 0, "gnImportGroup", "path")), "gnImportGroup"); },
    });

    ops::registerCommand({
        .name = "gnBuildGroup",
        .doc = "Create a group, or replace one's whole definition, from a compact spec: {name, description, inputs: [{name, type, "
               "default, label, description, subtype, min, max, softMin, softMax, step, panel}], outputs: [{name, type}], nodes: "
               "[{name, type, props: {...}, values: {...}, group (for Group nodes), position}], links: ['A.out -> B.in', ...], "
               "dependencies: [spec, ...]}. GroupInput and GroupOutput are added when missing; positions are laid out when "
               "omitted. Nothing changes unless the whole spec is valid; the error lists every problem with the valid socket "
               "names. Modifiers already using the group keep their values. Returns the group info.",
        .positionalDoc = "spec",
        .flags = {{"replace", "", Arg::Bool, true, "Replace an existing group of that name; False fails instead."}},
        .run = [](const Args& a) -> json {
            LILA_ZONE();

            if (a.positional.empty())
                throw CommandError("gnBuildGroup: spec required");
            json spec = a.positional[0];
            if (spec.is_string())
            {
                try
                {
                    spec = json::parse(spec.get<std::string>());
                }
                catch (const json::exception& e)
                {
                    throw CommandError(std::string("gnBuildGroup: spec isn't valid JSON: ") + e.what());
                }
            }
            bool replace = a.get<bool>("replace", true);
            std::map<std::string, json> available = availableGroups();
            std::vector<Group> built;
            SpecError err;
            std::vector<json> specs = spec.is_object() ? spec.value("dependencies", json::array()).get<std::vector<json>>() : std::vector<json>{};
            specs.push_back(spec);
            for (const json& s : specs)
            {
                SpecError one;
                Group g = buildFromSpec(s, one, available);
                for (const std::string& l : one.lines)
                    err.add((g.name.empty() ? std::string("spec") : g.name) + ": " + l);
                if (one.lines.empty())
                {
                    if (!replace && findGroupNode(Graph::current(), g.name))
                        err.add(g.name + ": the scene already has this group (pass replace=True)");
                    available[g.name] = g.toJson();
                    built.push_back(std::move(g));
                }
            }
            if (!err.lines.empty())
            {
                std::string msg = "gnBuildGroup: " + std::to_string(err.lines.size()) + " problem" + (err.lines.size() == 1 ? "" : "s");
                for (const std::string& l : err.lines)
                    msg += "\n  " + l;
                throw CommandError(msg);
            }
            for (Group& g : built)
            {
                try
                {
                    installGroup(g);
                }
                catch (const std::exception& e)
                {
                    throw CommandError("gnBuildGroup: " + g.name + ": " + e.what());
                }
            }
            return ops::execute("gnGroupInfo", {{"args", json::array({built.back().name})}});
        },
    });
}
} // namespace detail
} // namespace lila::gn
